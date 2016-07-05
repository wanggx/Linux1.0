/*
 *	SUCS	NET2 Debugged.
 *
 *	Generic datagram handling routines. These are generic for all protocols. Possibly a generic IP version on top
 *	of these would make sense. Not tonight however 8-).
 *	This is used because UDP, RAW, PACKET and the to be released IPX layer all have identical select code and mostly
 *	identical recvfrom() code. So we share it here. The select was shared before but buried in udp.c so I moved it.
 *
 *	Authors:	Alan Cox <iiitac@pyr.swan.ac.uk>. (datagram_select() from old udp.c code)
 *
 *	Fixes:
 *		Alan Cox	:	NULL return from skb_peek_copy() understood
 *		Alan Cox	:	Rewrote skb_read_datagram to avoid the skb_peek_copy stuff.
 *		Alan Cox	:	Added support for SOCK_SEQPACKET. IPX can no longer use the SO_TYPE hack but
 *					AX.25 now works right, and SPX is feasible.
 *		Alan Cox	:	Fixed write select of non IP protocol crash.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/sched.h>
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


/*
 *	Get a datagram skbuff, understands the peeking, nonblocking wakeups and possible
 *	races. This replaces identical code in packet,raw and udp, as well as the yet to
 *	be released IPX support. It also finally fixes the long standing peek and read
 *	race for datagram sockets. If you alter this routine remember it must be
 *	re-entrant.
 */

/* 查看套接字接受队列中是否有数据包，如果有，则直接返回该数据包，
 * 否则睡眠等待，在睡眠之前需要检查等待的必要性，这些检查包括
 * 1、套接字是否已经被关闭接收通道，对于这种情况盲目等待是不可取
 *    此时调用release_sock函数从可能的其他缓存队列中转移数据包(实际上
      调用release_sock函数对使用udp协议的套接字接收队列不会造成任何影响
      因为UDP协议根本没有使用back_log暂存队列，并直接返回NULL
 * 2、套接字在处理的过程当中是否发送错误
 */
struct sk_buff *skb_recv_datagram(struct sock *sk, unsigned flags, int noblock, int *err)
{
	struct sk_buff *skb;

	/* Socket is inuse - so the timer doesn't attack it */
restart:
	sk->inuse = 1;
	while(sk->rqueue == NULL)	/* No data */
	{
		/* If we are shutdown then no more data is going to appear. We are done */
		if (sk->shutdown & RCV_SHUTDOWN)
		{
			release_sock(sk);
			*err=0;
			return NULL;
		}

		if(sk->err)
		{
			release_sock(sk);
			*err=-sk->err;
			sk->err=0;
			return NULL;
		}

		/* Sequenced packets can come disconnected. If so we report the problem */
		if(sk->type==SOCK_SEQPACKET && sk->state!=TCP_ESTABLISHED)
		{
			release_sock(sk);
			*err=-ENOTCONN;
			return NULL;
		}

		/* User doesn't want to wait */
		if (noblock)
		{
			release_sock(sk);
			*err=-EAGAIN;
			return NULL;
		}
		release_sock(sk);

		/* Interrupts off so that no packet arrives before we begin sleeping.
		   Otherwise we might miss our wake up */
		cli();
                /* 如果此时，数据读取队列仍然为NULL，并且也不阻塞，
                  * 则当前进程可中断的睡眠
                  */
		if (sk->rqueue == NULL)
		{
			interruptible_sleep_on(sk->sleep);
			/* Signals may need a restart of the syscall */
			if (current->signal & ~current->blocked)
			{
				sti();
				*err=-ERESTARTSYS;
				return(NULL);
			}
			if(sk->err != 0)	/* Error while waiting for packet
						   eg an icmp sent earlier by the
						   peer has finaly turned up now */
			{
				*err = -sk->err;
				sti();
				sk->err=0;
				return NULL;
			}
		}
		sk->inuse = 1;
		sti();
	  }
	  /* Again only user level code calls this function, so nothing interrupt level
	     will suddenely eat the rqueue */
          /* 运行到这里则代表struct sock的读取队列中有数据包可读取，
            * 如果不是预读取，则从读取队列中移除一个skb，否则不移除， 
            * 仅仅是读取了里面的数据，例如需要读取前面几个字节需要知道数据包信息的 
            * 情况下  
            */
	  if (!(flags & MSG_PEEK))
	  {
	    /* 从读队列中获取一个skb */
		skb=skb_dequeue(&sk->rqueue);
		if(skb!=NULL)
			skb->users++;
		else
			goto restart;	/* Avoid race if someone beats us to the data */
	  }
	  else
	  {
		cli();
		skb=skb_peek(&sk->rqueue);
		if(skb!=NULL)
			skb->users++;
		sti();
		if(skb==NULL)	/* shouldn't happen but .. */
			*err=-EAGAIN;
	  }
	  return skb;
}


/* skb_free_datagram 函数释放一个数据包，166 行递减用户计数，每个使用该数据包的进程都
 * 回增加该 sk_buff 结构的 users 字段，一旦该字段为 0，表示这是一个游离的数据包，可以进
 * 行释放，否则表示还有进程在使用该数据包，此时不可进行释放，直接返回。148 行检查数
 * 据包是否仍然处于系统某个队列中， 如果数据包还被挂接在系统队列中， 也不可对其进行释
 * 放。否则调用 kfree_skb 函数释放数据包所占用的内存空间。
 */
void skb_free_datagram(struct sk_buff *skb)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	skb->users--;
	if(skb->users>0)
	{
		restore_flags(flags);
		return;
	}
	/* See if it needs destroying */
	if(skb->list == NULL)	/* Been dequeued by someone - ie its read */
		kfree_skb(skb,FREE_READ);
	restore_flags(flags);
}

/* 将内核缓冲区中数据复制到用户缓冲区当中 */
void skb_copy_datagram(struct sk_buff *skb, int offset, char *to, int size)
{
	/* We will know all about the fraglist options to allow >4K receives
	   but not this release */
	memcpy_tofs(to,skb->h.raw+offset,size);
}

/*
 *	Datagram select: Again totally generic. Moved from udp.c
 *	Now does seqpacket.
 */

/* udp协议的select系统调用 */
int datagram_select(struct sock *sk, int sel_type, select_table *wait)
{
	select_wait(sk->sleep, wait);
	switch(sel_type)
	{
		case SEL_IN:
			if (sk->type==SOCK_SEQPACKET && sk->state==TCP_CLOSE)
			{
				/* Connection closed: Wake up */
				return(1);
			}
			if (sk->rqueue != NULL || sk->err != 0)
			{	/* This appears to be consistent
				   with other stacks */
				return(1);
			}
			return(0);

		case SEL_OUT:
			if (sk->prot && sk->prot->wspace(sk) >= MIN_WRITE_SPACE)
			{
				return(1);
			}
			if (sk->prot==NULL && sk->sndbuf-sk->wmem_alloc >= MIN_WRITE_SPACE)
			{
				return(1);
			}
			return(0);

		case SEL_EX:
			if (sk->err)
				return(1); /* Socket has gone into error state (eg icmp error) */
			return(0);
	}
	return(0);
}
