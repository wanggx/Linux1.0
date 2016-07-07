/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the IP protocol.
 *
 * Version:	@(#)ip.h	1.0.2	04/28/93
 *
 * Authors:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _LINUX_IP_H
#define _LINUX_IP_H


#define IPOPT_END	0
#define IPOPT_NOOP	1
#define IPOPT_SEC	130
#define IPOPT_LSRR	131
#define IPOPT_SSRR	137
#define IPOPT_RR	7
#define IPOPT_SID	136
#define IPOPT_TIMESTAMP	68


struct timestamp {
  unsigned char	len;
  unsigned char ptr;
  union {
	unsigned char	flags:4,
			overflow:4;
	unsigned char	full_char;
  } x;
  unsigned long	data[9];
};


#define MAX_ROUTE	16

struct route {
  char		route_size;
  char		pointer;
  unsigned long route[MAX_ROUTE];
};

/* ip协议头的选项数据 */
struct options {
  struct route		record_route;
  struct route		loose_route;
  struct route		strict_route;
  struct timestamp	tstamp;
  unsigned short	security;
  unsigned short	compartment;
  unsigned short	handling;
  unsigned short	stream;
  unsigned		tcc;
};


struct iphdr {
  /* 包头长度，单位是4个字节，IP包头最少长度为20个字节  */
  unsigned char		ihl:4,             
			version:4;              /* 协议版本 */
  unsigned char		tos;               /* 服务类型 */
  unsigned short	tot_len;        /* 包总共长度 */
  unsigned short	id;
  unsigned short	frag_off;       /* 分段偏移 */
  unsigned char		ttl;                /* 存活时间 */
  unsigned char		protocol;  /* 表明上层使用的协议 */
  unsigned short	check;     /* ip协议校验和 */
  unsigned long		saddr;
  unsigned long		daddr;
  /*The options start here. */   /* 选项数据紧挨着后面存放 */
};


#endif	/* _LINUX_IP_H */
