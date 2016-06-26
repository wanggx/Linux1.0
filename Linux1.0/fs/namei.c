/*
 *  linux/fs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/stat.h>

#define ACC_MODE(x) ("\000\004\002\006"[(x)&O_ACCMODE])

/*
 * In order to reduce some races, while at the same time doing additional
 * checking and hopefully speeding things up, we copy filenames to the
 * kernel data space before using them..
 *
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 */

/* 从内核中申请一页的内存用来存放filename中的数据
 */
int getname(const char * filename, char **result)
{
	int error;
	unsigned long i, page;
	char * tmp, c;

	/* 将用户空间映射到内核空间*/
	i = (unsigned long) filename;
	if (!i || i >= TASK_SIZE)
		return -EFAULT;
	i = TASK_SIZE - i;
	error = -EFAULT;
	if (i > PAGE_SIZE) {
		i = PAGE_SIZE;
		error = -ENAMETOOLONG;
	}
	c = get_fs_byte(filename++);
	if (!c)
		return -ENOENT;
	if(!(page = __get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	*result = tmp = (char *) page;
	while (--i) {
		*(tmp++) = c;
		c = get_fs_byte(filename++);
		if (!c) {
			*tmp = '\0';
			return 0;
		}
	}
	free_page(page);
	return error;
}

/* 将name地址所在的一页内存给释放掉
 */
void putname(char * name)
{
	free_page((unsigned long) name);
}

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
int permission(struct inode * inode,int mask)
{
	int mode = inode->i_mode;

	if (inode->i_op && inode->i_op->permission)
		return inode->i_op->permission(inode, mask);
	else if (current->euid == inode->i_uid)
		mode >>= 6;
	else if (in_group_p(inode->i_gid))
		mode >>= 3;
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * lookup() looks up one part of a pathname, using the fs-dependent
 * routines (currently minix_lookup) for it. It also checks for
 * fathers (pseudo-roots, mount-points)
 */


/* 在dir对应的目录当中查找文件名为name,且长度为len的文件或目录的inode
 */
int lookup(struct inode * dir,const char * name, int len,
	struct inode ** result)
{
	struct super_block * sb;
	int perm;

	*result = NULL;
	if (!dir)
		return -ENOENT;
/* check permissions before traversing mount-points */
	perm = permission(dir,MAY_EXEC);
	if (len==2 && name[0] == '.' && name[1] == '.') {
		if (dir == current->root) {
			*result = dir;
			return 0;
		} else if ((sb = dir->i_sb) && (dir == sb->s_mounted)) {
			sb = dir->i_sb;
			iput(dir);
			dir = sb->s_covered;
			if (!dir)
				return -ENOENT;
			dir->i_count++;
		}
	}
	/* 判断不是一个目录，目录也是一种特殊的文件
	 */
	if (!dir->i_op || !dir->i_op->lookup) {
		iput(dir);
		return -ENOTDIR;
	}
 	if (!perm) {
		iput(dir);
		return -EACCES;
	}
	if (!len) {
		*result = dir;
		return 0;
	}
	return dir->i_op->lookup(dir,name,len,result);
}

int follow_link(struct inode * dir, struct inode * inode,
	int flag, int mode, struct inode ** res_inode)
{
	if (!dir || !inode) {
		iput(dir);
		iput(inode);
		*res_inode = NULL;
		return -ENOENT;
	}
	/* 如果follow_link为空，则释放dir，
	 * 同时将res_inode指向inode,有一种情况可能就是目录的符号连接
	 * 当读取到目录符号连接时，则回继续转到真正的目录下面
	 */
	if (!inode->i_op || !inode->i_op->follow_link) {
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	return inode->i_op->follow_link(dir,inode,flag,mode,res_inode);
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */

/* pathname路径       /usr/local/test.txt
 * namelen文件名长度  strlen("test.txt")
 * name      test.txt
 * base      文件系统的根目录，或当前进程的启动目录的i节点
 * res_inode 找到的目录文件的inode，注意这里是目录文件的inode
 */
static int dir_namei(const char * pathname, int * namelen, const char ** name,
	struct inode * base, struct inode ** res_inode)
{
	char c;
	const char * thisname;
	int len,error;
	struct inode * inode;

	*res_inode = NULL;
	if (!base) {
		base = current->pwd;
		base->i_count++;
	}
	if ((c = *pathname) == '/') {
		iput(base);
		base = current->root;
		pathname++;
		base->i_count++;
	}
	while (1) {
		thisname = pathname;
		/* 以'/'为分割字符，分割路径
		 * 在这里只会处理目录，如给出以下路径/usr/local/test,
		 * 以‘/’为根目录寻找下一个目录usr,在以usr为父目录寻找到
		 * local子目录，再当以local为父目录查找时，此时的c等于0，
		 * 并不会继续查找下去，此时就会返回local目录的inode
		 */
		for(len=0;(c = *(pathname++))&&(c != '/');len++)
			/* nothing */ ;
		if (!c)
			break;
		/*增加引用计数，如果不增加，则可能在当前进程被切换出去之后，把该inode给释放掉了*/
		base->i_count++;
		error = lookup(base,thisname,len,&inode);
		if (error) {
			iput(base);
			return error;
		}
		/* 注意这句非常重要，更改base和inode之间的关键
		 * 将找到的inode设置为base，然后下一轮循环继续
		 * 在base的目录下面寻找
		 */
		error = follow_link(base,inode,0,0,&base);
		if (error)
			return error;
	}
	if (!base->i_op || !base->i_op->lookup) {
		iput(base);
		return -ENOTDIR;
	}
	/*记录最后的文件名称、长度和文件所在的目录的inode*/
	*name = thisname;
	*namelen = len;
	*res_inode = base;
	return 0;
}


/* pathname代表文件路径
 * base代表是从哪个inode下面开始检查路径
 * follow_links代表如果是链接，是否继续读取链接真正指向的文件
 * res_inode表示最终路径查找成功后的inode
 */
static int _namei(const char * pathname, struct inode * base,
	int follow_links, struct inode ** res_inode)
{
	const char * basename;
	int namelen,error;
	struct inode * inode;

	*res_inode = NULL;
	error = dir_namei(pathname,&namelen,&basename,base,&base);
	if (error)
		return error;
	base->i_count++;	/* lookup uses up base */
	error = lookup(base,basename,namelen,&inode);
	if (error) {
		iput(base);
		return error;
	}
        /* 如果跟踪连接 */
	if (follow_links) {
		error = follow_link(base,inode,0,0,&inode);
		if (error)
			return error;
	} else
		iput(base);
	*res_inode = inode;
	return 0;
}

/* 不跟踪连接 */
int lnamei(const char * pathname, struct inode ** res_inode)
{
	int error;
	char * tmp;

	error = getname(pathname,&tmp);
	if (!error) {
		error = _namei(tmp,NULL,0,res_inode);
		putname(tmp);
	}
	return error;
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
/* 打开一个路径对应的inode，如果该路径中存在软连接，
  * 则继续下去，可以和上面函数对比 
  */
int namei(const char * pathname, struct inode ** res_inode)
{
	int error;
	char * tmp;

	error = getname(pathname,&tmp);
	if (!error) {
		error = _namei(tmp,NULL,1,res_inode);
		putname(tmp);
	}
	return error;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 *
 * Note that the low bits of "flag" aren't the same as in the open
 * system call - they are 00 - no permissions needed
 *			  01 - read permission needed
 *			  10 - write permission needed
 *			  11 - read/write permissions needed
 * which is a lot more logical, and also allows the "no perm" needed
 * for symlinks (where the permissions are checked later).
 */

/* 获取路径所对应文件的inode，将查找到的结果inode存放在res_inode当中，
 * base表示从哪个目录下面查找，一般会设置为NULL,具体从哪个目录下面查找会
 * 根据pathname来决定
 */
int open_namei(const char * pathname, int flag, int mode,
	struct inode ** res_inode, struct inode * base)
{
	const char * basename;
	int namelen,error;
	struct inode * dir, *inode;
	struct task_struct ** p;

	mode &= S_IALLUGO & ~current->umask;
	mode |= S_IFREG;
	/* 获取文件所在的目录的inode和文件名称、长度 */
	error = dir_namei(pathname,&namelen,&basename,base,&dir);
	if (error)
		return error;
	if (!namelen) {			/* special case: '/usr/' etc */
		if (flag & 2) {
			iput(dir);
			return -EISDIR;
		}
		/* thanks to Paul Pluzhnikov for noticing this was missing.. */
		if (!permission(dir,ACC_MODE(flag))) {
			iput(dir);
			return -EACCES;
		}
		*res_inode=dir;
		return 0;
	}
	dir->i_count++;		/* lookup eats the dir */
	if (flag & O_CREAT) {
		down(&dir->i_sem);
		error = lookup(dir,basename,namelen,&inode);
		if (!error) {
			if (flag & O_EXCL) {
				iput(inode);
				error = -EEXIST;
			}
		} else if (!permission(dir,MAY_WRITE | MAY_EXEC))
			error = -EACCES;
		else if (!dir->i_op || !dir->i_op->create)
			error = -EACCES;
		else if (IS_RDONLY(dir))
			error = -EROFS;
		else {
			dir->i_count++;		/* create eats the dir */
			error = dir->i_op->create(dir,basename,namelen,mode,res_inode);
			up(&dir->i_sem);
			iput(dir);
			return error;
		}
		up(&dir->i_sem);
	} else
		error = lookup(dir,basename,namelen,&inode);
	if (error) {
		iput(dir);
		return error;
	}
	error = follow_link(dir,inode,flag,mode,&inode);
	if (error)
		return error;
	if (S_ISDIR(inode->i_mode) && (flag & 2)) {
		iput(inode);
		return -EISDIR;
	}
	if (!permission(inode,ACC_MODE(flag))) {
		iput(inode);
		return -EACCES;
	}
	if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode)) {
		if (IS_NODEV(inode)) {
			iput(inode);
			return -EACCES;
		}
	} else {
		if (IS_RDONLY(inode) && (flag & 2)) {
			iput(inode);
			return -EROFS;
		}
	}
 	if ((inode->i_count > 1) && (flag & 2)) {
 		for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		        struct vm_area_struct * mpnt;
 			if (!*p)
 				continue;
 			if (inode == (*p)->executable) {
 				iput(inode);
 				return -ETXTBSY;
 			}
			for(mpnt = (*p)->mmap; mpnt; mpnt = mpnt->vm_next) {
				if (mpnt->vm_page_prot & PAGE_RW)
					continue;
				if (inode == mpnt->vm_inode) {
					iput(inode);
					return -ETXTBSY;
				}
			}
 		}
 	}
	if (flag & O_TRUNC) {
	      inode->i_size = 0;
	      if (inode->i_op && inode->i_op->truncate)
	           inode->i_op->truncate(inode);
	      if ((error = notify_change(NOTIFY_SIZE, inode))) {
		   iput(inode);
		   return error;
	      }
	      inode->i_dirt = 1;
	}
	*res_inode = inode;
	return 0;
}

int do_mknod(const char * filename, int mode, dev_t dev)
{
	const char * basename;
	int namelen, error;
	struct inode * dir;

	mode &= ~current->umask;
	/* 获取filename路径中目录的inode */
	error = dir_namei(filename,&namelen,&basename, NULL, &dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (IS_RDONLY(dir)) {
		iput(dir);
		return -EROFS;
	}
	if (!permission(dir,MAY_WRITE | MAY_EXEC)) {
		iput(dir);
		return -EACCES;
	}
	if (!dir->i_op || !dir->i_op->mknod) {
		iput(dir);
		return -EPERM;
	}
	/* 先占用该目录 */
	down(&dir->i_sem);
	error = dir->i_op->mknod(dir,basename,namelen,mode,dev);
	/* 用完之后就释放该目录 */
	up(&dir->i_sem);
	return error;
}

asmlinkage int sys_mknod(const char * filename, int mode, dev_t dev)
{
	int error;
	char * tmp;

	if (S_ISDIR(mode) || (!S_ISFIFO(mode) && !suser()))
		return -EPERM;
	switch (mode & S_IFMT) {
	case 0:
		mode |= S_IFREG;
		break;
	case S_IFREG: case S_IFCHR: case S_IFBLK: case S_IFIFO:
		break;
	default:
		return -EINVAL;
	}
	error = getname(filename,&tmp);
	if (!error) {
		error = do_mknod(tmp,mode,dev);
		putname(tmp);
	}
	return error;
}

static int do_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen, error;
	struct inode * dir;

	error = dir_namei(pathname,&namelen,&basename,NULL,&dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (IS_RDONLY(dir)) {
		iput(dir);
		return -EROFS;
	}
	if (!permission(dir,MAY_WRITE | MAY_EXEC)) {
		iput(dir);
		return -EACCES;
	}
	if (!dir->i_op || !dir->i_op->mkdir) {
		iput(dir);
		return -EPERM;
	}
	down(&dir->i_sem);
	error = dir->i_op->mkdir(dir,basename,namelen,mode);
	up(&dir->i_sem);
	return error;
}

asmlinkage int sys_mkdir(const char * pathname, int mode)
{
	int error;
	char * tmp;

	error = getname(pathname,&tmp);
	if (!error) {
		error = do_mkdir(tmp,mode);
		putname(tmp);
	}
	return error;
}

static int do_rmdir(const char * name)
{
	const char * basename;
	int namelen, error;
	struct inode * dir;

	error = dir_namei(name,&namelen,&basename,NULL,&dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (IS_RDONLY(dir)) {
		iput(dir);
		return -EROFS;
	}
	if (!permission(dir,MAY_WRITE | MAY_EXEC)) {
		iput(dir);
		return -EACCES;
	}
	if (!dir->i_op || !dir->i_op->rmdir) {
		iput(dir);
		return -EPERM;
	}
	return dir->i_op->rmdir(dir,basename,namelen);
}

asmlinkage int sys_rmdir(const char * pathname)
{
	int error;
	char * tmp;

	error = getname(pathname,&tmp);
	if (!error) {
		error = do_rmdir(tmp);
		putname(tmp);
	}
	return error;
}

static int do_unlink(const char * name)
{
	const char * basename;
	int namelen, error;
	struct inode * dir;

	error = dir_namei(name,&namelen,&basename,NULL,&dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -EPERM;
	}
	if (IS_RDONLY(dir)) {
		iput(dir);
		return -EROFS;
	}
	if (!permission(dir,MAY_WRITE | MAY_EXEC)) {
		iput(dir);
		return -EACCES;
	}
	if (!dir->i_op || !dir->i_op->unlink) {
		iput(dir);
		return -EPERM;
	}
	return dir->i_op->unlink(dir,basename,namelen);
}

asmlinkage int sys_unlink(const char * pathname)
{
	int error;
	char * tmp;

	error = getname(pathname,&tmp);
	if (!error) {
		error = do_unlink(tmp);
		putname(tmp);
	}
	return error;
}

static int do_symlink(const char * oldname, const char * newname)
{
	struct inode * dir;
	const char * basename;
	int namelen, error;

	error = dir_namei(newname,&namelen,&basename,NULL,&dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (IS_RDONLY(dir)) {
		iput(dir);
		return -EROFS;
	}
	if (!permission(dir,MAY_WRITE | MAY_EXEC)) {
		iput(dir);
		return -EACCES;
	}
	if (!dir->i_op || !dir->i_op->symlink) {
		iput(dir);
		return -EPERM;
	}
	down(&dir->i_sem);
	error = dir->i_op->symlink(dir,basename,namelen,oldname);
	up(&dir->i_sem);
	return error;
}

asmlinkage int sys_symlink(const char * oldname, const char * newname)
{
	int error;
	char * from, * to;

	error = getname(oldname,&from);
	if (!error) {
		error = getname(newname,&to);
		if (!error) {
			error = do_symlink(from,to);
			putname(to);
		}
		putname(from);
	}
	return error;
}

/* 将oldinode节点对应的文件链接到newname指向的路径
 */
static int do_link(struct inode * oldinode, const char * newname)
{
	struct inode * dir;
	const char * basename;
	int namelen, error;

	error = dir_namei(newname,&namelen,&basename,NULL,&dir);
	if (error) {
		iput(oldinode);
		return error;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	if (IS_RDONLY(dir)) {
		iput(oldinode);
		iput(dir);
		return -EROFS;
	}
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE | MAY_EXEC)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
	if (!dir->i_op || !dir->i_op->link) {
		iput(dir);
		iput(oldinode);
		return -EPERM;
	}
	down(&dir->i_sem);
	error = dir->i_op->link(oldinode, dir, basename, namelen);
	up(&dir->i_sem);
	return error;
}

/* 文件链接的函数，软链接，硬链接
 */
asmlinkage int sys_link(const char * oldname, const char * newname)
{
	int error;
	char * to;
	struct inode * oldinode;

	error = namei(oldname, &oldinode);
	if (error)
		return error;
	error = getname(newname,&to);
	if (error) {
		iput(oldinode);
		return error;
	}
	error = do_link(oldinode,to);
	putname(to);
	return error;
}

static int do_rename(const char * oldname, const char * newname)
{
	struct inode * old_dir, * new_dir;
	const char * old_base, * new_base;
	int old_len, new_len, error;

	error = dir_namei(oldname,&old_len,&old_base,NULL,&old_dir);
	if (error)
		return error;
	if (!permission(old_dir,MAY_WRITE | MAY_EXEC)) {
		iput(old_dir);
		return -EACCES;
	}
	if (!old_len || (old_base[0] == '.' &&
	    (old_len == 1 || (old_base[1] == '.' &&
	     old_len == 2)))) {
		iput(old_dir);
		return -EPERM;
	}
	error = dir_namei(newname,&new_len,&new_base,NULL,&new_dir);
	if (error) {
		iput(old_dir);
		return error;
	}
	if (!permission(new_dir,MAY_WRITE | MAY_EXEC)) {
		iput(old_dir);
		iput(new_dir);
		return -EACCES;
	}
	if (!new_len || (new_base[0] == '.' &&
	    (new_len == 1 || (new_base[1] == '.' &&
	     new_len == 2)))) {
		iput(old_dir);
		iput(new_dir);
		return -EPERM;
	}
	if (new_dir->i_dev != old_dir->i_dev) {
		iput(old_dir);
		iput(new_dir);
		return -EXDEV;
	}
	if (IS_RDONLY(new_dir) || IS_RDONLY(old_dir)) {
		iput(old_dir);
		iput(new_dir);
		return -EROFS;
	}
	if (!old_dir->i_op || !old_dir->i_op->rename) {
		iput(old_dir);
		iput(new_dir);
		return -EPERM;
	}
	down(&new_dir->i_sem);
	error = old_dir->i_op->rename(old_dir, old_base, old_len, 
		new_dir, new_base, new_len);
	up(&new_dir->i_sem);
	return error;
}

asmlinkage int sys_rename(const char * oldname, const char * newname)
{
	int error;
	char * from, * to;

	error = getname(oldname,&from);
	if (!error) {
		error = getname(newname,&to);
		if (!error) {
			error = do_rename(from,to);
			putname(to);
		}
		putname(from);
	}
	return error;
}
