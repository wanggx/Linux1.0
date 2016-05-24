#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

#define NEW_SWAP

/*
 * define DEBUG if you want the wait-queues to have some extra
 * debugging code. It's not normally used, but might catch some
 * wait-queue coding errors.
 *
 *  #define DEBUG
 */

#define HZ 100

/*
 * System setup flags..
 */
extern int hard_math;
extern int x86;
extern int ignore_irq13;
extern int wp_works_ok;

/*
 * Bus types (default is ISA, but people can check others with these..)
 * MCA_bus hardcoded to 0 for now.
 */
extern int EISA_bus;
#define MCA_bus 0

#include <linux/tasks.h>
#include <asm/system.h>

/*
 * User space process size: 3GB. This is hardcoded into a few places,
 * so don't change it unless you know what you are doing.
 */
#define TASK_SIZE	0xc0000000

/*
 * Size of io_bitmap in longwords: 32 is ports 0-0x3ff.
 */
#define IO_BITMAP_SIZE	32

/*
 * These are the constant used to fake the fixed-point load-average
 * counting. Some notes:
 *  - 11 bit fractions expand to 22 bits by the multiplies: this gives
 *    a load-average precision of 10 bits integer + 11 bits fractional
 *  - if you want to count load-averages more often, you need more
 *    precision, or rounding will get you. With 2-second counting freq,
 *    the EXP_n values would be 1981, 2034 and 2043 if still using only
 *    11 bit fractions.
 */
extern unsigned long avenrun[];		/* Load averages */

#define FSHIFT		11		/* nr of bits of precision */
#define FIXED_1		(1<<FSHIFT)	/* 1.0 as fixed-point */
#define LOAD_FREQ	(5*HZ)		/* 5 sec intervals */
#define EXP_1		1884		/* 1/exp(5sec/1min) as fixed-point */
#define EXP_5		2014		/* 1/exp(5sec/5min) */
#define EXP_15		2037		/* 1/exp(5sec/15min) */

#define CALC_LOAD(load,exp,n) \
	load *= exp; \
	load += n*(FIXED_1-exp); \
	load >>= FSHIFT;

#define CT_TO_SECS(x)	((x) / HZ)
#define CT_TO_USECS(x)	(((x) % HZ) * 1000000/HZ)

#define FIRST_TASK task[0]
#define LAST_TASK task[NR_TASKS-1]

#include <linux/head.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/time.h>
#include <linux/param.h>
#include <linux/resource.h>
#include <linux/vm86.h>
#include <linux/math_emu.h>

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2	/*内核一些特定流程，是不可被打断的，也就是可以忽略某些信号*/
#define TASK_ZOMBIE		3           /* 进程的僵尸状态 */
#define TASK_STOPPED		4
#define TASK_SWAPPING		5

#ifndef NULL
#define NULL ((void *) 0)
#endif

#ifdef __KERNEL__

extern void sched_init(void);
extern void show_state(void);
extern void trap_init(void);

asmlinkage void schedule(void);

#endif /* __KERNEL__ */

struct i387_hard_struct {
	long	cwd;
	long	swd;
	long	twd;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	st_space[20];	/* 8*10 bytes for each FP-reg = 80 bytes */
};

struct i387_soft_struct {
	long	cwd;
	long	swd;
	long	twd;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long    top;
	struct fpu_reg	regs[8];	/* 8*16 bytes for each FP-reg = 128 bytes */
	unsigned char	lookahead;
	struct info	*info;
	unsigned long	entry_eip;
};

union i387_union {
	struct i387_hard_struct hard;
	struct i387_soft_struct soft;
};

struct tss_struct {
	unsigned short	back_link,__blh;
	unsigned long	esp0;
	unsigned short	ss0,__ss0h;
	unsigned long	esp1;
	unsigned short	ss1,__ss1h;
	unsigned long	esp2;
	unsigned short	ss2,__ss2h;
	unsigned long	cr3;
	unsigned long	eip;
	unsigned long	eflags;
	unsigned long	eax,ecx,edx,ebx;
	unsigned long	esp;
	unsigned long	ebp;
	unsigned long	esi;
	unsigned long	edi;
	unsigned short	es, __esh;
	unsigned short	cs, __csh;
	unsigned short	ss, __ssh;
	unsigned short	ds, __dsh;
	unsigned short	fs, __fsh;
	unsigned short	gs, __gsh;
	unsigned short	ldt, __ldth;
	unsigned short	trace, bitmap;
	unsigned long	io_bitmap[IO_BITMAP_SIZE+1];
	unsigned long	tr;
	unsigned long	cr2, trap_no, error_code;
	union i387_union i387;
};

struct task_struct {
/* these are hardcoded - don't touch */
	volatile long state;	/* -1 unrunnable, 0 runnable, >0 stopped */
	long counter;           /* 动态优先级 */
	long priority;
	/* 注意很多地方的这种写法 if (current->signal & ~current->blocked)
	 * signal表示发送给进程的信号位图，blocked表示进程阻塞的信号位图
	 */
	unsigned long signal;   
	unsigned long blocked;	/* bitmap of masked signals */
	unsigned long flags;	/* per process flags, defined below */
	int errno;
	int debugreg[8];  /* Hardware debugging registers */
/* various fields */
	struct task_struct *next_task, *prev_task;
	struct sigaction sigaction[32];
	unsigned long saved_kernel_stack;
	unsigned long kernel_stack_page;
	/* exit_signal表示退出时给父进程发送的信号
	 */
	int exit_code, exit_signal;
        /* 是否是elf可执行文件格式 */
	int elf_executable:1;
	int dumpable:1;
	/* 表示内存吃紧时，该进程是否可以被交换 */
	int swappable:1;
    /* 区分进程正在执行老程序代码，还是用系统调用execve()装入一个新的程序 
      */
	int did_exec:1;
	/* start_code,end_code表示代码段的地址空间
	  * end_data表示数据段的结束地址 
	  * start_brk表示heap的起始空间，brk表示当前的heap指针 
	  * start_stack表示stack段的起始地址 
	  */ 
	unsigned long start_code,end_code,end_data,start_brk,brk,start_stack,start_mmap;
	unsigned long arg_start, arg_end, env_start, env_end;
	/* pgrp表示进程组号，pid表示进程号，进程组会有一个
	 * 进程组领导进程，领导进程的pid成为进程组的id 
	 * 用来识别进程组，多个进程组还可以构成一个会话
	 * 
	 */
	int pid,pgrp,session,leader;
    /* 在没有附加组ID（Suplimentary ID）的年代，一个用户只能属于自已的同名组，
      * 例如chinsung的用户ID是1000，那么它只属于chinsung组，这个组的组ID也是1000，
      * 如果要访问一个属于ftp的文件，那么应该先将自己的组ID换成ftp的组ID才行。
      * 显然，这有点麻烦。于是自BSD4.2以后，出现了附加组ID的概念：
      * 一个用户可以属于一个组，还可以属于若干附加组；在进行权限校验时，
      * 不光检查这个用户所在的组，还要检查这个用户所在的附加组。
      * 这更贴近生活实际了，好比我们同时会在好几个项目组
      */
	int	groups[NGROUPS];
	/* 
	 * pointers to (original) parent process, youngest child, younger sibling,
	 * older sibling, respectively.  (p->father can be replaced with 
	 * p->p_pptr->pid)
	 */

	/* p_opptr记录创建该进程的进程，p_pptr为当前父进程，p_cptr为最小孩子进程
	 * p_ysptr记录年轻的兄弟进程，p_osptr为老的兄弟进程
	 */
	struct task_struct *p_opptr,*p_pptr, *p_cptr, *p_ysptr, *p_osptr;
	/* 等待孩子进程的队列，是将自己添加到该队列当中
	 */
	struct wait_queue *wait_chldexit;	/* for wait4() */
	/*
	 * For ease of programming... Normal sleeps don't need to
	 * keep track of a wait-queue: every task has an entry of its own
	 */
	/* 用户id，有效id 一个进程如果没有SUID或SGID位，则euid=uid egid=gid，
	 * 分别是运行这个程序的用户的uid和gid。例如kevin用户的uid和gid分别为204和202，
	 * foo用户的uid和gid为 200，201，kevin运行myfile程序形成的进程的euid=uid=204，
	 * egid=gid=202，内核根据这些值来判断进程对资源访问的限制，
	 * 其实就是kevin用户对资源访问的权限，和foo没关系。 
      * 如果一个程序设置了SUID，则euid和egid变成被运行的程序的所有者的uid和gid，
      * 例如kevin用户运行myfile，euid=200，egid=201，uid=204，gid=202，
      * 则这个进程具有它的属主foo的资源访问权限。
      * 使用euid来确认资源的访问权限 
      */
	unsigned short uid,euid,suid;
	/* 用户组id，有效组id*/
	unsigned short gid,egid,sgid;
	/* 软件定时，指出进程间隔多久被重新唤醒，采用tick为单位 */
	unsigned long timeout;
	/* 每个tick使it_real_value减1，减到0时向进程发送信号SIGALRM，并重新设置初值。
	 * 初值保存在it_real_cr当中
	 * 不管是用户态还是内核态每个tick使it_prof_value减1，减到0时
	 * 项进程发送SIGPROF信号，并重新设置，初值由it_prof_incr保存
	 * 进程在用户态执行时每个tick使it_virt_value减1，减到0
	 * 时，向进程发送SIGVTALRM信号，并重新设置初值。初值由it_virt_incr保存
	 */
	unsigned long it_real_value, it_prof_value, it_virt_value;
	unsigned long it_real_incr, it_prof_incr, it_virt_incr;
	long utime,stime,cutime,cstime,start_time;
	/* 表示自进程启动以来所发生的缺页中断次数 ，
	  * maj_flt表示需要读写磁盘，可能是内存页在磁盘中需要load到物理内存当中 
	  * 也可能是物理页内存不足，需要淘汰部分物理页到磁盘中 
	  */
	unsigned long min_flt, maj_flt;
	unsigned long cmin_flt, cmaj_flt;
        /* 进程的各种资源限制，如进程数据段，堆栈段，进程的最大虚拟空间等等 */
	struct rlimit rlim[RLIM_NLIMITS]; 
	unsigned short used_math;
	unsigned short rss;	/* number of resident pages */ /*当前在主存中的内存页数*/
	char comm[16];
	struct vm86_struct * vm86_info;
	unsigned long screen_bitmap;
/* file system info */
	int link_count;
	/* 表示进程的控制终端 */
	int tty;		/* -1 if no tty, so it must be signed */
	unsigned short umask;  /* 进程创建文件时默认的权限反码 */
	struct inode * pwd;    /* 进程的当前工作目录 */
	struct inode * root;   /* 当前进程的根目录 */
	struct inode * executable; /* 当前进程可执行文件 */
	/* 虚拟地址链，在地址链表中，
	 * 地址空间是按照从小到大的顺序来存放的
	 */
	struct vm_area_struct * mmap;
	/* 进程的共享内存列表
	 */
	struct shm_desc *shm;
	/* 进程的undo信号列表，也就是在进程退出的时候
	 * 将进程信号量占用的资源给释放掉，不然需要该资源的
	 * 其他进程无法得到资源，导致资源乱费
	 */
	struct sem_undo *semun;
	struct file * filp[NR_OPEN];
	/* 当子进程执行exec族函数替换子进程时，表示需要关闭的文件描述符
	 * 不然文件永远处于打开状态
	 */
	fd_set close_on_exec;
/* ldt for this task - used by Wine.  If NULL, default_ldt is used */
	struct desc_struct *ldt;
/* tss for this task */
	struct tss_struct tss;
#ifdef NEW_SWAP
	unsigned long old_maj_flt;	/* old value of maj_flt */
	unsigned long dec_flt;		/* page fault count of the last time */
	unsigned long swap_cnt;		/* number of pages to swap on next pass */ /*当前需要交换的页的数量*/
	short swap_table;		/* current page table */  /*当前需要交换进程一级页表的索引*/
	short swap_page;		/* current page */ /* 当前需要交换进程二级页的索引 */
#endif NEW_SWAP
	struct vm_area_struct *stk_vma;
};

/*
 * Per process flags
 */
#define PF_ALIGNWARN	0x00000001	/* Print alignment warning msgs */
					/* Not implemented yet, only for 486*/
#define PF_PTRACED	0x00000010	/* set if ptrace (0) has been called. */
#define PF_TRACESYS	0x00000020	/* tracing system calls */

/*
 * cloning flags:
 */
#define CSIGNAL		0x000000ff	/* signal mask to be sent at exit */
#define COPYVM		0x00000100	/* set if VM copy desired (like normal fork()) */
#define COPYFD		0x00000200	/* set if fd's should be copied, not shared (NI) */

/*
 *  INIT_TASK is used to set up the first task table, touch at
 * your own risk!. Base=0, limit=0x1fffff (=2MB)
 */
#define INIT_TASK \
/* state etc */	{ 0,15,15,0,0,0,0, \
/* debugregs */ { 0, },            \
/* schedlink */	&init_task,&init_task, \
/* signals */	{{ 0, },}, \
/* stack */	0,(unsigned long) &init_kernel_stack, \
/* ec,brk... */	0,0,0,0,0,0,0,0,0,0,0,0,0, \
/* argv.. */	0,0,0,0, \
/* pid etc.. */	0,0,0,0, \
/* suppl grps*/ {NOGROUP,}, \
/* proc links*/ &init_task,&init_task,NULL,NULL,NULL,NULL, \
/* uid etc */	0,0,0,0,0,0, \
/* timeout */	0,0,0,0,0,0,0,0,0,0,0,0, \
/* min_flt */	0,0,0,0, \
/* rlimits */   { {LONG_MAX, LONG_MAX}, {LONG_MAX, LONG_MAX},  \
		  {LONG_MAX, LONG_MAX}, {LONG_MAX, LONG_MAX},  \
		  {       0, LONG_MAX}, {LONG_MAX, LONG_MAX}}, \
/* math */	0, \
/* rss */	2, \
/* comm */	"swapper", \
/* vm86_info */	NULL, 0, \
/* fs info */	0,-1,0022,NULL,NULL,NULL,NULL, \
/* ipc */	NULL, NULL, \
/* filp */	{NULL,}, \
/* cloe */	{{ 0, }}, \
/* ldt */	NULL, \
/*tss*/	{0,0, \
	 sizeof(init_kernel_stack) + (long) &init_kernel_stack, KERNEL_DS, 0, \
	 0,0,0,0,0,0, \
	 (long) &swapper_pg_dir, \
	 0,0,0,0,0,0,0,0,0,0, \
	 USER_DS,0,USER_DS,0,USER_DS,0,USER_DS,0,USER_DS,0,USER_DS,0, \
	 _LDT(0),0, \
	 0, 0x8000, \
/* ioperm */ 	{~0, }, \
	 _TSS(0), 0, 0,0, \
/* 387 state */	{ { 0, }, } \
	} \
}

extern struct task_struct init_task;
extern struct task_struct *task[NR_TASKS];
extern struct task_struct *last_task_used_math;
extern struct task_struct *current;
extern unsigned long volatile jiffies;
extern unsigned long itimer_ticks;
extern unsigned long itimer_next;
extern struct timeval xtime;
extern int need_resched;

#define CURRENT_TIME (xtime.tv_sec)

extern void sleep_on(struct wait_queue ** p);
extern void interruptible_sleep_on(struct wait_queue ** p);
extern void wake_up(struct wait_queue ** p);
extern void wake_up_interruptible(struct wait_queue ** p);

extern void notify_parent(struct task_struct * tsk);
extern int send_sig(unsigned long sig,struct task_struct * p,int priv);
extern int in_group_p(gid_t grp);

extern int request_irq(unsigned int irq,void (*handler)(int));
extern void free_irq(unsigned int irq);
extern int irqaction(unsigned int irq,struct sigaction * sa);

/*
 * Entry into gdt where to find first TSS. GDT layout:
 *   0 - nul
 *   1 - kernel code segment
 *   2 - kernel data segment
 *   3 - user code segment
 *   4 - user data segment
 * ...
 *   8 - TSS #0
 *   9 - LDT #0
 *  10 - TSS #1
 *  11 - LDT #1
 */
/* 系统为每个进程保有两项，依次交替，其中n为进程task_struct在任务数组中的索引
 * 由于每个描述符占用8byte，而每个进程占用两项，所以n<<4,然后还要加上前面的
 * 起始位置的偏移量，也就是_TSS(n)获取的是描述符的地址
 */
#define FIRST_TSS_ENTRY 8
#define FIRST_LDT_ENTRY (FIRST_TSS_ENTRY+1)
#define _TSS(n) ((((unsigned long) n)<<4)+(FIRST_TSS_ENTRY<<3))
#define _LDT(n) ((((unsigned long) n)<<4)+(FIRST_LDT_ENTRY<<3))
#define load_TR(n) __asm__("ltr %%ax": /* no output */ :"a" (_TSS(n)))
#define load_ldt(n) __asm__("lldt %%ax": /* no output */ :"a" (_LDT(n)))
#define store_TR(n) \
__asm__("str %%ax\n\t" \
	"subl %2,%%eax\n\t" \
	"shrl $4,%%eax" \
	:"=a" (n) \
	:"0" (0),"i" (FIRST_TSS_ENTRY<<3))
/*
 *	switch_to(n) should switch tasks to task nr n, first
 * checking that n isn't the current task, in which case it does nothing.
 * This also clears the TS-flag if the task we switched to has used
 * tha math co-processor latest.
 */
/* current在这个时候被改变，也就是切换到tsk这个进程
 */
#define switch_to(tsk) \
__asm__("cmpl %%ecx,_current\n\t" \
	"je 1f\n\t" \
	"cli\n\t" \
	"xchgl %%ecx,_current\n\t" \
	"ljmp %0\n\t" \
	"sti\n\t" \
	"cmpl %%ecx,_last_task_used_math\n\t" \
	"jne 1f\n\t" \
	"clts\n" \
	"1:" \
	: /* no output */ \
	:"m" (*(((char *)&tsk->tss.tr)-4)), \
	 "c" (tsk) \
	:"cx")

#define _set_base(addr,base) \
__asm__("movw %%dx,%0\n\t" \
	"rorl $16,%%edx\n\t" \
	"movb %%dl,%1\n\t" \
	"movb %%dh,%2" \
	: /* no output */ \
	:"m" (*((addr)+2)), \
	 "m" (*((addr)+4)), \
	 "m" (*((addr)+7)), \
	 "d" (base) \
	:"dx")

#define _set_limit(addr,limit) \
__asm__("movw %%dx,%0\n\t" \
	"rorl $16,%%edx\n\t" \
	"movb %1,%%dh\n\t" \
	"andb $0xf0,%%dh\n\t" \
	"orb %%dh,%%dl\n\t" \
	"movb %%dl,%1" \
	: /* no output */ \
	:"m" (*(addr)), \
	 "m" (*((addr)+6)), \
	 "d" (limit) \
	:"dx")

#define set_base(ldt,base) _set_base( ((char *)&(ldt)) , base )
#define set_limit(ldt,limit) _set_limit( ((char *)&(ldt)) , (limit-1)>>12 )

/*
 * The wait-queues are circular lists, and you have to be *very* sure
 * to keep them correct. Use only these two functions to add/remove
 * entries in the queues.
 */

/* 将wait添加到以p指向的队列为首部的下一个位置 */
extern inline void add_wait_queue(struct wait_queue ** p, struct wait_queue * wait)
{
	unsigned long flags;

#ifdef DEBUG
	if (wait->next) {
		unsigned long pc;
		__asm__ __volatile__("call 1f\n"
			"1:\tpopl %0":"=r" (pc));
		printk("add_wait_queue (%08x): wait->next = %08x\n",pc,(unsigned long) wait->next);
	}
#endif
	save_flags(flags);
	cli();
	/* 如果队列为空，则让wait指向队首，next指向自己
	 * 当继续向队列中添加节点时，整个队列就是一个闭合的圆环，
	 * 否则将wait添加到以*p为队首的下一个地方
	 */
	if (!*p) {
		wait->next = wait;
		*p = wait;
	} else {
		wait->next = (*p)->next;
		(*p)->next = wait;
	}
	restore_flags(flags);
}

/* 将wait从队列p指向的队列中删除 */
extern inline void remove_wait_queue(struct wait_queue ** p, struct wait_queue * wait)
{
	unsigned long flags;
	struct wait_queue * tmp;
#ifdef DEBUG
	unsigned long ok = 0;
#endif

	save_flags(flags);
	cli();
	/* 如果队列中只有一个，则将该队列指为NULL
	 * 反之扫描等待队列，并从中删除wait
	 */
	if ((*p == wait) &&
#ifdef DEBUG
	    (ok = 1) &&
#endif
	    ((*p = wait->next) == wait)) {
		*p = NULL;
	} else {
		/* 因为队列是一个闭合的圆环，所以tmp=wait最终是可以找到wait的，
		 * 但是为什么不从*p处开始查找?
		 */
		tmp = wait;
		while (tmp->next != wait) {
			tmp = tmp->next;
#ifdef DEBUG
			if (tmp == *p)
				ok = 1;
#endif
		}
		tmp->next = wait->next;
	}
	wait->next = NULL;
	restore_flags(flags);
#ifdef DEBUG
	if (!ok) {
		printk("removed wait_queue not on list.\n");
		printk("list = %08x, queue = %08x\n",(unsigned long) p, (unsigned long) wait);
		__asm__("call 1f\n1:\tpopl %0":"=r" (ok));
		printk("eip = %08x\n",ok);
	}
#endif
}

extern inline void select_wait(struct wait_queue ** wait_address, select_table * p)
{
	struct select_table_entry * entry;

	/* 如果有任意一个指针为NULL,则返回，不做处理 */
	if (!p || !wait_address)
		return;
	/* 数量不能超过 */
	if (p->nr >= __MAX_SELECT_TABLE_ENTRIES)
		return;
	/* 获取当前操作的entry的地址 */
 	entry = p->entry + p->nr;
	/* 注意这个地方很重要，也就是要记录wait变量是等待在哪个链表当中 */
	entry->wait_address = wait_address;
	entry->wait.task = current;
	entry->wait.next = NULL;
	add_wait_queue(wait_address,&entry->wait);
	/* 增加nr的数量 */
	p->nr++;
}

extern void __down(struct semaphore * sem);

extern inline void down(struct semaphore * sem)
{
	if (sem->count <= 0)
		__down(sem);
	sem->count--;
}

extern inline void up(struct semaphore * sem)
{
	sem->count++;
	wake_up(&sem->wait);
}	

static inline unsigned long _get_base(char * addr)
{
	unsigned long __base;
	__asm__("movb %3,%%dh\n\t"
		"movb %2,%%dl\n\t"
		"shll $16,%%edx\n\t"
		"movw %1,%%dx"
		:"=&d" (__base)
		:"m" (*((addr)+2)),
		 "m" (*((addr)+4)),
		 "m" (*((addr)+7)));
	return __base;
}

#define get_base(ldt) _get_base( ((char *)&(ldt)) )

static inline unsigned long get_limit(unsigned long segment)
{
	unsigned long __limit;
	__asm__("lsll %1,%0"
		:"=r" (__limit):"r" (segment));
	return __limit+1;
}

/*  如果进程创建失败，则将进程在进程队列中的关系给解除
 *
 */

#define REMOVE_LINKS(p) do { unsigned long flags; \
	save_flags(flags) ; cli(); \
	(p)->next_task->prev_task = (p)->prev_task; \
	(p)->prev_task->next_task = (p)->next_task; \
	restore_flags(flags); \
	if ((p)->p_osptr) \
		(p)->p_osptr->p_ysptr = (p)->p_ysptr; \
	if ((p)->p_ysptr) \
		(p)->p_ysptr->p_osptr = (p)->p_osptr; \
	else \
		(p)->p_pptr->p_cptr = (p)->p_osptr; \
	} while (0)

/*  将新建的task_struct插入到以init_task为首的双向队列的队尾
 *  
 */

#define SET_LINKS(p) do { unsigned long flags; \
	save_flags(flags); cli(); \
	(p)->next_task = &init_task; \
	(p)->prev_task = init_task.prev_task; \
	init_task.prev_task->next_task = (p); \
	init_task.prev_task = (p); \
	restore_flags(flags); \
	(p)->p_ysptr = NULL; \
	if (((p)->p_osptr = (p)->p_pptr->p_cptr) != NULL) \
		(p)->p_osptr->p_ysptr = p; \
	(p)->p_pptr->p_cptr = p; \
	} while (0)

#define for_each_task(p) \
	for (p = &init_task ; (p = p->next_task) != &init_task ; )

/*
 * This is the ldt that every process will get unless we need
 * something other than this.
 */
extern struct desc_struct default_ldt;

/* This special macro can be used to load a debugging register */

#define loaddebug(register) \
		__asm__("movl %0,%%edx\n\t" \
			"movl %%edx,%%db" #register "\n\t" \
			: /* no output */ \
			:"m" (current->debugreg[register]) \
			:"dx");

#endif
