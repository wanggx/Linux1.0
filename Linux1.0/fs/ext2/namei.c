/*
 *  linux/fs/ext2/namei.c
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>

/*
 * comment out this line if you want names > EXT2_NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

/*
 * define how far ahead to read directories while searching them.
 */
#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE        (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)
#define NAMEI_RA_INDEX(c,b)  (((c) * NAMEI_RA_BLOCKS) + (b))

/*
 * NOTE! unlike strncmp, ext2_match returns 1 for success, 0 for failure.
 */
static int ext2_match (int len, const char * const name,
		       struct ext2_dir_entry * de)
{
	unsigned char same;

	if (!de || !de->inode || len > EXT2_NAME_LEN)
		return 0;
	/*
	 * "" means "." ---> so paths like "/usr/lib//libc.a" work
	 */
	if (!len && de->name_len == 1 && (de->name[0] == '.') &&
	   (de->name[1] == '\0'))
		return 1;
	if (len != de->name_len)
		return 0;
	__asm__("cld\n\t"
		"repe ; cmpsb\n\t"
		"setz %0"
		:"=q" (same)
		:"S" ((long) name), "D" ((long) de->name), "c" (len)
		:"cx", "di", "si");
	return (int) same;
}

/*
 *	ext2_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
/* 该函数从一个父目录dir当中查找一个名称为name长度为namelen的文件或目录
 * 然后将查找到的结果存放在res_dir当中
 */
static struct buffer_head * ext2_find_entry (struct inode * dir,
					     const char * const name, int namelen,
					     struct ext2_dir_entry ** res_dir)
{
	struct super_block * sb;
	struct buffer_head * bh_use[NAMEI_RA_SIZE];
	struct buffer_head * bh_read[NAMEI_RA_SIZE];
	unsigned long offset;
	int block, toread, i, err;

	*res_dir = NULL;
	if (!dir)
		return NULL;
	sb = dir->i_sb;

#ifdef NO_TRUNCATE
	if (namelen > EXT2_NAME_LEN)
		return NULL;
#else
	if (namelen > EXT2_NAME_LEN)
		namelen = EXT2_NAME_LEN;
#endif

	memset (bh_use, 0, sizeof (bh_use));
	toread = 0;
	/* 读取目录文件的块，然后将读取的有效高速缓存存放在bh_read当中 */
	for (block = 0; block < NAMEI_RA_SIZE; ++block) {
		struct buffer_head * bh;

		/* 如果超过文件大小 */
		if ((block << EXT2_BLOCK_SIZE_BITS (sb)) >= dir->i_size)
			break;
		bh = ext2_getblk (dir, block, 0, &err);
		/* bh_use中存放的指针不一定有效 */
		bh_use[block] = bh;
		if (bh && !bh->b_uptodate)
			bh_read[toread++] = bh;
	}

	block = 0;
	offset = 0;
	/* 从目录文件的第0块，0处偏移开始，一块一块的开始查找，
	 * 如果找到了，则直接返回 
	 */
	while (offset < dir->i_size) {
		struct buffer_head * bh;
		struct ext2_dir_entry * de;
		char * dlimit;

		if ((block % NAMEI_RA_BLOCKS) == 0 && toread) {
			ll_rw_block (READ, toread, bh_read);
			toread = 0;
		}
		bh = bh_use[block % NAMEI_RA_SIZE];
		if (!bh)
			ext2_panic (sb, "ext2_find_entry",
				    "buffer head pointer is NULL");
		wait_on_buffer (bh);
		/* 高速缓存首先要是最新的 */
		if (!bh->b_uptodate) {
			/*
			 * read error: all bets are off
			 */
			break;
		}

		/* 循环处理目录文件的一块，如果找到了，则返回  */
		de = (struct ext2_dir_entry *) bh->b_data;
		dlimit = bh->b_data + sb->s_blocksize;
		while ((char *) de < dlimit) {
			/* 检查ext2_dir_entry的有效性 */
			if (!ext2_check_dir_entry ("ext2_find_entry", dir,
						   de, bh, offset))
				goto failure;
			/* 如果inode号不为0，且名称和长度匹配，则说明找到了，并返回 */
			if (de->inode != 0 && ext2_match (namelen, name, de)) {
				for (i = 0; i < NAMEI_RA_SIZE; ++i) {
					if (bh_use[i] != bh)
						brelse (bh_use[i]);
				}
				*res_dir = de;
				return bh;
			}
			/* 没找到则继续处理下一个ext2_dir_entry */
			offset += de->rec_len;
			de = (struct ext2_dir_entry *)
				((char *) de + de->rec_len);
		}
		/* 如果这一块还没有找到，则继续找下一块 */
		brelse (bh);
		if (((block + NAMEI_RA_SIZE) << EXT2_BLOCK_SIZE_BITS (sb)) >=
		    dir->i_size)
			bh = NULL;
		else
			bh = ext2_getblk (dir, block + NAMEI_RA_SIZE, 0, &err);
		bh_use[block++ % NAMEI_RA_SIZE] = bh;
		if (bh && !bh->b_uptodate)
			bh_read[toread++] = bh;
	}

failure:
	for (i = 0; i < NAMEI_RA_SIZE; ++i)
		brelse (bh_use[i]);
	return NULL;
}

/* 从目录中查找名称为name长度为len的文件或目录 */
int ext2_lookup (struct inode * dir, const char * name, int len,
		 struct inode ** result)
{
	unsigned long ino;
	struct ext2_dir_entry * de;
	struct buffer_head * bh;

	*result = NULL;
	if (!dir)
		return -ENOENT;
	/* 如果dir对应的不是目录的inode则返回出错 */
	if (!S_ISDIR(dir->i_mode)) {
		iput (dir);
		return -ENOENT;
	}
	/* 这个宏已被定义 */
#ifndef DONT_USE_DCACHE
	if (!(ino = ext2_dcache_lookup (dir->i_dev, dir->i_ino, name, len))) {
#endif
		/* 找到符合条件的ext2_dir_entry */
		if (!(bh = ext2_find_entry (dir, name, len, &de))) {
			iput (dir);
			return -ENOENT;
		}
		/* 获取找到的文件或目录的inode号 */
		ino = de->inode;
#ifndef DONT_USE_DCACHE
		ext2_dcache_add (dir->i_dev, dir->i_ino, de->name,
				 de->name_len, ino);
#endif
		brelse (bh);
#ifndef DONT_USE_DCACHE
	}
#endif
	/* 从超级块对应的磁盘中读取i节点号为ino的inode
	 */
	if (!(*result = iget (dir->i_sb, ino))) {
		iput (dir);
		return -EACCES;
	}
	/* 将父目录inode释放*/
	iput (dir);
	return 0;
}

/*
 *	ext2_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as ext2_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
/* 给目录文件中添加一个项，然后将添加的这项所在的高速缓存指针返回，
 * 以便其他函数来设置数据，在添加项的过程当中，首先会判断该目录文件下是否
 * 已经存在name，namelen的项
 */
static struct buffer_head * ext2_add_entry (struct inode * dir,
					    const char * name, int namelen,
					    struct ext2_dir_entry ** res_dir,
					    int *err)
{
	unsigned long offset;
	unsigned short rec_len;
	struct buffer_head * bh;
	struct ext2_dir_entry * de, * de1;
	struct super_block * sb;

	*err = -EINVAL;
	*res_dir = NULL;
	if (!dir)
		return NULL;
	sb = dir->i_sb;
#ifdef NO_TRUNCATE
	if (namelen > EXT2_NAME_LEN)
		return NULL;
#else
	if (namelen > EXT2_NAME_LEN)
		namelen = EXT2_NAME_LEN;
#endif
	if (!namelen)
		return NULL;
	/*
	 * Is this a busy deleted directory?  Can't create new files if so
	 */
	if (dir->i_size == 0)
	{
		*err = -ENOENT;
		return NULL;
	}
	/* 将目录文件的第0块读入到高速缓存，并返回高速缓存指针，
	 * 按照正常道理来说，一个空文件夹是没有数据块的，但是在新建一个
	 * 文件夹时，系统会默认带上两个特殊的文件.代表自己..代表上一级目录
	 * 所以此处在读取的时候参数0,0会读取到高速缓存
	 */
	bh = ext2_bread (dir, 0, 0, err);
	if (!bh)
		return NULL;
	rec_len = EXT2_DIR_REC_LEN(namelen);
	offset = 0;
	de = (struct ext2_dir_entry *) bh->b_data;
	*err = -ENOSPC;
	while (1) {
		/* 当前高速缓存块已经存满了，所以需要在读取另一个高速缓存块，
		 * 也就相当于用1的方式为目录文件新分配了一个数据块
		 */
		if ((char *)de >= sb->s_blocksize + bh->b_data) {
			brelse (bh);
			bh = NULL;
			bh = ext2_bread (dir, offset >> EXT2_BLOCK_SIZE_BITS(sb), 1, err);
			if (!bh)
				return NULL;
			/* 如果偏移量超过文件大小 */
			if (dir->i_size <= offset) {
				if (dir->i_size == 0) {
					*err = -ENOENT;
					return NULL;
				}

				ext2_debug ("creating next block\n");
				/* 在新的一块中，设置目录项的数据，此处很重要的一点就是inode号为0，
				 * 这里只是找到了一个存放ext2_dir_entry的位置，后面才会将参数中的name，namelen
				 * 写入高速缓存
				 */
				de = (struct ext2_dir_entry *) bh->b_data;
				de->inode = 0;
				de->rec_len = sb->s_blocksize;
				dir->i_size = offset + sb->s_blocksize;
				dir->i_dirt = 1;
#if 0 /* XXX don't update any times until successful completion of syscall */
				dir->i_ctime = CURRENT_TIME;
#endif
			} else {

				ext2_debug ("skipping to next block\n");

				de = (struct ext2_dir_entry *) bh->b_data;
			}
		}
		if (!ext2_check_dir_entry ("ext2_add_entry", dir, de, bh,
					   offset)) {
			*err = -ENOENT;
			brelse (bh);
			return NULL;
		}
		/* 如果已经存在了，则返回出错 */
		if (de->inode != 0 && ext2_match (namelen, name, de)) {
				*err = -EEXIST;
				brelse (bh);
				return NULL;
		}
		if ((de->inode == 0 && de->rec_len >= rec_len) ||
		    (de->rec_len >= EXT2_DIR_REC_LEN(de->name_len) + rec_len)) {
			offset += de->rec_len;
			if (de->inode) {
				de1 = (struct ext2_dir_entry *) ((char *) de +
					EXT2_DIR_REC_LEN(de->name_len));
				de1->rec_len = de->rec_len -
					EXT2_DIR_REC_LEN(de->name_len);
				de->rec_len = EXT2_DIR_REC_LEN(de->name_len);
				de = de1;
			}
			/* 这里的inode依然是0，因为这个函数仅仅是在目录的数据块中添加了一个
			 * ext2_inode_entry结构，inode号在ext2_create函数中设置
			 */
			de->inode = 0;
			de->name_len = namelen;
			memcpy (de->name, name, namelen);
			/*
			 * XXX shouldn't update any times until successful
			 * completion of syscall, but too many callers depend
			 * on this.
			 *
			 * XXX similarly, too many callers depend on
			 * ext2_new_inode() setting the times, but error
			 * recovery deletes the inode, so the worst that can
			 * happen is that the times are slightly out of date
			 * and/or different from the directory change time.
			 */
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
			dir->i_dirt = 1;
			bh->b_dirt = 1;
			*res_dir = de;
			*err = 0;
			return bh;
		}
		/* 处理下一个项 */
		offset += de->rec_len;
		de = (struct ext2_dir_entry *) ((char *) de + de->rec_len);
	}
	brelse (bh);
	return NULL;
}

/*
 * ext2_delete_entry deletes a directory entry by merging it with the
 * previous entry
 */
/* 删除则是仅仅改变了ext2_dir_entry中的rec_len，
 * 也就是直接把删除的那个dir给跳过了。
 */
static int ext2_delete_entry (struct ext2_dir_entry * dir,
			      struct buffer_head * bh)
{
	struct ext2_dir_entry * de, * pde;
	int i;

	i = 0;
	pde = NULL;
	de = (struct ext2_dir_entry *) bh->b_data;
	while (i < bh->b_size) {
		if (!ext2_check_dir_entry ("ext2_delete_entry", NULL, 
					   de, bh, i))
			return -EIO;
		if (de == dir)  {
			if (pde)
				pde->rec_len += dir->rec_len;
			dir->inode = 0;
			return 0;
		}
		i += de->rec_len;
		pde = de;
		de = (struct ext2_dir_entry *) ((char *) de + de->rec_len);
	}
	return -ENOENT;
}

/* 该函数首先在磁盘上为文件分配一个inode，然后调用ext2_add_entry函数
 * 为目录添加一个项，同时设置项的inode号，然后将新建文件的inode放在result中返回
 * 创建的只能是普通文件
 */
int ext2_create (struct inode * dir,const char * name, int len, int mode,
		 struct inode ** result)
{
	struct inode * inode;
	struct buffer_head * bh;
	struct ext2_dir_entry * de;
	int err;

	*result = NULL;
	if (!dir)
		return -ENOENT;
	inode = ext2_new_inode (dir, mode);
	if (!inode) {
		iput (dir);
		return -ENOSPC;
	}
	inode->i_op = &ext2_file_inode_operations;
	inode->i_mode = mode;
	inode->i_dirt = 1;
	bh = ext2_add_entry (dir, name, len, &de, &err);
	if (!bh) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput (inode);
		iput (dir);
		return err;
	}
	/* 设置文件的inode号
	 */
	de->inode = inode->i_ino;
#ifndef DONT_USE_DCACHE
	ext2_dcache_add (dir->i_dev, dir->i_ino, de->name, de->name_len,
			 de->inode);
#endif
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	brelse (bh);
	iput (dir);
	*result = inode;
	return 0;
}

/* 和ext2_create区别，可以创建字符设备文件，块文件结点等等 */
int ext2_mknod (struct inode * dir, const char * name, int len, int mode,
		int rdev)
{
	struct inode * inode;
	struct buffer_head * bh;
	struct ext2_dir_entry * de;
	int err;

	if (!dir)
		return -ENOENT;
	bh = ext2_find_entry (dir, name, len, &de);
	if (bh) {
		brelse (bh);
		iput (dir);
		return -EEXIST;
	}
	inode = ext2_new_inode (dir, mode);
	if (!inode) {
		iput (dir);
		return -ENOSPC;
	}
	inode->i_uid = current->euid;
	inode->i_mode = mode;
	inode->i_op = NULL;
	if (S_ISREG(inode->i_mode))
		inode->i_op = &ext2_file_inode_operations;
	else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ext2_dir_inode_operations;
		if (dir->i_mode & S_ISGID)
			inode->i_mode |= S_ISGID;
	}
	else if (S_ISLNK(inode->i_mode))
		inode->i_op = &ext2_symlink_inode_operations;
	else if (S_ISCHR(inode->i_mode))
		inode->i_op = &chrdev_inode_operations;
	else if (S_ISBLK(inode->i_mode))
		inode->i_op = &blkdev_inode_operations;
	else if (S_ISFIFO(inode->i_mode)) 
		init_fifo(inode);
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_rdev = rdev;
#if 0
	/*
	 * XXX we may as well use the times set by ext2_new_inode().  The
	 * following usually does nothing, but sometimes it invalidates
	 * inode->i_ctime.
	 */
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
#endif
	inode->i_dirt = 1;
	bh = ext2_add_entry (dir, name, len, &de, &err);
	if (!bh) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput (inode);
		iput (dir);
		return err;
	}
	de->inode = inode->i_ino;
#ifndef DONT_USE_DCACHE
	ext2_dcache_add (dir->i_dev, dir->i_ino, de->name, de->name_len,
			 de->inode);
#endif
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	brelse (bh);
	iput (dir);
	iput (inode);
	return 0;
}

/* 创建一个目录 */
int ext2_mkdir (struct inode * dir, const char * name, int len, int mode)
{
	struct inode * inode;
	struct buffer_head * bh, * dir_block;
	struct ext2_dir_entry * de;
	int err;

	if (!dir)
		return -ENOENT;
	bh = ext2_find_entry (dir, name, len, &de);
	if (bh) {
		brelse (bh);
		iput (dir);
		return -EEXIST;
	}
	if (dir->i_nlink >= EXT2_LINK_MAX) {
		iput (dir);
		return -EMLINK;
	}
	inode = ext2_new_inode (dir, S_IFDIR);
	if (!inode) {
		iput (dir);
		return -ENOSPC;
	}
	/* 设置目录文件的i_op */
	inode->i_op = &ext2_dir_inode_operations;
	inode->i_size = inode->i_sb->s_blocksize;
#if 0 /* XXX as above */
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
#endif
	/* 目录文件的第0块数据块缓冲 */
	dir_block = ext2_bread (inode, 0, 1, &err);
	if (!dir_block) {
		iput (dir);
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput (inode);
		return err;
	}
	/* 会自动创建两个文件 .和..*/
	inode->i_blocks = inode->i_sb->s_blocksize / 512;
	de = (struct ext2_dir_entry *) dir_block->b_data;
	de->inode = inode->i_ino;
	de->name_len = 1;
	de->rec_len = EXT2_DIR_REC_LEN(de->name_len);
	strcpy (de->name, ".");
	de = (struct ext2_dir_entry *) ((char *) de + de->rec_len);
	de->inode = dir->i_ino;
	de->rec_len = inode->i_sb->s_blocksize - EXT2_DIR_REC_LEN(1);
	de->name_len = 2;
	strcpy (de->name, "..");
	inode->i_nlink = 2;
	dir_block->b_dirt = 1;
	brelse (dir_block);
	inode->i_mode = S_IFDIR | (mode & S_IRWXUGO & ~current->umask);
	if (dir->i_mode & S_ISGID)
		inode->i_mode |= S_ISGID;
	inode->i_dirt = 1;
	bh = ext2_add_entry (dir, name, len, &de, &err);
	if (!bh) {
		iput (dir);
		inode->i_nlink = 0;
		inode->i_dirt = 1;
		iput (inode);
		return err;
	}
	/* 设置目录文件的inode号 */
	de->inode = inode->i_ino;
#ifndef DONT_USE_DCACHE
	ext2_dcache_add (dir->i_dev, dir->i_ino, de->name, de->name_len,
			 de->inode);
#endif
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	dir->i_nlink++;
	dir->i_dirt = 1;
	iput (dir);
	iput (inode);
	brelse (bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir (struct inode * inode)
{
	unsigned long offset;
	struct buffer_head * bh;
	struct ext2_dir_entry * de, * de1;
	struct super_block * sb;
	int err;

	sb = inode->i_sb;
	if (inode->i_size < EXT2_DIR_REC_LEN(1) + EXT2_DIR_REC_LEN(2) ||
	    !(bh = ext2_bread (inode, 0, 0, &err))) {
	    	ext2_warning (inode->i_sb, "empty_dir",
			      "bad directory (dir %lu)", inode->i_ino);
		return 1;
	}
	de = (struct ext2_dir_entry *) bh->b_data;
	de1 = (struct ext2_dir_entry *) ((char *) de + de->rec_len);
	if (de->inode != inode->i_ino || !de1->inode || 
	    strcmp (".", de->name) || strcmp ("..", de1->name)) {
	    	ext2_warning (inode->i_sb, "empty_dir",
			      "bad directory (dir %lu)", inode->i_ino);
		return 1;
	}
	offset = de->rec_len + de1->rec_len;
	de = (struct ext2_dir_entry *) ((char *) de1 + de1->rec_len);
	while (offset < inode->i_size ) {
		if ((void *) de >= (void *) (bh->b_data + sb->s_blocksize)) {
			brelse (bh);
			bh = ext2_bread (inode, offset >> EXT2_BLOCK_SIZE_BITS(sb), 1, &err);
			if (!bh) {
				offset += sb->s_blocksize;
				continue;
			}
			de = (struct ext2_dir_entry *) bh->b_data;
		}
		if (!ext2_check_dir_entry ("empty_dir", inode, de, bh,
					   offset)) {
			brelse (bh);
			return 1;
		}
		if (de->inode) {
			brelse (bh);
			return 0;
		}
		offset += de->rec_len;
		de = (struct ext2_dir_entry *) ((char *) de + de->rec_len);
	}
	brelse (bh);
	return 1;
}

int ext2_rmdir (struct inode * dir, const char * name, int len)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct ext2_dir_entry * de;

repeat:
	if (!dir)
		return -ENOENT;
	inode = NULL;
	bh = ext2_find_entry (dir, name, len, &de);
	retval = -ENOENT;
	if (!bh)
		goto end_rmdir;
	retval = -EPERM;
	if (!(inode = iget (dir->i_sb, de->inode)))
		goto end_rmdir;
	if (inode->i_dev != dir->i_dev)
		goto end_rmdir;
	if (de->inode != inode->i_ino) {
		iput(inode);
		brelse(bh);
		current->counter = 0;
		schedule();
		goto repeat;
	}
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid)
		goto end_rmdir;
	if (inode == dir)	/* we may not delete ".", but "../dir" is ok */
		goto end_rmdir;
	if (!S_ISDIR(inode->i_mode)) {
		retval = -ENOTDIR;
		goto end_rmdir;
	}
	down(&inode->i_sem);
	if (!empty_dir (inode))
		retval = -ENOTEMPTY;
	else if (de->inode != inode->i_ino)
		retval = -ENOENT;
	else {
		if (inode->i_count > 1) {
		/*
		 * Are we deleting the last instance of a busy directory?
		 * Better clean up if so.
		 *
		 * Make directory empty (it will be truncated when finally
		 * dereferenced).  This also inhibits ext2_add_entry.
		 */
			inode->i_size = 0;
		}
		retval = ext2_delete_entry (de, bh);
	}
	up(&inode->i_sem);
	if (retval)
		goto end_rmdir;
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
#ifndef DONT_USE_DCACHE
	ext2_dcache_remove(inode->i_dev, inode->i_ino, ".", 1);
	ext2_dcache_remove(inode->i_dev, inode->i_ino, "..", 2);
#endif
	if (inode->i_nlink != 2)
		ext2_warning (inode->i_sb, "ext2_rmdir",
			      "empty directory has nlink!=2 (%d)",
			      inode->i_nlink);
#ifndef DONT_USE_DCACHE
	ext2_dcache_remove (dir->i_dev, dir->i_ino, de->name, de->name_len);
#endif
	inode->i_nlink = 0;
	inode->i_dirt = 1;
	dir->i_nlink--;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt = 1;
end_rmdir:
	iput (dir);
	iput (inode);
	brelse (bh);
	return retval;
}

int ext2_unlink (struct inode * dir, const char * name, int len)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct ext2_dir_entry * de;

repeat:
	if (!dir)
		return -ENOENT;
	retval = -ENOENT;
	inode = NULL;
	bh = ext2_find_entry (dir, name, len, &de);
	if (!bh)
		goto end_unlink;
	if (!(inode = iget (dir->i_sb, de->inode)))
		goto end_unlink;
	retval = -EPERM;
	if (S_ISDIR(inode->i_mode))
		goto end_unlink;
	if (de->inode != inode->i_ino) {
		iput(inode);
		brelse(bh);
		current->counter = 0;
		schedule();
		goto repeat;
	}
	if ((dir->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid)
		goto end_unlink;
	if (!inode->i_nlink) {
		ext2_warning (inode->i_sb, "ext2_unlink",
			      "Deleting nonexistent file (%lu), %d",
			      inode->i_ino, inode->i_nlink);
		inode->i_nlink = 1;
	}
	retval = ext2_delete_entry (de, bh);
	if (retval)
		goto end_unlink;
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
#ifndef DONT_USE_DCACHE
	ext2_dcache_remove (dir->i_dev, dir->i_ino, de->name, de->name_len);
#endif
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt = 1;
	inode->i_nlink--;
	inode->i_dirt = 1;
	inode->i_ctime = dir->i_ctime;
	retval = 0;
end_unlink:
	brelse (bh);
	iput (inode);
	iput (dir);
	return retval;
}

int ext2_symlink (struct inode * dir, const char * name, int len,
		  const char * symname)
{
	struct ext2_dir_entry * de;
	struct inode * inode = NULL;
	struct buffer_head * bh = NULL, * name_block = NULL;
	char * link;
	int i, err;
	int l;
	char c;

	if (!(inode = ext2_new_inode (dir, S_IFLNK))) {
		iput (dir);
		return -ENOSPC;
	}
	inode->i_mode = S_IFLNK | S_IRWXUGO;
	inode->i_op = &ext2_symlink_inode_operations;
	for (l = 0; l < inode->i_sb->s_blocksize - 1 &&
	     symname [l]; l++)
		;
	if (l >= EXT2_N_BLOCKS * sizeof (unsigned long)) {

		ext2_debug ("l=%d, normal symlink\n", l);

		name_block = ext2_bread (inode, 0, 1, &err);
		if (!name_block) {
			iput (dir);
			inode->i_nlink--;
			inode->i_dirt = 1;
			iput (inode);
			return err;
		}
		link = name_block->b_data;
	} else {
		link = (char *) inode->u.ext2_i.i_data;

		ext2_debug ("l=%d, fast symlink\n", l);

	}
	i = 0;
	while (i < inode->i_sb->s_blocksize - 1 && (c = *(symname++)))
		link[i++] = c;
	link[i] = 0;
	if (name_block) {
		name_block->b_dirt = 1;
		brelse (name_block);
	}
	inode->i_size = i;
	inode->i_dirt = 1;
	bh = ext2_find_entry (dir, name, len, &de);
	if (bh) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput (inode);
		brelse (bh);
		iput (dir);
		return -EEXIST;
	}
	bh = ext2_add_entry (dir, name, len, &de, &err);
	if (!bh) {
		inode->i_nlink--;
		inode->i_dirt = 1;
		iput (inode);
		iput (dir);
		return err;
	}
	de->inode = inode->i_ino;
#ifndef DONT_USE_DCACHE
	ext2_dcache_add (dir->i_dev, dir->i_ino, de->name, de->name_len,
			 de->inode);
#endif
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	brelse (bh);
	iput (dir);
	iput (inode);
	return 0;
}


/* 将oldinode节点连接到dir目录节点下面，名称为name,长度为len
 */
int ext2_link (struct inode * oldinode, struct inode * dir,
	       const char * name, int len)
{
	struct ext2_dir_entry * de;
	struct buffer_head * bh;
	int err;

	/*不能是目录文件*/
	if (S_ISDIR(oldinode->i_mode)) {
		iput (oldinode);
		iput (dir);
		return -EPERM;
	}

	/*不能超过最大链接数*/
	if (oldinode->i_nlink >= EXT2_LINK_MAX) {
		iput (oldinode);
		iput (dir);
		return -EMLINK;
	}
	/*先判断需要链接的文件是否存在，如果存在则返回已存在的错误*/
	bh = ext2_find_entry (dir, name, len, &de);
	if (bh) {
		brelse (bh);
		iput (dir);
		iput (oldinode);
		return -EEXIST;
	}
	/* 向目录中添加一个文件链接
	 */ 
	bh = ext2_add_entry (dir, name, len, &de, &err);
	if (!bh) {
		iput (dir);
		iput (oldinode);
		return err;
	}
	/* 指向同一个inode号
	 */
	de->inode = oldinode->i_ino;
#ifndef DONT_USE_DCACHE
	ext2_dcache_add (dir->i_dev, dir->i_ino, de->name, de->name_len,
			 de->inode);
#endif
	bh->b_dirt = 1;
	if (IS_SYNC(dir)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	brelse (bh);
	iput (dir);
	oldinode->i_nlink++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput (oldinode);
	return 0;
}

static int subdir (struct inode * new_inode, struct inode * old_inode)
{
	int ino;
	int result;

	new_inode->i_count++;
	result = 0;
	for (;;) {
		if (new_inode == old_inode) {
			result = 1;
			break;
		}
		if (new_inode->i_dev != old_inode->i_dev)
			break;
		ino = new_inode->i_ino;
		if (ext2_lookup (new_inode, "..", 2, &new_inode))
			break;
		if (new_inode->i_ino == ino)
			break;
	}
	iput (new_inode);
	return result;
}

#define PARENT_INO(buffer) \
	((struct ext2_dir_entry *) ((char *) buffer + \
	((struct ext2_dir_entry *) buffer)->rec_len))->inode

#define PARENT_NAME(buffer) \
	((struct ext2_dir_entry *) ((char *) buffer + \
	((struct ext2_dir_entry *) buffer)->rec_len))->name

/*
 * rename uses retrying to avoid race-conditions: at least they should be
 * minimal.
 * it tries to allocate all the blocks, then sanity-checks, and if the sanity-
 * checks fail, it tries to restart itself again. Very practical - no changes
 * are done until we know everything works ok.. and then all the changes can be
 * done in one fell swoop when we have claimed all the buffers needed.
 *
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 */
static int do_ext2_rename (struct inode * old_dir, const char * old_name,
			   int old_len, struct inode * new_dir,
			   const char * new_name, int new_len)
{
	struct inode * old_inode, * new_inode;
	struct buffer_head * old_bh, * new_bh, * dir_bh;
	struct ext2_dir_entry * old_de, * new_de;
	int retval;

	goto start_up;
try_again:
	if (new_bh && new_de)
		ext2_delete_entry(new_de, new_bh);
	brelse (old_bh);
	brelse (new_bh);
	brelse (dir_bh);
	iput (old_inode);
	iput (new_inode);
	current->counter = 0;
	schedule ();
start_up:
	old_inode = new_inode = NULL;
	old_bh = new_bh = dir_bh = NULL;
	new_de = NULL;
	old_bh = ext2_find_entry (old_dir, old_name, old_len, &old_de);
	retval = -ENOENT;
	if (!old_bh)
		goto end_rename;
	old_inode = __iget (old_dir->i_sb, old_de->inode, 0); /* don't cross mnt-points */
	if (!old_inode)
		goto end_rename;
	retval = -EPERM;
	if ((old_dir->i_mode & S_ISVTX) && 
	    current->euid != old_inode->i_uid &&
	    current->euid != old_dir->i_uid && !suser())
		goto end_rename;
	new_bh = ext2_find_entry (new_dir, new_name, new_len, &new_de);
	if (new_bh) {
		new_inode = __iget (new_dir->i_sb, new_de->inode, 0); /* no mntp cross */
		if (!new_inode) {
			brelse (new_bh);
			new_bh = NULL;
		}
	}
	if (new_inode == old_inode) {
		retval = 0;
		goto end_rename;
	}
	if (new_inode && S_ISDIR(new_inode->i_mode)) {
		retval = -EISDIR;
		if (!S_ISDIR(old_inode->i_mode))
			goto end_rename;
		retval = -EINVAL;
		if (subdir (new_dir, old_inode))
			goto end_rename;
		retval = -ENOTEMPTY;
		if (!empty_dir (new_inode))
			goto end_rename;
		retval = -EBUSY;
		if (new_inode->i_count > 1)
			goto end_rename;
	}
	retval = -EPERM;
	if (new_inode && (new_dir->i_mode & S_ISVTX) &&
	    current->euid != new_inode->i_uid &&
	    current->euid != new_dir->i_uid && !suser())
		goto end_rename;
	if (S_ISDIR(old_inode->i_mode)) {
		retval = -ENOTDIR;
		if (new_inode && !S_ISDIR(new_inode->i_mode))
			goto end_rename;
		retval = -EINVAL;
		if (subdir (new_dir, old_inode))
			goto end_rename;
		dir_bh = ext2_bread (old_inode, 0, 0, &retval);
		if (!dir_bh)
			goto end_rename;
		if (PARENT_INO(dir_bh->b_data) != old_dir->i_ino)
			goto end_rename;
		retval = -EMLINK;
		if (!new_inode && new_dir->i_nlink >= EXT2_LINK_MAX)
			goto end_rename;
	}
	if (!new_bh)
		new_bh = ext2_add_entry (new_dir, new_name, new_len, &new_de,
					 &retval);
	if (!new_bh)
		goto end_rename;
	/*
	 * sanity checking before doing the rename - avoid races
	 */
	if (new_inode && (new_de->inode != new_inode->i_ino))
		goto try_again;
	if (new_de->inode && !new_inode)
		goto try_again;
	if (old_de->inode != old_inode->i_ino)
		goto try_again;
	/*
	 * ok, that's it
	 */
	new_de->inode = old_inode->i_ino;
#ifndef DONT_USE_DCACHE
	ext2_dcache_remove (old_dir->i_dev, old_dir->i_ino, old_de->name,
			    old_de->name_len);
	ext2_dcache_add (new_dir->i_dev, new_dir->i_ino, new_de->name,
			 new_de->name_len, new_de->inode);
#endif
	retval = ext2_delete_entry (old_de, old_bh);
	if (retval == -ENOENT)
		goto try_again;
	if (retval)
		goto end_rename;
	if (new_inode) {
		new_inode->i_nlink--;
		new_inode->i_ctime = CURRENT_TIME;
		new_inode->i_dirt = 1;
	}
	old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
	old_dir->i_dirt = 1;
	old_bh->b_dirt = 1;
	if (IS_SYNC(old_dir)) {
		ll_rw_block (WRITE, 1, &old_bh);
		wait_on_buffer (old_bh);
	}
	new_bh->b_dirt = 1;
	if (IS_SYNC(new_dir)) {
		ll_rw_block (WRITE, 1, &new_bh);
		wait_on_buffer (new_bh);
	}
	if (dir_bh) {
		PARENT_INO(dir_bh->b_data) = new_dir->i_ino;
		dir_bh->b_dirt = 1;
		old_dir->i_nlink--;
		old_dir->i_dirt = 1;
		if (new_inode) {
			new_inode->i_nlink--;
			new_inode->i_dirt = 1;
		} else {
			new_dir->i_nlink++;
			new_dir->i_dirt = 1;
		}
	}
	retval = 0;
end_rename:
	brelse (dir_bh);
	brelse (old_bh);
	brelse (new_bh);
	iput (old_inode);
	iput (new_inode);
	iput (old_dir);
	iput (new_dir);
	return retval;
}

/*
 * Ok, rename also locks out other renames, as they can change the parent of
 * a directory, and we don't want any races. Other races are checked for by
 * "do_rename()", which restarts if there are inconsistencies.
 *
 * Note that there is no race between different filesystems: it's only within
 * the same device that races occur: many renames can happen at once, as long
 * as they are on different partitions.
 *
 * In the second extended file system, we use a lock flag stored in the memory
 * super-block.  This way, we really lock other renames only if they occur
 * on the same file system
 */
int ext2_rename (struct inode * old_dir, const char * old_name, int old_len,
		 struct inode * new_dir, const char * new_name, int new_len)
{
	int result;

	while (old_dir->i_sb->u.ext2_sb.s_rename_lock)
		sleep_on (&old_dir->i_sb->u.ext2_sb.s_rename_wait);
	old_dir->i_sb->u.ext2_sb.s_rename_lock = 1;
	result = do_ext2_rename (old_dir, old_name, old_len, new_dir,
				 new_name, new_len);
	old_dir->i_sb->u.ext2_sb.s_rename_lock = 0;
	wake_up (&old_dir->i_sb->u.ext2_sb.s_rename_wait);
	return result;
}
