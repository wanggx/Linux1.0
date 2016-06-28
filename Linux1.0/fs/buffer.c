/*
 *  linux/fs/buffer.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting an interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it.
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/errno.h>

#include <asm/system.h>
#include <asm/io.h>

#ifdef CONFIG_SCSI
#ifdef CONFIG_BLK_DEV_SR
extern int check_cdrom_media_change(int, int);
#endif
#ifdef CONFIG_BLK_DEV_SD
extern int check_scsidisk_media_change(int, int);
extern int revalidate_scsidisk(int, int);
#endif
#endif
#ifdef CONFIG_CDU31A
extern int check_cdu31a_media_change(int, int);
#endif
#ifdef CONFIG_MCD
extern int check_mcd_media_change(int, int);
#endif

static int grow_buffers(int pri, int size);

/* 缓存机制是将物理内存块，映射到相应的缓冲头，如果缓冲头没有，
 * 则去申请一页的缓冲头，并且用unused_list来指向，当需要缓冲头时
 * 则会从unused_list链表中去取，然后将映射好的缓冲头依次添加到
 * free_list对应的双向链表当中
 */
static struct buffer_head * hash_table[NR_HASH];
static struct buffer_head * free_list = NULL;      
static struct buffer_head * unused_list = NULL;

/* 该队列是等待使用缓存的队列，当申请使用缓存而无法满足时getblk，
 * 则在该队列中等待，当释放缓存时，则唤醒等待使用缓存的进程(brelse)
 */
static struct wait_queue * buffer_wait = NULL;

int nr_buffers = 0;
int buffermem = 0;
int nr_buffer_heads = 0;
static int min_free_pages = 20;	/* nr free pages needed before buffer grows */
extern int *blksize_size[];

/*
 * Rewrote the wait-routines to use the "new" wait-queue functionality,
 * and getting rid of the cli-sti pairs. The wait-queue routines still
 * need cli-sti, but now it's just a couple of 386 instructions or so.
 *
 * Note that the real wait_on_buffer() is an inline function that checks
 * if 'b_wait' is set before calling this, so that the queues aren't set
 * up unnecessarily.
 */
void __wait_on_buffer(struct buffer_head * bh)
{
	struct wait_queue wait = { current, NULL };

	bh->b_count++;
	add_wait_queue(&bh->b_wait, &wait);
repeat:
	current->state = TASK_UNINTERRUPTIBLE;
	if (bh->b_lock) {
		schedule();
		goto repeat;
	}
	remove_wait_queue(&bh->b_wait, &wait);
	bh->b_count--;
	current->state = TASK_RUNNING;
}

/* Call sync_buffers with wait!=0 to ensure that the call does not
   return until all buffer writes have completed.  Sync() may return
   before the writes have finished; fsync() may not. */

/* 将设备dev在缓存中的数据全部写回到设备，
 * wait大于0，则表示阻塞，否则非阻塞
 */
static int sync_buffers(dev_t dev, int wait)
{
	int i, retry, pass = 0, err = 0;
	struct buffer_head * bh;

	/* One pass for no-wait, three for wait:
	   0) write out all dirty, unlocked buffers;
	   1) write out all dirty buffers, waiting if locked;
	   2) wait for completion by waiting for all buffers to unlock.
	 */
repeat:
	retry = 0;
	bh = free_list;
	for (i = nr_buffers*2 ; i-- > 0 ; bh = bh->b_next_free) {
		if (dev && bh->b_dev != dev)
			continue;
#ifdef 0 /* Disable bad-block debugging code */
		if (bh->b_req && !bh->b_lock &&
		    !bh->b_dirt && !bh->b_uptodate)
			printk ("Warning (IO error) - orphaned block %08x on %04x\n",
				bh->b_blocknr, bh->b_dev);
#endif
		if (bh->b_lock)
		{
			/* Buffer is locked; skip it unless wait is
			   requested AND pass > 0. */
			if (!wait || !pass) {
				retry = 1;
				continue;
			}
			wait_on_buffer (bh);
		}
		/* If an unlocked buffer is not uptodate, there has been 
		   an IO error. Skip it. */
		if (wait && bh->b_req && !bh->b_lock &&
		    !bh->b_dirt && !bh->b_uptodate)
		{
			err = 1;
			continue;
		}
		/* Don't write clean buffers.  Don't write ANY buffers
		   on the third pass. */
		if (!bh->b_dirt || pass>=2)
			continue;
		bh->b_count++;
		ll_rw_block(WRITE, 1, &bh);
		bh->b_count--;
		retry = 1;
	}
	/* If we are waiting for the sync to succeed, and if any dirty
	   blocks were written, then repeat; on the second pass, only
	   wait for buffers being written (do not pass to write any
	   more buffers on the second pass). */
	if (wait && retry && ++pass<=2)
		goto repeat;
	return err;
}

void sync_dev(dev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	sync_buffers(dev, 0);
}

/* 同步设备 */
int fsync_dev(dev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	return sync_buffers(dev, 1);
}

asmlinkage int sys_sync(void)
{
	sync_dev(0);
	return 0;
}

int file_fsync (struct inode *inode, struct file *filp)
{
	return fsync_dev(inode->i_dev);
}

asmlinkage int sys_fsync(unsigned int fd)
{
	struct file * file;
	struct inode * inode;

	if (fd>=NR_OPEN || !(file=current->filp[fd]) || !(inode=file->f_inode))
		return -EBADF;
	if (!file->f_op || !file->f_op->fsync)
		return -EINVAL;
	if (file->f_op->fsync(inode,file))
		return -EIO;
	return 0;
}

/* 更改设备dev的所有缓存标记
 */
void invalidate_buffers(dev_t dev)
{
	int i;
	struct buffer_head * bh;

	bh = free_list;
	for (i = nr_buffers*2 ; --i > 0 ; bh = bh->b_next_free) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = bh->b_req = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(dev_t dev)
{
	int i;
	struct buffer_head * bh;

	switch(MAJOR(dev)){
	case FLOPPY_MAJOR:
		if (!(bh = getblk(dev,0,1024)))
			return;
		i = floppy_change(bh);
		brelse(bh);
		break;

#if defined(CONFIG_BLK_DEV_SD) && defined(CONFIG_SCSI)
         case SCSI_DISK_MAJOR:
		i = check_scsidisk_media_change(dev, 0);
		break;
#endif

#if defined(CONFIG_BLK_DEV_SR) && defined(CONFIG_SCSI)
	 case SCSI_CDROM_MAJOR:
		i = check_cdrom_media_change(dev, 0);
		break;
#endif

#if defined(CONFIG_CDU31A)
         case CDU31A_CDROM_MAJOR:
		i = check_cdu31a_media_change(dev, 0);
		break;
#endif

#if defined(CONFIG_MCD)
         case MITSUMI_CDROM_MAJOR:
		i = check_mcd_media_change(dev, 0);
		break;
#endif

         default:
		return;
	};

	if (!i)	return;

	printk("VFS: Disk change detected on device %d/%d\n",
					MAJOR(dev), MINOR(dev));
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_blocks[i].s_dev == dev)
			put_super(super_blocks[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);

#if defined(CONFIG_BLK_DEV_SD) && defined(CONFIG_SCSI)
/* This is trickier for a removable hardisk, because we have to invalidate
   all of the partitions that lie on the disk. */
	if (MAJOR(dev) == SCSI_DISK_MAJOR)
		revalidate_scsidisk(dev, 0);
#endif
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

/* 和inode中的hash结构差不多,
  * 将bh从hash链表的第一项删除，同时将自己所在的hash链表
  * 中删除删除
  */
static inline void remove_from_hash_queue(struct buffer_head * bh)
{
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
	bh->b_next = bh->b_prev = NULL;
}

/* 将bh从free_list当中移除
 */
static inline void remove_from_free_list(struct buffer_head * bh)
{
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("VFS: Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
		free_list = bh->b_next_free;
	bh->b_next_free = bh->b_prev_free = NULL;
}

/* 将bh从hash链表和free_list链表中删除
 */
static inline void remove_from_queues(struct buffer_head * bh)
{
	remove_from_hash_queue(bh);
	remove_from_free_list(bh);
}
/* 将bh放置在链首，然后让free_list指向bh
  */
static inline void put_first_free(struct buffer_head * bh)
{
	if (!bh || (bh == free_list))
		return;
	remove_from_free_list(bh);
/* add to front of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
	free_list = bh;
}

/* 将bh放在以firee_list为首的最后一个
 */
static inline void put_last_free(struct buffer_head * bh)
{
	if (!bh)
		return;
	if (bh == free_list) {
		free_list = bh->b_next_free;
		return;
	}
	remove_from_free_list(bh);
/* add to back of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
}

/* 将bh插入到free_list的最后一个，
  * 因为查找空闲都是从free_list的第一个开始
  * 查找的，而查找已经映射的dev，block，size时，
  * 则是从hash表的第一项开始的，
  * 所以将其插入到hash链的链首
  */
static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	if (bh->b_next)
		bh->b_next->b_prev = bh;
}

/* 从缓冲hash链中查找满足dev，block，size的缓冲块
 * 注意此处并没有多链表进行处理，只是做了判断比较
 * 使用的方法是在hash链表中查找
 */
static struct buffer_head * find_buffer(dev_t dev, int block, int size)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			if (tmp->b_size == size)
				return tmp;
			else {
				printk("VFS: Wrong blocksize on device %d/%d\n",
							MAJOR(dev), MINOR(dev));
				return NULL;
			}
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
struct buffer_head * get_hash_table(dev_t dev, int block, int size)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block,size)))
			return NULL;
		bh->b_count++;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block && bh->b_size == size)
			return bh;
		bh->b_count--;
	}
}

/* 设置设备数据块的大小，dev包含设备的主设备号，次设备号
 */
void set_blocksize(dev_t dev, int size)
{
	int i;
	struct buffer_head * bh, *bhnext;

	if (!blksize_size[MAJOR(dev)])
		return;

	switch(size) {
		default: panic("Invalid blocksize passed to set_blocksize");
		case 512: case 1024: case 2048: case 4096:;
	}

	if (blksize_size[MAJOR(dev)][MINOR(dev)] == 0 && size == BLOCK_SIZE) {
		blksize_size[MAJOR(dev)][MINOR(dev)] = size;
		return;
	}
	if (blksize_size[MAJOR(dev)][MINOR(dev)] == size)
		return;

	/* 如果设备的数据块大小发生了更改，则将之前的文件都写入到设备 */
	sync_buffers(dev, 2);
	blksize_size[MAJOR(dev)][MINOR(dev)] = size;

  /* We need to be quite careful how we do this - we are moving entries
     around on the free list, and we can get in a loop if we are not careful.*/

	bh = free_list;
	for (i = nr_buffers*2 ; --i > 0 ; bh = bhnext) {
		bhnext = bh->b_next_free; 
		if (bh->b_dev != dev)
			continue;
		if (bh->b_size == size)
			continue;

		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_size != size)
			bh->b_uptodate = bh->b_dirt = 0;
		remove_from_hash_queue(bh);
/*    put_first_free(bh); */
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 *
 * 14.02.92: changed it to sync dirty buffers a bit: better performance
 * when the filesystem starts to get full of dirty blocks (I hope).
 */

/* 注意该函数获取对应设备dev，block，size
  * 大小的高速缓存，如果在查找的过程当中
  * 没有找到相应的高速缓存，则会去grow_buffer
  * 注意此函数是一个阻塞式的函数，block为设备的逻辑块号，size为设备的块大小
  */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(dev_t dev, int block, int size)
{
	struct buffer_head * bh, * tmp;
	int buffers;
	static int grow_size = 0;

repeat:
	bh = get_hash_table(dev, block, size);
	if (bh) {
		if (bh->b_uptodate && !bh->b_dirt)
			put_last_free(bh);
		return bh;
	}
	grow_size -= size;
	if (nr_free_pages > min_free_pages && grow_size <= 0) {
		if (grow_buffers(GFP_BUFFER, size))
			grow_size = PAGE_SIZE;
	}
	buffers = nr_buffers;
	bh = NULL;

	/* 扫描整个缓冲链
	 */
	for (tmp = free_list; buffers-- > 0 ; tmp = tmp->b_next_free) {
		if (tmp->b_count || tmp->b_size != size)
			continue;
		if (mem_map[MAP_NR((unsigned long) tmp->b_data)] != 1)
			continue;
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			/*既是干净的也没有被锁住，则该缓冲块满足条件*/
			if (!BADNESS(tmp))
				break;
		}
#if 0
		if (tmp->b_dirt) {
			tmp->b_count++;
			ll_rw_block(WRITEA, 1, &tmp);
			tmp->b_count--;
		}
#endif
	}

	if (!bh) {
		if (nr_free_pages > 5)
			if (grow_buffers(GFP_BUFFER, size))
				goto repeat;
		if (!grow_buffers(GFP_ATOMIC, size))
			sleep_on(&buffer_wait);
		goto repeat;
	}

	wait_on_buffer(bh);
	if (bh->b_count || bh->b_size != size)
		goto repeat;
	if (bh->b_dirt) {
		sync_buffers(0,0);
		goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */

/* 注意此处是如果找到了，则继续repeat，
  * 然后将找到条件的高速缓存给返回，
  * 注意上面的英文注释
  */
	if (find_buffer(dev,block,size))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of its kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */

/* 此时确定可以使用该高速缓存，
  * 然后设置引用计数，脏设备号，块号等标记
  */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	bh->b_req=0;
	remove_from_queues(bh);
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh);
	return bh;
}

/* 释放缓冲头，仅仅是减少b_count，
 * 如getblk找到后由于某种原因又用不到该缓冲块
 */
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (buf->b_count) {
		/* 如果b_count==1则以为没有其他进程用到它
		 */
		if (--buf->b_count)
			return;
		wake_up(&buffer_wait);
		return;
	}
	printk("VFS: brelse: Trying to free free buffer\n");
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */

/* 将设备中指定的块数据读取到高速缓存
 * 在将设备中指定块数据读取到高速缓存之前
 * 会在高速缓冲当中查找该块是否已经读取到高速缓冲当中
 * 如果已经在高速缓冲中，但是不是最新的，则依然会从设备中
 * 重新读取
 */
struct buffer_head * bread(dev_t dev, int block, int size)
{
	struct buffer_head * bh;

	if (!(bh = getblk(dev, block, size))) {
		printk("VFS: bread: READ error on device %d/%d\n",
						MAJOR(dev), MINOR(dev));
		return NULL;
	}
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ, 1, &bh);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	/* getblk得到的缓冲块不符合要求，
	 * 也即是数据不是最新的,然后释放缓冲块，
	 * 并返回空表示从设备读取数据失败
	 */
	brelse(bh);
	return NULL;
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(dev_t dev,int first, ...)
{
	va_list args;
	unsigned int blocksize;
	struct buffer_head * bh, *tmp;

	va_start(args,first);

	blocksize = BLOCK_SIZE;
	if (blksize_size[MAJOR(dev)] && blksize_size[MAJOR(dev)][MINOR(dev)])
		blocksize = blksize_size[MAJOR(dev)][MINOR(dev)];

	if (!(bh = getblk(dev, first, blocksize))) {
		printk("VFS: breada: READ error on device %d/%d\n",
						MAJOR(dev), MINOR(dev));
		return NULL;
	}
	if (!bh->b_uptodate)
		ll_rw_block(READ, 1, &bh);
	while ((first=va_arg(args,int))>=0) {
		tmp = getblk(dev, first, blocksize);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA, 1, &tmp);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

/*
 * See fs/inode.c for the weird use of volatile..
 */

/* 将该缓冲头的数据清空，并保留等待队列，将bh放在unused_list的链首
 */
static void put_unused_buffer_head(struct buffer_head * bh)
{
	struct wait_queue * wait;

	wait = ((volatile struct buffer_head *) bh)->b_wait;
	memset((void *) bh,0,sizeof(*bh));
	((volatile struct buffer_head *) bh)->b_wait = wait;
	bh->b_next_free = unused_list;
	unused_list = bh;
}

/* 创建更多的缓冲头，但是并没有指定缓冲头指向的数据b_data
 */
static void get_more_buffer_heads(void)
{
	int i;
	struct buffer_head * bh;

	/*如果还有未使用的缓冲头则不做处理*/
	if (unused_list)
		return;

	if(! (bh = (struct buffer_head*) get_free_page(GFP_BUFFER)))
		return;
	/* 将申请的一页物理内存，连接起来，注意是单向连接，
	 * 并且将unused_list指向链首，同时增加缓冲头的数量
	 */
	for (nr_buffer_heads+=i=PAGE_SIZE/sizeof*bh ; i>0; i--) {
		bh->b_next_free = unused_list;	/* only make link */
		unused_list = bh++;
	}
}

/* 从缓冲头unused_list链表的链首取出一个节点
 * 并初始化数据
 */
static struct buffer_head * get_unused_buffer_head(void)
{
	struct buffer_head * bh;

	get_more_buffer_heads();
	if (!unused_list)
		return NULL;
	bh = unused_list;
	unused_list = bh->b_next_free;
	bh->b_next_free = NULL;
	bh->b_data = NULL;
	bh->b_size = 0;
	bh->b_req = 0;
	return bh;
}

/*
 * Create the appropriate buffers when given a page for data area and
 * the size of each buffer.. Use the bh->b_this_page linked list to
 * follow the buffers created.  Return NULL if unable to create more
 * buffers.
 */

/* 将申请到的一页物理内存分成size大小的块，
 * 然后依次将这些块映射到相应的缓冲头，然后
 * 将数据指向该物理块的缓冲头用b_this_page链接起来
 */
static struct buffer_head * create_buffers(unsigned long page, unsigned long size)
{
	struct buffer_head *bh, *head;
	unsigned long offset;

	head = NULL;
	offset = PAGE_SIZE;
	while ((offset -= size) < PAGE_SIZE) {
		bh = get_unused_buffer_head();
		if (!bh)
			goto no_grow;
		/* 为了防止在page块在映射到一般时，
		 * 找不到空闲的缓冲头，然后直接跳转到no_grow
		 * 解除之前的部分映射
		 */
		bh->b_this_page = head;
		head = bh;
		bh->b_data = (char *) (page+offset);
		bh->b_size = size;
	}
	return head; /*将指向该物理块最后一块的缓冲头返回*/
/*
 * In case anything failed, we just free everything we got.
 */
no_grow:
	/* head是上一个指向page这块物理块数据的缓冲头，
	 * 然后将数据指向该物理块的所有缓冲头给释放，
	 * 之前已经映射缓冲头和page对应的物理块的(上面的while循环)，
	 * 将被解除映射
	 */
	bh = head;
	while (bh) {
		head = bh;
		bh = bh->b_this_page;
		put_unused_buffer_head(head);
	}
	return NULL;
}

/* 将nrbuf块bh对应的设备读入到bh当中，
 * 注意此时bh中已经记录了读取设备的基本信息
 */
static void read_buffers(struct buffer_head * bh[], int nrbuf)
{
	int i;
	int bhnum = 0;
	struct buffer_head * bhr[8];

	for (i = 0 ; i < nrbuf ; i++) {
		if (bh[i] && !bh[i]->b_uptodate)
			bhr[bhnum++] = bh[i];
	}
	if (bhnum)
		ll_rw_block(READ, bhnum, bhr);
	for (i = 0 ; i < nrbuf ; i++) {
		if (bh[i]) {
			wait_on_buffer(bh[i]);
		}
	}
}

/* first是根据dev，block条件hash出来的第一个节点
 * 此函数是检测物理设备的多个逻辑块号对应的高速缓存数据b_data
 * 是否在同一个物理页中连续，并且第一个块对应的数据块物理地址
 * 是否和页对齐，如果是对齐的则将address对应的物理页给释放掉
 */
static unsigned long check_aligned(struct buffer_head * first, unsigned long address,
	dev_t dev, int *b, int size)
{
	struct buffer_head * bh[8];
	unsigned long page;
	unsigned long offset;
	int block;
	int nrbuf;

	/* 获取高速缓存的数据块，
	 * 如果没有和4KB对齐则不做处理
	 */
	page = (unsigned long) first->b_data;
	if (page & ~PAGE_MASK) {
		brelse(first);
		return 0;
	}
	mem_map[MAP_NR(page)]++;
	bh[0] = first;
	nrbuf = 1;
	/* 一页内存最多缓存块也就8块=4KB/412B
	 */
	for (offset = size ; offset < PAGE_SIZE ; offset += size) {
		block = *++b;   /* 获取b中的逻辑块号 */
		if (!block)
			goto no_go;
		first = get_hash_table(dev, block, size);
		if (!first)
			goto no_go;
		bh[nrbuf++] = first;
		/* 保证多个逻辑块号对应的高速缓存数据在同一个页中连续存放
		 * 其中第一个页的b_data在物理页的其实地址处
		 */
		if (page+offset != (unsigned long) first->b_data)
			goto no_go;
	}
	read_buffers(bh,nrbuf);		/* make sure they are actually read correctly */
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	free_page(address);
	++current->min_flt;
	return page;
no_go:
	while (nrbuf-- > 0)
		brelse(bh[nrbuf]);
	free_page(page);
	return 0;
}

/* 将b中逻辑块依次连续的读入到address对应的物理页中
 */
static unsigned long try_to_load_aligned(unsigned long address,
	dev_t dev, int b[], int size)
{
	struct buffer_head * bh, * tmp, * arr[8];
	unsigned long offset;
	int * p;
	int block;

	/* 将address对应的物理页分成size大小的高速缓存数据区
	 * 然后将逻辑块号对应的设备数据依次连续读入到address所在的页
	 */
	bh = create_buffers(address, size);
	if (!bh)
		return 0;
	/* do any of the buffers already exist? punt if so.. */
	p = b;
	for (offset = 0 ; offset < PAGE_SIZE ; offset += size) {
		block = *(p++);
		/*逻辑块号不能为0*/
		if (!block)
			goto not_aligned;
		/*如果对应的逻辑块已经加载到了高速缓存则出错*/
		if (find_buffer(dev, block, size))
			goto not_aligned;
	}
	tmp = bh;
	p = b;
	block = 0;
	while (1) {
		arr[block++] = bh;
		bh->b_count = 1;
		bh->b_dirt = 0;
		bh->b_uptodate = 0;
		bh->b_dev = dev;
		bh->b_blocknr = *(p++);
		nr_buffers++;
		insert_into_queues(bh);
		if (bh->b_this_page)
			bh = bh->b_this_page; /*获取b_data相邻的下一个缓存节点*/
		else
			break;
	}
	buffermem += PAGE_SIZE;
	bh->b_this_page = tmp;
	mem_map[MAP_NR(address)]++;
	read_buffers(arr,block);
	while (block-- > 0)
		brelse(arr[block]);
	++current->maj_flt;
	return address;
not_aligned:
	while ((tmp = bh) != NULL) {
		bh = bh->b_this_page;
		put_unused_buffer_head(tmp);
	}
	return 0;
}

/*
 * Try-to-share-buffers tries to minimize memory use by trying to keep
 * both code pages and the buffer area in the same page. This is done by
 * (a) checking if the buffers are already aligned correctly in memory and
 * (b) if none of the buffer heads are in memory at all, trying to load
 * them into memory the way we want them.
 *
 * This doesn't guarantee that the memory is shared, but should under most
 * circumstances work very well indeed (ie >90% sharing of code pages on
 * demand-loadable executables).
 */
static inline unsigned long try_to_share_buffers(unsigned long address,
	dev_t dev, int *b, int size)
{
	struct buffer_head * bh;
	int block;

	block = b[0];
	if (!block)
		return 0;
	/*从对应的hash链表中找到第一个可用的高速缓存节点*/
	bh = get_hash_table(dev, block, size);
	if (bh)
		return check_aligned(bh, address, dev, b, size);
	return try_to_load_aligned(address, dev, b, size);
}


/* 从物理地址from处复制size大小到to物理地址处
 */
#define COPYBLK(size,from,to) \
__asm__ __volatile__("rep ; movsl": \
	:"c" (((unsigned long) size) >> 2),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc. This also allows us to optimize memory usage by sharing code pages
 * and filesystem buffers..
 */

/* 注意此处的address是物理地址
 * size是物理设备中块的大小
 * 该函数作用是从dev设备中读取多少块(逻辑块号)的数据到
 * address开始的地址处
 */
unsigned long bread_page(unsigned long address, dev_t dev, int b[], int size, int prot)
{
	struct buffer_head * bh[8];
	unsigned long where;
	int i, j;

	if (!(prot & PAGE_RW)) {
		where = try_to_share_buffers(address,dev,b,size);
		if (where)
			return where;
	}
	++current->maj_flt;
	/*size大小不固定，所以需要读取的块数不一定就是8块
	 *如果是512KB则是8块，如果是1024KB则是4块
	 */
 	for (i=0, j=0; j<PAGE_SIZE ; i++, j+= size) {
		bh[i] = NULL;
		if (b[i])
			bh[i] = getblk(dev, b[i], size);
	}
	/*将上面得到的多少块逻辑块读入到高速缓存*/
	read_buffers(bh,i);
	where = address;
 	for (i=0, j=0; j<PAGE_SIZE ; i++, j += size,address += size) {
		if (bh[i]) {
			if (bh[i]->b_uptodate)
				COPYBLK(size, (unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
	}
	return where;
}

/*
 * Try to increase the number of buffers available: the size argument
 * is used to determine what kind of buffers we want.
 */
 
/* 增长高速缓存，因为高速缓存的大小可以设置为512KB,1024KB等等
 * 该函数是用来增长一页(4KB)的高速缓存，该页被分成size大小的均等块
 * 高速缓存结构体不占用该块内存，空闲的换冲头结构体存放在以unused_list为首的
 * 链表当中
 */
static int grow_buffers(int pri, int size)
{
	unsigned long page;
	struct buffer_head *bh, *tmp;

	/* 限制增长buffer块的大小，要么是512KB,要么是1024KB
	 */
	if ((size & 511) || (size > PAGE_SIZE)) {
		printk("VFS: grow_buffers: size = %d\n",size);
		return 0;
	}
	if(!(page = __get_free_page(pri)))
		return 0;
	bh = create_buffers(page, size);
	if (!bh) {
		free_page(page);
		return 0;
	}
	tmp = bh;
	/* 将刚才分配的一夜物理内存对应的缓冲头给添加到free_list双向链表当中
	 */
	while (1) {
		if (free_list) {
			tmp->b_next_free = free_list;
			tmp->b_prev_free = free_list->b_prev_free;
			free_list->b_prev_free->b_next_free = tmp;
			free_list->b_prev_free = tmp;
		} else {
			tmp->b_prev_free = tmp;
			tmp->b_next_free = tmp;
		}
		free_list = tmp;
		++nr_buffers; /*增加缓存数量*/
		if (tmp->b_this_page)
			tmp = tmp->b_this_page;
		else
			break;
	}
	tmp->b_this_page = bh;
	/*增加缓寸大小*/
	buffermem += PAGE_SIZE;
	return 1;
}

/*
 * try_to_free() checks if all the buffers on this particular page
 * are unused, and free's the page if so.
 */
/* bhp获取bh指向的下一个空闲的缓冲头，如果下一个缓冲头需要被释放
 * 则bhp继续指向下一个的下一个
 */
static int try_to_free(struct buffer_head * bh, struct buffer_head ** bhp)
{
	unsigned long page;
	struct buffer_head * tmp, * p;

	*bhp = bh;
	page = (unsigned long) bh->b_data;
	page &= PAGE_MASK;
	tmp = bh;
	/* 扫描一圈判断缓冲头指向数据是否和bh->b_data在同一物理页
	 */
	do {
		if (!tmp)
			return 0;
		if (tmp->b_count || tmp->b_dirt || tmp->b_lock || tmp->b_wait)
			return 0;
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	tmp = bh;
	/* 依次释放数据指向同一块内存的缓冲头，
	 * 并将缓冲头添加到unused_list链表当中，
	 * 同时减少nr_buffers和buffermem大小和释放缓冲头指向的数据块
	 */
	do {
		p = tmp;
		tmp = tmp->b_this_page;
		nr_buffers--;
		if (p == *bhp)
			*bhp = p->b_prev_free;
		remove_from_queues(p);
		put_unused_buffer_head(p);
	} while (tmp != bh);
	buffermem -= PAGE_SIZE;
	free_page(page);
	return !mem_map[MAP_NR(page)];
}

/*
 * Try to free up some pages by shrinking the buffer-cache
 *
 * Priority tells the routine how hard to try to shrink the
 * buffers: 3 means "don't bother too much", while a value
 * of 0 means "we'd better get some free pages now".
 */

/* 收缩缓存空间
  */
int shrink_buffers(unsigned int priority)
{
	struct buffer_head *bh;
	int i;

	if (priority < 2)
		sync_buffers(0,0);
	bh = free_list;
	i = nr_buffers >> priority;
	for ( ; i-- > 0 ; bh = bh->b_next_free) {
		if (bh->b_count ||
		    (priority >= 5 &&
		     mem_map[MAP_NR((unsigned long) bh->b_data)] > 1)) {
			put_last_free(bh);
			continue;
		}
		if (!bh->b_this_page)
			continue;
		if (bh->b_lock)
			if (priority)
				continue;
			else
				wait_on_buffer(bh);
		if (bh->b_dirt) {
			bh->b_count++;
			ll_rw_block(WRITEA, 1, &bh);
			bh->b_count--;
			continue;
		}
		/* 更改空闲链的结构，并记下下一个空闲节点，
		 * 然后继续循环处理,返回1表示成功释放一页
		 */
		if (try_to_free(bh, &bh))
			return 1;
	}
	return 0;
}

void show_buffers(void)
{
	struct buffer_head * bh;
	int found = 0, locked = 0, dirty = 0, used = 0, lastused = 0;

	printk("Buffer memory:   %6dkB\n",buffermem>>10);
	printk("Buffer heads:    %6d\n",nr_buffer_heads);
	printk("Buffer blocks:   %6d\n",nr_buffers);
	bh = free_list;
	do {
		found++;
		if (bh->b_lock)
			locked++;
		if (bh->b_dirt)
			dirty++;
		if (bh->b_count)
			used++, lastused = found;
		bh = bh->b_next_free;
	} while (bh != free_list);
	printk("Buffer mem: %d buffers, %d used (last=%d), %d locked, %d dirty\n",
		found, used, lastused, locked, dirty);
}

/*
 * This initializes the initial buffer free list.  nr_buffers is set
 * to one less the actual number of buffers, as a sop to backwards
 * compatibility --- the old code did this (I think unintentionally,
 * but I'm not sure), and programs in the ps package expect it.
 * 					- TYT 8/30/92
 */

/* 高速缓存初始化函数
  */
void buffer_init(void)
{
	int i;

	if (high_memory >= 4*1024*1024)
		min_free_pages = 200;
	else
		min_free_pages = 20;
	for (i = 0 ; i < NR_HASH ; i++)
		hash_table[i] = NULL;
	free_list = 0;
	/* 一开始就增长一页的高速缓存
	 */
	grow_buffers(GFP_KERNEL, BLOCK_SIZE);
	if (!free_list)
		panic("VFS: Unable to initialize buffer free list!");
	return;
}
