/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the TCP protocol.
 *
 * Version:	@(#)tcp.h	1.0.2	04/28/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _LINUX_TCP_H
#define _LINUX_TCP_H


#define HEADER_SIZE	64		/* maximum header size		*/


struct tcphdr {
  unsigned short	source;  /* 本地端口号 */
  unsigned short	dest;  /* 目的端口号 */
  unsigned long		seq;
  unsigned long		ack_seq;
  unsigned short	res1:4,
			doff:4,
			fin:1,
			syn:1,
			rst:1,
			psh:1,
			ack:1,
			urg:1,
			res2:2;
  unsigned short	window;
  unsigned short	check;
  unsigned short	urg_ptr;
};


/* http://www.2cto.com/net/201209/157585.html */

enum {
  TCP_ESTABLISHED = 1, /* 代表一个打开的连接 */
  TCP_SYN_SENT, /* 再发送连接请求后等待匹配的连接请求 */
  TCP_SYN_RECV, /* 再收到和发送一个连接请求后等待对方对连接请求的确认 */
#if 0
  TCP_CLOSING, /* not a valid state, just a seperator so we can use
		  < tcp_closing or > tcp_closing for checks. */
#endif
  TCP_FIN_WAIT1, /* 等待远程TCP连接中断请求，或先前的连接中断请求的确认 */
  TCP_FIN_WAIT2, /* 从远程TCP等待连接中断请求 */
  TCP_TIME_WAIT, /* 等待足够的时间以确保远程TCP接收到连接中断请求的确认 */
  TCP_CLOSE,	 /* 没有任何连接状态 */
  TCP_CLOSE_WAIT, /* 等待从本地用户发来的连接中断请求 */
  TCP_LAST_ACK,	 /* 等待原来的发向远程TCP的连接中断请求的确认 */
  TCP_LISTEN  /* 侦听来自远方的TCP端口的连接请求 */
};

#endif	/* _LINUX_TCP_H */
