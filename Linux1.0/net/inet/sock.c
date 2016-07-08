/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		SOCK - AF_INET protocol family socket handler.
 *
 * Version:	@(#)sock.c	1.0.17	06/02/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *
 * Fixes:
 *		Alan Cox	: 	Numerous verify_area() problems
 *		Alan Cox	:	Connecting on a connecting socket
 *					now returns an error for tcp.
 *		Alan Cox	:	sock->protocol is set correctly.
 *					and is not sometimes left as 0.
 *		Alan Cox	:	connect handles icmp errors on a
 *					connect properly. Unfortunately there
 *					is a restart syscall nasty there. I
 *					can't match BSD without hacking the C
 *					library. Ideas urgently sought!
 *		Alan Cox	:	Disallow bind() to addresses that are
 *					not ours - especially broadcast ones!!
 *		Alan Cox	:	Socket 1024 _IS_ ok for users. (fencepost)
 *		Alan Cox	:	sock_wfree/sock_rfree don't destroy sockets,
 *					instead they leave that for the DESTROY timer.
 *		Alan Cox	:	Clean up error flag in accept
 *		Alan Cox	:	TCP ack handling is buggy, the DESTROY timer
 *					was buggy. Put a remove_sock() in the handler
 *					for memory when we hit 0. Also altered the timer
 *					code. The ACK stuff can wait and needs major 
 *					TCP layer surgery.
 *		Alan Cox	:	Fixed TCP ack bug, removed remove sock
 *					and fixed timer/inet_bh race.
 *		Alan Cox	:	Added zapped flag for TCP
 *		Alan Cox	:	Move kfree_skb into skbuff.c and tidied up surplus code
 *		Alan Cox	:	for new sk_buff allocations wmalloc/rmalloc now call alloc_skb
 *		Alan Cox	:	kfree_s calls now are kfree_skbmem so we can track skb resources
 *		Alan Cox	:	Supports socket option broadcast now as does udp. Packet and raw need fixing.
 *		Alan Cox	:	Added RCVBUF,SNDBUF size setting. It suddenely occured to me how easy it was so...
 *		Rick Sladkey	:	Relaxed UDP rules for matching packets.
 *		C.E.Hawkins	:	IFF_PROMISC/SIOCGHWADDR support
 *	Pauline Middelink	:	Pidentd support
 *		Alan Cox	:	Fixed connect() taking signals I think.
 *		Alan Cox	:	SO_LINGER supported
 *		Alan Cox	:	Error reporting fixes
 *		Anonymous	:	inet_create tidied up (sk->reuse setting)
 *		Alan Cox	:	inet sockets don't set sk->type!
 *		Alan Cox	:	Split socket option code
 *		Alan Cox	:	Callbacks
 *		Alan Cox	:	Nagle flag for Charles & Johannes stuff
 *
 * To Fix:
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

#include <asm/segment.h>
#include <asm/system.h>

#include "inet.h"
#include "dev.h"
#include "ip.h"
#include "protocol.h"
#include "arp.h"
#include "route.h"
#include "tcp.h"
#include "udp.h"
#include "skbuff.h"
#include "sock.h"
#include "raw.h"
#include "icmp.h"


int inet_debug = DBG_OFF;		/* INET module debug flag	*/


#define min(a,b)	((a)<(b)?(a):(b))

extern struct proto packet_prot;


void
print_sk(struct sock *sk)
{
  if (!sk) {
	printk("  print_sk(NULL)\n");
	return;
  }
  printk("  wmem_alloc = %lu\n", sk->wmem_alloc);
  printk("  rmem_alloc = %lu\n", sk->rmem_alloc);
  printk("  send_head = %p\n", sk->send_head);
  printk("  state = %d\n",sk->state);
  printk("  wback = %p, rqueue = %p\n", sk->wback, sk->rqueue);
  printk("  wfront = %p\n", sk->wfront);
  printk("  daddr = %lX, saddr = %lX\n", sk->daddr,sk->saddr);
  printk("  num = %d", sk->num);
  printk(" next = %p\n", sk->next);
  printk("  write_seq = %ld, acked_seq = %ld, copied_seq = %ld\n",
	  sk->write_seq, sk->acked_seq, sk->copied_seq);
  printk("  rcv_ack_seq = %ld, window_seq = %ld, fin_seq = %ld\n",
	  sk->rcv_ack_seq, sk->window_seq, sk->fin_seq);
  printk("  prot = %p\n", sk->prot);
  printk("  pair = %p, back_log = %p\n", sk->pair,sk->back_log);
  printk("  inuse = %d , blog = %d\n", sk->inuse, sk->blog);
  printk("  dead = %d delay_acks=%d\n", sk->dead, sk->delay_acks);
  printk("  retransmits = %ld, timeout = %d\n", sk->retransmits, sk->timeout);
  printk("  cong_window = %d, packets_out = %d\n", sk->cong_window,
	  sk->packets_out);
  printk("  shutdown=%d\n", sk->shutdown);
}


void
print_skb(struct sk_buff *skb)
{
  if (!skb) {
	printk("  print_skb(NULL)\n");
	return;
  }
  printk("  prev = %p, next = %p\n", skb->prev, skb->next);
  printk("  sk = %p link3 = %p\n", skb->sk, skb->link3);
  printk("  mem_addr = %p, mem_len = %lu\n", skb->mem_addr, skb->mem_len);
  printk("  used = %d free = %d\n", skb->used,skb->free);
}


/* 用于检测一个端口号是否已被使用
 * prot表示传输层操作函数的一个结构
 * 每个传输层协议都有一个proto结构对应
 * 所有使用某种协议的套接字均被插入到sock_array数据中
 * 元素指向的sock结构链表当中
 */
static int sk_inuse(struct proto *prot, int num)
{
  struct sock *sk;
  /* 首先根据端口号hash出一个套接字 */
  for(sk = prot->sock_array[num & (SOCK_ARRAY_SIZE -1 )];
      sk != NULL;
      sk=sk->next) {
	  /* 如果在端口号对应的sock链中找到了该端口号的sock，
	    * 则表示该端口已被占用，从这个函数当中就可以知道，
	    * 对于不同协议使用相同端口号是没有问题的。
	    */
	if (sk->num == num) return(1);
  }
  /* 该端口没有占用 */
  return(0);
}

/* 获取一个新的空闲端口号 
 * prot表示所使用的协议，
 * base表示最小起始端口号
 * 在对应的struct proto当中都有一个SOCK_ARRAY,该数组中具有SOCK_ARRAY_SIZE=64个元素
 * 每个元素对应一个链表，所以在获取该协议的一个新的端口号时，总是寻找所有链表中长度最短的那个
 * 链表，然后从该链表中获取一个端口号
 */
unsigned short get_new_socknum(struct proto *prot, unsigned short base)
{
  static int start=0;

  /*
   * Used to cycle through the port numbers so the
   * chances of a confused connection drop.
   */
  int i, j;
  int best = 0;
  /* sock链表的最大长度 */
  int size = 32767; /* a big num. */
  struct sock *sk;

  /* 1024之下的端口号保留，或者必须使用特权才能使用 */
  if (base == 0) base = PROT_SOCK+1+(start % 1024);
  if (base <= PROT_SOCK) {
	base += PROT_SOCK+(start % 1024);
  }

  /* Now look through the entire array and try to find an empty ptr. */
  /* 从套接字中所有的hash链表中查找 */
  for(i=0; i < SOCK_ARRAY_SIZE; i++) {
	j = 0;
	sk = prot->sock_array[(i+base+1) &(SOCK_ARRAY_SIZE -1)];
	while(sk != NULL) {
		sk = sk->next;
		j++;
	}
	/* 该端口对应的链尚未使用，可以直接返回端口号 */
	if (j == 0) {
		start =(i+1+start )%1024;
		DPRINTF((DBG_INET, "get_new_socknum returning %d, start = %d\n",
							i + base + 1, start));
		return(i+base+1);
	}
	/* 否则取最小j值的表项，
	 * 此处的j代表的是端口号对应表项的长度，
	 * 也就是找出hash数组中对应链表长度最短的那个项
	 */
	if (j < size) {
		best = i;
		size = j;
	}
  }

  /* Now make sure the one we want is not in use. */
  while(sk_inuse(prot, base +best+1)) {
	best += SOCK_ARRAY_SIZE;
  }
  DPRINTF((DBG_INET, "get_new_socknum returning %d, start = %d\n",
						best + base + 1, start));
  return(best+base+1);
}


/* 因为每一个struct proto结构都有一个sock_array数组，
 * 该数组是一个hash数组，根据端口号来hash出sock在数组中的
 * 索引，然后数组中的每一项都是一个单链表，将sk添加到对应的
 * 链表的链首
 * num为端口号 
 */
void put_sock(unsigned short num, struct sock *sk)
{
  struct sock *sk1;
  struct sock *sk2;
  int mask;

  DPRINTF((DBG_INET, "put_sock(num = %d, sk = %X\n", num, sk));
  sk->num = num;
  sk->next = NULL;
  /* 获取在hash数组中的索引 */
  num = num &(SOCK_ARRAY_SIZE -1);

  /* We can't have an interupt re-enter here. */
  cli();
  /* 如果hash链表的首部为NULL，则直接赋值 */
  if (sk->prot->sock_array[num] == NULL) {
	sk->prot->sock_array[num] = sk;
	sti();
	return;
  }
  sti();
  /* 最多循环三次,这个 for 语句用于估计本地地址子网反掩码 */
  for(mask = 0xff000000; mask != 0xffffffff; mask = (mask >> 8) | mask) {
	if ((mask & sk->saddr) &&
	    (mask & sk->saddr) != (mask & 0xffffffff)) {
		mask = mask << 8;
		break;
	}
  }

  /* 此时mask的值可能是 0.0.0.0/255.0.0.0/255.255.0.0/255.255.255.0/255.255.255.255
    * 则对应的saddr为    (0-255).x.x.x/(0.0-255.255).x.x/(0.0.0-255.255.255).x/(0.0.0.0-255.255.255.255)
    */
  DPRINTF((DBG_INET, "mask = %X\n", mask));

  cli();
  /* 使用本地地址掩码进行地址排列
    * 
    */
  sk1 = sk->prot->sock_array[num];
  for(sk2 = sk1; sk2 != NULL; sk2=sk2->next) {
  	/* 如果if满足，则saddr为0.x.x.x/0.0.x.x/0.0.0.x */
	if (!(sk2->saddr & mask)) {
		if (sk2 == sk1) {
			sk->next = sk->prot->sock_array[num];
			sk->prot->sock_array[num] = sk;
			sti();
			return;
		}
		sk->next = sk2;
		sk1->next= sk;
		sti();
		return;
	}
	/* 让sk1指针跟着sk2指针移动 */
	sk1 = sk2;
  }

  /* Goes at the end. */
  /* 将sk添加到链表的最后，并设置next字段为NULL*/
  sk->next = NULL;
  sk1->next = sk;
  sti();
}

/* 移除一个指定sock结构,将struct sock从sock的队列中删除  */
static void remove_sock(struct sock *sk1)
{
  struct sock *sk2;

  DPRINTF((DBG_INET, "remove_sock(sk1=%X)\n", sk1));
  if (!sk1) {
	printk("sock.c: remove_sock: sk1 == NULL\n");
	return;
  }

  if (!sk1->prot) {
	printk("sock.c: remove_sock: sk1->prot == NULL\n");
	return;
  }

  /* We can't have this changing out from under us. */
  cli();
  /* 使用sock的端口号hash出sock在sock_array中的位置，
   * 并取得hash链表的链首
   */
  sk2 = sk1->prot->sock_array[sk1->num &(SOCK_ARRAY_SIZE -1)];
  /* 如果找到了，则sk1从链表首部删除，将下一个sock作为首部 */
  if (sk2 == sk1) {
	sk1->prot->sock_array[sk1->num &(SOCK_ARRAY_SIZE -1)] = sk1->next;
	sti();
	return;
  }

  /* 开始搜索hash链表 */	
  while(sk2 && sk2->next != sk1) {
	sk2 = sk2->next;
  }
  /* 如果找到了，则更改链表关系，也就是将sk1从单链表中删除 */	
  if (sk2) {
	sk2->next = sk1->next;
	sti();
	return;
  }
  sti();
  /* 没找到则不做任何处理 */
  if (sk1->num != 0) DPRINTF((DBG_INET, "remove_sock: sock not found.\n"));
}

/* 这里是真正意义上的销毁套接字了，
 * 在销毁之前需要消费sock的数据包，避免内存泄露
 */
void destroy_sock(struct sock *sk)
{
	struct sk_buff *skb;

  	DPRINTF((DBG_INET, "destroying socket %X\n", sk));
  	sk->inuse = 1;			/* just to be safe. */

  	/* Incase it's sleeping somewhere. */
	/* 对于消费的sock结构，其dead字段必须首先设置为1 */
  	if (!sk->dead) 
  		sk->write_space(sk);

  	remove_sock(sk);
  
  	/* Now we can no longer get new packets. */
	/* 将sock的timer从next_timer中删除 */
  	delete_timer(sk);

	/* 以便减少数据包的数量，此处的操作是释放数据包 */
	while ((skb = tcp_dequeue_partial(sk)) != NULL) 
  	{
  		IS_SKB(skb);
  		kfree_skb(skb, FREE_WRITE);
  	}

  /* Cleanup up the write buffer. */
  /* 将写缓存清空释放内存 */
  	for(skb = sk->wfront; skb != NULL; ) 
  	{
		struct sk_buff *skb2;

		skb2=(struct sk_buff *)skb->next;
		if (skb->magic != TCP_WRITE_QUEUE_MAGIC) {
			printk("sock.c:destroy_sock write queue with bad magic(%X)\n",
								skb->magic);
			break;
		}
		IS_SKB(skb);
		kfree_skb(skb, FREE_WRITE);
		skb = skb2;
  	}

  	sk->wfront = NULL;
  	sk->wback = NULL;

	/* 释放读sk_buff队列 */
  	if (sk->rqueue != NULL) 
  	{
	  	while((skb=skb_dequeue(&sk->rqueue))!=NULL)
	  	{
		/*
		 * This will take care of closing sockets that were
		 * listening and didn't accept everything.
		 */
			if (skb->sk != NULL && skb->sk != sk) 
			{
				IS_SKB(skb);
				skb->sk->dead = 1;
				skb->sk->prot->close(skb->sk, 0);
			}
			IS_SKB(skb);
			kfree_skb(skb, FREE_READ);
		}
  	}
  	sk->rqueue = NULL;

  /* Now we need to clean up the send head. */
  	for(skb = sk->send_head; skb != NULL; ) 
  	{
		struct sk_buff *skb2;

		/*
		 * We need to remove skb from the transmit queue,
		 * or maybe the arp queue.
		 */
		cli();
		/* see if it's in a transmit queue. */
		/* this can be simplified quite a bit.  Look */
		/* at tcp.c:tcp_ack to see how. */
		if (skb->next != NULL) 
		{
			IS_SKB(skb);
			skb_unlink(skb);
		}
		skb->dev = NULL;
		sti();
		skb2 = (struct sk_buff *)skb->link3;
		kfree_skb(skb, FREE_WRITE);
		skb = skb2;
  	}	
  	sk->send_head = NULL;

  	/* And now the backlog. */
  	if (sk->back_log != NULL) 
  	{
		/* this should never happen. */
		printk("cleaning back_log. \n");
		cli();
		skb = (struct sk_buff *)sk->back_log;
		do 
		{
			struct sk_buff *skb2;
	
			skb2 = (struct sk_buff *)skb->next;
			kfree_skb(skb, FREE_READ);
			skb = skb2;
		}
		while(skb != sk->back_log);
		sti();
	}
	sk->back_log = NULL;

  /* Now if it has a half accepted/ closed socket. */
	if (sk->pair) 
	{
		sk->pair->dead = 1;
		sk->pair->prot->close(sk->pair, 0);
		sk->pair = NULL;
  	}

  /*
   * Now if everything is gone we can free the socket
   * structure, otherwise we need to keep it around until
   * everything is gone.
   */
	  if (sk->rmem_alloc == 0 && sk->wmem_alloc == 0) 
	  {
		kfree_s((void *)sk,sizeof(*sk));
	  } 
	  else 
	  {
		/* this should never happen. */
		/* actually it can if an ack has just been sent. */
		DPRINTF((DBG_INET, "possible memory leak in socket = %X\n", sk));
		sk->destroy = 1;
		sk->ack_backlog = 0;
		sk->inuse = 0;
		reset_timer(sk, TIME_DESTROY, SOCK_DESTROY_TIME);
  	}
  	DPRINTF((DBG_INET, "leaving destroy_sock\n"));
}


static int
inet_fcntl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  switch(cmd) {
	case F_SETOWN:
		/*
		 * This is a little restrictive, but it's the only
		 * way to make sure that you can't send a sigurg to
		 * another process.
		 */
		if (!suser() && current->pgrp != -arg &&
				current->pid != arg) return(-EPERM);
		sk->proc = arg;
		return(0);
	case F_GETOWN:
		return(sk->proc);
	default:
		return(-EINVAL);
  }
}

/*
 *	Set socket options on an inet socket.
 */

/* level指定选项代码的类型
 * optname选项名称
 * optval选项值
 * optlen选项长度
 */
static int inet_setsockopt(struct socket *sock, int level, int optname,
		    char *optval, int optlen)
{
  	struct sock *sk = (struct sock *) sock->data;  
	if (level == SOL_SOCKET)
		return sock_setsockopt(sk,level,optname,optval,optlen);
	if (sk->prot->setsockopt==NULL)
		return(-EOPNOTSUPP);
	else
		return sk->prot->setsockopt(sk,level,optname,optval,optlen);
}


static int inet_getsockopt(struct socket *sock, int level, int optname,
		    char *optval, int *optlen)
{
  	struct sock *sk = (struct sock *) sock->data;  	
  	if (level == SOL_SOCKET) 
  		return sock_getsockopt(sk,level,optname,optval,optlen);
  	if(sk->prot->getsockopt==NULL)  	
  		return(-EOPNOTSUPP);
  	else
  		return sk->prot->getsockopt(sk,level,optname,optval,optlen);
}

/*
 *	This is meant for all protocols to use and covers goings on
 *	at the socket level. Everything here is generic.
 */



int sock_setsockopt(struct sock *sk, int level, int optname,
		char *optval, int optlen)
{
	int val;
	int err;
	struct linger ling;

  	if (optval == NULL) 
  		return(-EINVAL);

  	err=verify_area(VERIFY_READ, optval, sizeof(int));
  	if(err)
  		return err;
  	
  	val = get_fs_long((unsigned long *)optval);
  	switch(optname) 
  	{
		case SO_TYPE:
		case SO_ERROR:
		  	return(-ENOPROTOOPT);

		case SO_DEBUG:	
			sk->debug=val?1:0;
		case SO_DONTROUTE:	/* Still to be implemented */
			return(0);
		case SO_BROADCAST:
			sk->broadcast=val?1:0;
			return 0;
		case SO_SNDBUF:
			if(val>32767)
				val=32767;
			if(val<256)
				val=256;
			sk->sndbuf=val;
			return 0;
		case SO_LINGER:
			err=verify_area(VERIFY_READ,optval,sizeof(ling));
			if(err)
				return err;
			memcpy_fromfs(&ling,optval,sizeof(ling));
			if(ling.l_onoff==0)
				sk->linger=0;
			else
			{
				sk->lingertime=ling.l_linger;
				sk->linger=1;
			}
			return 0;
		case SO_RCVBUF:
			if(val>32767)
				val=32767;
			if(val<256)
				val=256;
			sk->rcvbuf=val;
			return(0);

		case SO_REUSEADDR:
			if (val) 
				sk->reuse = 1;
			else 
				sk->reuse = 0;
			return(0);

		case SO_KEEPALIVE:  /* 表示是否使用保活定时器 */
			if (val)
				sk->keepopen = 1;
			else 
				sk->keepopen = 0;
			return(0);

	 	case SO_OOBINLINE:
			if (val) 
				sk->urginline = 1;
			else 
				sk->urginline = 0;
			return(0);

	 	case SO_NO_CHECK:
			if (val) 
				sk->no_check = 1;
			else 
				sk->no_check = 0;
			return(0);

		 case SO_PRIORITY:
			if (val >= 0 && val < DEV_NUMBUFFS) 
			{
				sk->priority = val;
			} 
			else 
			{
				return(-EINVAL);
			}
			return(0);

		default:
		  	return(-ENOPROTOOPT);
  	}
}


int sock_getsockopt(struct sock *sk, int level, int optname,
		   char *optval, int *optlen)
{		
  	int val;
  	int err;
  	struct linger ling;

  	switch(optname) 
  	{
		case SO_DEBUG:		
			val = sk->debug;
			break;
		
		case SO_DONTROUTE:	/* One last option to implement */
			val = 0;
			break;
		
		case SO_BROADCAST:
			val= sk->broadcast;
			break;
		
		case SO_LINGER:	
			err=verify_area(VERIFY_WRITE,optval,sizeof(ling));
			if(err)
				return err;
			err=verify_area(VERIFY_WRITE,optlen,sizeof(int));
			if(err)
				return err;
			put_fs_long(sizeof(ling),(unsigned long *)optlen);
			ling.l_onoff=sk->linger;
			ling.l_linger=sk->lingertime;
			memcpy_tofs(optval,&ling,sizeof(ling));
			return 0;
		
		case SO_SNDBUF:
			val=sk->sndbuf;
			break;
		
		case SO_RCVBUF:
			val =sk->rcvbuf;
			break;

		case SO_REUSEADDR:
			val = sk->reuse;
			break;

		case SO_KEEPALIVE:
			val = sk->keepopen;
			break;

		case SO_TYPE:
			if (sk->prot == &tcp_prot) 
				val = SOCK_STREAM;
		  	else 
		  		val = SOCK_DGRAM;
			break;

		case SO_ERROR:
			val = sk->err;
			sk->err = 0;
			break;

		case SO_OOBINLINE:
			val = sk->urginline;
			break;
	
		case SO_NO_CHECK:
			val = sk->no_check;
			break;

		case SO_PRIORITY:
			val = sk->priority;
			break;

		default:
			return(-ENOPROTOOPT);
	}
	err=verify_area(VERIFY_WRITE, optlen, sizeof(int));
	if(err)
  		return err;
  	put_fs_long(sizeof(int),(unsigned long *) optlen);

  	err=verify_area(VERIFY_WRITE, optval, sizeof(int));
  	if(err)
  		return err;
  	put_fs_long(val,(unsigned long *)optval);

  	return(0);
}



/* 开始监听套接字，并没有什么特别操作，只是将sock的状态修改了 */
static int inet_listen(struct socket *sock, int backlog)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }

  /* We might as well re use these. */ 
  sk->max_ack_backlog = backlog;
  if (sk->state != TCP_LISTEN) {
	sk->ack_backlog = 0;
	sk->state = TCP_LISTEN;
  }
  return(0);
}

/*
 *	Default callbacks for user INET sockets. These just wake up
 *	the user owning the socket.
 */

/* 有数据可以读了，则唤醒操作该struct sock的进程 */
static void def_callback1(struct sock *sk)
{
	/* 如果struct sock没有被释放 */
	if(!sk->dead)
		wake_up_interruptible(sk->sleep);
}

static void def_callback2(struct sock *sk,int len)
{
	if(!sk->dead)
		wake_up_interruptible(sk->sleep);
}

/* 根据协议来创建传输层套接字 */
static int inet_create(struct socket *sock, int protocol)
{
  struct sock *sk;
  struct proto *prot;
  int err;

  sk = (struct sock *) kmalloc(sizeof(*sk), GFP_KERNEL);
  if (sk == NULL) 
  	return(-ENOMEM);
  /* 新创建的struct sock端口号都为0 */
  sk->num = 0;
  /* 0表示该sock已被使用 */
  sk->reuse = 0;
  /* 注意在此处会根据socket的类型来确定协议的类型，
    * 如下面如果是SOCK_STREAM类型的，则协议只能是IPPROTO_TCP
    * 否则返回出错
    */
  switch(sock->type) {
	case SOCK_STREAM:
	case SOCK_SEQPACKET:
		if (protocol && protocol != IPPROTO_TCP) {
			kfree_s((void *)sk, sizeof(*sk));
			return(-EPROTONOSUPPORT);
		}
		protocol = IPPROTO_TCP;
		sk->no_check = TCP_NO_CHECK;
		/* 注意这里是在创建套接字的函数当中，在创建套接字的时候，
		 * 如果需要使用tcp协议，则在具体的struct sock结构当中传输层协议的
		 * 函数操作集都指向tcp_prot，也就是同一个变量，所以在struct proto结构
		 * 当中存在一个SOCK_ARRAY数组，用来记录所有使用TCP协议的套接字
		 */
		prot = &tcp_prot;
		break;

	case SOCK_DGRAM:
		if (protocol && protocol != IPPROTO_UDP) {
			kfree_s((void *)sk, sizeof(*sk));
			return(-EPROTONOSUPPORT);
		}
                /* 默认的设置为udp的协议 */
		protocol = IPPROTO_UDP;
		sk->no_check = UDP_NO_CHECK;
		prot=&udp_prot;          /* 注意数据报的则是UDP协议 */
		break;
      
	case SOCK_RAW:
		if (!suser()) {
			kfree_s((void *)sk, sizeof(*sk));
			return(-EPERM);
		}
		if (!protocol) {
			kfree_s((void *)sk, sizeof(*sk));
			return(-EPROTONOSUPPORT);
		}
		prot = &raw_prot;          /* 原始套接字的则是raw_prot操作集 */
		sk->reuse = 1;
		sk->no_check = 0;	/*
					 * Doesn't matter no checksum is
					 * preformed anyway.
					 */
		sk->num = protocol;
		break;

	case SOCK_PACKET:
		if (!suser()) {
			kfree_s((void *)sk, sizeof(*sk));
			return(-EPERM);
		}
		if (!protocol) {
			kfree_s((void *)sk, sizeof(*sk));
			return(-EPROTONOSUPPORT);
		}
		prot = &packet_prot;
		sk->reuse = 1;
		sk->no_check = 0;	/* Doesn't matter no checksum is
					 * preformed anyway.
					 */
		sk->num = protocol;
		break;

	default:
		kfree_s((void *)sk, sizeof(*sk));
		return(-ESOCKTNOSUPPORT);
  }
  sk->socket = sock;
#ifdef CONFIG_TCP_NAGLE_OFF
  sk->nonagle = 1;
#else    
  sk->nonagle = 0;
#endif  
  /* 设置struct sock中的type和protocol字段，
    * 同时还有下面很多字段的初始化信息 
    */
  sk->type = sock->type;
  sk->protocol = protocol;
  sk->wmem_alloc = 0;
  sk->rmem_alloc = 0;
  /* 设置最大的接收和发送缓冲 */
  sk->sndbuf = SK_WMEM_MAX;
  sk->rcvbuf = SK_RMEM_MAX;
  sk->pair = NULL;
  sk->opt = NULL;
  /* 初始化应用程序下次要写的字节的序列号为0 */
  sk->write_seq = 0;
  /* 初始化本地希望从远端接收的到字节序号为0 */
  sk->acked_seq = 0;
  /* 初始化应用程序已经读取的字节数为0 */
  sk->copied_seq = 0;
  sk->fin_seq = 0;
  sk->urg_seq = 0;
  sk->urg_data = 0;
  sk->proc = 0;
  sk->rtt = TCP_WRITE_TIME << 3;
  sk->rto = TCP_WRITE_TIME;
  sk->mdev = 0;
  sk->backoff = 0;
  sk->packets_out = 0;
  sk->cong_window = 1; /* start with only sending one packet at a time. */
  sk->cong_count = 0;
  sk->ssthresh = 0;
  /* 最大窗口初始化为0 */
  sk->max_window = 0;
  sk->urginline = 0;
  sk->intr = 0;
  sk->linger = 0;
  sk->destroy = 0;

  sk->priority = 1;
  sk->shutdown = 0;
  sk->keepopen = 0;
  sk->zapped = 0;
  sk->done = 0;
  /* 初始化需要给远端确认，但还没有确认的包的数量为0 */
  sk->ack_backlog = 0;
  sk->window = 0;
  /* 初始化已接收的字节总数为0 */
  sk->bytes_rcv = 0;
  /* socket系统调用完成后，socket的状态为TCP_CLOSE */
  sk->state = TCP_CLOSE;  
  sk->dead = 0;
  sk->ack_timed = 0;
  sk->partial = NULL;
  /* 初始化用户设定的最大报文段长度为0 */
  sk->user_mss = 0;
  sk->debug = 0;

  /* this is how many unacked bytes we will accept for this socket.  */
  sk->max_unacked = 2048; /* needs to be at most 2 full packets. */

  /* how many packets we should send before forcing an ack. 
     if this is set to zero it is the same as sk->delay_acks = 0 */

  /* 在监听的时候会作为listen的参数给传递进来 */
  sk->max_ack_backlog = 0;
  sk->inuse = 0;
  sk->delay_acks = 0;
  sk->wback = NULL;
  sk->wfront = NULL;
  sk->rqueue = NULL;
  /* 初始化最大传输单元 */
  sk->mtu = 576;
  sk->prot = prot;
  /* 注意struct socket和struct sock中的等待队列是一个 */
  sk->sleep = sock->wait;

  /* 初始化本地和远端地址 */
  sk->daddr = 0;
  sk->saddr = my_addr();  /* 获取的地址为127.0.0.1 */
  sk->err = 0;
  sk->next = NULL;
  sk->pair = NULL;
  sk->send_tail = NULL;
  sk->send_head = NULL;
  sk->timeout = 0;
  sk->broadcast = 0;
  /* 设置struct sock中timer的数据和响应函数 */
  sk->timer.data = (unsigned long)sk;
  sk->timer.function = &net_timer;
  sk->back_log = NULL;
  sk->blog = 0;
  /* 指定套接字的协议数据，此时的套接字数据仅仅是被初始化 */
  sock->data =(void *) sk;
  sk->dummy_th.doff = sizeof(sk->dummy_th)/4;
  sk->dummy_th.res1=0;
  sk->dummy_th.res2=0;
  sk->dummy_th.urg_ptr = 0;
  sk->dummy_th.fin = 0;
  sk->dummy_th.syn = 0;
  sk->dummy_th.rst = 0;
  sk->dummy_th.psh = 0;
  sk->dummy_th.ack = 0;
  sk->dummy_th.urg = 0;
  sk->dummy_th.dest = 0;

  sk->ip_tos=0;
  sk->ip_ttl=64;
  	
  sk->state_change = def_callback1;
  sk->data_ready = def_callback2;
  sk->write_space = def_callback1;
  sk->error_report = def_callback1;

  /* 在创建套接字的时候，如果是RAW类型的，sock的num初始化为protocol
    * 就把套接字对应的sock加入到协议的,其他类型的则没有改操作
    * hash结构当中
    */
  if (sk->num) {
	/*
	 * It assumes that any protocol which allows
	 * the user to assign a number at socket
	 * creation time automatically
	 * shares.
	 */
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }

  /* 注意在这里对协议对应的套接字进行初始化 */
  if (sk->prot->init) {
	err = sk->prot->init(sk);
	if (err != 0) {
		destroy_sock(sk);
		return(err);
	}
  }
  return(0);
}

/* 复制一个socket结构，其实就是创建一个新的socket */
static int inet_dup(struct socket *newsock, struct socket *oldsock)
{
  return(inet_create(newsock,
		   ((struct sock *)(oldsock->data))->protocol));
}


/* The peer socket should always be NULL. */
static int
inet_release(struct socket *sock, struct socket *peer)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) return(0);

  DPRINTF((DBG_INET, "inet_release(sock = %X, peer = %X)\n", sock, peer));
  sk->state_change(sk);

  /* Start closing the connection.  This may take a while. */
  /*
   * If linger is set, we don't return until the close
   * is complete.  Other wise we return immediately. The
   * actually closing is done the same either way.
   */
  if (sk->linger == 0) {
	sk->prot->close(sk,0);
	sk->dead = 1;
  } else {
	DPRINTF((DBG_INET, "sk->linger set.\n"));
	sk->prot->close(sk, 0);
	cli();
	if (sk->lingertime)
		current->timeout = jiffies + HZ*sk->lingertime;
	while(sk->state != TCP_CLOSE && current->timeout>0) {
		interruptible_sleep_on(sk->sleep);
		if (current->signal & ~current->blocked) {
			break;
#if 0
			/* not working now - closes can't be restarted */
			sti();
			current->timeout=0;
			return(-ERESTARTSYS);
#endif
		}
	}
	current->timeout=0;
	sti();
	sk->dead = 1;
  }
  sk->inuse = 1;

  /* This will destroy it. */
  release_sock(sk);
  sock->data = NULL;
  DPRINTF((DBG_INET, "inet_release returning\n"));
  return(0);
}


/* this needs to be changed to dissallow
   the rebinding of sockets.   What error
   should it return? */

/* 开始真正的绑定 */
static int inet_bind(struct socket *sock, struct sockaddr *uaddr,
	       int addr_len)
{
  struct sockaddr_in addr;
  struct sock *sk, *sk2;
  unsigned short snum;
  int err;

  /* 获取协议数据 */
  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  /* check this error. */
  if (sk->state != TCP_CLOSE) return(-EIO);

  /* 在绑定之前sock的端口号必须为0 */
  if (sk->num != 0) return(-EINVAL);

  err=verify_area(VERIFY_READ, uaddr, addr_len);
  if(err)
  	return err;
  memcpy_fromfs(&addr, uaddr, min(sizeof(addr), addr_len));

  /* 获取绑定地址端口号 */
  snum = ntohs(addr.sin_port);
  DPRINTF((DBG_INET, "bind sk =%X to port = %d\n", sk, snum));
  sk = (struct sock *) sock->data;

  /*
   * We can't just leave the socket bound wherever it is, it might
   * be bound to a privileged port. However, since there seems to
   * be a bug here, we will leave it if the port is not privileged.
   */
  /* 如果没有指定端口号，则系统自定获取一个空闲端口号 */
  if (snum == 0) {
	snum = get_new_socknum(sk->prot, 0);
  }

  /* 只有超级用户才能使用0-1024的端口 */
  if (snum < PROT_SOCK && !suser()) return(-EACCES);

  /* 绑定的地址必须是本机的 */
  if (addr.sin_addr.s_addr!=0 && chk_addr(addr.sin_addr.s_addr)!=IS_MYADDR)
  	return(-EADDRNOTAVAIL);	/* Source address MUST be ours! */
  	
  if (chk_addr(addr.sin_addr.s_addr) || addr.sin_addr.s_addr == 0)
					sk->saddr = addr.sin_addr.s_addr;

  DPRINTF((DBG_INET, "sock_array[%d] = %X:\n", snum &(SOCK_ARRAY_SIZE -1),
	  		sk->prot->sock_array[snum &(SOCK_ARRAY_SIZE -1)]));

  /* Make sure we are allowed to bind here. */
  cli();
outside_loop:
  /* 依次扫描端口号对应的hash链表 */
  for(sk2 = sk->prot->sock_array[snum & (SOCK_ARRAY_SIZE -1)];
					sk2 != NULL; sk2 = sk2->next) {
#if 	1	/* should be below! */
	if (sk2->num != snum) continue;
/*	if (sk2->saddr != sk->saddr) continue; */
#endif
	if (sk2->dead) {
		destroy_sock(sk2);
		goto outside_loop;
	}
	if (!sk->reuse) {
		sti();
		return(-EADDRINUSE);
	}
	if (sk2->num != snum) continue;		/* more than one */
	if (sk2->saddr != sk->saddr) continue;	/* socket per slot ! -FB */
	/* 如果绑定的本地地址和本地端口已被使用，则绑定失败 */
	if (!sk2->reuse) {
		sti();
		return(-EADDRINUSE);
	}
  }
  sti();

  remove_sock(sk);
  /* 将sock和端口绑定，并添加到协议的数组链表当中 */
  put_sock(snum, sk);
  /* 设置本地端口号和远端端口号 */
  sk->dummy_th.source = ntohs(sk->num);
  /* 注意绑定套接字的远端地址为0，在accept时从绑定套接字里面新建的struct sock
    * 虽然端口和绑定套接字的端口相同，但是套接字的远端地址不同，注意函数put_sock
    * 和get_sock的参数的区别，get_sock是根据本地套接字和远端套接字来获取的。put_sock
    * 仅仅是本地套接字
    */
  sk->daddr = 0;
  sk->dummy_th.dest = 0;
  return(0);
}


static int
inet_connect(struct socket *sock, struct sockaddr * uaddr,
		  int addr_len, int flags)
{
  struct sock *sk;
  int err;

  sock->conn = NULL;
  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  if (sock->state == SS_CONNECTING && sk->state == TCP_ESTABLISHED)
  {
	sock->state = SS_CONNECTED;
  /* Connection completing after a connect/EINPROGRESS/select/connect */
	return 0;	/* Rock and roll */
  }

  if (sock->state == SS_CONNECTING && sk->protocol == IPPROTO_TCP &&
  	(flags & O_NONBLOCK))
  	return -EALREADY;	/* Connecting is currently in progress */

  /* 刚调用socket系统调用时struct sock的state为TCP_CLOSE,
    * struct socket的状态为SS_UNCONNECTED
    */
  if (sock->state != SS_CONNECTING) {
	/* We may need to bind the socket. */
	if (sk->num == 0) {
		sk->num = get_new_socknum(sk->prot, 0);
		if (sk->num == 0) 
			return(-EAGAIN);
		put_sock(sk->num, sk);
		sk->dummy_th.source = htons(sk->num);
	}

	if (sk->prot->connect == NULL) 
		return(-EOPNOTSUPP);
  
	err = sk->prot->connect(sk, (struct sockaddr_in *)uaddr, addr_len);
	if (err < 0) return(err);

    /* 设置状态为正在连接 */
	sock->state = SS_CONNECTING;
  }

  if (sk->state != TCP_ESTABLISHED &&(flags & O_NONBLOCK)) 
  	return(-EINPROGRESS);

  cli(); /* avoid the race condition */
  while(sk->state == TCP_SYN_SENT || sk->state == TCP_SYN_RECV) 
  {
	interruptible_sleep_on(sk->sleep);
	if (current->signal & ~current->blocked) {
		sti();
		return(-ERESTARTSYS);
	}
	/* This fixes a nasty in the tcp/ip code. There is a hideous hassle with
	   icmp error packets wanting to close a tcp or udp socket. */
	if(sk->err && sk->protocol == IPPROTO_TCP)
	{
		sti();
		sock->state = SS_UNCONNECTED;
		err = -sk->err;
		sk->err=0;
		return err; /* set by tcp_err() */
	}
  }
  sti();
  sock->state = SS_CONNECTED;

  if (sk->state != TCP_ESTABLISHED && sk->err) {
	sock->state = SS_UNCONNECTED;
	err=sk->err;
	sk->err=0;
	return(-err);
  }
  return(0);
}


static int
inet_socketpair(struct socket *sock1, struct socket *sock2)
{
  return(-EOPNOTSUPP);
}

/* sock监听的套接字
 * newsock是accept中分配的的新的套接字
 * 返回0表示成功
 */
static int inet_accept(struct socket *sock, struct socket *newsock, int flags)
{
  struct sock *sk1, *sk2;
  int err;

  sk1 = (struct sock *) sock->data;
  if (sk1 == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  /*
   * We've been passed an extra socket.
   * We need to free it up because the tcp module creates
   * it's own when it accepts one.
   */

  /* 释放新套接字的协议数据 */
  if (newsock->data) kfree_s(newsock->data, sizeof(struct sock));
  newsock->data = NULL;

  if (sk1->prot->accept == NULL) return(-EOPNOTSUPP);

  /* Restore the state if we have been interrupted, and then returned. */
  if (sk1->pair != NULL ) {
	sk2 = sk1->pair;
	sk1->pair = NULL;
  } else {
	sk2 = sk1->prot->accept(sk1,flags);
	if (sk2 == NULL) {
		if (sk1->err <= 0)
			printk("Warning sock.c:sk1->err <= 0.  Returning non-error.\n");
		err=sk1->err;
		sk1->err=0;
		return(-err);
	}
  }
  /* 设置新socket的协议数据,sk2是从监听socket的接收队列中获取的 */
  newsock->data = (void *)sk2;
  sk2->sleep = newsock->wait;
  newsock->conn = NULL;
  if (flags & O_NONBLOCK) return(0);

  cli(); /* avoid the race. */
  while(sk2->state == TCP_SYN_RECV) {
	interruptible_sleep_on(sk2->sleep);
	if (current->signal & ~current->blocked) {
		sti();
		sk1->pair = sk2;
		sk2->sleep = NULL;
		newsock->data = NULL;
		return(-ERESTARTSYS);
	}
  }
  sti();

  if (sk2->state != TCP_ESTABLISHED && sk2->err > 0) {

	err = -sk2->err;
	sk2->err=0;
	destroy_sock(sk2);
	newsock->data = NULL;
	return(err);
  }
  /* 设置接收的newsock的状态为链接状态 */
  newsock->state = SS_CONNECTED;
  return(0);
}


/* 获取socket的信息
 * peer表示是获取本地信息还是远端信息
 */
static int
inet_getname(struct socket *sock, struct sockaddr *uaddr,
		 int *uaddr_len, int peer)
{
  struct sockaddr_in sin;
  struct sock *sk;
  int len;
  int err;
  
  
  err = verify_area(VERIFY_WRITE,uaddr_len,sizeof(long));
  if(err)
  	return err;
  	
  len=get_fs_long(uaddr_len);
  
  err = verify_area(VERIFY_WRITE, uaddr, len);
  if(err)
  	return err;
  	
  /* Check this error. */
  if (len < sizeof(sin)) return(-EINVAL);

  sin.sin_family = AF_INET;
  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }
  /* 获取远端信息，否则就是本地信息 */
  if (peer) {
	if (!tcp_connected(sk->state)) return(-ENOTCONN);
	sin.sin_port = sk->dummy_th.dest;
	sin.sin_addr.s_addr = sk->daddr;
  } else {
	sin.sin_port = sk->dummy_th.source;
	if (sk->saddr == 0) sin.sin_addr.s_addr = my_addr();
	  else sin.sin_addr.s_addr = sk->saddr;
  }
  len = sizeof(sin);
/*  verify_area(VERIFY_WRITE, uaddr, len); NOW DONE ABOVE */
  memcpy_tofs(uaddr, &sin, sizeof(sin));
/*  verify_area(VERIFY_WRITE, uaddr_len, sizeof(len)); NOW DONE ABOVE */
  put_fs_long(len, uaddr_len);
  return(0);
}


/* 从socket中读取数据 
 * noblock表示读取操作是否是阻塞的
 */
static int inet_read(struct socket *sock, char *ubuf, int size, int noblock)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }
  return(sk->prot->read(sk, (unsigned char *) ubuf, size, noblock,0));
}

/* 几乎和inet_read函数功能一样 */
static int inet_recv(struct socket *sock, void *ubuf, int size, int noblock,
	  unsigned flags)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }
  return(sk->prot->read(sk, (unsigned char *) ubuf, size, noblock, flags));
}


/* 向socket文件中写入数据 */
static int inet_write(struct socket *sock, char *ubuf, int size, int noblock)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }
  if (sk->shutdown & SEND_SHUTDOWN) {
	send_sig(SIGPIPE, current, 1);
	return(-EPIPE);
  }

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }

  return(sk->prot->write(sk, (unsigned char *) ubuf, size, noblock, 0));
}


/* 和inet_write功能一样 */
static int inet_send(struct socket *sock, void *ubuf, int size, int noblock, 
	       unsigned flags)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }
  if (sk->shutdown & SEND_SHUTDOWN) {
	send_sig(SIGPIPE, current, 1);
	return(-EPIPE);
  }

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }

  return(sk->prot->write(sk, (unsigned char *) ubuf, size, noblock, flags));
}


static int
inet_sendto(struct socket *sock, void *ubuf, int size, int noblock, 
	    unsigned flags, struct sockaddr *sin, int addr_len)
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }
  if (sk->shutdown & SEND_SHUTDOWN) {
	send_sig(SIGPIPE, current, 1);
	return(-EPIPE);
  }

  if (sk->prot->sendto == NULL) return(-EOPNOTSUPP);

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }

  return(sk->prot->sendto(sk, (unsigned char *) ubuf, size, noblock, flags, 
			   (struct sockaddr_in *)sin, addr_len));
}


static int
inet_recvfrom(struct socket *sock, void *ubuf, int size, int noblock, 
		   unsigned flags, struct sockaddr *sin, int *addr_len )
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  if (sk->prot->recvfrom == NULL) return(-EOPNOTSUPP);

  /* We may need to bind the socket. */
  if (sk->num == 0) {
	sk->num = get_new_socknum(sk->prot, 0);
	if (sk->num == 0) return(-EAGAIN);
	put_sock(sk->num, sk);
	sk->dummy_th.source = ntohs(sk->num);
  }

  return(sk->prot->recvfrom(sk, (unsigned char *) ubuf, size, noblock, flags,
			     (struct sockaddr_in*)sin, addr_len));
}


/* 关闭套接字 */
static int inet_shutdown(struct socket *sock, int how)
{
  struct sock *sk;

  /*
   * This should really check to make sure
   * the socket is a TCP socket.
   */
  how++; /* maps 0->1 has the advantage of making bit 1 rcvs and
		       1->2 bit 2 snds.
		       2->3 */
  if (how & ~SHUTDOWN_MASK) return(-EINVAL);
  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }
  if (sock->state == SS_CONNECTING && sk->state == TCP_ESTABLISHED)
						sock->state = SS_CONNECTED;

  if (!tcp_connected(sk->state)) return(-ENOTCONN);
  sk->shutdown |= how;
  if (sk->prot->shutdown) sk->prot->shutdown(sk, how);
  return(0);
}


static int
inet_select(struct socket *sock, int sel_type, select_table *wait )
{
  struct sock *sk;

  sk = (struct sock *) sock->data;
  if (sk == NULL) {
	printk("Warning: sock->data = NULL: %d\n" ,__LINE__);
	return(0);
  }

  if (sk->prot->select == NULL) {
	DPRINTF((DBG_INET, "select on non-selectable socket.\n"));
	return(0);
  }
  return(sk->prot->select(sk, sel_type, wait));
}


/*  INET协议族的端口控制函数 */
static int
inet_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
  struct sock *sk;
  int err;

  DPRINTF((DBG_INET, "INET: in inet_ioctl\n"));
  sk = NULL;
  if (sock && (sk = (struct sock *) sock->data) == NULL) {
	printk("AF_INET: Warning: sock->data = NULL: %d\n" , __LINE__);
	return(0);
  }

  switch(cmd) {
	case FIOSETOWN:
	case SIOCSPGRP:
		err=verify_area(VERIFY_READ,(int *)arg,sizeof(long));
		if(err)
			return err;
		if (sk)
			sk->proc = get_fs_long((int *) arg);
		return(0);
	case FIOGETOWN:
	case SIOCGPGRP:
		if (sk) {
			err=verify_area(VERIFY_WRITE,(void *) arg, sizeof(long));
			if(err)
				return err;
			put_fs_long(sk->proc,(int *)arg);
		}
		return(0);
#if 0	/* FIXME: */
	case SIOCATMARK:
		printk("AF_INET: ioctl(SIOCATMARK, 0x%08X)\n",(void *) arg);
		return(-EINVAL);
#endif

	case DDIOCSDBG:
		return(dbg_ioctl((void *) arg, DBG_INET));

	case SIOCADDRT: case SIOCADDRTOLD:
	case SIOCDELRT: case SIOCDELRTOLD:
		return(rt_ioctl(cmd,(void *) arg));

	case SIOCDARP:
	case SIOCGARP:
	case SIOCSARP:
		return(arp_ioctl(cmd,(void *) arg));

	case IP_SET_DEV:
	case SIOCGIFCONF:
	case SIOCGIFFLAGS:
	case SIOCSIFFLAGS:
	case SIOCGIFADDR:
	case SIOCSIFADDR:
	case SIOCGIFDSTADDR:
	case SIOCSIFDSTADDR:
	case SIOCGIFBRDADDR:
	case SIOCSIFBRDADDR:
	case SIOCGIFNETMASK:
	case SIOCSIFNETMASK:
	case SIOCGIFMETRIC:
	case SIOCSIFMETRIC:
	case SIOCGIFMEM:
	case SIOCSIFMEM:
	case SIOCGIFMTU:
	case SIOCSIFMTU:
	case SIOCSIFLINK:
	case SIOCGIFHWADDR:
		return(dev_ioctl(cmd,(void *) arg));

	default:
		if (!sk || !sk->prot->ioctl) return(-EINVAL);
		return(sk->prot->ioctl(sk, cmd, arg));
  }
  /*NOTREACHED*/
  return(0);
}

/* 可以通过force=1来设置强制打破这个sndbuf这个限制
 * 分配成功后，增加sk->wmem_alloc数量
 */
struct sk_buff *sock_wmalloc(struct sock *sk, unsigned long size, int force,
	     int priority)
{
  if (sk) {
	if (sk->wmem_alloc + size < sk->sndbuf || force) {
		struct sk_buff * c = alloc_skb(size, priority);
		if (c) {
			cli();
			sk->wmem_alloc+= size;
			sti();
		}
		return c;
	}
	DPRINTF((DBG_INET, "sock_wmalloc(%X,%d,%d,%d) returning NULL\n",
						sk, size, force, priority));
	return(NULL);
  }
  return(alloc_skb(size, priority));
}

/* 用于分配接收缓冲区，内存分配基本
  * 相同，只不过此时更新的是接收缓冲区
  * 该函数功能和sock_wmalloc差不多
  */
struct sk_buff *sock_rmalloc(struct sock *sk, unsigned long size, int force, int priority)
{
  if (sk) {
	if (sk->rmem_alloc + size < sk->rcvbuf || force) {
		struct sk_buff *c = alloc_skb(size, priority);
		if (c) {
			cli();
			sk->rmem_alloc += size;
			sti();
		}
		return(c);
	}
	DPRINTF((DBG_INET, "sock_rmalloc(%X,%d,%d,%d) returning NULL\n",
						sk,size,force, priority));
	return(NULL);
  }
  return(alloc_skb(size, priority));
}

/* sock_rspace 函数用于检查接收缓冲区空闲空间大小
  */
unsigned long sock_rspace(struct sock *sk)
{
  int amt;

  if (sk != NULL) {
	if (sk->rmem_alloc >= sk->rcvbuf-2*MIN_WINDOW) return(0);
	amt = min((sk->rcvbuf-sk->rmem_alloc)/2-MIN_WINDOW, MAX_WINDOW);
	if (amt < 0) return(0);
	return(amt);
  }
  return(0);
}

/* 获取发送缓冲区空闲空间的大小*/
unsigned long sock_wspace(struct sock *sk)
{
  if (sk != NULL) {
	if (sk->shutdown & SEND_SHUTDOWN) return(0);
	if (sk->wmem_alloc >= sk->sndbuf) return(0);
	return(sk->sndbuf-sk->wmem_alloc );
  }
  return(0);
}

/* 释放sk_buff 同时减少sock中sk_buff占用内存大小
 */
void sock_wfree(struct sock *sk, void *mem, unsigned long size)
{
  DPRINTF((DBG_INET, "sock_wfree(sk=%X, mem=%X, size=%d)\n", sk, mem, size));

  IS_SKB(mem);
  /* 释放mem处size大小的内存 */
  kfree_skbmem(mem, size);
  if (sk) {
  	/* 减少sock中sk_buff大小 */
	sk->wmem_alloc -= size;

	/* In case it might be waiting for more memory. */
	if (!sk->dead) sk->write_space(sk);
	if (sk->destroy && sk->wmem_alloc == 0 && sk->rmem_alloc == 0) {
		DPRINTF((DBG_INET,
			"recovered lost memory, sock = %X\n", sk));
	}
	return;
  }
}

void sock_rfree(struct sock *sk, void *mem, unsigned long size)
{
  DPRINTF((DBG_INET, "sock_rfree(sk=%X, mem=%X, size=%d)\n", sk, mem, size));
  IS_SKB(mem);
  kfree_skbmem(mem, size);
  if (sk) {
	sk->rmem_alloc -= size;
	if (sk->destroy && sk->wmem_alloc == 0 && sk->rmem_alloc == 0) {
		DPRINTF((DBG_INET,
			"recovered lot memory, sock = %X\n", sk));
	}
  }
}


/*
 * This routine must find a socket given a TCP or UDP header.
 * Everyhting is assumed to be in net order.
 */

/* 获取协议上操作某个端口的sock，其中限制条件是
 * 本地地址和本地端口，远程地址和远程端口
 * 该函数和put_sock功能相反，为什么要按照这个条件来获取struct sock 
 * 例如当客户端和服务器之间有一个保活的连接，在保活的时间间隔当中 
 * 服务器重启，当保活的探测包到来时，此时就找不到对应的struct sock结构 
 * 就会给客户端发送一个reset命令，告诉客户端重新连接 
 */
struct sock *get_sock(struct proto *prot, unsigned short num,
				unsigned long raddr,
				unsigned short rnum, unsigned long laddr)
{
  struct sock *s;
  unsigned short hnum;

  hnum = ntohs(num);
  DPRINTF((DBG_INET, "get_sock(prot=%X, num=%d, raddr=%X, rnum=%d, laddr=%X)\n",
	  prot, num, raddr, rnum, laddr));

  /*
   * SOCK_ARRAY_SIZE must be a power of two.  This will work better
   * than a prime unless 3 or more sockets end up using the same
   * array entry.  This should not be a problem because most
   * well known sockets don't overlap that much, and for
   * the other ones, we can just be careful about picking our
   * socket number when we choose an arbitrary one.
   */
  for(s = prot->sock_array[hnum & (SOCK_ARRAY_SIZE - 1)];
      s != NULL; s = s->next) 
  {
    /* 判断本地端口 */
	if (s->num != hnum) 
		continue;
	if(s->dead && (s->state == TCP_CLOSE))
		continue;
	if(prot == &udp_prot)
		return s;
	/* 判断远程地址 */
	if(ip_addr_match(s->daddr,raddr)==0)
		continue;
	/* 判断远程端口 */
	if (s->dummy_th.dest != rnum && s->dummy_th.dest != 0) 
		continue;
	/* 判断本地地址 */
	if(ip_addr_match(s->saddr,laddr) == 0)
		continue;
	return(s);
  }
  return(NULL);
}


void release_sock(struct sock *sk)
{
  if (!sk) {
	printk("sock.c: release_sock sk == NULL\n");
	return;
  }
  if (!sk->prot) {
/*	printk("sock.c: release_sock sk->prot == NULL\n"); */
	return;
  }

  /* 这个blog非常重要，在下面的while循环中会调用到tcp_rcv,
    * 在该函数中也可能调用到release_sock，如果是同一个sock
    * 就不会造成函数调用的死循环 
    */
  if (sk->blog) return;

  /* See if we have any packets built up. */
  cli();
  sk->inuse = 1;
  /* 如果接收数据包缓存队列不为NULL,网络层模块在将一个数据包传递给传输层
    * 模块处理时(调用tcp_rcv),如果当前对应的套接字正忙，则将数据包插入到sock结构的
    * back_log队列当中，但是插入到该队列中的数据包并不算是被接收，该队列中的数据包
    * 需要进行一系列处理后插入rqueue接收队列中时，方才算是完成接收。release_sock
    * 函数就是从back_log中取数据包重新调用tcp_rcv函数对数据包进行接收，所谓的
    * back_log队列只是数据包暂居之所，不可久留，所以也就必须对这个队列中数据包尽快进行
    * 处理
    */
  while(sk->back_log != NULL) {
	struct sk_buff *skb;
	/* 表示之后的数据包都要被丢弃，这里应该不是丢弃，而是不处理 */
	sk->blog = 1;
	skb =(struct sk_buff *)sk->back_log;
	DPRINTF((DBG_INET, "release_sock: skb = %X:\n", skb));
	/* 如果以back_log为首的链表不止一个节点，
	 * 则将头部节点删除,因为在while循环里面，
	 * 最终的结果就是整个back_log被删除
	 */
	if (skb->next != skb) {
		sk->back_log = skb->next;
		skb->prev->next = skb->next;
		skb->next->prev = skb->prev;
	} else {
		sk->back_log = NULL;
	}
	sti();
	DPRINTF((DBG_INET, "sk->back_log = %X\n", sk->back_log));
        /* 注意在这里如果是tcp协议的则调用的tcp_rcv，
          * 如果是udp则调用的是udp_rcv函数
          */
	if (sk->prot->rcv) sk->prot->rcv(skb, skb->dev, sk->opt,
					 skb->saddr, skb->len, skb->daddr, 1,

	/* Only used for/by raw sockets. */
	(struct inet_protocol *)sk->pair); 
	cli();
  }
  /* 打开标记 */
  sk->blog = 0;
  sk->inuse = 0;
  sti();
  if (sk->dead && sk->state == TCP_CLOSE) {
	/* Should be about 2 rtt's */
	reset_timer(sk, TIME_DONE, min(sk->rtt * 2, TCP_DONE_TIME));
  }
}


static int
inet_fioctl(struct inode *inode, struct file *file,
	 unsigned int cmd, unsigned long arg)
{
  int minor, ret;

  /* Extract the minor number on which we work. */
  minor = MINOR(inode->i_rdev);
  if (minor != 0) return(-ENODEV);

  /* Now dispatch on the minor device. */
  switch(minor) {
	case 0:		/* INET */
		ret = inet_ioctl(NULL, cmd, arg);
		break;
	case 1:		/* IP */
		ret = ip_ioctl(NULL, cmd, arg);
		break;
	case 2:		/* ICMP */
		ret = icmp_ioctl(NULL, cmd, arg);
		break;
	case 3:		/* TCP */
		ret = tcp_ioctl(NULL, cmd, arg);
		break;
	case 4:		/* UDP */
		ret = udp_ioctl(NULL, cmd, arg);
		break;
	default:
		ret = -ENODEV;
  }

  return(ret);
}


/* 这个文件操作和net_fops有啥区别?*/

static struct file_operations inet_fops = {
  NULL,		/* LSEEK	*/
  NULL,		/* READ		*/
  NULL,		/* WRITE	*/
  NULL,		/* READDIR	*/
  NULL,		/* SELECT	*/
  inet_fioctl,	/* IOCTL	*/
  NULL,		/* MMAP		*/
  NULL,		/* OPEN		*/
  NULL		/* CLOSE	*/
};


static struct proto_ops inet_proto_ops = {
  AF_INET,

  inet_create,
  inet_dup,
  inet_release,
  inet_bind,
  inet_connect,
  inet_socketpair,
  inet_accept,
  inet_getname, 
  inet_read,
  inet_write,
  inet_select,
  inet_ioctl,
  inet_listen,
  inet_send,
  inet_recv,
  inet_sendto,
  inet_recvfrom,
  inet_shutdown,
  inet_setsockopt,
  inet_getsockopt,
  inet_fcntl,
};

extern unsigned long seq_offset;

/* Called by ddi.c on kernel startup.  */

/* AF_INET族协议初始化，注册INET协议设备的文件操作符
  * 同时注册INET协议族的函数操作集合 
  */
void inet_proto_init(struct ddi_proto *pro)
{
  struct inet_protocol *p;
  int i;

  printk("Swansea University Computer Society Net2Debugged [1.30]\n");
  /* Set up our UNIX VFS major device. */
  if (register_chrdev(AF_INET_MAJOR, "af_inet", &inet_fops) < 0) {
	printk("%s: cannot register major device %d!\n",
					pro->name, AF_INET_MAJOR);
	return;
  }

  /* Tell SOCKET that we are alive... */
  /* 注册的变量都存放在p_ops数组当中，当创建socket的时候
    * 会根据传递的family来确定使用哪个struct proto_ops
    */
  (void) sock_register(inet_proto_ops.family, &inet_proto_ops);

  seq_offset = CURRENT_TIME*250;

  /* Add all the protocols. */
  /* 将各种协议的套接字数组置空，也就是协议的套接字链表为空 */
  for(i = 0; i < SOCK_ARRAY_SIZE; i++) {
	tcp_prot.sock_array[i] = NULL;
	udp_prot.sock_array[i] = NULL;
	raw_prot.sock_array[i] = NULL;
  }
  printk("IP Protocols: ");
  
  /* 将静态变量组成的一个链进行初始化，全部添加到inet_protocol协议数组当中 
    * 该inet_protocol链表是网络层向上层传递数据时调用的协议结构 
    */
  for(p = inet_protocol_base; p != NULL;) {
	struct inet_protocol *tmp;

	tmp = (struct inet_protocol *) p->next;
	/* 添加的变量最终都存放在inet_protos数组当中 */
	inet_add_protocol(p);
	printk("%s%s",p->name,tmp?", ":"\n");
	p = tmp;
  }

  /* Initialize the DEV module. */
  /* 初始化网络设备 */
  dev_init();

  /* Initialize the "Buffer Head" pointers. */
  /* 初始化inet协议族的网络中断下半部分处理 */
  bh_base[INET_BH].routine = inet_bh;
}
