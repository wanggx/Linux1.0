/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Internet Control Message Protocol (ICMP)
 *
 * Version:	@(#)icmp.c	1.0.11	06/02/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *
 * Fixes:	
 *		Alan Cox	:	Generic queue usage.
 *		Gerhard Koerting:	ICMP addressing corrected
 *		Alan Cox	:	Use tos/ttl settings
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/fcntl.h>
#include <linux/socket.h>
#include <linux/in.h>
#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "route.h"
#include "protocol.h"
#include "icmp.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include <linux/errno.h>
#include <linux/timer.h>
#include <asm/system.h>
#include <asm/segment.h>


#define min(a,b)	((a)<(b)?(a):(b))


/* An array of errno for error messages from dest unreach. */
struct icmp_err icmp_err_convert[] = {
  { ENETUNREACH,	1 },	/*	ICMP_NET_UNREACH	*/
  { EHOSTUNREACH,	1 },	/*	ICMP_HOST_UNREACH	*/
  { ENOPROTOOPT,	1 },	/*	ICMP_PROT_UNREACH	*/
  { ECONNREFUSED,	1 },	/*	ICMP_PORT_UNREACH	*/
  { EOPNOTSUPP,		0 },	/*	ICMP_FRAG_NEEDED	*/
  { EOPNOTSUPP,		0 },	/*	ICMP_SR_FAILED		*/
  { ENETUNREACH,	1 },	/* 	ICMP_NET_UNKNOWN	*/
  { EHOSTDOWN,		1 },	/*	ICMP_HOST_UNKNOWN	*/
  { ENONET,		1 },	/*	ICMP_HOST_ISOLATED	*/
  { ENETUNREACH,	1 },	/*	ICMP_NET_ANO		*/
  { EHOSTUNREACH,	1 },	/*	ICMP_HOST_ANO		*/
  { EOPNOTSUPP,		0 },	/*	ICMP_NET_UNR_TOS	*/
  { EOPNOTSUPP,		0 }	/*	ICMP_HOST_UNR_TOS	*/
};


/* Display the contents of an ICMP header. */
static void
print_icmp(struct icmphdr *icmph)
{
  if (inet_debug != DBG_ICMP) return;

  printk("ICMP: type = %d, code = %d, checksum = %X\n",
			icmph->type, icmph->code, icmph->checksum);
  printk("      gateway = %s\n", in_ntoa(icmph->un.gateway));
}


/* Send an ICMP message. */
/* 该函数被调用发送一个ICMP错误通报数据包，ICMP错误通报数据包
 * 一般在接收到远端一个不合法的数据包产生的，用于通知远端发生错误的原因
 * 1〉 一个 ICMP 错误通报数据包。
 * 2〉 数据包远端地址是一个广播地址或者是一个多播地址。
 * 3〉 链路广播数据包（MAC 远端地址设置为全 1） 。
 * 4〉 分片数据包中非第一个数据包（换句话说，只对所有分片中第一个分片产生 ICMP 错误
 * 通报数据包，原因很简单，只有第一个分片才包含有传输层协议头部） 。
 * 5〉 数据包源端地址非单播地址，即全 0 地址，回环地址，多播或广播地址。
 * 满足以上 5 个条件中之一，即不可产生一个 ICMP 错误通报数据包。
 * icmp_send 函数实现代码虽然较长，但实现思想非常简单，首先判断引起错误的数据包是否
 * 满足以上 5 种条件之一，如果满足，则直接返回调用这，不产生 ICMP 错误通报数据包，否
 * 则更新本地 ICMP 统计信息后，构建一个 ICMP 错误通报数据包并直接调用 ip_queue_xmit
 * 函数发送个 IP 模块进行处理从而将这个 ICMP 错误通报数据包发送给引起该错误的原数据
 * 包的发送端。
 */
void
icmp_send(struct sk_buff *skb_in, int type, int code, struct device *dev)
{
  struct sk_buff *skb;
  struct iphdr *iph;
  int offset;
  struct icmphdr *icmph;
  int len;

  DPRINTF((DBG_ICMP, "icmp_send(skb_in = %X, type = %d, code = %d, dev=%X)\n",
	   					skb_in, type, code, dev));

  /* Get some memory for the reply. */
  len = sizeof(struct sk_buff) + dev->hard_header_len +
	sizeof(struct iphdr) + sizeof(struct icmphdr) +
	sizeof(struct iphdr) + 8;	/* amount of header to return */
	   
  skb = (struct sk_buff *) alloc_skb(len, GFP_ATOMIC);
  if (skb == NULL) 
  	return;

  skb->sk = NULL;
  skb->mem_addr = skb;
  skb->mem_len = len;
  len -= sizeof(struct sk_buff);

  /* Find the IP header. */
  iph = (struct iphdr *) (skb_in->data + dev->hard_header_len);

  /* Build Layer 2-3 headers for message back to source. */
  offset = ip_build_header(skb, dev->pa_addr, iph->saddr,
			   &dev, IPPROTO_ICMP, NULL, len, skb_in->ip_hdr->tos,255);
  if (offset < 0) {
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return;
  }

  /* Re-adjust length according to actual IP header size. */
  skb->len = offset + sizeof(struct icmphdr) + sizeof(struct iphdr) + 8;
  icmph = (struct icmphdr *) (skb->data + offset);
  icmph->type = type;
  icmph->code = code;
  icmph->checksum = 0;
  icmph->un.gateway = 0;
  memcpy(icmph + 1, iph, sizeof(struct iphdr) + 8);

  icmph->checksum = ip_compute_csum((unsigned char *)icmph,
                         sizeof(struct icmphdr) + sizeof(struct iphdr) + 8);

  DPRINTF((DBG_ICMP, ">>\n"));
  print_icmp(icmph);

  /* Send it and free it. */
  ip_queue_xmit(NULL, dev, skb, 1);
}


/* Handle ICMP_UNREACH and ICMP_QUENCH. */
/* icmp_unreach 函数处理的 ICMP 错误类型比较广，这点可以从下文中介绍的 icmp_rcv 总入
 * 口函数看出。此处我们就事论事，单方面进行该函数的分析。函数实现思想较为简单，对相
 * 关错误打印错误信息后，调用传输层协议错误处理函数进行处理（tcp_err, udp_err） 。注意
 * 242 行代码 err 变量被初始化为 ICMP 类型和代码值。最低 8bit 为代码（code）值，次低 8bit
 * 为类型（type）值。243 行代码初始化 iph 变量指向原数据包中 IP 首部以及传输层 8 字节数
 * 据，从而传输层错误处理函数可以由此获知上层对应应用进程。269 行代码计算传输层协议
 * 在 inet_protos 数组中的位置。inet_protos 一个元素类型为 inet_protocol 结构的数组，数组中
 * 每个元素对应一个传输层协议，协议在数组中的位置由协议编号索引，如 TCP 协议对应索
 * 引号为 6，UDP 为 17。274-287 行代码完成对传输层协议的遍历，调用协议对应错误处理函
 * 数进行返回错误的处理。 注意 inet_protos 数组中每个元素对应一个协议， 所以 274 行在一次
 * 循环后会退出，但这种编码方式提高了程序的可扩展性。 
 * Internet不可达错误处理在继续调用传输层的错误回调句柄，err_handler 
 */
static void
icmp_unreach(struct icmphdr *icmph, struct sk_buff *skb)
{
  struct inet_protocol *ipprot;
  struct iphdr *iph;
  unsigned char hash;
  int err;

  err = (icmph->type << 8) | icmph->code;
  iph = (struct iphdr *) (icmph + 1);
  switch(icmph->code & 7) {
	case ICMP_NET_UNREACH:
		DPRINTF((DBG_ICMP, "ICMP: %s: network unreachable.\n",
							in_ntoa(iph->daddr)));
		break;
	case ICMP_HOST_UNREACH:
		DPRINTF((DBG_ICMP, "ICMP: %s: host unreachable.\n",
						in_ntoa(iph->daddr)));
		break;
	case ICMP_PROT_UNREACH:
		printk("ICMP: %s:%d: protocol unreachable.\n",
			in_ntoa(iph->daddr), ntohs(iph->protocol));
		break;
	case ICMP_PORT_UNREACH:
		DPRINTF((DBG_ICMP, "ICMP: %s:%d: port unreachable.\n",
			in_ntoa(iph->daddr), -1 /* FIXME: ntohs(iph->port) */));
		break;
	case ICMP_FRAG_NEEDED:
		printk("ICMP: %s: fragmentation needed and DF set.\n",
							in_ntoa(iph->daddr));
		break;
	case ICMP_SR_FAILED:
		printk("ICMP: %s: Source Route Failed.\n", in_ntoa(iph->daddr));
		break;
	default:
		DPRINTF((DBG_ICMP, "ICMP: Unreachable: CODE=%d from %s\n",
		    		(icmph->code & 7), in_ntoa(iph->daddr)));
		break;
  }

  /* Get the protocol(s). */
  hash = iph->protocol & (MAX_INET_PROTOS -1);

  /* This can change while we are doing it. */
  ipprot = (struct inet_protocol *) inet_protos[hash];
  while(ipprot != NULL) {
	struct inet_protocol *nextip;

	nextip = (struct inet_protocol *) ipprot->next;

	/* Pass it off to everyone who wants it. */
	if (iph->protocol == ipprot->protocol && ipprot->err_handler) {
        /* 调用上层协议的错误处理函数 */
		ipprot->err_handler(err, (unsigned char *)(icmph + 1),
				    iph->daddr, iph->saddr, ipprot);
	}

	ipprot = nextip;
  }
  skb->sk = NULL;
  kfree_skb(skb, FREE_READ);
}


/* Handle ICMP_REDIRECT. */
/* 路由器为特定的目的主机发送重定向消息将
 * 主机重定向到一个更优的路由器或者告诉主机目的主机实际上是在同一链路上的邻居节点。 
 */
static void
icmp_redirect(struct icmphdr *icmph, struct sk_buff *skb, struct device *dev)
{
  struct iphdr *iph;
  unsigned long ip;

  iph = (struct iphdr *) (icmph + 1);
  ip = iph->daddr;
  switch(icmph->code & 7) {
	case ICMP_REDIR_NET:
#ifdef not_a_good_idea
		rt_add((RTF_DYNAMIC | RTF_MODIFIED | RTF_GATEWAY),
			ip, 0, icmph->un.gateway, dev);
		break;
#endif
	case ICMP_REDIR_HOST:
		rt_add((RTF_DYNAMIC | RTF_MODIFIED | RTF_HOST | RTF_GATEWAY),
			ip, 0, icmph->un.gateway, dev);
		break;
	case ICMP_REDIR_NETTOS:
	case ICMP_REDIR_HOSTTOS:
		printk("ICMP: cannot handle TOS redirects yet!\n");
		break;
	default:
		DPRINTF((DBG_ICMP, "ICMP: Unreach: CODE=%d\n",
						(icmph->code & 7)));
		break;
  }
  skb->sk = NULL;
  kfree_skb(skb, FREE_READ);
}


/* Handle ICMP_ECHO ("ping") requests. */
/* 本函数实现功能单一，即回复一个 Echo 应答数据包
 */
static void
icmp_echo(struct icmphdr *icmph, struct sk_buff *skb, struct device *dev,
	  unsigned long saddr, unsigned long daddr, int len,
	  struct options *opt)
{
  struct icmphdr *icmphr;
  struct sk_buff *skb2;
  int size, offset;

  size = sizeof(struct sk_buff) + dev->hard_header_len + 64 + len;
  skb2 = alloc_skb(size, GFP_ATOMIC);
  if (skb2 == NULL) {
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return;
  }
  skb2->sk = NULL;
  skb2->mem_addr = skb2;
  skb2->mem_len = size;
  skb2->free = 1;

  /* Build Layer 2-3 headers for message back to source */
  offset = ip_build_header(skb2, daddr, saddr, &dev,
			 	IPPROTO_ICMP, opt, len, skb->ip_hdr->tos,255);
  if (offset < 0) {
	printk("ICMP: Could not build IP Header for ICMP ECHO Response\n");
	kfree_skb(skb2,FREE_WRITE);
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return;
  }

  /* Re-adjust length according to actual IP header size. */
  skb2->len = offset + len;

  /* Build ICMP_ECHO Response message. */
  icmphr = (struct icmphdr *) (skb2->data + offset);
  memcpy((char *) icmphr, (char *) icmph, len);
  icmphr->type = ICMP_ECHOREPLY;
  icmphr->code = 0;
  icmphr->checksum = 0;
  icmphr->checksum = ip_compute_csum((unsigned char *)icmphr, len);

  /* Ship it out - free it when done */
  ip_queue_xmit((struct sock *)NULL, dev, skb2, 1);

  skb->sk = NULL;
  kfree_skb(skb, FREE_READ);
}


/* Handle the ICMP INFORMATION REQUEST. */
static void
icmp_info(struct icmphdr *icmph, struct sk_buff *skb, struct device *dev,
	  unsigned long saddr, unsigned long daddr, int len,
	  struct options *opt)
{
  /* NOT YET */
  skb->sk = NULL;
  kfree_skb(skb, FREE_READ);
}


/* Handle ICMP_ADRESS_MASK requests. */
static void
icmp_address(struct icmphdr *icmph, struct sk_buff *skb, struct device *dev,
	  unsigned long saddr, unsigned long daddr, int len,
	  struct options *opt)
{
  struct icmphdr *icmphr;
  struct sk_buff *skb2;
  int size, offset;

  size = sizeof(struct sk_buff) + dev->hard_header_len + 64 + len;
  skb2 = alloc_skb(size, GFP_ATOMIC);
  if (skb2 == NULL) {
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return;
  }
  skb2->sk = NULL;
  skb2->mem_addr = skb2;
  skb2->mem_len = size;
  skb2->free = 1;

  /* Build Layer 2-3 headers for message back to source */
  offset = ip_build_header(skb2, daddr, saddr, &dev,
			 	IPPROTO_ICMP, opt, len, skb->ip_hdr->tos,255);
  if (offset < 0) {
	printk("ICMP: Could not build IP Header for ICMP ADDRESS Response\n");
	kfree_skb(skb2,FREE_WRITE);
	skb->sk = NULL;
	kfree_skb(skb, FREE_READ);
	return;
  }

  /* Re-adjust length according to actual IP header size. */
  skb2->len = offset + len;

  /* Build ICMP ADDRESS MASK Response message. */
  icmphr = (struct icmphdr *) (skb2->data + offset);
  icmphr->type = ICMP_ADDRESSREPLY;
  icmphr->code = 0;
  icmphr->checksum = 0;
  icmphr->un.echo.id = icmph->un.echo.id;
  icmphr->un.echo.sequence = icmph->un.echo.sequence;
  memcpy((char *) (icmphr + 1), (char *) &dev->pa_mask, sizeof(dev->pa_mask));

  icmphr->checksum = ip_compute_csum((unsigned char *)icmphr, len);

  /* Ship it out - free it when done */
  ip_queue_xmit((struct sock *)NULL, dev, skb2, 1);

  skb->sk = NULL;
  kfree_skb(skb, FREE_READ);
}


/* Deal with incoming ICMP packets. */
/* icmp协议数据接收函数，当通过IP协议发送数据时，
  * 数据包发送到某个路由器或站点时，由于各种原因无法再继续 
  * 发送时，此时数据包所在的站点就会给数据包的源站点发送 
  * 数据包不可达信息，在该函数中会调用icmp_unreach函数  
  */
int
icmp_rcv(struct sk_buff *skb1, struct device *dev, struct options *opt,
	 unsigned long daddr, unsigned short len,
	 unsigned long saddr, int redo, struct inet_protocol *protocol)
{
  struct icmphdr *icmph;
  unsigned char *buff;

  /* Drop broadcast packets. */
  if (chk_addr(daddr) == IS_BROADCAST) {
	DPRINTF((DBG_ICMP, "ICMP: Discarded broadcast from %s\n",
							in_ntoa(saddr)));
	skb1->sk = NULL;
	kfree_skb(skb1, FREE_READ);
	return(0);
  }

  buff = skb1->h.raw;
  icmph = (struct icmphdr *) buff;

  /* Validate the packet first */
  if (ip_compute_csum((unsigned char *) icmph, len)) {
	/* Failed checksum! */
	printk("ICMP: failed checksum from %s!\n", in_ntoa(saddr));
	skb1->sk = NULL;
	kfree_skb(skb1, FREE_READ);
	return(0);
  }
  print_icmp(icmph);

  /* Parse the ICMP message */
  switch(icmph->type) {
	case ICMP_TIME_EXCEEDED:
	case ICMP_DEST_UNREACH:
	case ICMP_SOURCE_QUENCH:
		icmp_unreach(icmph, skb1);
		return(0);
	case ICMP_REDIRECT:
		icmp_redirect(icmph, skb1, dev);
		return(0);
	case ICMP_ECHO: 
		icmp_echo(icmph, skb1, dev, saddr, daddr, len, opt);
		return 0;
	case ICMP_ECHOREPLY:
		skb1->sk = NULL;
		kfree_skb(skb1, FREE_READ);
		return(0);
	case ICMP_INFO_REQUEST:
		icmp_info(icmph, skb1, dev, saddr, daddr, len, opt);
		return 0;
	case ICMP_INFO_REPLY:
		skb1->sk = NULL;
		kfree_skb(skb1, FREE_READ);
		return(0);
	case ICMP_ADDRESS:
		icmp_address(icmph, skb1, dev, saddr, daddr, len, opt);
		return 0;
	case ICMP_ADDRESSREPLY:
		skb1->sk = NULL;
		kfree_skb(skb1, FREE_READ);
		return(0);
	default:
		DPRINTF((DBG_ICMP,
			"ICMP: Unsupported ICMP from %s, type = 0x%X\n",
						in_ntoa(saddr), icmph->type));
		skb1->sk = NULL;
		kfree_skb(skb1, FREE_READ);
		return(0);
  }
  /*NOTREACHED*/
  skb1->sk = NULL;
  kfree_skb(skb1, FREE_READ);
  return(-1);
}


/* Perform any ICMP-related I/O control requests. */
int
icmp_ioctl(struct sock *sk, int cmd, unsigned long arg)
{
  switch(cmd) {
	case DDIOCSDBG:
		return(dbg_ioctl((void *) arg, DBG_ICMP));
	default:
		return(-EINVAL);
  }
  return(0);
}
