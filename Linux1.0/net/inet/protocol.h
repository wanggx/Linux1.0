/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the protocol dispatcher.
 *
 * Version:	@(#)protocol.h	1.0.2	05/07/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Changes:
 *		Alan Cox	:	Added a name field and a frag handler
 *					field for later.
 */
 
#ifndef _PROTOCOL_H
#define _PROTOCOL_H


#define MAX_INET_PROTOS	32		/* Must be a power of 2		*/


/* This is used to register protocols. */

/* 每个“上层协议”都由一个inet_protocol结构表示，IP协议模块完成其自身处理后，根据IP首
 * 部中上层协议字段值从inet_protos数组中查找对应inet_protocol结构，调用该结构中
 * handler函数指针字段指向的函数，如TCP协议handler指向tcp_rcv, UDP为udp_rcv, ICMP
 * 对应icmp_rcv, IGMP对应igmp_rcv。这些对上层函数的调用是在IP协议实现模块中ip_rcv
 * 函数中完成的。也就是IP层协议的上层协议结构
 */
struct inet_protocol {
  int			(*handler)(struct sk_buff *skb, struct device *dev,
				   struct options *opt, unsigned long daddr,
				   unsigned short len, unsigned long saddr,
				   int redo, struct inet_protocol *protocol);
  int			(*frag_handler)(struct sk_buff *skb, struct device *dev,
				   struct options *opt, unsigned long daddr,
				   unsigned short len, unsigned long saddr,
				   int redo, struct inet_protocol *protocol);
  void			(*err_handler)(int err, unsigned char *buff,
				       unsigned long daddr,
				       unsigned long saddr,
				       struct inet_protocol *protocol);
  /* 表示同一个协议的链表，先通过protocol来hash出来索引 */
  struct inet_protocol *next;  
  unsigned char		protocol;  /*不同协议的一个ID,通过枚举来赋值的*/
  unsigned char		copy:1;    /* 表示在next的链表中，是否还有后续的元素的protocol值和当前相同 */
  /* 在raw套接字协议当中，该data携带的是struct inet_protocol结构 */
  void			*data;
  char 			*name;	/* 协议名称 */
};


extern struct inet_protocol *inet_protocol_base;
extern struct inet_protocol *inet_protos[MAX_INET_PROTOS];


extern void		inet_add_protocol(struct inet_protocol *prot);
extern int		inet_del_protocol(struct inet_protocol *prot);


#endif	/* _PROTOCOL_H */
