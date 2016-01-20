/*
 * This file contains the procedures for the handling of select
 *
 * Created for Linux based loosely upon Mathius Lattner's minix
 * patches by Peter MacDonald. Heavily edited by Linus.
 */

#include <linux/types.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/signal.h>
#include <linux/errno.h>

#include <asm/segment.h>
#include <asm/system.h>

#define ROUND_UP(x,y) (((x)+(y)-1)/(y))

/*
 * Ok, Peter made a complicated, but straightforward multiple_wait() function.
 * I have rewritten this, taking some shortcuts: This code may not be easy to
 * follow, but it should be free of race-conditions, and it's practical. If you
 * understand what I'm doing here, then you understand how the linux
 * sleep/wakeup mechanism works.
 *
 * Two very simple procedures, select_wait() and free_wait() make all the work.
 * select_wait() is a inline-function defined in <linux/sched.h>, as all select
 * functions have to call it to add an entry to the select table.
 */

/*
 * I rewrote this again to make the select_table size variable, take some
 * more shortcuts, improve responsiveness, and remove another race that
 * Linus noticed.  -- jrs
 */

static void free_wait(select_table * p)
{
	struct select_table_entry * entry = p->entry + p->nr;

	while (p->nr > 0) {
		p->nr--;
		entry--;
		/* entry中的wait从实际的等待链表中给删除 */
		remove_wait_queue(entry->wait_address,&entry->wait);
	}
}

/*
 * The check function checks the ready status of a file using the vfs layer.
 *
 * If the file was not ready we were added to its wait queue.  But in
 * case it became ready just after the check and just before it called
 * select_wait, we call it again, knowing we are already on its
 * wait queue this time.  The second call is not necessary if the
 * select_table is NULL indicating an earlier file check was ready
 * and we aren't going to sleep on the select_table.  -- jrs
 */

/* 检查文件标记 
 */
static int check(int flag, select_table * wait, struct file * file)
{
	struct inode * inode;
	struct file_operations *fops;
	int (*select) (struct inode *, struct file *, int, select_table *);

	inode = file->f_inode;
	if ((fops = file->f_op) && (select = fops->select))
		return select(inode, file, flag, wait)
		    || (wait && select(inode, file, flag, NULL));
	/* 普通文件肯定是可以被读写的 */
	if (S_ISREG(inode->i_mode))
		return 1;
	return 0;
}

/* 所有文件描述符的范围，一般是最大的文件描述符加1，
 * 在in,out，ex三个参数中返回的是检测到变动的文件句柄
 * 函数返回值代表检测到变动的数量 
 */
int do_select(int n, fd_set *in, fd_set *out, fd_set *ex,
	fd_set *res_in, fd_set *res_out, fd_set *res_ex)
{
	int count;
	select_table wait_table, *wait;
	struct select_table_entry *entry;
	unsigned long set;
	int i,j;
	int max = -1;

	/* 循环检查每个文件集合的位 */
	for (j = 0 ; j < __FDSET_LONGS ; j++) {
		/* 一个unsigned long能够表示32个文件描述符，32=2的5次方*/
		i = j << 5;
		/* 超过最大文件描述符，则停止 */
		if (i >= n)
			break;
		set = in->fds_bits[j] | out->fds_bits[j] | ex->fds_bits[j];
		/* set移动8次就等于0了 */
		for ( ; set ; i++,set >>= 1) {
			if (i >= n)
				goto end_check;
			/* 测试集合中的最后一位 */
			if (!(set & 1))
				continue;
			if (!current->filp[i])
				return -EBADF;
			if (!current->filp[i]->f_inode)
				return -EBADF;
			/* 记录最大的文件描述符 */
			max = i;
		}
	}
end_check:
	/* 记录实际监视的文件描述符的最大值+1 */
	n = max + 1;

	/* 获取占用一页大小的select_table_entry内存 */
	if(!(entry = (struct select_table_entry*) __get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	FD_ZERO(res_in);
	FD_ZERO(res_out);
	FD_ZERO(res_ex);
	count = 0;
	/* 初始化等待列表 */
	wait_table.nr = 0;
	wait_table.entry = entry;
	wait = &wait_table;
repeat:
	current->state = TASK_INTERRUPTIBLE;
	/* 循环扫描所有监视的文件描述符，注意这里的wait第一次检测到有文件变动之前传入check函数不为NULL,
	 * 之后就都为NULL,因为每一个网络文件描述符都对应一个唯一的struct sock，当前进程只需要在struct sock
	 * 的sleep中等待一次就够了。
	 */
	for (i = 0 ; i < n ; i++) {
		if (FD_ISSET(i,in) && check(SEL_IN,wait,current->filp[i])) {
			FD_SET(i, res_in);
			count++;
			wait = NULL; /* 此处设置为NULL,就代表已经检测到了一个文件变动，所以不需要在等待在其他文件上
			                  * 在if(!count-----)判断时就可以快速返回 
			                  */
		}
		if (FD_ISSET(i,out) && check(SEL_OUT,wait,current->filp[i])) {
			FD_SET(i, res_out);
			count++;
			wait = NULL;
		}
		if (FD_ISSET(i,ex) && check(SEL_EX,wait,current->filp[i])) {
			FD_SET(i, res_ex);
			count++;
			wait = NULL;
		}
	}
	/* 所有的文件被扫描一次过后，就已经添加到struct sock中的sleep当中，
	 * 如果wait不为NULL，则会继续添加一次 
	 */
	wait = NULL;
	/* 注意这里的阻塞道理，经过一轮扫描过后，一旦监视到文件有变动，就立即返回
	 * 在第三个判断条件当中，如果当前进程接收到了信号，并且没有
	 * 被阻塞，则select函数不会继续阻塞了，因为该函数是在内核态当中，
	 * 因为信号的处理函数是在内核态返回到用户态的时候执行的，所以为了
	 * 尽快响应信号，就让进程从该函数当中快速退出 
	 */
	if (!count && current->timeout && !(current->signal & ~current->blocked)) {
		/* 此时当前进程已经添加到所有strut sock中的sleep链表当中，主动放弃cpu */
		schedule();
		goto repeat;
	}
	/* 将当前进程从所有已经添加到struct sock的sleep链表中删除 */
	free_wait(&wait_table);
	free_page((unsigned long) entry);
	current->state = TASK_RUNNING;
	return count;
}

/*
 * We do a VERIFY_WRITE here even though we are only reading this time:
 * we'll write to it eventually..
 */

/* 将fs_pointer数据拷贝到fdset为地址的内存当中
 */
static int __get_fd_set(int nr, unsigned long * fs_pointer, unsigned long * fdset)
{
	int error;

	FD_ZERO(fdset);
	if (!fs_pointer)
		return 0;
	error = verify_area(VERIFY_WRITE,fs_pointer,sizeof(fd_set));
	if (error)
		return error;
	/* 注意一个fdset占用32字节，每个字节8位，通过位来表示，
	 * 则可以表示32*8个文件描述符，这个数足够表示当前系统的文件描述符的范围
	 */
	while (nr > 0) {
		*fdset = get_fs_long(fs_pointer);
		fdset++;
		fs_pointer++;
		/* 因为一个unsigned long有32位，可以表示32个文件描述符 */
		nr -= 32;
	}
	return 0;
}

/* 将fdset中的数据复制到fs_pointer地址的起始处
 */
static void __set_fd_set(int nr, unsigned long * fs_pointer, unsigned long * fdset)
{
	if (!fs_pointer)
		return;
	while (nr > 0) {
		put_fs_long(*fdset, fs_pointer);
		fdset++;
		fs_pointer++;
		nr -= 32;
	}
}

#define get_fd_set(nr,fsp,fdp) \
__get_fd_set(nr, (unsigned long *) (fsp), (unsigned long *) (fdp))

#define set_fd_set(nr,fsp,fdp) \
__set_fd_set(nr, (unsigned long *) (fsp), (unsigned long *) (fdp))

/*
 * We can actually return ERESTARTSYS insetad of EINTR, but I'd
 * like to be certain this leads to no problems. So I return
 * EINTR just for safety.
 *
 * Update: ERESTARTSYS breaks at least the xview clock binary, so
 * I'm trying ERESTARTNOHAND which restart only when you want to.
 */
asmlinkage int sys_select( unsigned long *buffer )
{
/* Perform the select(nd, in, out, ex, tv) system call. */
	int i;
	fd_set res_in, in, *inp;
	fd_set res_out, out, *outp;
	fd_set res_ex, ex, *exp;
	int n;
	struct timeval *tvp;
	unsigned long timeout;

	i = verify_area(VERIFY_READ, buffer, 20);
	if (i)
		return i;
	n = get_fs_long(buffer++);
	if (n < 0)
		return -EINVAL;
	if (n > NR_OPEN)
		n = NR_OPEN;
	/* 监视这个文件描述符集合是否可读 */
	inp = (fd_set *) get_fs_long(buffer++);
	/* 监视这个文件描述符集合是否可写 */
	outp = (fd_set *) get_fs_long(buffer++);
	exp = (fd_set *) get_fs_long(buffer++);
	tvp = (struct timeval *) get_fs_long(buffer);
	if ((i = get_fd_set(n, inp, &in)) ||
	    (i = get_fd_set(n, outp, &out)) ||
	    (i = get_fd_set(n, exp, &ex))) return i;
	timeout = ~0UL;
	if (tvp) {
		i = verify_area(VERIFY_WRITE, tvp, sizeof(*tvp));
		if (i)
			return i;
		timeout = ROUND_UP(get_fs_long((unsigned long *)&tvp->tv_usec),(1000000/HZ));
		timeout += get_fs_long((unsigned long *)&tvp->tv_sec) * HZ;
		if (timeout)
			timeout += jiffies + 1;
	}
	/* 设置当前进程的等待时间 */
	current->timeout = timeout;
	i = do_select(n, &in, &out, &ex, &res_in, &res_out, &res_ex);
	/* 记录实际等待时间 */
	if (current->timeout > jiffies)
		timeout = current->timeout - jiffies;
	else
		timeout = 0;
	current->timeout = 0;
	if (tvp) {
		put_fs_long(timeout/HZ, (unsigned long *) &tvp->tv_sec);
		timeout %= HZ;
		timeout *= (1000000/HZ);
		put_fs_long(timeout, (unsigned long *) &tvp->tv_usec);
	}
	if (i < 0)
		return i;
	if (!i && (current->signal & ~current->blocked))
		return -ERESTARTNOHAND;
	/* 将监测到的结果回写到参数对应的内存当中 */
	set_fd_set(n, inp, &res_in);
	set_fd_set(n, outp, &res_out);
	set_fd_set(n, exp, &res_ex);
	return i;
}
