/*
 *  linux/fs/ext2/inode.c
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Goal-directed block allocation by Stephen Tweedie (sct@dcs.ed.ac.uk), 1993
 */

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>

#define clear_block(addr,size) \
	__asm__("cld\n\t" \
		"rep\n\t" \
		"stosl" \
		: \
		:"a" (0), "c" (size / 4), "D" ((long) (addr)) \
		:"cx", "di")

void ext2_put_inode (struct inode * inode)
{
	ext2_discard_prealloc (inode);
	if (inode->i_nlink || inode->i_ino == EXT2_ACL_IDX_INO ||
	    inode->i_ino == EXT2_ACL_DATA_INO)
		return;
	inode->i_size = 0;
	if (inode->i_blocks)
		ext2_truncate (inode);
	ext2_free_inode (inode);
}

/* 获取文件逻辑块号对应的设备逻辑块号 */
#define inode_bmap(inode, nr) ((inode)->u.ext2_i.i_data[(nr)])

/* 返回高速缓存当中偏移为nr的数据，也就是设备的逻辑块号 */
static int block_bmap (struct buffer_head * bh, int nr)
{
	int tmp;

	if (!bh)
		return 0;
	tmp = ((unsigned long *) bh->b_data)[nr];
	brelse (bh);
	return tmp;
}

/* 
 * ext2_discard_prealloc and ext2_alloc_block are atomic wrt. the
 * superblock in the same manner as are ext2_free_blocks and
 * ext2_new_block.  We just wait on the super rather than locking it
 * here, since ext2_new_block will do the necessary locking and we
 * can't block until then.
 */

/* 丢弃所有的预分配剩余的块
 */
void ext2_discard_prealloc (struct inode * inode)
{
#ifdef EXT2_PREALLOCATE
	if (inode->u.ext2_i.i_prealloc_count) {
		int i = inode->u.ext2_i.i_prealloc_count;
		inode->u.ext2_i.i_prealloc_count = 0;
		ext2_free_blocks (inode->i_sb,
				  inode->u.ext2_i.i_prealloc_block,
				  i);
	}
#endif
}

/* 返回一个设备的逻辑块号，表示新块的实际分配位置 
 * goal只是希望分配得到的块号
 */
static int ext2_alloc_block (struct inode * inode, unsigned long goal)
{
#ifdef EXT2FS_DEBUG
	static unsigned long alloc_hits = 0, alloc_attempts = 0;
#endif
	unsigned long result;
	struct buffer_head * bh;

	wait_on_super (inode->i_sb);

#ifdef EXT2_PREALLOCATE
	/* 如果先前有预分配，且希望分配的块号真好是预先分配的块号，
	 * 则返回预分配的逻辑块号 
	 */
	if (inode->u.ext2_i.i_prealloc_count &&
	    (goal == inode->u.ext2_i.i_prealloc_block ||
	     goal + 1 == inode->u.ext2_i.i_prealloc_block))
	{		
		/* 返回之前预先分配的块号，同时增加下一次预先分配的块号，
		 * 在ext2文件系统中为文件分配的块号尽量靠在一起，所以预先分配的块号
		 * 会紧邻之前已经分配的块号 
		 */
		result = inode->u.ext2_i.i_prealloc_block++;
		/* 较小预先分配的块数量 */
		inode->u.ext2_i.i_prealloc_count--;
		ext2_debug ("preallocation hit (%lu/%lu).\n",
			    ++alloc_hits, ++alloc_attempts);

		/* It doesn't matter if we block in getblk() since
		   we have already atomically allocated the block, and
		   are only clearing it now. */
		if (!(bh = getblk (inode->i_sb->s_dev, result,
				   inode->i_sb->s_blocksize))) {
			ext2_error (inode->i_sb, "ext2_alloc_block",
				    "cannot get block %lu", result);
			return 0;
		}
		clear_block (bh->b_data, inode->i_sb->s_blocksize);
		bh->b_uptodate = 1;
		bh->b_dirt = 1;
		brelse (bh);
	} else {
		ext2_discard_prealloc (inode);
		ext2_debug ("preallocation miss (%lu/%lu).\n",
			    alloc_hits, ++alloc_attempts);
		if (S_ISREG(inode->i_mode))
			result = ext2_new_block
				(inode->i_sb, goal,
				 &inode->u.ext2_i.i_prealloc_count,
				 &inode->u.ext2_i.i_prealloc_block);
		else
			result = ext2_new_block (inode->i_sb, goal, 0, 0);
	}
#else
	result = ext2_new_block (inode->i_sb, goal, 0, 0);
#endif

	return result;
}


/* 将文件的数据块号block映射到设备的逻辑块号
 */
int ext2_bmap (struct inode * inode, int block)
{
	int i;
	/* 每一块数据当中，可以存放多少个地址 */
	int addr_per_block = EXT2_ADDR_PER_BLOCK(inode->i_sb);

	if (block < 0) {
		ext2_warning (inode->i_sb, "ext2_bmap", "block < 0");
		return 0;
	}

	/* 判断文件块号是不是超过ext2系统支持的最大文件的块号 */
	if (block >= EXT2_NDIR_BLOCKS + addr_per_block +
		     addr_per_block * addr_per_block +
		     addr_per_block * addr_per_block * addr_per_block) {
		ext2_warning (inode->i_sb, "ext2_bmap", "block > big");
		return 0;
	}

	/* 如果是直接映射 */
	if (block < EXT2_NDIR_BLOCKS)
		return inode_bmap (inode, block);
	block -= EXT2_NDIR_BLOCKS;
	if (block < addr_per_block) {
		/* 取出一级映射所在的块号 */
		i = inode_bmap (inode, EXT2_IND_BLOCK);
		if (!i)
			return 0;
		return block_bmap (bread (inode->i_dev, i,
					  inode->i_sb->s_blocksize), block);
	}
	/* 开始二级映射 */
	block -= addr_per_block;
	if (block < addr_per_block * addr_per_block) {
		/* 获取二级映射的块号 */
		i = inode_bmap (inode, EXT2_DIND_BLOCK);
		if (!i)
			return 0;
		/* 找到在二级块中的偏移块号 */
		i = block_bmap (bread (inode->i_dev, i,
				       inode->i_sb->s_blocksize),
				block / addr_per_block);
		if (!i)
			return 0;
		return block_bmap (bread (inode->i_dev, i,
					  inode->i_sb->s_blocksize),
				   block & (addr_per_block - 1));
	}
	/* 开始三级映射，最终返回文件的逻辑块号对应的设备逻辑块号 */
	block -= addr_per_block * addr_per_block;
	i = inode_bmap (inode, EXT2_TIND_BLOCK);
	if (!i)
		return 0;
	i = block_bmap (bread (inode->i_dev, i, inode->i_sb->s_blocksize),
			block / (addr_per_block * addr_per_block));
	if (!i)
		return 0;
	i = block_bmap (bread (inode->i_dev, i, inode->i_sb->s_blocksize),
			(block / addr_per_block) & (addr_per_block - 1));
	if (!i)
		return 0;
	return block_bmap (bread (inode->i_dev, i, inode->i_sb->s_blocksize),
			   block & (addr_per_block - 1));
}

/* 获取文件nr逻辑块的数据到高速缓存，并返回该高速缓存，
 * 如果nr有效，则直接读取，否则给文件创建一个新的数据块
 * 如果create参数不为0，其中new_block是要分配的逻辑块号
 * 如果nr块已经在内存了，那么直接返回对应的高速缓存，
 * 则new_block不起作用
 */
static struct buffer_head * inode_getblk (struct inode * inode, int nr,
					  int create, int new_block, int * err)
{
	int tmp, goal = 0;
	unsigned long * p;
	struct buffer_head * result;
	int blocks = inode->i_sb->s_blocksize / 512;
	/* 获取文件的nr逻辑块对应的设备逻辑块号地址 */
	p = inode->u.ext2_i.i_data + nr;
repeat:
	tmp = *p;
	/* 如果设备逻辑块号有效,否则就要创建一个新的块，如果create为1 */
	if (tmp) {
		result = getblk (inode->i_dev, tmp, inode->i_sb->s_blocksize);
		/* 这个判断是防止在getblk的时候inode的数据被其他进程修改 */
		if (tmp == *p)
			return result;
		brelse (result);
		goto repeat;
	}
	if (!create || new_block >= 
	    (current->rlim[RLIMIT_FSIZE].rlim_cur >>
	     EXT2_BLOCK_SIZE_BITS(inode->i_sb))) {
		*err = -EFBIG;
		return NULL;
	}

	/* 如果要分配的一块正好和inode中记录的下一个分配的块号相同，
	 * 则把下一个要分配的设备逻辑块号给goal
	 */
	if (inode->u.ext2_i.i_next_alloc_block == new_block)
		goal = inode->u.ext2_i.i_next_alloc_goal;

	ext2_debug ("hint = %d,", goal);

	/* 如果没有合适的分配目标 */
	if (!goal) {
		/* 扫描文件nr之前的逻辑块号，
		 * 设置goal为文件最后一个有效逻辑块号对应的设备逻辑块号,
		 * goal只是希望分到的设备逻辑块号，但在真正使用的时候，
		 * 最终不一定实际分配的就是goal这块
		 */
		for (tmp = nr - 1; tmp >= 0; tmp--) {
			if (inode->u.ext2_i.i_data[tmp]) {
				goal = inode->u.ext2_i.i_data[tmp];
				break;
			}
		}
		/* 如果依然没有找到(这种情况在一个空文件的情况下，一个字节都没有写入)，
		 * 则希望分到inode所在的组中的第一块
		 */
		if (!goal)
			goal = (inode->u.ext2_i.i_block_group * 
				EXT2_BLOCKS_PER_GROUP(inode->i_sb)) +
			       inode->i_sb->u.ext2_sb.s_es->s_first_data_block;
	}

	ext2_debug ("goal = %d.\n", goal);

	tmp = ext2_alloc_block (inode, goal);
	if (!tmp)
		return NULL;
	/* 为设备的逻辑块分配一个高速缓存 */
	result = getblk (inode->i_dev, tmp, inode->i_sb->s_blocksize);
	if (*p) {
		ext2_free_blocks (inode->i_sb, tmp, 1);
		brelse (result);
		goto repeat;
	}
	/* 设置文件逻辑块号对应的设备逻辑块号 */
	*p = tmp;
	inode->u.ext2_i.i_next_alloc_block = new_block;
	inode->u.ext2_i.i_next_alloc_goal = tmp;
	inode->i_ctime = CURRENT_TIME;
	inode->i_blocks += blocks;
	if (IS_SYNC(inode))
		ext2_sync_inode (inode);
	else
		inode->i_dirt = 1;
	return result;
}

/* 从文件的非直接映射块中获取文件逻辑块号为new_block的设备逻辑块号 */
static struct buffer_head * block_getblk (struct inode * inode,
					  struct buffer_head * bh, int nr,
					  int create, int blocksize, 
					  int new_block, int * err)
{
	int tmp, goal = 0;
	unsigned long * p;
	struct buffer_head * result;
	int blocks = inode->i_sb->s_blocksize / 512;

	if (!bh)
		return NULL;
	/* 首先块要是最新的，否则返回失败 */
	if (!bh->b_uptodate) {
		ll_rw_block (READ, 1, &bh);
		wait_on_buffer (bh);
		if (!bh->b_uptodate) {
			brelse (bh);
			return NULL;
		}
	}
	/* 获取bh块中偏移为nr的块地址 */
	p = (unsigned long *) bh->b_data + nr;
repeat:
	/* 获取对应的逻辑块号 */
	tmp = *p;
	if (tmp) {
		/* 这个判断是防止在getblk的时候inode的数据被其他进程修改 */
		result = getblk (bh->b_dev, tmp, blocksize);
		if (tmp == *p) {
			brelse (bh);
			return result;
		}
		brelse (result);
		goto repeat;
	}
	if (!create || new_block >= 
	    (current->rlim[RLIMIT_FSIZE].rlim_cur >> 
	     EXT2_BLOCK_SIZE_BITS(inode->i_sb))) {
		brelse (bh);
		*err = -EFBIG;
		return NULL;
	}
	/* 这段goal的道理和inode_getblk差不多 */
	if (inode->u.ext2_i.i_next_alloc_block == new_block)
		goal = inode->u.ext2_i.i_next_alloc_goal;
	if (!goal) {
		for (tmp = nr - 1; tmp >= 0; tmp--) {
			if (((unsigned long *) bh->b_data)[tmp]) {
				goal = ((unsigned long *)bh->b_data)[tmp];
				break;
			}
		}
		if (!goal)
			goal = bh->b_blocknr;
	}
	tmp = ext2_alloc_block (inode, goal);
	if (!tmp) {
		brelse (bh);
		return NULL;
	}
	result = getblk (bh->b_dev, tmp, blocksize);
	if (*p) {
		ext2_free_blocks (inode->i_sb, tmp, 1);
		brelse (result);
		goto repeat;
	}
	/* 这是文件逻辑块号对应的设备逻辑块号 */
	*p = tmp;
	bh->b_dirt = 1;
	if (IS_SYNC(inode)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	inode->i_ctime = CURRENT_TIME;
	inode->i_blocks += blocks;
	inode->i_dirt = 1;
	inode->u.ext2_i.i_next_alloc_block = new_block;
	inode->u.ext2_i.i_next_alloc_goal = tmp;
	brelse (bh);
	return result;
}

/* 只是分配了逻辑块号的高速缓存，
 * 但并没有将文件相应逻辑块的数据读入到高速缓存 ,
 * block代表文件的逻辑块号
 */
struct buffer_head * ext2_getblk (struct inode * inode, long block,
				  int create, int * err)
{
	struct buffer_head * bh;
	unsigned long b;

	/* 每个数据块可以存放的地址数量 */
	unsigned long addr_per_block = EXT2_ADDR_PER_BLOCK(inode->i_sb);

	*err = -EIO;
	if (block < 0) {
		ext2_warning (inode->i_sb, "ext2_getblk", "block < 0");
		return NULL;
	}

	/* 如果大于系统支持的最大文件块数 */
	if (block > EXT2_NDIR_BLOCKS + addr_per_block  +
		    addr_per_block * addr_per_block +
		    addr_per_block * addr_per_block * addr_per_block) {
		ext2_warning (inode->i_sb, "ext2_getblk", "block > big");
		return NULL;
	}
	/*
	 * If this is a sequential block allocation, set the next_alloc_block
	 * to this block now so that all the indblock and data block
	 * allocations use the same goal zone
	 */

	ext2_debug ("block %lu, next %lu, goal %lu.\n", block, 
		    inode->u.ext2_i.i_next_alloc_block,
		    inode->u.ext2_i.i_next_alloc_goal);

	if (block == inode->u.ext2_i.i_next_alloc_block + 1) {
		inode->u.ext2_i.i_next_alloc_block++;
		inode->u.ext2_i.i_next_alloc_goal++;
	}

	*err = -ENOSPC;
	b = block;
	/* 如果是直接映射，则在inode所在的块组中分配一个空闲数据块 */
	if (block < EXT2_NDIR_BLOCKS)
		return inode_getblk (inode, block, create, b, err);
	block -= EXT2_NDIR_BLOCKS;
	if (block < addr_per_block) {
		bh = inode_getblk (inode, EXT2_IND_BLOCK, create, b, err);
		return block_getblk (inode, bh, block, create,
				     inode->i_sb->s_blocksize, b, err);
	}
	block -= addr_per_block;
	/* 开始处理二级映射的逻辑块号 ，再减依次就是三级映射处理 */
	if (block < addr_per_block * addr_per_block) {
		bh = inode_getblk (inode, EXT2_DIND_BLOCK, create, b, err);
		bh = block_getblk (inode, bh, block / addr_per_block, create,
				   inode->i_sb->s_blocksize, b, err);
		return block_getblk (inode, bh, block & (addr_per_block - 1),
				     create, inode->i_sb->s_blocksize, b, err);
	}
	block -= addr_per_block * addr_per_block;
	bh = inode_getblk (inode, EXT2_TIND_BLOCK, create, b, err);
	bh = block_getblk (inode, bh, block/(addr_per_block * addr_per_block),
			   create, inode->i_sb->s_blocksize, b, err);
	bh = block_getblk (inode, bh, (block/addr_per_block) & (addr_per_block - 1),
			   create, inode->i_sb->s_blocksize, b, err);
	return block_getblk (inode, bh, block & (addr_per_block - 1), create,
			     inode->i_sb->s_blocksize, b, err);
}

/* 将文件的逻辑块读入到高速缓存，同时返回高速缓存的地址 
 */
struct buffer_head * ext2_bread (struct inode * inode, int block, 
				 int create, int *err)
{
	struct buffer_head * bh;

	bh = ext2_getblk (inode, block, create, err);
	if (!bh || bh->b_uptodate)
		return bh;
	ll_rw_block (READ, 1, &bh);
	wait_on_buffer (bh);
	if (bh->b_uptodate)
		return bh;
	brelse (bh);
	*err = -EIO;
	return NULL;
}

/* 对应超级块中inode的读取函数，也就是从
 * 硬盘中读取文件inode信息，同时设置inode，file的操作函数
 */
void ext2_read_inode (struct inode * inode)
{
	struct buffer_head * bh;
	struct ext2_inode * raw_inode;
	unsigned long block_group;
	unsigned long group_desc;
	unsigned long desc;
	unsigned long block;
	struct ext2_group_desc * gdp;

	if ((inode->i_ino != EXT2_ROOT_INO && inode->i_ino != EXT2_ACL_IDX_INO &&
	     inode->i_ino != EXT2_ACL_DATA_INO && inode->i_ino < EXT2_FIRST_INO) ||
	    inode->i_ino > inode->i_sb->u.ext2_sb.s_es->s_inodes_count) {
		ext2_error (inode->i_sb, "ext2_read_inode",
			    "bad inode number: %lu", inode->i_ino);
		return;
	}
	/* 获取该ino在哪个块组里面 */
	block_group = (inode->i_ino - 1) / EXT2_INODES_PER_GROUP(inode->i_sb);
	/* 如果组号大于系统最大组号，则出错 */
	if (block_group >= inode->i_sb->u.ext2_sb.s_groups_count)
		ext2_panic (inode->i_sb, "ext2_read_inode",
			    "group >= groups count");
	/* 找到第几块中存放的第几个组描述符 */
	group_desc = block_group / EXT2_DESC_PER_BLOCK(inode->i_sb);
	desc = block_group % EXT2_DESC_PER_BLOCK(inode->i_sb);
	/* 获取相应数据块号在高速缓存中的地址 */
	bh = inode->i_sb->u.ext2_sb.s_group_desc[group_desc];
	if (!bh)
		ext2_panic (inode->i_sb, "ext2_read_inode",
			    "Descriptor not loaded");
	gdp = (struct ext2_group_desc *) bh->b_data;
	/* 获取inode数据的数据块号，然后根据数据块号来读取硬盘中ext2_inode的数据 */
	block = gdp[desc].bg_inode_table +
		(((inode->i_ino - 1) % EXT2_INODES_PER_GROUP(inode->i_sb))
		 / EXT2_INODES_PER_BLOCK(inode->i_sb));
	if (!(bh = bread (inode->i_dev, block, inode->i_sb->s_blocksize)))
		ext2_panic (inode->i_sb, "ext2_read_inode",
			    "unable to read i-node block\n"
			    "inode=%lu, block=%lu", inode->i_ino, block);
	/* 因为读取的是一整块数据，而一整块数据中存放了多个ext2_inode的数据，
	 * 所以我们要获取需要ext2_inode在数据块中的偏移
	 */
	raw_inode = ((struct ext2_inode *) bh->b_data) +
		(inode->i_ino - 1) % EXT2_INODES_PER_BLOCK(inode->i_sb);
	/* 从磁盘中读取的ext2文件系统中文件的inode的数据
	 * 并赋值
	 */
	inode->i_mode = raw_inode->i_mode;
	inode->i_uid = raw_inode->i_uid;
	inode->i_gid = raw_inode->i_gid;
	inode->i_nlink = raw_inode->i_links_count;
	inode->i_size = raw_inode->i_size;
	inode->i_atime = raw_inode->i_atime;
	inode->i_ctime = raw_inode->i_ctime;
	inode->i_mtime = raw_inode->i_mtime;
	inode->u.ext2_i.i_dtime = raw_inode->i_dtime;
	inode->i_blksize = inode->i_sb->s_blocksize;
	inode->i_blocks = raw_inode->i_blocks;
	inode->u.ext2_i.i_flags = raw_inode->i_flags;
	inode->u.ext2_i.i_faddr = raw_inode->i_faddr;
	inode->u.ext2_i.i_frag = raw_inode->i_frag;
	inode->u.ext2_i.i_fsize = raw_inode->i_fsize;
	inode->u.ext2_i.i_file_acl = raw_inode->i_file_acl;
	inode->u.ext2_i.i_dir_acl = raw_inode->i_dir_acl;
	inode->u.ext2_i.i_version = raw_inode->i_version;
	inode->u.ext2_i.i_block_group = block_group;
	inode->u.ext2_i.i_next_alloc_block = 0;
	inode->u.ext2_i.i_next_alloc_goal = 0;
	if (inode->u.ext2_i.i_prealloc_count)
		ext2_error (inode->i_sb, "ext2_read_inode",
			    "New inode has non-zero prealloc count!");
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		inode->i_rdev = raw_inode->i_block[0];
	else for (block = 0; block < EXT2_N_BLOCKS; block++)
		inode->u.ext2_i.i_data[block] = raw_inode->i_block[block];
	brelse (bh);
	inode->i_op = NULL;
	/* 通过判断文件类型来设置文件的读写函数
	 */
	if (inode->i_ino == EXT2_ACL_IDX_INO ||
	    inode->i_ino == EXT2_ACL_DATA_INO)
		/* Nothing to do */ ;
	else if (S_ISREG(inode->i_mode))             /* 如果是普通文件 */
		inode->i_op = &ext2_file_inode_operations;
	else if (S_ISDIR(inode->i_mode))              /* 如果是目录文件 */
		inode->i_op = &ext2_dir_inode_operations;
	else if (S_ISLNK(inode->i_mode))            /* 如果是符号链接文件 */
		inode->i_op = &ext2_symlink_inode_operations;
	else if (S_ISCHR(inode->i_mode))             /* 字符文件 */
		inode->i_op = &chrdev_inode_operations;
	else if (S_ISBLK(inode->i_mode))             /* 设备文件 */
		inode->i_op = &blkdev_inode_operations;
	else if (S_ISFIFO(inode->i_mode))
		init_fifo(inode);
	if (inode->u.ext2_i.i_flags & EXT2_SYNC_FL)
		inode->i_flags |= MS_SYNC;
}

/* 将inode对应的文件写回高速缓存 */
static struct buffer_head * ext2_update_inode (struct inode * inode)
{
	struct buffer_head * bh;
	struct ext2_inode * raw_inode;
	unsigned long block_group;
	unsigned long group_desc;
	unsigned long desc;
	unsigned long block;
	struct ext2_group_desc * gdp;

	/* 判断ino的合法性 */
	if ((inode->i_ino != EXT2_ROOT_INO && inode->i_ino < EXT2_FIRST_INO) ||
	    inode->i_ino > inode->i_sb->u.ext2_sb.s_es->s_inodes_count) {
		ext2_error (inode->i_sb, "ext2_write_inode",
			    "bad inode number: %lu", inode->i_ino);
		return 0;
	}
	/* 获取inode所在的块组 */
	block_group = (inode->i_ino - 1) / EXT2_INODES_PER_GROUP(inode->i_sb);
	if (block_group >= inode->i_sb->u.ext2_sb.s_groups_count)
		ext2_panic (inode->i_sb, "ext2_write_inode",
			    "group >= groups count");
	/* 获取组描述符的块号 */
	group_desc = block_group / EXT2_DESC_PER_BLOCK(inode->i_sb);
	/* 获取组描述符在块中的偏移 */
	desc = block_group % EXT2_DESC_PER_BLOCK(inode->i_sb);
	bh = inode->i_sb->u.ext2_sb.s_group_desc[group_desc];
	if (!bh)
		ext2_panic (inode->i_sb, "ext2_write_inode",
			    "Descriptor not loaded");
	gdp = (struct ext2_group_desc *) bh->b_data;
	/* bg_inode_table为块组中存放inode的数据块的第一个块号 */
	block = gdp[desc].bg_inode_table +
		(((inode->i_ino - 1) % EXT2_INODES_PER_GROUP(inode->i_sb))
		 / EXT2_INODES_PER_BLOCK(inode->i_sb));
	if (!(bh = bread (inode->i_dev, block, inode->i_sb->s_blocksize)))
		ext2_panic (inode->i_sb, "ext2_write_inode",
			    "unable to read i-node block\n"
			    "inode=%lu, block=%lu", inode->i_ino, block);
	/* 获取高速缓存中i_ino对应的inode内存 */
	raw_inode = ((struct ext2_inode *)bh->b_data) +
		(inode->i_ino - 1) % EXT2_INODES_PER_BLOCK(inode->i_sb);
	raw_inode->i_mode = inode->i_mode;
	raw_inode->i_uid = inode->i_uid;
	raw_inode->i_gid = inode->i_gid;
	raw_inode->i_links_count = inode->i_nlink;
	raw_inode->i_size = inode->i_size;
	raw_inode->i_atime = inode->i_atime;
	raw_inode->i_ctime = inode->i_ctime;
	raw_inode->i_mtime = inode->i_mtime;
	raw_inode->i_blocks = inode->i_blocks;
	raw_inode->i_dtime = inode->u.ext2_i.i_dtime;
	raw_inode->i_flags = inode->u.ext2_i.i_flags;
	raw_inode->i_faddr = inode->u.ext2_i.i_faddr;
	raw_inode->i_frag = inode->u.ext2_i.i_frag;
	raw_inode->i_fsize = inode->u.ext2_i.i_fsize;
	raw_inode->i_file_acl = inode->u.ext2_i.i_file_acl;
	raw_inode->i_dir_acl = inode->u.ext2_i.i_dir_acl;
	raw_inode->i_version = inode->u.ext2_i.i_version;
	/* 这是文件的设备逻辑块号 */
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_block[0] = inode->i_rdev;
	else for (block = 0; block < EXT2_N_BLOCKS; block++)
		raw_inode->i_block[block] = inode->u.ext2_i.i_data[block];
	/* 写完之后inode就不是脏的了，
	 * 注意此时inode并没有写到磁盘上，仅仅是在高速缓存，
	 * 此时的高速缓存为脏，在释放高速缓存的时候就会写到磁盘上
	 */
	bh->b_dirt = 1;
	inode->i_dirt = 0;  
	return bh;
}

/* 仅仅是写到了高速缓冲，ext2_sync_inode函数才在这之后将内容写到了磁盘 */
void ext2_write_inode (struct inode * inode)
{
	struct buffer_head * bh;
	bh = ext2_update_inode (inode);
	brelse (bh);
}


/* 将inode数据先写入到高速缓存，然后将高速缓冲中数据写入到磁盘当中 */
int ext2_sync_inode (struct inode *inode)
{
	int err = 0;
	struct buffer_head *bh;

	bh = ext2_update_inode (inode);
	if (bh && bh->b_dirt)
	{
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
		if (bh->b_req && !bh->b_uptodate)
		{
			printk ("IO error syncing ext2 inode [%04x:%08lx]\n",
				inode->i_dev, inode->i_ino);
			err = -1;
		}
	}
	else if (!bh)
		err = -1;
	brelse (bh);
	return err;
}
