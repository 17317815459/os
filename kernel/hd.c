/*************************************************************************//**
 *****************************************************************************
 * @file   hd.c
 * @brief  Hard disk (winchester) driver.
 * The `device nr' in this file means minor device nr.
 * @author Forrest Y. Yu
 * @date   2005~2008
 *****************************************************************************
 *****************************************************************************/

#include "type.h"
#include "stdio.h"
#include "const.h"
#include "protect.h"
#include "string.h"
#include "fs.h"
#include "proc.h"
#include "tty.h"
#include "console.h"
#include "global.h"
#include "proto.h"
#include "hd.h"


PRIVATE void	init_hd			();
PRIVATE void	hd_open			(int device);
PRIVATE void	hd_close		(int device);
PRIVATE void	hd_rdwt			(MESSAGE * p);
PRIVATE void	hd_ioctl		(MESSAGE * p);
PRIVATE void	hd_cmd_out		(struct hd_cmd* cmd);
PRIVATE void	get_part_table		(int drive, int sect_nr, struct part_ent * entry);
PRIVATE void	partition		(int device, int style);
PRIVATE void	print_hdinfo		(struct hd_info * hdi);
PRIVATE int	waitfor			(int mask, int val, int timeout);
PRIVATE void	interrupt_wait		();
PRIVATE	void	hd_identify		(int drive);
PRIVATE void	print_identify_info	(u16* hdinfo);


PRIVATE	u8		hd_status;
PRIVATE	u8		hdbuf[SECTOR_SIZE * 2];
PRIVATE	struct hd_info	hd_info[1];

//PUBLIC struct buf_node rwbuff[RWBUF_SIZE];                                           //缓存区

PUBLIC struct rw_buf rwbuf;                                                //链表缓冲区

#define	DRV_OF_DEV(dev) (dev <= MAX_PRIM ? \
			 dev / NR_PRIM_PER_DRIVE : \
			 (dev - MINOR_hd1a) / NR_SUB_PER_DRIVE)

/*****************************************************************************
 *                                task_hd
 *****************************************************************************/
/**
 * Main loop of HD driver.
 * 
 *****************************************************************************/
PUBLIC void task_hd()
{
	MESSAGE msg;

	init_hd();

	while (1) {
		send_recv(RECEIVE, ANY, &msg);

		int src = msg.source;

		switch (msg.type) {
		case DEV_OPEN:
			hd_open(msg.DEVICE);
			break;

		case DEV_CLOSE:
			hd_close(msg.DEVICE);
			break;

		case DEV_READ:
		case DEV_WRITE:
			hd_rdwt(&msg);
			break;

		case DEV_IOCTL:
			hd_ioctl(&msg);
			break;

		default:
			dump_msg("HD driver::unknown msg", &msg);
			spin("FS::main_loop (invalid msg.type)");
			break;
		}

		send_recv(SEND, src, &msg);
	}
}

/*****************************************************************************
 *                                init_hd
 *****************************************************************************/
/**
 * <Ring 1> Check hard drive, set IRQ handler, enable IRQ and initialize data
 *          structures.
 *****************************************************************************/
PRIVATE void init_hd()
{
	init_rwbuf();
	int i;

	/* Get the number of drives from the BIOS data area */
	u8 * pNrDrives = (u8*)(0x475);
	printl("NrDrives:%d.\n", *pNrDrives);
	assert(*pNrDrives);

	put_irq_handler(AT_WINI_IRQ, hd_handler);
	enable_irq(CASCADE_IRQ);
	enable_irq(AT_WINI_IRQ);

	for (i = 0; i < (sizeof(hd_info) / sizeof(hd_info[0])); i++)
		memset(&hd_info[i], 0, sizeof(hd_info[0]));
	hd_info[0].open_cnt = 0;
}

/*****************************************************************************
 *                                hd_open
 *****************************************************************************/
/**
 * <Ring 1> This routine handles DEV_OPEN message. It identify the drive
 * of the given device and read the partition table of the drive if it
 * has not been read.
 * 
 * @param device The device to be opened.
 *****************************************************************************/
PRIVATE void hd_open(int device)
{
	int drive = DRV_OF_DEV(device);
	assert(drive == 0);	/* only one drive */

	hd_identify(drive);

	if (hd_info[drive].open_cnt++ == 0) {
		partition(drive * (NR_PART_PER_DRIVE + 1), P_PRIMARY);
		print_hdinfo(&hd_info[drive]);
	}
}

/*****************************************************************************
 *                                hd_close
 *****************************************************************************/
/**
 * <Ring 1> This routine handles DEV_CLOSE message. 
 * 
 * @param device The device to be opened.
 *****************************************************************************/
PRIVATE void hd_close(int device)
{
	int drive = DRV_OF_DEV(device);
	assert(drive == 0);	/* only one drive */

	hd_info[drive].open_cnt--;
}


/*****************************************************************************
 *                                hd_rdwt
 *****************************************************************************/
/**
 * <Ring 1> This routine handles DEV_READ and DEV_WRITE message.
 * 
 * @param p Message ptr.
 *****************************************************************************/
PRIVATE void hd_rdwt(MESSAGE * p)
{

	/*Step1. 判断请求是否合法，并计算要读/写的扇区号*/
	int drive = DRV_OF_DEV(p->DEVICE);

	u64 pos = p->POSITION;
	assert((pos >> SECTOR_SIZE_SHIFT) < (1 << 31));     //最大不超过2^30KB


	assert((pos & 0x1FF) == 0);        //只能按扇区读写

	u32 sect_nr = (u32)(pos >> SECTOR_SIZE_SHIFT);              //位于当前逻辑分区的第几个扇区
	int logidx = (p->DEVICE - MINOR_hd1a) % NR_SUB_PER_DRIVE;       //第几个逻辑分区
	sect_nr += p->DEVICE < MAX_PRIM ?
		hd_info[drive].primary[p->DEVICE].base :                   //再加上当前逻辑分区的首扇区号
		hd_info[drive].logical[logidx].base;

    /*判断是否是单个扇区并且在缓冲区内，如果是，就直接读缓冲区*/
	if(p->CNT<=SECTOR_SIZE){
		if(p->type==DEV_READ){
			struct buf_node* node=rwexist(sect_nr);
			if(node){
				phys_copy((void*)va2la(p->PROC_NR, p->BUF), (void*)va2la(TASK_HD, node->secbuf),SECTOR_SIZE);
				return;
			}
		}
	}

	/*Step2. 建立一个命令结构体，包括数据长度，数据位置，命令类型等内容，并向端口写入*/
	struct hd_cmd cmd;
	cmd.features	= 0;
	cmd.count	= (p->CNT + SECTOR_SIZE - 1) / SECTOR_SIZE;               //消息中的数据占用几个扇区
	cmd.lba_low	= sect_nr & 0xFF;                                         //扇区号的低中高8位
	cmd.lba_mid	= (sect_nr >>  8) & 0xFF;
	cmd.lba_high	= (sect_nr >> 16) & 0xFF;
	cmd.device	= MAKE_DEVICE_REG(1, drive, (sect_nr >> 24) & 0xF);           //lba寻址，0号驱动(主设备)
	cmd.command	= (p->type == DEV_READ) ? ATA_READ : ATA_WRITE;                //读/写指令
	hd_cmd_out(&cmd);                                                          //将内容写到端口寄存器


	/*Step3. 获取消息中数据的物理地址，根据命令类型，和数据长度向端口读/写数据*/
	int bytes_left = p->CNT;                                                  //剩余要写的字节数
	void * la = (void*)va2la(p->PROC_NR, p->BUF);                             //将消息中buf的虚拟地址转换为物理地址

	if(p->type==DEV_WRITE){
		invalid_buf(sect_nr, sect_nr+(p->CNT / SECTOR_SIZE));
	}

	u32 idx=sect_nr;
	while (bytes_left) {
		int bytes = min(SECTOR_SIZE, bytes_left);                          //每次读写最多一个扇区，512字节
		if (p->type == DEV_READ) {                                          //读指令
			interrupt_wait();                                               //等待硬件处理完成后通知，然后才能继续执行
			port_read(REG_DATA, hdbuf, SECTOR_SIZE);                        //从data寄存器处读出512个字节的数据到hdbuf
			phys_copy(la, (void*)va2la(TASK_HD, hdbuf), bytes);             //将hdbuf地址转换为物理地址，然后将hdbuf内容复制到消息中的buf处

			struct buf_node* node=get_empty_buf(idx);
			phys_copy((void*)va2la(TASK_HD, node->secbuf), la, bytes);        //将读取的扇区内容写入缓冲区
		}
		else {                                                                //写指令
			if (!waitfor(STATUS_DRQ, STATUS_DRQ, HD_TIMEOUT))                 //确认是否可写（状态空闲？）
				panic("hd writing error.");
 
			port_write(REG_DATA, la, bytes);                                 //将bytes个字节的数据从消息中buf的物理地址la处写到data寄存器中
			interrupt_wait();                                                //等待硬件处理完成

			struct buf_node* node=get_empty_buf(idx);
			phys_copy((void*)va2la(TASK_HD, node->secbuf), la, bytes);        //将写入的扇区内容写入缓冲区
		}
		bytes_left -= SECTOR_SIZE;
		la += SECTOR_SIZE;  
		idx++;                                                 
	}
}															


/*****************************************************************************
 *                                hd_ioctl
 *****************************************************************************/
/**
 * <Ring 1> This routine handles the DEV_IOCTL message.
 * 
 * @param p  Ptr to the MESSAGE.
 *****************************************************************************/
PRIVATE void hd_ioctl(MESSAGE * p)
{
	int device = p->DEVICE;
	int drive = DRV_OF_DEV(device);

	struct hd_info * hdi = &hd_info[drive];

	if (p->REQUEST == DIOCTL_GET_GEO) {
		void * dst = va2la(p->PROC_NR, p->BUF);
		void * src = va2la(TASK_HD,
				   device < MAX_PRIM ?
				   &hdi->primary[device] :
				   &hdi->logical[(device - MINOR_hd1a) %
						NR_SUB_PER_DRIVE]);

		phys_copy(dst, src, sizeof(struct part_info));
	}
	else {
		assert(0);
	}
}

/*****************************************************************************
 *                                get_part_table
 *****************************************************************************/
/**
 * <Ring 1> Get a partition table of a drive.
 * 
 * @param drive   Drive nr (0 for the 1st disk, 1 for the 2nd, ...)n
 * @param sect_nr The sector at which the partition table is located.
 * @param entry   Ptr to part_ent struct.
 *****************************************************************************/
PRIVATE void get_part_table(int drive, int sect_nr, struct part_ent * entry)
{
	struct hd_cmd cmd;
	cmd.features	= 0;
	cmd.count	= 1;
	cmd.lba_low	= sect_nr & 0xFF;
	cmd.lba_mid	= (sect_nr >>  8) & 0xFF;
	cmd.lba_high	= (sect_nr >> 16) & 0xFF;
	cmd.device	= MAKE_DEVICE_REG(1, /* LBA mode*/
					  drive,
					  (sect_nr >> 24) & 0xF);
	cmd.command	= ATA_READ;
	hd_cmd_out(&cmd);
	interrupt_wait();

	port_read(REG_DATA, hdbuf, SECTOR_SIZE);
	memcpy(entry,
	       hdbuf + PARTITION_TABLE_OFFSET,
	       sizeof(struct part_ent) * NR_PART_PER_DRIVE);
}

/*****************************************************************************
 *                                partition
 *****************************************************************************/
/**
 * <Ring 1> This routine is called when a device is opened. It reads the
 * partition table(s) and fills the hd_info struct.
 * 
 * @param device Device nr.
 * @param style  P_PRIMARY or P_EXTENDED.
 *****************************************************************************/
PRIVATE void partition(int device, int style)
{
	int i;
	int drive = DRV_OF_DEV(device);
	struct hd_info * hdi = &hd_info[drive];

	struct part_ent part_tbl[NR_SUB_PER_DRIVE];

	if (style == P_PRIMARY) {
		get_part_table(drive, drive, part_tbl);

		int nr_prim_parts = 0;
		for (i = 0; i < NR_PART_PER_DRIVE; i++) { /* 0~3 */
			if (part_tbl[i].sys_id == NO_PART) 
				continue;

			nr_prim_parts++;
			int dev_nr = i + 1;		  /* 1~4 */
			hdi->primary[dev_nr].base = part_tbl[i].start_sect;
			hdi->primary[dev_nr].size = part_tbl[i].nr_sects;

			if (part_tbl[i].sys_id == EXT_PART) /* extended */
				partition(device + dev_nr, P_EXTENDED);
		}
		assert(nr_prim_parts != 0);
	}
	else if (style == P_EXTENDED) {
		int j = device % NR_PRIM_PER_DRIVE; /* 1~4 */
		int ext_start_sect = hdi->primary[j].base;
		int s = ext_start_sect;
		int nr_1st_sub = (j - 1) * NR_SUB_PER_PART; /* 0/16/32/48 */

		for (i = 0; i < NR_SUB_PER_PART; i++) {
			int dev_nr = nr_1st_sub + i;/* 0~15/16~31/32~47/48~63 */

			get_part_table(drive, s, part_tbl);

			hdi->logical[dev_nr].base = s + part_tbl[0].start_sect;
			hdi->logical[dev_nr].size = part_tbl[0].nr_sects;

			s = ext_start_sect + part_tbl[1].start_sect;

			/* no more logical partitions
			   in this extended partition */
			if (part_tbl[1].sys_id == NO_PART)
				break;
		}
	}
	else {
		assert(0);
	}
}

/*****************************************************************************
 *                                print_hdinfo
 *****************************************************************************/
/**
 * <Ring 1> Print disk info.
 * 
 * @param hdi  Ptr to struct hd_info.
 *****************************************************************************/
PRIVATE void print_hdinfo(struct hd_info * hdi)
{
	int i;
	for (i = 0; i < NR_PART_PER_DRIVE + 1; i++) {
		printl("%sPART_%d: base %d(0x%x), size %d(0x%x) (in sector)\n",
		       i == 0 ? " " : "     ",
		       i,
		       hdi->primary[i].base,
		       hdi->primary[i].base,
		       hdi->primary[i].size,
		       hdi->primary[i].size);
	}
	for (i = 0; i < NR_SUB_PER_DRIVE; i++) {
		if (hdi->logical[i].size == 0)
			continue;
		printl("         "
		       "%d: base %d(0x%x), size %d(0x%x) (in sector)\n",
		       i,
		       hdi->logical[i].base,
		       hdi->logical[i].base,
		       hdi->logical[i].size,
		       hdi->logical[i].size);
	}
}

/*****************************************************************************
 *                                hd_identify
 *****************************************************************************/
/**
 * <Ring 1> Get the disk information.
 * 
 * @param drive  Drive Nr.
 *****************************************************************************/
PRIVATE void hd_identify(int drive)
{
	struct hd_cmd cmd;
	cmd.device  = MAKE_DEVICE_REG(0, drive, 0);
	cmd.command = ATA_IDENTIFY;
	hd_cmd_out(&cmd);
	interrupt_wait();
	port_read(REG_DATA, hdbuf, SECTOR_SIZE);

	print_identify_info((u16*)hdbuf);

	u16* hdinfo = (u16*)hdbuf;

	hd_info[drive].primary[0].base = 0;
	/* Total Nr of User Addressable Sectors */
	hd_info[drive].primary[0].size = ((int)hdinfo[61] << 16) + hdinfo[60];
}

/*****************************************************************************
 *                            print_identify_info
 *****************************************************************************/
/**
 * <Ring 1> Print the hdinfo retrieved via ATA_IDENTIFY command.
 * 
 * @param hdinfo  The buffer read from the disk i/o port.
 *****************************************************************************/
PRIVATE void print_identify_info(u16* hdinfo)
{
	int i, k;
	char s[64];

	struct iden_info_ascii {
		int idx;
		int len;
		char * desc;
	} iinfo[] = {{10, 20, "HD SN"}, /* Serial number in ASCII */
		     {27, 40, "HD Model"} /* Model number in ASCII */ };

	for (k = 0; k < sizeof(iinfo)/sizeof(iinfo[0]); k++) {
		char * p = (char*)&hdinfo[iinfo[k].idx];
		for (i = 0; i < iinfo[k].len/2; i++) {
			s[i*2+1] = *p++;
			s[i*2] = *p++;
		}
		s[i*2] = 0;
		printl("%s: %s\n", iinfo[k].desc, s);
	}

	int capabilities = hdinfo[49];
	printl("LBA supported: %s\n",
	       (capabilities & 0x0200) ? "Yes" : "No");

	int cmd_set_supported = hdinfo[83];
	printl("LBA48 supported: %s\n",
	       (cmd_set_supported & 0x0400) ? "Yes" : "No");

	int sectors = ((int)hdinfo[61] << 16) + hdinfo[60];
	printl("HD size: %dMB\n", sectors * 512 / 1000000);
}

/*****************************************************************************
 *                                hd_cmd_out
 *****************************************************************************/
/**
 * <Ring 1> Output a command to HD controller.
 * 
 * @param cmd  The command struct ptr.
 *****************************************************************************/
PRIVATE void hd_cmd_out(struct hd_cmd* cmd)
{
	/**
	 * For all commands, the host must first check if BSY=1,
	 * and should proceed no further unless and until BSY=0
	 */
	if (!waitfor(STATUS_BSY, 0, HD_TIMEOUT))
		panic("hd error.");

	/* Activate the Interrupt Enable (nIEN) bit */
	out_byte(REG_DEV_CTRL, 0);
	/* Load required parameters in the Command Block Registers */
	out_byte(REG_FEATURES, cmd->features);
	out_byte(REG_NSECTOR,  cmd->count);
	out_byte(REG_LBA_LOW,  cmd->lba_low);
	out_byte(REG_LBA_MID,  cmd->lba_mid);
	out_byte(REG_LBA_HIGH, cmd->lba_high);
	out_byte(REG_DEVICE,   cmd->device);
	/* Write the command code to the Command Register */
	out_byte(REG_CMD,     cmd->command);
}

/*****************************************************************************
 *                                interrupt_wait
 *****************************************************************************/
/**
 * <Ring 1> Wait until a disk interrupt occurs.
 * 
 *****************************************************************************/
PRIVATE void interrupt_wait()
{
	MESSAGE msg;
	send_recv(RECEIVE, INTERRUPT, &msg);
}

/*****************************************************************************
 *                                waitfor
 *****************************************************************************/
/**
 * <Ring 1> Wait for a certain status.
 * 
 * @param mask    Status mask.
 * @param val     Required status.
 * @param timeout Timeout in milliseconds.
 * 
 * @return One if sucess, zero if timeout.
 *****************************************************************************/
PRIVATE int waitfor(int mask, int val, int timeout)
{
	int t = get_ticks();

	while(((get_ticks() - t) * 1000 / HZ) < timeout)
		if ((in_byte(REG_STATUS) & mask) == val)
			return 1;

	return 0;
}

/*****************************************************************************
 *                                hd_handler
 *****************************************************************************/
/**
 * <Ring 0> Interrupt handler.
 * 
 * @param irq  IRQ nr of the disk interrupt.
 *****************************************************************************/
PUBLIC void hd_handler(int irq)
{
	/*
	 * Interrupts are cleared when the host
	 *   - reads the Status Register,
	 *   - issues a reset, or
	 *   - writes to the Command Register.
	 */
	hd_status = in_byte(REG_STATUS);

	inform_int(TASK_HD);
}


PRIVATE void init_rwbuf()                          //初始化缓存区
{
	// for(int i=0;i<RWBUF_SIZE;i++){
	// 	rwbuff[i].valid=0;
	// }


	rwbuf.first_node=0;
	rwbuf.last_node=0;
	rwbuf.node_nr=0;
}

PRIVATE struct buf_node* rwexist(u32 sec_nr){                   //扇区是否在缓存区内,并移动节点
	// for(int i=0;i<RWBUF_SIZE;i++)
	// {
	// 	if(rwbuff[i].valid==1&&rwbuff[i].sec_nr==sec_nr) return i;
	// }
	// return -1;


	struct buf_node* node=rwbuf.first_node;
	if(node->sec_nr==sec_nr){                      //先判断第一个结点
		rwbuf.last_node->next_node=node;
		rwbuf.last_node=node;
		rwbuf.last_node->next_node=0;
		rwbuf.first_node=rwbuf.first_node->next_node;
		return rwbuf.last_node;
	}
	for(int i=1;i<rwbuf.node_nr;i++)
	{
		if(node->next_node->sec_nr==sec_nr){
			rwbuf.last_node->next_node=node->next_node;
			rwbuf.last_node=node->next_node;
			rwbuf.last_node->next_node=0;
			node->next_node=node->next_node->next_node;

			return rwbuf.last_node;
		}
		else node=node->next_node;
	}
	return 0;
} 



PRIVATE struct buf_node* get_empty_buf(u32 idx)                       //分配缓冲区
{
	// for(int i=0;i<RWBUF_SIZE;i++)
	// {
	// 	if(rwbuff[i].valid==0){ 
	// 		rwbuff[i].valid=1;
	// 		rwbuff[i].sec_nr=idx;
	// 		return i;
	// 		}
	// }
	// rwbuff[0].sec_nr=idx;
	// return 0;


	struct buf_node* node;
	//node->valid=1;
	node->sec_nr=idx;
	node->next_node=0;
	if(rwbuf.node_nr==0)                                //缓存为空
	{
		rwbuf.first_node=node;
		rwbuf.last_node=node;
		rwbuf.node_nr++;
		return rwbuf.last_node;
	}
	else if(rwbuf.node_nr<RWBUF_SIZE){                    //缓存未满
		rwbuf.last_node->next_node=node;
		rwbuf.last_node=node;
		rwbuf.node_nr++;
		return rwbuf.last_node;
	}
	else if(rwbuf.node_nr==RWBUF_SIZE){                   //缓存已满，替换掉第一个
		rwbuf.last_node->next_node=node;
		rwbuf.last_node=node;
		rwbuf.first_node=rwbuf.first_node->next_node;
		return rwbuf.last_node;
	}
	return 0;
}

PRIVATE void invalid_buf(u32 start, u32 end){                   //将缓存设为无效
	// for(int i=1;i<RWBUF_SIZE;i++)
	// {
	// 	if(rwbuff[i].sec_nr>=start&&rwbuff[i].sec_nr<=end)
	// 		rwbuff[i].valid=0;
	// }


	struct buf_node* node=rwbuf.first_node;

	if(node->sec_nr>=start&&node->sec_nr<=end){            //先判断第一个
		rwbuf.first_node=rwbuf.first_node->next_node;
		node=rwbuf.first_node;
		rwbuf.node_nr--;
	}
	while(node)                                 //判断以后的
	{
		if(node->next_node->sec_nr>=start&&node->next_node->sec_nr<=end){
		node->next_node=node->next_node->next_node;
		rwbuf.node_nr--;
		}

		node=node->next_node;
	}
}