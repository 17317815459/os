#ifndef	_ORANGES_VFS_H_
#define	_ORANGES_VFS_H_
#include <string.h>
#define uint32 unsigned int
/*
structs defined here can be used  by  any file system;
*/

#define MAX_FILE_NAME_LENGTH 32
typedef struct inode
{
    uint32 ino; // inode 编号
    uint32 mode;  // 文件的访问权限
    uint32 link_count; // 硬链接的数量
    uint32 uid; 
    uint32 gid;
    uint32 ctime; // create time 
    uint32 mtime; // modified time 
    uint32 atime; // access time 
    uint32 start_addr; // start addr of fat 
    uint32 file_type;  // dir,ordinary,char special etc.
    uint32 block_count; 
    uint32 size;
    inode_opearation i_ops;
}inode;
typedef struct super_block
{
    uint32 fs_magic;
    uint32 dev_num;
    uint32 block_number;
    uint32 inode_number;
    uint32 free_inode_number;
    uint32 free_block_number;
    super_operations* s_ops;
}super_block;

typedef struct  super_operations
{
    inode *(*create_file)(inode * pInode,
    int file_type,char *name[MAX_FILE_NAME_LENGTH] ); //创建并初始化一个inode,以其父节点为坐标
    int (*write_inode)(inode*,int);
    inode*  (*read_iode)(super_block*,int ino); // 用inode number 读出一个inode，返回值待定
    // 由于没有malloc，在函数内部出来的都是栈内存，需要先进的内存管理机制以分配堆内存
    int (*sync_inode)(super_block*,inode*);
}super_operations;
typedef struct inode_opearation
{
    void (*i_link)();
    void (*i_follow_link)();
    int (*look_up)(inode *p_inode,char *name[MAX_FILE_NAME_LENGTH]); // 找到一个子目录的ino
    int (*write_at)(inode *i,int offset,char data[]);
    int (*remove)(inode* i , char *name[MAX_FILE_NAME_LENGTH]); // 删除一个当前目录下的文件
    // stat, 
}inode_opearation;

typedef struct file_desc {
	int		fd_mode;	/**< R or W */
	int		fd_pos;		/**< Current position for R/W. */
	int		fd_cnt;		/**< How many procs share this desc */
	struct inode*	fd_inode;	/**< Ptr to the i-node */
}file_desc;
//
typedef struct vfs_mount
{
    uint32 fd_magic;
    super_block* sb;
    char MOUNT_POINT[30];
}vfs_mount;

#endif