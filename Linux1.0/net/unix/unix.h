/*
 * UNIX		An implementation of the AF_UNIX network domain for the
 *		LINUX operating system.  UNIX is implemented using the
 *		BSD Socket interface as the means of communication with
 *		the user level.
 *
 *		This file descibes some things of the UNIX protocol family
 *		module.  It is mainly used for the "proc" sub-module now,
 *		but it may be useful for cleaning up the UNIX module as a
 *		whole later.
 *
 * Version:	@(#)unix.h	1.0.3	05/25/93
 *
 * Authors:	Orest Zborowski, <obz@Kodak.COM>
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Dmitry Gorodchanin	-	proc locking
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */


#ifdef _LINUX_UN_H

/* UNIX域协议数据 */
struct unix_proto_data {
        /* 初始化分配的时候该变量为-1 */
	int		refcnt;		/* cnt of reference 0=free	*/
					/* -1=not initialised	-bgm	*/
        /*  server对应的套接字 */
	struct socket	*socket;	/* socket we're bound to	*/
	int		protocol;
	struct sockaddr_un	sockaddr_un;        /* 绑定的地址 */
	short		sockaddr_len;	/* >0 if name bound		*/  /* 设定绑定地址长度 */
	char		*buf;   /* 一页的缓冲大小 */
        /* 标记缓存中数据的起始位置和结束位置，
          * 来确定还没处理的数据有多少，以及剩余空间的大小 
          */
	int		bp_head, bp_tail;     
	struct inode	*inode;                /* 连接对应的inode */
	struct unix_proto_data	*peerupd;  /* 连接成功后设置对等的协议数据 */
	struct wait_queue *wait;	/* Lock across page faults (FvK) */
	int		lock_flag;            /* 是否锁住标记 */
};

extern struct unix_proto_data unix_datas[NSOCKETS];


#define last_unix_data		(unix_datas + NSOCKETS - 1)


#define UN_DATA(SOCK) 		((struct unix_proto_data *)(SOCK)->data)
#define UN_PATH_OFFSET		((unsigned long)((struct sockaddr_un *)0) \
							->sun_path)

/*
 * Buffer size must be power of 2. buffer mgmt inspired by pipe code.
 * note that buffer contents can wraparound, and we can write one byte less
 * than full size to discern full vs empty.
 */
#define BUF_SIZE		PAGE_SIZE
#define UN_BUF_AVAIL(UPD)	(((UPD)->bp_head - (UPD)->bp_tail) & \
								(BUF_SIZE-1))
#define UN_BUF_SPACE(UPD)	((BUF_SIZE-1) - UN_BUF_AVAIL(UPD))

#endif	/* _LINUX_UN_H */


extern void	unix_proto_init(struct ddi_proto *pro);
