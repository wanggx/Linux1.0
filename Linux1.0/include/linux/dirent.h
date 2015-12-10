#ifndef _LINUX_DIRENT_H
#define _LINUX_DIRENT_H

#include <linux/limits.h>


/* 在内存当中的目录结构，会和不同文件系统的目录结构进行转换 */
struct dirent {
	long		d_ino;
	off_t		d_off;
	unsigned short	d_reclen;
	char		d_name[NAME_MAX+1];
};

#endif
