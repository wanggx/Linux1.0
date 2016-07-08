#ifndef _LINUX_FS_H
#define _LINUX_FS_H

/*
 * This file has definitions for some important file table
 * structures etc.
 */

#include <linux/linkage.h>
#include <linux/limits.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/dirent.h>
#include <linux/vfs.h>
#include <linux/net.h>

/*
 * It's silly to have NR_OPEN bigger than NR_FILE, but I'll fix
 * that later. Anyway, now the file code is no longer dependent
 * on bitmaps in unsigned longs, but uses the new fd_set structure..
 *
 * Some programs (notably those using select()) may have to be 
 * recompiled to take full advantage of the new limits..
 */
#undef NR_OPEN
#define NR_OPEN 256

#define NR_INODE 2048	/* this should be bigger than NR_FILE */
#define NR_FILE 1024	/* this can well be larger on a larger system */
#define NR_SUPER 32
#define NR_HASH 997
#define NR_IHASH 131
#define NR_FILE_LOCKS 64
#define BLOCK_SIZE 1024
#define BLOCK_SIZE_BITS 10

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

#define READ 0
#define WRITE 1
#define READA 2		/* read-ahead - don't pause */
#define WRITEA 3	/* "write-ahead" - silly, but somewhat useful */

extern void buffer_init(void);
extern unsigned long inode_init(unsigned long start, unsigned long end);
extern unsigned long file_table_init(unsigned long start, unsigned long end);

#define MAJOR(a) (int)((unsigned short)(a) >> 8)   /* 主设备号 */
#define MINOR(a) (int)((unsigned short)(a) & 0xFF) /* 次设备号，此设备号在最低8位 */
#define MKDEV(a,b) ((int)((((a) & 0xff) << 8) | ((b) & 0xff)))  /* 生成一个设备号，包括主次设备号 */

#ifndef NULL
#define NULL ((void *) 0)
#endif

#define NIL_FILP	((struct file *)0)
#define SEL_IN		1	/* 探测数据是否可以读取 */
#define SEL_OUT		2 	/* 探测对应套接字发送缓冲区的空闲大小 */
#define SEL_EX		4	/* 探测之前操作是否有错误发生 */

/*
 * These are the fs-independent mount-flags: up to 16 flags are supported
 */

/* 文件系统的挂载标记 */
#define MS_RDONLY    1 /* mount read-only */
#define MS_NOSUID    2 /* ignore suid and sgid bits */
#define MS_NODEV     4 /* disallow access to device special files */
#define MS_NOEXEC    8 /* disallow program execution */
#define MS_SYNC     16 /* writes are synced at once */
#define	MS_REMOUNT  32 /* alter flags of a mounted FS */

/*
 * Flags that can be altered by MS_REMOUNT
 */
#define MS_RMT_MASK (MS_RDONLY)

/*
 * Magic mount flag number. Has to be or-ed to the flag values.
 */
#define MS_MGC_VAL 0xC0ED0000 /* magic flag number to indicate "new" flags */
#define MS_MGC_MSK 0xffff0000 /* magic flag number mask */

/*
 * Note that read-only etc flags are inode-specific: setting some file-system
 * flags just means all the inodes inherit those flags by default. It might be
 * possible to overrride it sevelctively if you really wanted to with some
 * ioctl() that is not currently implemented.
 *
 * Exception: MS_RDONLY is always applied to the entire file system.
 */
#define IS_RDONLY(inode) (((inode)->i_sb) && ((inode)->i_sb->s_flags & MS_RDONLY))
#define IS_NOSUID(inode) ((inode)->i_flags & MS_NOSUID)
#define IS_NODEV(inode) ((inode)->i_flags & MS_NODEV)
#define IS_NOEXEC(inode) ((inode)->i_flags & MS_NOEXEC)
#define IS_SYNC(inode) ((inode)->i_flags & MS_SYNC)

/* the read-only stuff doesn't really belong here, but any other place is
   probably as bad and I don't want to create yet another include file. */

#define BLKROSET 4701 /* set device read-only (0 = read-write) */
#define BLKROGET 4702 /* get read-only status (0 = read_write) */
#define BLKRRPART 4703 /* re-read partition table */
#define BLKGETSIZE 4704 /* return device size */
#define BLKFLSBUF 4705 /* flush buffer cache */

/* These are a few other constants  only used by scsi  devices */

#define SCSI_IOCTL_GET_IDLUN 0x5382

/* Used to turn on and off tagged queueing for scsi devices */

#define SCSI_IOCTL_TAGGED_ENABLE 0x5383
#define SCSI_IOCTL_TAGGED_DISABLE 0x5384


#define BMAP_IOCTL 1	/* obsolete - kept for compatibility */
#define FIBMAP	   1	/* bmap access */
#define FIGETBSZ   2	/* get the block size used for bmap */

/* these flags tell notify_change what is being changed */

#define NOTIFY_SIZE	1
#define NOTIFY_MODE	2
#define NOTIFY_TIME	4
#define NOTIFY_UIDGID	8

typedef char buffer_block[BLOCK_SIZE];

struct buffer_head {
	char * b_data;			/* pointer to data block (1024 bytes) */
	unsigned long b_size;		/* block size */
	unsigned long b_blocknr;	/* block number */
	dev_t b_dev;			/* device (0 = free) */
	unsigned short b_count;		/* users using this block */
	unsigned char b_uptodate;
	unsigned char b_dirt;		/* 0-clean,1-dirty */
	unsigned char b_lock;		/* 0 - ok, 1 -locked */
	unsigned char b_req;		/* 0 if the buffer has been invalidated */
	struct wait_queue * b_wait;
	struct buffer_head * b_prev;		/* doubly linked list of hash-queue */
	struct buffer_head * b_next;
	struct buffer_head * b_prev_free;	/* doubly linked list of buffers */
	struct buffer_head * b_next_free;
	struct buffer_head * b_this_page;	/* circular list of buffers in one page */
	struct buffer_head * b_reqnext;		/* request queue */
};

#include <linux/pipe_fs_i.h>
#include <linux/minix_fs_i.h>
#include <linux/ext_fs_i.h>
#include <linux/ext2_fs_i.h>
#include <linux/hpfs_fs_i.h>
#include <linux/msdos_fs_i.h>
#include <linux/iso_fs_i.h>
#include <linux/nfs_fs_i.h>
#include <linux/xia_fs_i.h>
#include <linux/sysv_fs_i.h>


/* inode中有一部分数据是在磁盘当中的
 * 注意inode中并没有struct file的指针
 * 注意inode是VFS中一个重要的结构体，
 * inode中除了存储一些VFS中必须的数据，
 * 还存储了特定文件系统中的inode信息，
 * 如ext2中的struct ext2_inode_info等等
 */
struct inode {
	dev_t		i_dev;			/* 设备号 */
	unsigned long	i_ino;		/* i节点号 */
	umode_t		i_mode;			/* 文件类型以及文件权限 */
	nlink_t		i_nlink;		/* 硬链接计数 */
	uid_t		i_uid;          	/* 创建文件的用户ID */
	gid_t		i_gid;			/* 创建文件的用户组ID */
	dev_t		i_rdev;			/* 是设备标识符，也存储了主次设备号，
							* 表示设备的实际设备号，不是逻辑设备号 
							*/
	off_t		i_size;			/* 文件大小单位为字节 */
	time_t		i_atime;
	time_t		i_mtime;
	time_t		i_ctime;		/* inode的修改时间 */
	unsigned long	i_blksize;	/* 数据块大小 */
	unsigned long	i_blocks;	/* 文件数据块数 */
	struct semaphore i_sem;     /* 操作inode的信号量 */
	struct inode_operations * i_op;
	struct super_block * i_sb;      /*对应的超级块，在inode的读取或查找等操作会用到 */
	struct wait_queue * i_wait;		/* 操作inode的等待队列 */
	struct file_lock * i_flock;	    /* 文件锁结构，用于同步或控制 */
	struct vm_area_struct * i_mmap; /* 该文件映射到的虚拟地址段的地址 */
	struct inode * i_next, * i_prev;     /*空闲双向链表*/
	struct inode * i_hash_next, * i_hash_prev; /*hash双向链表*/
	struct inode * i_bound_to, * i_bound_by;
	struct inode * i_mount;		/* inode所在文件系统的挂载点 */
	struct socket * i_socket;  /*如果是网络inode，则指向网络数据*/
	unsigned short i_count;
	unsigned short i_flags;
	unsigned char i_lock;
	unsigned char i_dirt;
	unsigned char i_pipe;            /* 表明是管道对应的inode */
	unsigned char i_seek;
	unsigned char i_update;
	/* 因为Linux采用的时VFS文件系统，上面数据是所有文件系统都需要使用的数据 
	 * 下面的数据是相应文件系统对应的特定inode的信息
	 */
	union {
		struct pipe_inode_info pipe_i;
		struct minix_inode_info minix_i;
		struct ext_inode_info ext_i;
		struct ext2_inode_info ext2_i;
		struct hpfs_inode_info hpfs_i;
		struct msdos_inode_info msdos_i;
		struct iso_inode_info isofs_i;
		struct nfs_inode_info nfs_i;
		struct xiafs_inode_info xiafs_i;
		struct sysv_inode_info sysv_i;
	} u;
};

struct file {
	mode_t f_mode;		    /* 文件不存在时，创建文件的权限 */
	dev_t f_rdev;			/* needed for /dev/tty */
	off_t f_pos;            /* 文件读写偏移量 */
	unsigned short f_flags; /* 以什么样的方式打开文件，如只读，只写等等 */
	unsigned short f_count;  /*文件的引用计数*/
	unsigned short f_reada;
	struct file *f_next, *f_prev;
	struct inode * f_inode;		/* 文件对应的inode */
	struct file_operations * f_op;
};

/* 文件锁结构，该结构可以用来锁定文件字节中的某一段 */
struct file_lock {
	struct file_lock *fl_next;	/* singly linked list */
	struct task_struct *fl_owner;	/* NULL if on free list, for sanity checks */
        unsigned int fl_fd;             /* File descriptor for this lock */
	struct wait_queue *fl_wait;
	char fl_type;			/* 锁的类型 */
	char fl_whence;			/* 指定起始位置的特点，从头开始，还是当前开始，还是文件末尾 */
	off_t fl_start;
	off_t fl_end;
};

#include <linux/minix_fs_sb.h>
#include <linux/ext_fs_sb.h>
#include <linux/ext2_fs_sb.h>
#include <linux/hpfs_fs_sb.h>
#include <linux/msdos_fs_sb.h>
#include <linux/iso_fs_sb.h>
#include <linux/nfs_fs_sb.h>
#include <linux/xia_fs_sb.h>
#include <linux/sysv_fs_sb.h>

/* super_block和inode道理一样，除了超级块的公共信息之外，
 * 还包含一些特定文件系统的超级块的数据，该数据是用一个union
 * 来表示的。
 */
struct super_block {
	dev_t s_dev;					/*对应设备号*/
	unsigned long s_blocksize;      /* 设备数据块的大小 */
	unsigned char s_blocksize_bits; /* 块大小用2的幂次方表示的幂*/
	unsigned char s_lock;  /* 超级块是否被锁住 */
	unsigned char s_rd_only;
	unsigned char s_dirt;		/* 超级块的脏标记 */
	struct super_operations *s_op;
	unsigned long s_flags;       /* 超级块的挂载标记 */
	unsigned long s_magic;
	unsigned long s_time;
	/* 在Linux当中如果要将某个文件系统关在到某个目录
	  * 则会将挂载目录之前的所有内容给屏蔽掉，也就是看不见 
	  * 看见的则是挂载之后的文件系统的内容，如果将该挂载的文件 
	  * 系统卸载掉，则原来被屏蔽掉的内容又会重新显示出来  
	  */
	struct inode * s_covered;    
	struct inode * s_mounted;   /*文件系统的挂载点，如果是根文件系统则是/,否则就是挂载点的inode */
	struct wait_queue * s_wait; /* 等待操作超级块的进程队列 */
	/* 注意这部分信息是超级块的物理信息 */
	union {
		struct minix_sb_info minix_sb;
		struct ext_sb_info ext_sb;
		struct ext2_sb_info ext2_sb;
		struct hpfs_sb_info hpfs_sb;
		struct msdos_sb_info msdos_sb;
		struct isofs_sb_info isofs_sb;
		struct nfs_sb_info nfs_sb;		/* 网络文件系统的超级块 */
		struct xiafs_sb_info xiafs_sb;
		struct sysv_sb_info sysv_sb;
	} u;
};

/* 对inode对应文件的操作
 */
struct file_operations {
	int (*lseek) (struct inode *, struct file *, off_t, int);
	int (*read) (struct inode *, struct file *, char *, int);
	int (*write) (struct inode *, struct file *, char *, int);
	int (*readdir) (struct inode *, struct file *, struct dirent *, int);
	int (*select) (struct inode *, struct file *, int, select_table *);
	int (*ioctl) (struct inode *, struct file *, unsigned int, unsigned long);
	int (*mmap) (struct inode *, struct file *, unsigned long, size_t, int, unsigned long);
	int (*open) (struct inode *, struct file *);
	void (*release) (struct inode *, struct file *);
	int (*fsync) (struct inode *, struct file *);
};

/* 对inode的操作
 */
struct inode_operations {
	struct file_operations * default_file_ops;
	int (*create) (struct inode *,const char *,int,int,struct inode **);
	int (*lookup) (struct inode *,const char *,int,struct inode **);
	int (*link) (struct inode *,struct inode *,const char *,int);
	int (*unlink) (struct inode *,const char *,int);
	int (*symlink) (struct inode *,const char *,int,const char *);
	int (*mkdir) (struct inode *,const char *,int,int);
	int (*rmdir) (struct inode *,const char *,int);
	int (*mknod) (struct inode *,const char *,int,int,int);
	int (*rename) (struct inode *,const char *,int,struct inode *,const char *,int);
	int (*readlink) (struct inode *,char *,int);
	int (*follow_link) (struct inode *,struct inode *,int,int,struct inode **);
	int (*bmap) (struct inode *,int);
	void (*truncate) (struct inode *);
	int (*permission) (struct inode *, int);
};

struct super_operations {
	void (*read_inode) (struct inode *);
	int (*notify_change) (int flags, struct inode *);
	void (*write_inode) (struct inode *);
	void (*put_inode) (struct inode *);
	void (*put_super) (struct super_block *);
	void (*write_super) (struct super_block *);
	void (*statfs) (struct super_block *, struct statfs *);
	int (*remount_fs) (struct super_block *, int *, char *);
};

struct file_system_type {
	struct super_block *(*read_super) (struct super_block *, void *, int);
	char *name;
	int requires_dev;
};

#ifdef __KERNEL__

asmlinkage int sys_open(const char *, int, int);
asmlinkage int sys_close(unsigned int);		/* yes, it's really unsigned */

extern int getname(const char * filename, char **result);
extern void putname(char * name);

extern int register_blkdev(unsigned int, const char *, struct file_operations *);
extern int unregister_blkdev(unsigned int major, const char * name);
extern int blkdev_open(struct inode * inode, struct file * filp);
extern struct file_operations def_blk_fops;
extern struct inode_operations blkdev_inode_operations;

extern int register_chrdev(unsigned int, const char *, struct file_operations *);
extern int unregister_chrdev(unsigned int major, const char * name);
extern int chrdev_open(struct inode * inode, struct file * filp);
extern struct file_operations def_chr_fops;
extern struct inode_operations chrdev_inode_operations;

extern void init_fifo(struct inode * inode);

extern struct file_operations connecting_fifo_fops;
extern struct file_operations read_fifo_fops;
extern struct file_operations write_fifo_fops;
extern struct file_operations rdwr_fifo_fops;
extern struct file_operations read_pipe_fops;
extern struct file_operations write_pipe_fops;
extern struct file_operations rdwr_pipe_fops;

extern struct file_system_type *get_fs_type(char *name);

extern int fs_may_mount(dev_t dev);
extern int fs_may_umount(dev_t dev, struct inode * mount_root);
extern int fs_may_remount_ro(dev_t dev);

extern struct file *first_file;
extern int nr_files;
extern struct super_block super_blocks[NR_SUPER];

extern int shrink_buffers(unsigned int priority);

extern int nr_buffers;
extern int buffermem;
extern int nr_buffer_heads;

extern void check_disk_change(dev_t dev);
extern void invalidate_inodes(dev_t dev);
extern void invalidate_buffers(dev_t dev);
extern int floppy_change(struct buffer_head * first_block);
extern void sync_inodes(dev_t dev);
extern void sync_dev(dev_t dev);
extern int fsync_dev(dev_t dev);
extern void sync_supers(dev_t dev);
extern int bmap(struct inode * inode,int block);
extern int notify_change(int flags, struct inode * inode);
extern int namei(const char * pathname, struct inode ** res_inode);
extern int lnamei(const char * pathname, struct inode ** res_inode);
extern int permission(struct inode * inode,int mask);
extern int open_namei(const char * pathname, int flag, int mode,
	struct inode ** res_inode, struct inode * base);
extern int do_mknod(const char * filename, int mode, dev_t dev);
extern void iput(struct inode * inode);
extern struct inode * __iget(struct super_block * sb,int nr,int crsmnt);
extern struct inode * iget(struct super_block * sb,int nr);
extern struct inode * get_empty_inode(void);
extern void insert_inode_hash(struct inode *);
extern void clear_inode(struct inode *);
extern struct inode * get_pipe_inode(void);
extern struct file * get_empty_filp(void);
extern struct buffer_head * get_hash_table(dev_t dev, int block, int size);
extern struct buffer_head * getblk(dev_t dev, int block, int size);
extern void ll_rw_block(int rw, int nr, struct buffer_head * bh[]);
extern void ll_rw_page(int rw, int dev, int nr, char * buffer);
extern void ll_rw_swap_file(int rw, int dev, unsigned int *b, int nb, char *buffer);
extern void brelse(struct buffer_head * buf);
extern void set_blocksize(dev_t dev, int size);
extern struct buffer_head * bread(dev_t dev, int block, int size);
extern unsigned long bread_page(unsigned long addr,dev_t dev,int b[],int size,int prot);
extern struct buffer_head * breada(dev_t dev,int block,...);
extern void put_super(dev_t dev);
extern dev_t ROOT_DEV;

extern void show_buffers(void);
extern void mount_root(void);

extern int char_read(struct inode *, struct file *, char *, int);
extern int block_read(struct inode *, struct file *, char *, int);
extern int read_ahead[];

extern int char_write(struct inode *, struct file *, char *, int);
extern int block_write(struct inode *, struct file *, char *, int);

extern int generic_mmap(struct inode *, struct file *, unsigned long, size_t, int, unsigned long);

extern int block_fsync(struct inode *, struct file *);
extern int file_fsync(struct inode *, struct file *);

#endif /* __KERNEL__ */

#endif
