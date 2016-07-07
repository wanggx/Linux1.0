/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the 'struct sk_buff' memory handlers.
 *
 * Version:	@(#)skbuff.h	1.0.4	05/20/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *
 * Fixes:
 *		Alan Cox		: 	Volatiles (this makes me unhappy - we want proper asm linked list stuff)
 *		Alan Cox		:	Declaration for new primitives
 *		Alan Cox		:	Fraglist support (idea by Donald Becker)
 *		Alan Cox		:	'users' counter. Combines with datagram changes to avoid skb_peek_copy
 *						being used.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _SKBUFF_H
#define _SKBUFF_H
#include <linux/malloc.h>

#ifdef CONFIG_IPX
#include "ipx.h"
#endif

#define HAVE_ALLOC_SKB		/* For the drivers to know */


#define FREE_READ	1
#define FREE_WRITE	0

/* 套接字缓冲区结构 */
struct sk_buff {
  unsigned long			magic_debug_cookie;
  /* sk_buff是一个双向循环链表，但是同时这个双向链表也有一个头部，
    * 该头部就是用list来指向 
    */
  struct sk_buff		*volatile next;
  struct sk_buff		*volatile prev;
  /* 这个指针也是用于构成sk_buff的链表，只不过该链表是重发的链表
   * send_head执行链表的链首，send_tail指向尾部，该链表通过link3来连接
   */
  struct sk_buff		*volatile link3;
  struct sk_buff		*volatile* list;
  struct sock			*sk;     /* 指定这个sk_buff是那个sock的 */
  volatile unsigned long	when;	/* used to compute rtt's	*/   /* 表示数据的发送时间 */
  /* skb对应的设备，最终skb中的数据需要交给设备发送出去，也就是链路层 */
  struct device			*dev;         
  void				*mem_addr; /* 记录自己在内存中地址 */
  /* 不同协议的头部
    */
  union {
	struct tcphdr	*th;	/* tcp协议头部 */
	struct ethhdr	*eth;
	struct iphdr	*iph;
	struct udphdr	*uh;
	struct arphdr	*arp;   /* arp首部 */
	unsigned char	*raw;
	unsigned long	seq;
#ifdef CONFIG_IPX	
	ipx_packet	*ipx;
#endif	
  } h;
  /* IP协议的头部，在RAW的套接字中会用到 */
  struct iphdr		*ip_hdr;		/* For IPPROTO_RAW */
  unsigned long			mem_len;    /* sk_buff的内存长度 */
  unsigned long 		len;		/* 实际数据长度 */
  /* 分片数据包的个数 */
  unsigned long			fraglen;
  /* 分片数据包的链表 */
  struct sk_buff		*fraglist;	/* Fragment list */
  unsigned long			truesize;
  unsigned long 		saddr;
  unsigned long 		daddr;
  int				magic;
  volatile char 		acked, /* =1,表示该数据包已得到确认，可以从重发队列中删除 */
				used, /* =1,表示该数据包的数据已被程序读完，可以进行释放 */
				free, /* =1,用于数据包发送，当某个待发送数据包的free标记等于1，
				         * 则表示无论数据包是否发送成功，在进行发送操作后立即释放，无需缓存
				         */
				arp;  /* 用于待发送数据包，此字段等于1表示此待发送数据包已完成MAC首部
				         * 建立，arp=0表示mac首部中目的端硬件地址尚不知晓，故需使用arp协议询问对方，
				         * 在mac首部尚未完成建立之前，该数据包一直处于发送缓冲队列中
                              */
  unsigned char			tries,lock;	/* Lock is now unused */
						 /* 使用该数据包的模块数，使用struct sock的进程数量 */
  unsigned short		users;		/* User count - see datagram.c (and soon seqpacket.c/stream.c) */
  unsigned long			padding[0];  /* 填充字节，目前定义为0字节，无需填充 */
  /* 之后的内存是需要发送到网络的数据，data即是sk_buff的末尾
    * 也是数据的首部 
    */
  unsigned char			data[0];
};

#define SK_WMEM_MAX	8192
#define SK_RMEM_MAX	32767

#define SK_FREED_SKB	0x0DE2C0DE
#define SK_GOOD_SKB	0xDEC0DED1

extern void			print_skb(struct sk_buff *);
extern void			kfree_skb(struct sk_buff *skb, int rw);
extern void			skb_queue_head(struct sk_buff * volatile *list,struct sk_buff *buf);
extern void			skb_queue_tail(struct sk_buff * volatile *list,struct sk_buff *buf);
extern struct sk_buff *		skb_dequeue(struct sk_buff * volatile *list);
extern void 			skb_insert(struct sk_buff *old,struct sk_buff *newsk);
extern void			skb_append(struct sk_buff *old,struct sk_buff *newsk);
extern void			skb_unlink(struct sk_buff *buf);
extern void 			skb_new_list_head(struct sk_buff *volatile* list);
extern struct sk_buff *		skb_peek(struct sk_buff * volatile *list);
extern struct sk_buff *		skb_peek_copy(struct sk_buff * volatile *list);
extern struct sk_buff *		alloc_skb(unsigned int size, int priority);
extern void			kfree_skbmem(void *mem, unsigned size);
extern void			skb_kept_by_device(struct sk_buff *skb);
extern void			skb_device_release(struct sk_buff *skb, int mode);
extern int			skb_device_locked(struct sk_buff *skb);
extern void 			skb_check(struct sk_buff *skb,int, char *);
#define IS_SKB(skb)	skb_check((skb),__LINE__,__FILE__)

extern struct sk_buff *		skb_recv_datagram(struct sock *sk,unsigned flags,int noblock, int *err);
extern int			datagram_select(struct sock *sk, int sel_type, select_table *wait);
extern void			skb_copy_datagram(struct sk_buff *from, int offset, char *to,int size);
extern void			skb_free_datagram(struct sk_buff *skb);
#endif	/* _SKBUFF_H */
