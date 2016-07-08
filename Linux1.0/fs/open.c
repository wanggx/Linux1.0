/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/vfs.h>
#include <linux/types.h>
#include <linux/utime.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/tty.h>
#include <linux/time.h>

#include <asm/segment.h>

extern void fcntl_remove_locks(struct task_struct *, struct file *, unsigned int fd);

asmlinkage int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

asmlinkage int sys_statfs(const char * path, struct statfs * buf)
{
	struct inode * inode;
	int error;

	error = verify_area(VERIFY_WRITE, buf, sizeof(struct statfs));
	if (error)
		return error;
	error = namei(path,&inode);
	if (error)
		return error;
	if (!inode->i_sb->s_op->statfs) {
		iput(inode);
		return -ENOSYS;
	}
	inode->i_sb->s_op->statfs(inode->i_sb, buf);
	iput(inode);
	return 0;
}

asmlinkage int sys_fstatfs(unsigned int fd, struct statfs * buf)
{
	struct inode * inode;
	struct file * file;
	int error;

	error = verify_area(VERIFY_WRITE, buf, sizeof(struct statfs));
	if (error)
		return error;
	if (fd >= NR_OPEN || !(file = current->filp[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if (!inode->i_sb->s_op->statfs)
		return -ENOSYS;
	inode->i_sb->s_op->statfs(inode->i_sb, buf);
	return 0;
}

asmlinkage int sys_truncate(const char * path, unsigned int length)
{
	struct inode * inode;
	int error;

	error = namei(path,&inode);
	if (error)
		return error;
	if (S_ISDIR(inode->i_mode) || !permission(inode,MAY_WRITE)) {
		iput(inode);
		return -EACCES;
	}
	if (IS_RDONLY(inode)) {
		iput(inode);
		return -EROFS;
	}
	inode->i_size = length;
	if (inode->i_op && inode->i_op->truncate)
		inode->i_op->truncate(inode);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	inode->i_dirt = 1;
	error = notify_change(NOTIFY_SIZE, inode);
	iput(inode);
	return error;
}

asmlinkage int sys_ftruncate(unsigned int fd, unsigned int length)
{
	struct inode * inode;
	struct file * file;

	if (fd >= NR_OPEN || !(file = current->filp[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if (S_ISDIR(inode->i_mode) || !(file->f_mode & 2))
		return -EACCES;
	inode->i_size = length;
	if (inode->i_op && inode->i_op->truncate)
		inode->i_op->truncate(inode);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	inode->i_dirt = 1;
	return notify_change(NOTIFY_SIZE, inode);
}

/* If times==NULL, set access and modification to current time,
 * must be owner or have write permission.
 * Else, update from *times, must be owner or super user.
 */
asmlinkage int sys_utime(char * filename, struct utimbuf * times)
{
	struct inode * inode;
	long actime,modtime;
	int error;

	error = namei(filename,&inode);
	if (error)
		return error;
	if (IS_RDONLY(inode)) {
		iput(inode);
		return -EROFS;
	}
	if (times) {
		if ((current->euid != inode->i_uid) && !suser()) {
			iput(inode);
			return -EPERM;
		}
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
		inode->i_ctime = CURRENT_TIME;
	} else {
		if ((current->euid != inode->i_uid) &&
		    !permission(inode,MAY_WRITE)) {
			iput(inode);
			return -EACCES;
		}
		actime = modtime = inode->i_ctime = CURRENT_TIME;
	}
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	error = notify_change(NOTIFY_TIME, inode);
	iput(inode);
	return error;
}

/*
 * XXX we should use the real ids for checking _all_ components of the
 * path.  Now we only use them for the final component of the path.
 */

/* 判断文件的访问权限
  */
asmlinkage int sys_access(const char * filename,int mode)
{
	struct inode * inode;
	int res, i_mode;

	/*判断mode前三位的有效性*/
	if (mode != (mode & S_IRWXO))	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;
	res = namei(filename,&inode);
	if (res)
		return res;
	i_mode = inode->i_mode;
	res = i_mode & S_IRWXUGO;
	if (current->uid == inode->i_uid)
		res >>= 6;		/* needs cleaning? */
	else if (in_group_p(inode->i_gid))
		res >>= 3;		/* needs cleaning? */
	iput(inode);
	if ((res & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 *
	 * XXX nope.  suser() is inappropriate and swapping the ids while
	 * decomposing the path would be racy.
	 */
	if ((!current->uid) &&
	    (S_ISDIR(i_mode) || !(mode & S_IXOTH) || (i_mode & S_IXUGO)))
		return 0;
	return -EACCES;
}


/* 更改当前进程的工作目录 */
asmlinkage int sys_chdir(const char * filename)
{
	struct inode * inode;
	int error;

	error = namei(filename,&inode);
	if (error)
		return error;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	if (!permission(inode,MAY_EXEC)) {
		iput(inode);
		return -EACCES;
	}
	iput(current->pwd);
	current->pwd = inode;
	return (0);
}

asmlinkage int sys_fchdir(unsigned int fd)
{
	struct inode * inode;
	struct file * file;

	if (fd >= NR_OPEN || !(file = current->filp[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	if (!permission(inode,MAY_EXEC))
		return -EACCES;
	iput(current->pwd);
	current->pwd = inode;
	inode->i_count++;
	return (0);
}

/* 更改进程的root目录，默认的访问/etc,则是真正的系统/etc目录，
 * 如果使用该函数更改成/home/demo,则访问/etc目录则实际是/home/demo/etc
 * 想象一下ftp的根目录
 */
asmlinkage int sys_chroot(const char * filename)
{
	struct inode * inode;
	int error;

	error = namei(filename,&inode);
	if (error)
		return error;

    /* 不是目录则失败返回 */
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	if (!suser()) {
		iput(inode);
		return -EPERM;
	}
	iput(current->root);
	current->root = inode;
	return (0);
}

/* 更改文件的mode */
asmlinkage int sys_fchmod(unsigned int fd, mode_t mode)
{
	struct inode * inode;
	struct file * file;

	if (fd >= NR_OPEN || !(file = current->filp[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if ((current->euid != inode->i_uid) && !suser())
		return -EPERM;
    /* 判断文件系统是不是只读的 */
	if (IS_RDONLY(inode))
		return -EROFS;
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	inode->i_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	if (!suser() && !in_group_p(inode->i_gid))
		inode->i_mode &= ~S_ISGID;
	inode->i_ctime = CURRENT_TIME;
	inode->i_dirt = 1;
	return notify_change(NOTIFY_MODE, inode);
}

/* 改变文件或目录的操作权限，在Linux中目录是一种特殊的文件 */
asmlinkage int sys_chmod(const char * filename, mode_t mode)
{
	struct inode * inode;
	int error;

	/* 找到路径对应文件的inode */
	error = namei(filename,&inode);
	if (error)
		return error;
	/* 只有文件属主的进程或者超级用户才可以修改 */
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EPERM;
	}
	if (IS_RDONLY(inode)) {
		iput(inode);
		return -EROFS;
	}
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	inode->i_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	if (!suser() && !in_group_p(inode->i_gid))
		inode->i_mode &= ~S_ISGID;
	/* 修改inode的修改时间和脏标记，以便回写数据 */
	inode->i_ctime = CURRENT_TIME;
	inode->i_dirt = 1;
	error = notify_change(NOTIFY_MODE, inode);
	iput(inode);
	return error;
}

asmlinkage int sys_fchown(unsigned int fd, uid_t user, gid_t group)
{
	struct inode * inode;
	struct file * file;

	if (fd >= NR_OPEN || !(file = current->filp[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if (IS_RDONLY(inode))
		return -EROFS;
	if (user == (uid_t) -1)
		user = inode->i_uid;
	if (group == (gid_t) -1)
		group = inode->i_gid;
	if ((current->euid == inode->i_uid && user == inode->i_uid &&
	     (in_group_p(group) || group == inode->i_gid)) ||
	    suser()) {
		inode->i_uid = user;
		inode->i_gid = group;
		inode->i_ctime = CURRENT_TIME;
		inode->i_dirt = 1;
		return notify_change(NOTIFY_UIDGID, inode);
	}
	return -EPERM;
}

/* 更改文件属主 */
asmlinkage int sys_chown(const char * filename, uid_t user, gid_t group)
{
	struct inode * inode;
	int error;

	error = lnamei(filename,&inode);
	if (error)
		return error;
	if (IS_RDONLY(inode)) {
		iput(inode);
		return -EROFS;
	}
	if (user == (uid_t) -1)
		user = inode->i_uid;
	if (group == (gid_t) -1)
		group = inode->i_gid;
	if ((current->euid == inode->i_uid && user == inode->i_uid &&
	     (in_group_p(group) || group == inode->i_gid)) ||
	    suser()) {
		inode->i_uid = user;
		inode->i_gid = group;
		inode->i_ctime = CURRENT_TIME;
		inode->i_dirt = 1;
		error = notify_change(NOTIFY_UIDGID, inode);
		iput(inode);
		return error;
	}
	iput(inode);
	return -EPERM;
}

/*
 * Note that while the flag value (low two bits) for sys_open means:
 *	00 - read-only
 *	01 - write-only
 *	10 - read-write
 *	11 - special
 * it is changed into
 *	00 - no permissions needed
 *	01 - read-permission
 *	10 - write-permission
 *	11 - read-write
 * for the internal routines (ie open_namei()/follow_link() etc). 00 is
 * used by symlinks.
 */
int do_open(const char * filename,int flags,int mode)
{
	struct inode * inode;
	struct file * f;
	int flag,error,fd;

	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
	if (fd>=NR_OPEN)
		return -EMFILE;
	FD_CLR(fd,&current->close_on_exec);
	f = get_empty_filp();
	if (!f)
		return -ENFILE;
	current->filp[fd] = f;
	f->f_flags = flag = flags;
	f->f_mode = (flag+1) & O_ACCMODE;
	if (f->f_mode)
		flag++;
	if (flag & (O_TRUNC | O_CREAT))
		flag |= 2;
	/* 通过一个路径filename来打开一个文件，并获取文件的inode
	 */
	error = open_namei(filename,flag,mode,&inode,NULL);
	if (error) {
		current->filp[fd]=NULL;
		f->f_count--;
		return error;
	}

	/* 在open函数当中，先通过open_namei函数获取对应路径文件的inode
	 * 在获取的inode当中会有f_op的操作符，Linux的文件系统是根据所要
	 * 操作文件的类型来确定文件操作的f_op和i_op，因为Linux支持的文件系统众多
	 * 每种文件系统的f_op和i_op都有特定的实现，文件系统具体函数的实现在相应的
	 * 文件夹当中，如ext2文件系统的实现在/fs/ext2/当中
	 */
	f->f_inode = inode;
	f->f_pos = 0;
	f->f_reada = 0;
	f->f_op = NULL;
        /* 根据inode来设置file的f_op指针 */
	if (inode->i_op)
		f->f_op = inode->i_op->default_file_ops;
	if (f->f_op && f->f_op->open) {
		error = f->f_op->open(inode,f);
		if (error) {
			iput(inode);
			f->f_count--;
			current->filp[fd]=NULL;
			return error;
		}
	}
	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);
	return (fd);
}

/* flag为打开文件方式，如可读，可写，可读写，
 * 如果文件没有则会去创建一个新的文件
 * mode中有在创建新的文件时候才起作用，也就是确定新建文件的权限
 */
asmlinkage int sys_open(const char * filename,int flags,int mode)
{
	char * tmp;
	int error;

	error = getname(filename, &tmp);
	if (error)
		return error;
	error = do_open(tmp,flags,mode);
	putname(tmp);
	return error;
}

asmlinkage int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/* 在关闭的时候会考虑两个比较的重要的东西，
 * f_count和i_count
 */
int close_fp(struct file *filp, unsigned int fd)
{
	struct inode *inode;

	if (filp->f_count == 0) {
		printk("VFS: Close: file count is 0\n");
		return 0;
	}
	inode = filp->f_inode;
	if (inode && S_ISREG(inode->i_mode))
		fcntl_remove_locks(current, filp, fd);
	/*如果引用计数大于1,则减小一个即可*/
	if (filp->f_count > 1) {
		filp->f_count--;
		return 0;
	}
	if (filp->f_op && filp->f_op->release)
		filp->f_op->release(inode,filp);
	/* 设置struct file中的f_count和f_inode指针，
	 * 此处只是斩断了struct file和struct inode的关系，
	 * f_inode并不一定就释放了。
	 */
	filp->f_count--;
	filp->f_inode = NULL;
	iput(inode);
	return 0;
}

/* 关闭fd描述符指向的文件，同时将对应的struct file指针设置为NULL */
asmlinkage int sys_close(unsigned int fd)
{	
	struct file * filp;

	if (fd >= NR_OPEN)
		return -EBADF;
	FD_CLR(fd, &current->close_on_exec);
	if (!(filp = current->filp[fd]))
		return -EBADF;
	current->filp[fd] = NULL;
	return (close_fp (filp, fd));
}

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */

/* 挂起当前中断 */
asmlinkage int sys_vhangup(void)
{
	struct tty_struct *tty;

	if (!suser())
		return -EPERM;
	/* See if there is a controlling tty. */
	if (current->tty < 0)
		return 0;
	tty = TTY_TABLE(MINOR(current->tty));
	tty_vhangup(tty);
	return 0;
}
