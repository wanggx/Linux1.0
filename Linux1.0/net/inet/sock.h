/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the AF_INET socket handler.
 *
 * Version:	@(#)sock.h	1.0.4	05/13/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche <flla@stud.uni-sb.de>
 *
 * Fixes:
 *		Alan Cox	:	Volatiles in skbuff pointers. See
 *					skbuff comments. May be overdone,
 *					better to prove they can be removed
 *					than the reverse.
 *		Alan Cox	:	Added a zapped field for tcp to note
 *					a socket is reset and must stay shut up
 *		Alan Cox	:	New fields for options
 *	Pauline Middelink	:	identd support
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _SOCK_H
#define _SOCK_H

#include <linux/timer.h>
#include <linux/ip.h>		/* struct options */
#include <linux/tcp.h>		/* struct tcphdr */

#include "skbuff.h"		/* struct sk_buff */
#include "protocol.h"		/* struct inet_protocol */
#ifdef CONFIG_AX25
#include "ax25.h"
#endif
#ifdef CONFIG_IPX
#include "ipx.h"
#endif

#define SOCK_ARRAY_SIZE	64


/*
 * This structure really needs to be cleaned up.
 * Most of it is for TCP, and not used by any of
 * the other protocols.
 */
/* 网络层数据结构 
 */
struct sock {
  struct options		*opt;
  /*wmem_alloc是一个计数器，用来累计为本插口分配的sk_buff存储空间，
    * 一般不应该超过限额sndbuf
    */
  volatile unsigned long	wmem_alloc;
  /* 当前读缓冲区大小，该值不可大于系统规定的最大值
    */
  volatile unsigned long	rmem_alloc;
  /* 表示应用程序下一次写数据时所对应的第一个字节的序列号
    */
  unsigned long			write_seq;
  /* 表示本地将要发送的下一个数据包中第一个字节对应的序列号
    */
  unsigned long			sent_seq;
  /* 表示本地希望从远端接收的下一个数据的序列号
    */
  unsigned long			acked_seq;
  /* 应用程序有待读取(但尚未读取)数据的第一个序列号，
    * 如果copied_seq=100，则序列号100之前的数据均已被用户读取
    * 序列号100之后的数据都没有被读取 
    */
  unsigned long			copied_seq;
  /* 表示目前本地接收到的对本地发送数据的应答序列号
    */
  unsigned long			rcv_ack_seq;
  /* 窗口大小，是一个绝对值，表示本地将要发送数据包中所包含最后一个数据的序列号，
    * 不可大于window_seq
    */
  unsigned long			window_seq;
  /* 该字段在对方发送FIN数据包时使用，在接收到远端发送的 FIN数据包后，
    * fin_seq 被初始化为对方的 FIN 数据包最后一个字节的序列号加 1，表示本地对此 FIN 数据包进行应答的序列号
    */
  unsigned long			fin_seq;

  /*  以上两个字段用于紧急数据处理，urg_seq 表示紧急数据最大序列号。
    * urg_data 是一个标志位，当设置为 1 时，表示接收到紧急数据。
    */
  unsigned long			urg_seq;
  unsigned long			urg_data;

  /*
   * Not all are volatile, but some are, so we
   * might as well say they all are.
   */
  /* inuse=1 表示其它进程正在使用该 sock 结构，本进程需等待 */
  volatile char                 inuse,
				dead, /* dead=1 表示该 sock 结构已处于释放状态*/
				urginline,/* urginline=1 表示紧急数据将被当作普通数据处理。*/
				intr,
				blog,/* blog=1 表示对应套接字处于节制状态，此时接收的数据包均被丢弃*/
				done,
				reuse,  /* 注意和inuse的区别 */
				keepopen,/* keepopen=1 表示使用保活定时器 */
				linger,/* linger=1 表示在关闭套接字时需要等待一段时间以确认其已关闭。*/
				delay_acks,/* delay_acks=1表示延迟应答，可一次对多个数据包进行应答 */
				destroy,/* destroy=1 表示该 sock 结构等待销毁*/
				ack_timed,
				no_check,
				/* 如果zapped=1，则表示该套接字已被远端复位，要发送数据包，必须重新建立
                                  * 连接，也就是表示接收到了一个rst 
                                  */
				zapped,	/* In ax25 & ipx means not linked */
				broadcast,
				nonagle;/* noagle=1 表示不使用 NAGLE 算法*/
  unsigned long		        lingertime;/*表示等待关闭操作的时间，只有当 linger 标志位为 1 时，该字段才有意义。*/
  int				proc;/* 该 sock 结构（即该套接字）所属的进程的进程号。*/
  struct sock			*next;   /* 形成struct sock的一个链表 */
  /* 在RAW套接字创建和关闭的时候用来记录struct inet_protocol指针
    * 在PACKET套接字创建和关闭时用来记录struct packet_type指针 
    */
  struct sock			*pair;   

  /* send_head, send_tail 用于 TCP协议重发队列。
    * send_head 指向的队列 （send_tail 指向该队列的尾部）， 
    * 该队列缓存已发送出去但尚未得到对方应答的数据包。
    */
  struct sk_buff		*volatile send_tail;
  struct sk_buff		*volatile send_head;

  /* back_log为接收的数据包缓存队列，在函数tcp_data中如果struct sock的inuse为1，
    * 则将接收到的skb插入到back_log中
    */
  struct sk_buff		*volatile back_log;

  /* partial 队列中缓存的单个数据包源于 TCP 协议的流式传输，对于 TCP 协议，为了避免在网络中
    * 传输小数据包，充分利用网络效率，底层网络栈实现对于用户应用程序发送的少量数据进行收
    * 集缓存，当积累到一定数量后（MSS） ，方才作为整个包发送出去。partial 队列中数据包的意义
    * 即在于此，对于少量数据，如果数据并非是 OOB 数据（即无需立刻发送给远端） ，则暂时分配
    * 一个大容量的数据包，将数据拷贝到该大数据包中，之后将该数据包缓存到 partial 队列中，当
    * 下次用户继续发送数据时，内核首先检查 partial 队列中是否有之前未填充满的数据包，则这些
    * 数据可以继续填充到该数据包，直到填满才将其发送出去。当然为了尽量减少对应用程序效率
    * 的影响，这个等待填满的时间是一定的，在实现上，内核设置一个定时器，当定时器超时时，
    * 如果 partial 队列中缓存有未填满的数据包， 仍然将其发送出去， 超时发送函数为 tcp_send_partial.
    * 此外在其它条件下，当需要发送 partial 中数据包时，内核也直接调用 tcp_send_partial 函数进行
    * 发送。*/
  struct sk_buff		*partial;  /* 创建最大长度的待发送数据包。
  								     * 即使用最大MTU值创建的数据包
  								     */
  struct timer_list		partial_timer;  /*按时发送 partial 指针指向的数据包，以免缓存（等待）时间过长。*/
  long				retransmits; /* 重发次数*/
  struct sk_buff		*volatile wback,  /* wback,wfront表示写队列的前和后 */
				*volatile wfront,
				*volatile rqueue; /* socket接收包队列，读取的时候是从这个队列中读取的 */
  /* 不同协议的协议操作函数，注意和struct proto_ops结构区分
    */
  struct proto			*prot;
  /* 如果还没有收到数据，则在该等待队列中等待
   */
  struct wait_queue		**sleep;/*进程等待sock的地位*/
  unsigned long			daddr;   /*套接字的远端地址*/
  /*绑定地址*/
  unsigned long			saddr;  /*套接字的本地地址*/
  unsigned short		max_unacked;/* 最大未处理请求连接数（应答数） */
  unsigned short		window; /* 远端窗口大小 */
  unsigned short		bytes_rcv;  /* 已接收字节总数*/
/* mss is min(mtu, max_window) */
  unsigned short		mtu;  /*最大传输单元*/     /* mss negotiated in the syn's */

  /* 最大报文长度：MSS=MTU-IP首部长度-TCP首部长度,MSS=TCP报文段长度-TCP首部长度。
    * current eff. mss - can change  
    */
  volatile unsigned short	mss; 
  volatile unsigned short	user_mss; /*用户指定的 MSS值*/ /* mss requested by user in ioctl */
  volatile unsigned short	max_window;
  unsigned short		num;  		/*对应本地端口号*/
  volatile unsigned short	cong_window;
  volatile unsigned short	cong_count;
  volatile unsigned short	ssthresh;
  volatile unsigned short	packets_out;
  volatile unsigned short	shutdown;
  volatile unsigned long	rtt;/* 往返时间估计值*/
  volatile unsigned long	mdev;/* mean deviation, 即RTTD,  绝对偏差*/
  volatile unsigned long	rto;
/* currently backoff isn't used, but I'm maintaining it in case
 * we want to go back to a backoff formula that needs it
 */
  volatile unsigned short	backoff;/* 退避算法度量值 */
  volatile short		err; /* 错误标志值*/
  unsigned char			protocol; /* 传输层协议值 表示当前域中套接字所属的协议 */
  /* 套接字状态 */
  volatile unsigned char	state;

  /* ack_backlog字段记录目前累计的应发送而未发送的
    * 应答数据包的个数
    */
  volatile unsigned char	ack_backlog; 
  unsigned char			max_ack_backlog;  /*表示最大侦听队列*/
  unsigned char			priority;
  unsigned char			debug;
  unsigned short		rcvbuf;  /*表示接收缓冲区的字节长度*/
  unsigned short		sndbuf;  /*表示发送缓冲区的字节长度*/
  unsigned short		type;  /* 表示套接字的类型 */
#ifdef CONFIG_IPX
  ipx_address			ipx_source_addr,ipx_dest_addr;
  unsigned short		ipx_type;
#endif
#ifdef CONFIG_AX25
/* Really we want to add a per protocol private area */
  ax25_address			ax25_source_addr,ax25_dest_addr;
  struct sk_buff *volatile	ax25_retxq[8];
  char				ax25_state,ax25_vs,ax25_vr,ax25_lastrxnr,ax25_lasttxnr;
  char				ax25_condition;
  char				ax25_retxcnt;
  char				ax25_xx;
  char				ax25_retxqi;
  char				ax25_rrtimer;
  char				ax25_timer;
  ax25_digi			*ax25_digipeat;
#endif  
/* IP 'private area' or will be eventually */
  int				ip_ttl;	 /* IP首部 TTL 字段值，实际上表示路由器跳数*/	/* TTL setting */
  int				ip_tos;	/* IP首部 TOS字段值，服务类型值*/	/* TOS */
  struct tcphdr			dummy_th;/* 缓存的 TCP首部，在 TCP协议中创建一个发送数据包时可以利用此字段快速创建 TCP 首部。*/

  /* This part is used for the timeout functions (timer.c). */
  /* 表示struct sock是什么类型的时钟，如TIME_WRITE等等 */
  int				timeout;	/* What are we waiting for? */
  struct timer_list		timer;

  /* identd */
  /* 协议数据对应的socket指针 */
  struct socket			*socket;
  
  /* Callbacks */
  /* 唤醒等待socket的进程，也就是sock的状态发生改变 */
  void				(*state_change)(struct sock *sk);
  /* 表示sock的数据已经准备好 */
  void				(*data_ready)(struct sock *sk,int bytes);
  void				(*write_space)(struct sock *sk);
  /* 错误报告函数，通过icmp_rcv函数调用 */
  void				(*error_report)(struct sock *sk);
  
};

/* 具体网络协议的操作函数,表示传输层函数的操作集的一个结构
 * 对于不同协议可以使用相同的端口号TCP，UDP可以同时使用1000端口
 */
struct proto {
  struct sk_buff *	(*wmalloc)(struct sock *sk,
				    unsigned long size, int force,
				    int priority);
  struct sk_buff *	(*rmalloc)(struct sock *sk,
				    unsigned long size, int force,
				    int priority);
  void			(*wfree)(struct sock *sk, void *mem,
				 unsigned long size);
  void			(*rfree)(struct sock *sk, void *mem,
				 unsigned long size);
  unsigned long		(*rspace)(struct sock *sk);
  unsigned long		(*wspace)(struct sock *sk);
  void			(*close)(struct sock *sk, int timeout);
  int			(*read)(struct sock *sk, unsigned char *to,
				int len, int nonblock, unsigned flags);
  int			(*write)(struct sock *sk, unsigned char *to,
				 int len, int nonblock, unsigned flags);
  int			(*sendto)(struct sock *sk,
				  unsigned char *from, int len, int noblock,
				  unsigned flags, struct sockaddr_in *usin,
				  int addr_len);
  int			(*recvfrom)(struct sock *sk,
				    unsigned char *from, int len, int noblock,
				    unsigned flags, struct sockaddr_in *usin,
				    int *addr_len);
  int			(*build_header)(struct sk_buff *skb,
					unsigned long saddr,
					unsigned long daddr,
					struct device **dev, int type,
					struct options *opt, int len, int tos, int ttl);
  int			(*connect)(struct sock *sk,
				  struct sockaddr_in *usin, int addr_len);
  struct sock *		(*accept) (struct sock *sk, int flags);
  void			(*queue_xmit)(struct sock *sk,
				      struct device *dev, struct sk_buff *skb,
				      int free);
  void			(*retransmit)(struct sock *sk, int all);
  void			(*write_wakeup)(struct sock *sk);
  void			(*read_wakeup)(struct sock *sk);
  int			(*rcv)(struct sk_buff *buff, struct device *dev,
			       struct options *opt, unsigned long daddr,
			       unsigned short len, unsigned long saddr,
			       int redo, struct inet_protocol *protocol);
  int			(*select)(struct sock *sk, int which,
				  select_table *wait);
  int			(*ioctl)(struct sock *sk, int cmd,
				 unsigned long arg);
  int			(*init)(struct sock *sk);
  void			(*shutdown)(struct sock *sk, int how);
  int			(*setsockopt)(struct sock *sk, int level, int optname,
  				 char *optval, int optlen);
  int			(*getsockopt)(struct sock *sk, int level, int optname,
  				char *optval, int *option);  	 
  unsigned short	max_header;
  unsigned long		retransmits;      /* 表示协议超时重传的次数 */
  /* 通过端口号和SOCK_ARRAY_SIZE取与得到索引 */
  struct sock *		sock_array[SOCK_ARRAY_SIZE];
  char			name[80];   /* 协议名称如TCP,UDP等等 */
};

#define TIME_WRITE	1  /* 超时重传 */
#define TIME_CLOSE	2   /* 等待关闭 */
#define TIME_KEEPOPEN	3  /* 保活 */
#define TIME_DESTROY	4  /* 套接字释放 */
#define TIME_DONE	5	/* used to absorb those last few packets */
#define TIME_PROBE0	6   /* 非0窗口探测 */
#define SOCK_DESTROY_TIME 1000	/* about 10 seconds			*/

#define PROT_SOCK	1024	/* Sockets 0-1023 can't be bound too unless you are superuser */

#define SHUTDOWN_MASK	3   /* 完全关闭 */
#define RCV_SHUTDOWN	1     /* 接收通道关闭 */
#define SEND_SHUTDOWN	2   /* 发送通道关闭 */


extern void			destroy_sock(struct sock *sk);
extern unsigned short		get_new_socknum(struct proto *, unsigned short);
extern void			put_sock(unsigned short, struct sock *); 
extern void			release_sock(struct sock *sk);
extern struct sock		*get_sock(struct proto *, unsigned short,
					  unsigned long, unsigned short,
					  unsigned long);
extern void			print_sk(struct sock *);
extern struct sk_buff		*sock_wmalloc(struct sock *sk,
					      unsigned long size, int force,
					      int priority);
extern struct sk_buff		*sock_rmalloc(struct sock *sk,
					      unsigned long size, int force,
					      int priority);
extern void			sock_wfree(struct sock *sk, void *mem,
					   unsigned long size);
extern void			sock_rfree(struct sock *sk, void *mem,
					   unsigned long size);
extern unsigned long		sock_rspace(struct sock *sk);
extern unsigned long		sock_wspace(struct sock *sk);

extern int			sock_setsockopt(struct sock *sk,int level,int op,char *optval,int optlen);
extern int			sock_getsockopt(struct sock *sk,int level,int op,char *optval,int *optlen);

/* declarations from timer.c */
extern struct sock *timer_base;

void delete_timer (struct sock *);
void reset_timer (struct sock *, int, unsigned long);
void net_timer (unsigned long);


#endif	/* _SOCK_H */
