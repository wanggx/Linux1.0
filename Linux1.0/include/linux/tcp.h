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
  unsigned long		seq; 	/* tcp字节的序列号，本报文段的第一个字节的序号 */
  unsigned long		ack_seq; /* 对远端确认的序列号 */
  unsigned short	res1:4,    /* 保留位 */
			doff:4,     /* 数据偏移，它指出tcp报文段的数据起始处到tcp报文段的起始处有多远，
						 * 因为tcp首部长度是不固定的，单位为4字节 
						 */
			fin:1,    /* 通知远方本端要关闭连接 */
			syn:1,	/* 连接请求标记 */
			rst:1,  /* 复位标志有效，用于复位相应的tcp连接 */
			psh:1,  /* 推标记该标记置位时，高速远端尽快将缓存中的数据读走，
                  * 接收端不将该数据进行队列处理，而是尽可能快将
					        * 数据转由应用处理，在处理telnet等交互模式的连接时，该标志是置位的
					        */
			ack:1,	/* 表示是确认包 */
			urg:1,    /* 发送紧急数据标记，如果该标记为1，则表示该数据包中有紧急数据 */
			res2:2;
  unsigned short	window;     /* 它指出了现在允许对方发送的数据量，接收方的缓冲空间是有限的
   								 * 故用窗口值作为接收方让发送方设置其发送窗口的依据，单位为字节
   								 */
  unsigned short	check;		/* 校验和 */
  unsigned short	urg_ptr;       /* 发送紧急数据的指针 */
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
