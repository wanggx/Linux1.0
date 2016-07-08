/*
 *  linux/fs/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>

#include <asm/system.h>

static struct inode_hash_entry {
	struct inode * inode;
	int updating;       /* 防止多个进程同时操作一个hash节点中的链表时，出现错乱 */
} hash_table[NR_IHASH];

static struct inode * first_inode;   /* inode空闲链表的首部 */
static struct wait_queue * inode_wait = NULL;
static int nr_inodes = 0, nr_free_inodes = 0;

/* 通过设备号，inode号来映射hash表项
 */
static inline int const hashfn(dev_t dev, unsigned int i)
{
	return (dev ^ i) % NR_IHASH;
}

static inline struct inode_hash_entry * const hash(dev_t dev, int i)
{
	return hash_table + hashfn(dev, i);
}


/* 将inode节点插入到first_inode节点的上一个，
 * 然后将first_inode指向inode节点，first_inode是一个双向环
 */
static void insert_inode_free(struct inode *inode)
{
	inode->i_next = first_inode;
	inode->i_prev = first_inode->i_prev;
	inode->i_next->i_prev = inode;
	inode->i_prev->i_next = inode;
	first_inode = inode;
}

/* 将inode节点从双向链表中移除，但是并没有释放inode的内存
 * 只是斩断了inode的指针指向
 */
static void remove_inode_free(struct inode *inode)
{
	if (first_inode == inode)
		first_inode = first_inode->i_next;
	if (inode->i_next)
		inode->i_next->i_prev = inode->i_prev;
	if (inode->i_prev)
		inode->i_prev->i_next = inode->i_next;
	inode->i_next = inode->i_prev = NULL;
}

/* 将inode插入hash双向链表的上一个，并让h->inode指向inode
 */
void insert_inode_hash(struct inode *inode)
{
	struct inode_hash_entry *h;
	h = hash(inode->i_dev, inode->i_ino);

	inode->i_hash_next = h->inode;
	inode->i_hash_prev = NULL;
	if (inode->i_hash_next)
		inode->i_hash_next->i_hash_prev = inode;
	h->inode = inode;
}

/* inode节点有一个hash结构 hash_table[NR_IHASH],
 * 其中的每一项都对应一个hash的双向链表，该双向链表中
 * 的节点都是通过设备号和inode号映射过来的。如果有多个inode
 * 映射到hash_table中的一项，则将这多个节点用双向链表给链接起来
 * 该函数就是将inode从双向链表中删除，inode中有多个双向链表结构如
 * i_hash_prev,i_hash_next是一对双向链表，i_next和i_prev是一对
 */

static void remove_inode_hash(struct inode *inode)
{
	struct inode_hash_entry *h;
	h = hash(inode->i_dev, inode->i_ino);

	/* 如果是hash中的第一个，则h->inode指向下一个
	 */
	if (h->inode == inode)
		h->inode = inode->i_hash_next;
	/* 将inode从hash双向链表中给删除，并斩断指针连接
	 */
	if (inode->i_hash_next)
		inode->i_hash_next->i_hash_prev = inode->i_hash_prev;
	if (inode->i_hash_prev)
		inode->i_hash_prev->i_hash_next = inode->i_hash_next;
	inode->i_hash_prev = inode->i_hash_next = NULL;
}

/* 将inode从first_inode中删除，然后将inode添加到
 * 以first_inode为首的双向链表的末端，相当于将inode
 * 在双向链表中给移动一下位置
 */
static void put_last_free(struct inode *inode)
{
	remove_inode_free(inode);
	inode->i_prev = first_inode->i_prev;
	inode->i_prev->i_next = inode;
	inode->i_next = first_inode;
	inode->i_next->i_prev = inode;
}

/* 重新分配一页的内存来存放inode，
 * 并初始化各inode之间的连接关系,
 * 这个和struct file道理一样，分配了就不会释放，
 * 但是会有最大数量限制
 */
void grow_inodes(void)
{
	struct inode * inode;
	int i;

	if (!(inode = (struct inode*) get_free_page(GFP_KERNEL)))
		return;

	i=PAGE_SIZE / sizeof(struct inode);
	/* nr_inodes记录所有的inode节点数，只会增加，不会减小
	 * nr_free_inodes记录当前空闲inode的数量
	 */
	nr_inodes += i;
	nr_free_inodes += i;

	if (!first_inode)
		inode->i_next = inode->i_prev = first_inode = inode++, i--;

	for ( ; i ; i-- )
		insert_inode_free(inode++);
}

unsigned long inode_init(unsigned long start, unsigned long end)
{
	memset(hash_table, 0, sizeof(hash_table));
	first_inode = NULL;
	return start;
}

static void __wait_on_inode(struct inode *);

static inline void wait_on_inode(struct inode * inode)
{
	if (inode->i_lock)
		__wait_on_inode(inode);
}

static inline void lock_inode(struct inode * inode)
{
	wait_on_inode(inode);
	inode->i_lock = 1;
}

static inline void unlock_inode(struct inode * inode)
{
	inode->i_lock = 0;
	wake_up(&inode->i_wait);
}

/*
 * Note that we don't want to disturb any wait-queues when we discard
 * an inode.
 *
 * Argghh. Got bitten by a gcc problem with inlining: no way to tell
 * the compiler that the inline asm function 'memset' changes 'inode'.
 * I've been searching for the bug for days, and was getting desperate.
 * Finally looked at the assembler output... Grrr.
 *
 * The solution is the weird use of 'volatile'. Ho humm. Have to report
 * it to the gcc lists, and hope we can do this more cleanly some day..
 */

/* 如在设备被拔掉情况下，设备文件的inode仍然在内存
 * 将inode从两个链表中删除，并清除数据，但不改变inode的等待队列
 */
void clear_inode(struct inode * inode)
{
	struct wait_queue * wait;

	wait_on_inode(inode);
	remove_inode_hash(inode);
	remove_inode_free(inode);
	wait = ((volatile struct inode *) inode)->i_wait;
	if (inode->i_count)
		nr_free_inodes++;   /*???????*/
	memset(inode,0,sizeof(*inode));
	((volatile struct inode *) inode)->i_wait = wait;
	insert_inode_free(inode);
}

/*  判断文件系统是否可以挂载 */
int fs_may_mount(dev_t dev)
{
	struct inode * inode, * next;
	int i;

	next = first_inode;
	for (i = nr_inodes ; i > 0 ; i--) {
		inode = next;
		next = inode->i_next;	/* clear_inode() changes the queues.. */
		if (inode->i_dev != dev)
			continue;
                /* 运行到这里，就是对当前高速缓存当中所有inode设备号为dev的inode进行判断
                  * 如果if为true，则表明该设备对应的文件系统已被挂载，或者出现异常 
                  */
		if (inode->i_count || inode->i_dirt || inode->i_lock)
			return 0;
		clear_inode(inode);
	}
	return 1;
}

/* 判断是否可以卸载 */
int fs_may_umount(dev_t dev, struct inode * mount_root)
{
	struct inode * inode;
	int i;

	inode = first_inode;
        /* 判断设备对应的所有inode的引用计数是否为0，
          * 也就是设备的文件是否还被用到 
          */
	for (i=0 ; i < nr_inodes ; i++, inode = inode->i_next) {
		if (inode->i_dev != dev || !inode->i_count)
			continue;
		if (inode == mount_root && inode->i_count == 1)
			continue;
		return 0;
	}
	return 1;
}

/* 判断超级块是否可以重新挂载 */
int fs_may_remount_ro(dev_t dev)
{
	struct file * file;
	int i;

	/* Check that no files are currently opened for writing. */
	for (file = first_file, i=0; i<nr_files; i++, file=file->f_next) {
		if (!file->f_count || !file->f_inode ||
		    file->f_inode->i_dev != dev)
			continue;
                /* 判断设备对应的文件是否正常 */
		if (S_ISREG(file->f_inode->i_mode) && (file->f_mode & 2))
			return 0;
	}
	return 1;
}

/* 仅仅是将inode写到高速缓冲 */
static void write_inode(struct inode * inode)
{
	if (!inode->i_dirt)
		return;
	wait_on_inode(inode);
	if (!inode->i_dirt)
		return;
	if (!inode->i_sb || !inode->i_sb->s_op || !inode->i_sb->s_op->write_inode) {
		inode->i_dirt = 0;
		return;
	}
	inode->i_lock = 1;	
	inode->i_sb->s_op->write_inode(inode);
	unlock_inode(inode);
}

/* 将此时inode中记录的dev，block，size的块中数据读取到高速缓冲 */
static void read_inode(struct inode * inode)
{
	lock_inode(inode);
	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->read_inode)
		inode->i_sb->s_op->read_inode(inode);
	unlock_inode(inode);
}

/*
 * notify_change is called for inode-changing operations such as
 * chown, chmod, utime, and truncate.  It is guaranteed (unlike
 * write_inode) to be called from the context of the user requesting
 * the change.  It is not called for ordinary access-time updates.
 * NFS uses this to get the authentication correct.  -- jrs
 */

/* 当文件被修改时，需要通知一下自己被更改了,在ext,ext2当中notify_change
 * 函数都为NULL，只有在NFS当中才会发起通知
 */
int notify_change(int flags, struct inode * inode)
{
	if (inode->i_sb && inode->i_sb->s_op  &&
	    inode->i_sb->s_op->notify_change)
		return inode->i_sb->s_op->notify_change(flags, inode);
	return 0;
}

/*
 * bmap is needed for demand-loading and paging: if this function
 * doesn't exist for a filesystem, then those things are impossible:
 * executables cannot be run from the filesystem etc...
 *
 * This isn't as bad as it sounds: the read-routines might still work,
 * so the filesystem would be otherwise ok (for example, you might have
 * a DOS filesystem, which doesn't lend itself to bmap very well, but
 * you could still transfer files to/from the filesystem)
 */

/* 实现文件的数据块号到物理设备的逻辑块号的映射
 */
int bmap(struct inode * inode, int block)
{
	if (inode->i_op && inode->i_op->bmap)
		return inode->i_op->bmap(inode,block);
	return 0;
}

void invalidate_inodes(dev_t dev)
{
	struct inode * inode, * next;
	int i;

	next = first_inode;
	for(i = nr_inodes ; i > 0 ; i--) {
		inode = next;
		next = inode->i_next;		/* clear_inode() changes the queues.. */
		if (inode->i_dev != dev)
			continue;
		/* 如果inode仍在使用当中，则不做处理 ，注意下面的printk打印 */
		if (inode->i_count || inode->i_dirt || inode->i_lock) {
			printk("VFS: inode busy on removed device %d/%d\n", MAJOR(dev), MINOR(dev));
			continue;
		}
		clear_inode(inode);
	}
}

/* 同步高速缓冲中设备dev的所有inode*/
void sync_inodes(dev_t dev)
{
	int i;
	struct inode * inode;

	inode = first_inode;
	for(i = 0; i < nr_inodes*2; i++, inode = inode->i_next) {
		if (dev && inode->i_dev != dev)
			continue;
		wait_on_inode(inode);
		if (inode->i_dirt)
			write_inode(inode);
	}
}

/* 如果成功则增加一个空闲的inode */
void iput(struct inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count) {
		printk("VFS: iput: trying to free free inode\n");
		printk("VFS: device %d/%d, inode %lu, mode=0%07o\n",
			MAJOR(inode->i_rdev), MINOR(inode->i_rdev),
					inode->i_ino, inode->i_mode);
		return;
	}
	/* 如果是管道文件，则只能从一边读，另一边写，
	 * 当写完成之后就应该开始唤醒等待读的进程
	 */
	if (inode->i_pipe)
		wake_up_interruptible(&PIPE_WAIT(*inode));
repeat:
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	/* 唤醒等待使用inode进程
	 */
	wake_up(&inode_wait);
	if (inode->i_pipe) {
		unsigned long page = (unsigned long) PIPE_BASE(*inode);
		PIPE_BASE(*inode) = NULL;
		free_page(page);
	}
	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->put_inode) {
		inode->i_sb->s_op->put_inode(inode);
		if (!inode->i_nlink)
			return;
	}
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	nr_free_inodes++;
	return;
}

/* 从空闲链表中获取一个空闲inode */
struct inode * get_empty_inode(void)
{
	struct inode * inode, * best;
	int i;
	/*如果空闲inode少于已有inode的1/4，则会增加inode*/
	if (nr_inodes < NR_INODE && nr_free_inodes < (nr_inodes >> 2))
		grow_inodes();
repeat:
	inode = first_inode;
	best = NULL;
	for (i = 0; i<nr_inodes; inode = inode->i_next, i++) {
		if (!inode->i_count) { /*首先inode没有被用到*/
			if (!best)
				best = inode;
			/*找到不是dirt,lock的节点*/
			if (!inode->i_dirt && !inode->i_lock) {
				best = inode;
				break;
			}
		}
	}
	if (!best || best->i_dirt || best->i_lock)
		if (nr_inodes < NR_INODE) {
			grow_inodes();
			goto repeat;
		}
	inode = best;
	/* 运行到这个地方，则找到的合适的inode已经存放在inode当中，
	 * 如果inode为NULL，则表示还没有找到
	 */ 
	if (!inode) {
		printk("VFS: No free inodes - contact Linus\n");
		/* 申请inode时，如果不能满足要求，则让进程等待，
		 * 在iput的时候唤醒需要inode的进程
		 */
		sleep_on(&inode_wait);
		goto repeat;
	}
	if (inode->i_lock) {
		wait_on_inode(inode);
		goto repeat;
	}
	if (inode->i_dirt) {
		write_inode(inode);
		goto repeat;
	}
	if (inode->i_count)
		goto repeat;
	clear_inode(inode);
	/* 初始化空闲节点的数据
	 */ 
	inode->i_count = 1;
	inode->i_nlink = 1;
	inode->i_sem.count = 1;
	nr_free_inodes--;
	if (nr_free_inodes < 0) {
		printk ("VFS: get_empty_inode: bad free inode count.\n");
		nr_free_inodes = 0;
	}
	return inode;
}

/* 获取命名管道的inode
 */
struct inode * get_pipe_inode(void)
{
	struct inode * inode;
	extern struct inode_operations pipe_inode_operations;

	if (!(inode = get_empty_inode()))
		return NULL;
	/* 给命名管道分配一页的数据，从这里可以看出命名管道不适合大量数据传输*/
	if (!(PIPE_BASE(*inode) = (char*) __get_free_page(GFP_USER))) {
		iput(inode);
		return NULL;
	}
	inode->i_op = &pipe_inode_operations;
	/*命名管道是一头读，一头写*/
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_WAIT(*inode) = NULL;
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	PIPE_RD_OPENERS(*inode) = PIPE_WR_OPENERS(*inode) = 0;
        /* 设置读写数量为1 */
	PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 1;
	PIPE_LOCK(*inode) = 0;
	inode->i_pipe = 1;
	inode->i_mode |= S_IFIFO | S_IRUSR | S_IWUSR;
	inode->i_uid = current->euid;
	inode->i_gid = current->egid;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	return inode;
}

struct inode * iget(struct super_block * sb,int nr)
{
	return __iget(sb,nr,1);
}

/* 获取超级块对应i节点号的数据，如果在高速缓冲中可以找到，则返回找到的inode，
 * 如果没有找到，则先是获取一个空的inode，
 * 然后根据inode调用read_inode函数从硬盘中读取ext2_inode的数据
 */
struct inode * __iget(struct super_block * sb, int nr, int crossmntp)
{
	static struct wait_queue * update_wait = NULL;
	struct inode_hash_entry * h;
	struct inode * inode;
	struct inode * empty = NULL;

	/* 注意以上数据都是在进程的内核堆栈中，
	 * 当多个进程同时执行到这段代码时,它们都是不同值，
	 * 但是update_wait不是，它是内核静态变量
	 */
	
	if (!sb)
		panic("VFS: iget with sb==NULL");
	h = hash(sb->s_dev, nr);
repeat:
	for (inode = h->inode; inode ; inode = inode->i_hash_next)
		if (inode->i_dev == sb->s_dev && inode->i_ino == nr)
			goto found_it;
	if (!empty) {
		h->updating++;
        /* 因为get_empty_inode需要操作inode链表，所以此时需要排它性操作 */
		empty = get_empty_inode();
		if (!--h->updating)
			wake_up(&update_wait);
		if (empty)
			goto repeat;
		return (NULL);
	}
	/*重新增加一个和nr，s_dev对应的inode*/
	inode = empty;
	inode->i_sb = sb;
	inode->i_dev = sb->s_dev;
	inode->i_ino = nr;
	inode->i_flags = sb->s_flags;
	put_last_free(inode);
	insert_inode_hash(inode);
	/* inode此时具有的信息只有s_dev,nr,s_flags
	 * read_inode就是根据以上信息从磁盘中读取inode
	 * 对应文件的信息到inode的数据当中，此时会
	 * 根据文件类型来决定inode中的i_op结构
	 */
	read_inode(inode);
	goto return_it;

found_it:
	if (!inode->i_count)
		nr_free_inodes--;
	inode->i_count++;
	wait_on_inode(inode);
	if (inode->i_dev != sb->s_dev || inode->i_ino != nr) {
		printk("Whee.. inode changed from under us. Tell Linus\n");
		iput(inode);
		goto repeat;
	}
	if (crossmntp && inode->i_mount) {
		struct inode * tmp = inode->i_mount;
		tmp->i_count++;
		iput(inode);
		inode = tmp;
		wait_on_inode(inode);
	}
	if (empty)
		iput(empty);

return_it:
	while (h->updating)
		sleep_on(&update_wait);
	return inode;
}

/*
 * The "new" scheduling primitives (new as of 0.97 or so) allow this to
 * be done without disabling interrupts (other than in the actual queue
 * updating things: only a couple of 386 instructions). This should be
 * much better for interrupt latency.
 */
static void __wait_on_inode(struct inode * inode)
{
	struct wait_queue wait = { current, NULL };

	add_wait_queue(&inode->i_wait, &wait);
repeat:
	current->state = TASK_UNINTERRUPTIBLE;
	/*如果该i节点被锁定，则需要调度，让其他进程执行*/
	if (inode->i_lock) {
		schedule();
		goto repeat;
	}
	/*将自己从i节点的等待队列中移除*/
	remove_wait_queue(&inode->i_wait, &wait);
	current->state = TASK_RUNNING;
}
