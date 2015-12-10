/*
 *  linux/include/linux/ext2_fs_i.h
 *
 *  Copyright (C) 1992, 1993, 1994  Remy Card (card@masi.ibp.fr)
 *                                  Laboratoire MASI - Institut Blaise Pascal
 *                                  Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs_i.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifndef _LINUX_EXT2_FS_I
#define _LINUX_EXT2_FS_I

/*
 * second extended file system inode data in memory
 */
struct ext2_inode_info {
	/* ext2文件的数据块 */
	unsigned long  i_data[15];
	unsigned long  i_flags;
	unsigned long  i_faddr;
	unsigned char  i_frag;
	unsigned char  i_fsize;
	unsigned short i_pad1;
	unsigned long  i_file_acl;
	unsigned long  i_dir_acl;
	unsigned long  i_dtime;
	unsigned long  i_version;
	unsigned long  i_block_group;  /* 数据所在块组号 */
	unsigned long  i_next_alloc_block;  /* 下一个文件分配的逻辑块号 */
	unsigned long  i_next_alloc_goal; /* 下一个分配的设备逻辑块号 */
	unsigned long  i_prealloc_block; /*存放下一次要使用的预分配的逻辑块号 */
	unsigned long  i_prealloc_count; /*存放预分配给文件的还没有使用的数据块的数量 */
};

#endif	/* _LINUX_EXT2_FS_I */
