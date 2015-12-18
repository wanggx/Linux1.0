/*
 *  linux/fs/fcntl.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <asm/segment.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/string.h>

extern int fcntl_getlk(unsigned int, struct flock *);
extern int fcntl_setlk(unsigned int, unsigned int, struct flock *);
extern int sock_fcntl (struct file *, unsigned int cmd, unsigned long arg);


/* 复制文件句柄
 * fd是要被复制的句柄
 * arg是从哪个文件句柄查找可以复制到的文件句柄
 * 复制成功之后会有两个(也可能是多个dup多次)
 * 对应的文件指针，而指针指向的内存区域相同，
 * 并且引用计数会加1
 */
static int dupfd(unsigned int fd, unsigned int arg)
{
	if (fd >= NR_OPEN || !current->filp[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	/*找到一个可用的fd*/
	while (arg < NR_OPEN)
		if (current->filp[arg])
			arg++;
		else
			break;
	/*如果超过进程可以打开的最大文件数，则失败*/
	if (arg >= NR_OPEN)
		return -EMFILE;
	FD_CLR(arg, &current->close_on_exec);
	(current->filp[arg] = current->filp[fd])->f_count++;
	return arg;
}

/* 指定复制后的文件描述符，如果该文件描述符已经打开，则将其关闭 */
asmlinkage int sys_dup2(unsigned int oldfd, unsigned int newfd)
{
	if (oldfd >= NR_OPEN || !current->filp[oldfd])
		return -EBADF;
	if (newfd == oldfd)
		return newfd;
	/*
	 * errno's for dup2() are slightly different than for fcntl(F_DUPFD)
	 * for historical reasons.
	 */
	if (newfd > NR_OPEN)	/* historical botch - should have been >= */
		return -EBADF;	/* dupfd() would return -EINVAL */
#if 1
	if (newfd == NR_OPEN)
		return -EBADF;	/* dupfd() does return -EINVAL and that may
				 * even be the standard!  But that is too
				 * weird for now.
				 */
#endif
	sys_close(newfd);
	return dupfd(oldfd,newfd);
}

asmlinkage int sys_dup(unsigned int fildes)
{
	return dupfd(fildes,0);
}

asmlinkage int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{	
	struct file * filp;

	if (fd >= NR_OPEN || !(filp = current->filp[fd]))
		return -EBADF;
	switch (cmd) {
		case F_DUPFD:   /* 复制一个文件描述符 */
			return dupfd(fd,arg);
		case F_GETFD:   /* 判断文件描述符是不是在进程的close_on_exec当中 */
			return FD_ISSET(fd, &current->close_on_exec);
		case F_SETFD:
			if (arg&1)
				FD_SET(fd, &current->close_on_exec);
			else
				FD_CLR(fd, &current->close_on_exec);
			return 0;
		case F_GETFL:   /* 返回文件的标记 */
			return filp->f_flags;
		case F_SETFL:   /* 设置文件标记 */
			filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
			filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
			return 0;
		case F_GETLK:
			return fcntl_getlk(fd, (struct flock *) arg);
		case F_SETLK:
			return fcntl_setlk(fd, cmd, (struct flock *) arg);
		case F_SETLKW:
			return fcntl_setlk(fd, cmd, (struct flock *) arg);
		default:
			/* sockets need a few special fcntls. */
			if (S_ISSOCK (filp->f_inode->i_mode))
			  {
			     return (sock_fcntl (filp, cmd, arg));
			  }
			return -EINVAL;
	}
}
