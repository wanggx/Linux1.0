/*
 *  linux/fs/ext2/file.c
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 fs regular file handling primitives
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/locks.h>

#define	NBUF	32

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#include <linux/fs.h>
#include <linux/ext2_fs.h>

static int ext2_file_read (struct inode *, struct file *, char *, int);
static int ext2_file_write (struct inode *, struct file *, char *, int);
static void ext2_release_file (struct inode *, struct file *);

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the ext2 filesystem.
 */
static struct file_operations ext2_file_operations = {
	NULL,			/* lseek - default */
	ext2_file_read,		/* read */
	ext2_file_write,	/* write */
	NULL,			/* readdir - bad */
	NULL,			/* select - default */
	ext2_ioctl,		/* ioctl */
	generic_mmap,  		/* mmap */
	NULL,			/* no special open is needed */
	ext2_release_file,	/* release */
	ext2_sync_file		/* fsync */
};

/* 普通文件的inode操作函数
 */
struct inode_operations ext2_file_inode_operations = {
	&ext2_file_operations,/* default file operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	ext2_bmap,		/* bmap */
	ext2_truncate,		/* truncate */
	ext2_permission		/* permission */
};

/* ext2文件系统的文件读函数 */
static int ext2_file_read (struct inode * inode, struct file * filp,
		    char * buf, int count)
{
	int read, left, chars;
	int block, blocks, offset;
	int bhrequest, uptodate;
	struct buffer_head ** bhb, ** bhe;
	/* 依次磁盘请求最多允许32块数据 */
	struct buffer_head * bhreq[NBUF];
	struct buffer_head * buflist[NBUF];
	struct super_block * sb;
	unsigned int size;
	int err;

	if (!inode) {
		printk ("ext2_file_read: inode = NULL\n");
		return -EINVAL;
	}
	sb = inode->i_sb;
	if (!S_ISREG(inode->i_mode)) {
		ext2_warning (sb, "ext2_file_read", "mode = %07o",
			      inode->i_mode);
		return -EINVAL;
	}
	/* 读取偏移量和文件大小 */
	offset = filp->f_pos;
	size = inode->i_size;
	if (offset > size)
		left = 0;
	else
		left = size - offset;
	if (left > count)
		left = count;
	if (left <= 0)
		return 0;
	read = 0;
	/* 获取当前偏移量所在的块号和所在块中的偏移量 */
	block = offset >> EXT2_BLOCK_SIZE_BITS(sb);
	offset &= (sb->s_blocksize - 1);
	/* size表示文件总共占用了多少块 */
	size = (size + sb->s_blocksize - 1) >> EXT2_BLOCK_SIZE_BITS(sb);
	/* 还需要读取块的数量 */
	blocks = (left + offset + sb->s_blocksize - 1) >> EXT2_BLOCK_SIZE_BITS(sb);
	bhb = bhe = buflist;
	if (filp->f_reada) {
		blocks += read_ahead[MAJOR(inode->i_dev)] >>
			(EXT2_BLOCK_SIZE_BITS(sb) - 9);
		if (block + blocks > size)
			blocks = size - block;
	}

	/*
	 * We do this in a two stage process.  We first try and request
	 * as many blocks as we can, then we wait for the first one to
	 * complete, and then we try and wrap up as many as are actually
	 * done.  This routine is rather generic, in that it can be used
	 * in a filesystem by substituting the appropriate function in
	 * for getblk
	 *
	 * This routine is optimized to make maximum use of the various
	 * buffers and caches.
	 */
	/* 一轮循环最多只能读取32个数据块
	 */
	do {
		bhrequest = 0;
		uptodate = 1;
		/* blocks表示总共要读取的块数，block表示开始读取的块号 */
		while (blocks) {
			--blocks;
			/* 获取的高度缓存地址都存放在buflist数组当中 */
			*bhb = ext2_getblk (inode, block++, 0, &err);
			if (*bhb && !(*bhb)->b_uptodate) {
				uptodate = 0;
				/* 将读取的高速缓存指针存放在bhreq当中，这个时候bhreq中元素其实和buflist中相同 */
				bhreq[bhrequest++] = *bhb;
			}

			/* 如果当前读取到最后一个高速缓存，则bhb又回到数组首地址 */
			if (++bhb == &buflist[NBUF])
				bhb = buflist;

			/*
			 * If the block we have on hand is uptodate, go ahead
			 * and complete processing
			 */
			if (uptodate)
				break;

			/* 表示已经获取8个高速缓存，则这一轮就要停止了 */
			if (bhb == bhe)
				break;
		}

		/*
		 * Now request them all
		 */
		/* 将所有逻辑块的数据读取到相应的高速缓存当中 
		 */
		if (bhrequest)
			ll_rw_block (READ, bhrequest, bhreq);

		do {
			/*
			 * Finish off all I/O that has actually completed
			 */
			if (*bhe) {
				wait_on_buffer (*bhe);
				/* 如果读取的数据不是最新的，则返回出错 */
				if (!(*bhe)->b_uptodate) { /* read error? */
				        brelse(*bhe);
					if (++bhe == &buflist[NBUF])
					  bhe = buflist;
					left = 0;
					break;
				}
			}
			/* 获取这次读取的字节数chars */
			if (left < sb->s_blocksize - offset)
				chars = left;
			else
				chars = sb->s_blocksize - offset;
			/* 修改文件指针便宜、剩下读取字节数和已读取字节数 */
			filp->f_pos += chars;
			left -= chars;
			read += chars;
			/* 将数据从高速缓存当中拷贝到buf当中 */
			if (*bhe) {
				memcpy_tofs (buf, offset + (*bhe)->b_data,
					     chars);
				brelse (*bhe);
				buf += chars;
			} else {
				while (chars-- > 0)
					put_fs_byte (0, buf++);
			}
			offset = 0;
			/* 如果bhe等于buflist数组最后一个元素，则又回到数组首部 */
			if (++bhe == &buflist[NBUF])
				bhe = buflist;
		} while (left > 0 && bhe != bhb && (!*bhe || !(*bhe)->b_lock));
	} while (left > 0);   /* 如果还有没读完，则继续读 */

	/*
	 * Release the read-ahead blocks
	 */
	while (bhe != bhb) {
		brelse (*bhe);
		if (++bhe == &buflist[NBUF])
			bhe = buflist;
	}
	if (!read)
		return -EIO;
	filp->f_reada = 1;
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
		inode->i_dirt = 1;
	}
	/* 返回实际读取字节数 */
	return read;
}

static int ext2_file_write (struct inode * inode, struct file * filp,
			    char * buf, int count)
{
	off_t pos;
	int written, c;
	struct buffer_head * bh;
	char * p;
	struct super_block * sb;
	int err;

	if (!inode) {
		printk("ext2_file_write: inode = NULL\n");
		return -EINVAL;
	}
	sb = inode->i_sb;
	if (sb->s_flags & MS_RDONLY)
		/*
		 * This fs has been automatically remounted ro because of errors
		 */
		return -ENOSPC;

	if (!S_ISREG(inode->i_mode)) {
		ext2_warning (sb, "ext2_file_write", "mode = %07o\n",
			      inode->i_mode);
		return -EINVAL;
	}
/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
 	/* 判断文件写的位置 */
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	written = 0;
	while (written < count) {
		/* 获取要写位置的高速缓存指针，当当前读取的块号大于当前文件的最大块号，
		 * 则ext2_getblk函数为文件分配一个块号
		 */
		bh = ext2_getblk (inode, pos / sb->s_blocksize, 1, &err);
		if (!bh) {
			if (!written)
				written = err;
			break;
		}
		/* 获取块内偏移位置处剩下的字节数 */
		c = sb->s_blocksize - (pos % sb->s_blocksize);
		/* 获取当前块要写的数量 */
		if (c > count-written)
			c = count - written;
		if (c != sb->s_blocksize && !bh->b_uptodate) {
			/* 将块数据读入到高速缓存 */
			ll_rw_block (READ, 1, &bh);
			wait_on_buffer (bh);
			/* 不是最新的则返回出错 */
			if (!bh->b_uptodate) {
				brelse (bh);
				if (!written)
					written = -EIO;
				break;
			}
		}
		/* 获取块内高速缓存写入的地址，并增加偏移量的位置 */
		p = (pos % sb->s_blocksize) + bh->b_data;
		pos += c;
		/*如果最后偏移位置大于文件大小则修改文件大小，同时设置inode为脏 */
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1;
		}
		/* 增加已写入的字节数，同时将数据拷贝到高速缓存 */
		written += c;
		memcpy_fromfs (p, buf, c);
		buf += c;
		/* 因为是写入操作所以一定要设置高速缓存内容为最新的，并设置脏标记以便在同步的时候，
		 * 把刚才写入的数据写入到文件 
		 */
		bh->b_uptodate = 1;
		bh->b_dirt = 1;
		brelse (bh);
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	filp->f_pos = pos;
	inode->i_dirt = 1;
	return written;
}

/*
 * Called when a inode is released. Note that this is different
 * from ext2_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */

/* sys_close关闭文件时，在确定最后需要关闭文件的时候，
 * 也就是f_count=1
 * 会调用此函数
 */
static void ext2_release_file (struct inode * inode, struct file * filp)
{
	if (filp->f_mode & 2)
		ext2_discard_prealloc (inode);
}
