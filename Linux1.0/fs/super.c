/*
 *  linux/fs/super.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/locks.h>

#include <asm/system.h>
#include <asm/segment.h>

 
/*
 * The definition of file_systems that used to be here is now in
 * filesystems.c.  Now super.c contains no fs specific code.  -- jrs
 */

extern struct file_system_type file_systems[];
extern struct file_operations * get_blkfops(unsigned int);
extern struct file_operations * get_chrfops(unsigned int);

extern void wait_for_keypress(void);
extern void fcntl_init_locks(void);

extern int root_mountflags;

/* 支持超级快的数量
 */ 
struct super_block super_blocks[NR_SUPER];

static int do_remount_sb(struct super_block *sb, int flags, char * data);

/* this is initialized in init/main.c */
dev_t ROOT_DEV = 0;

/* 根据文件系统名称来获取文件系统的超级块读取函数 */
struct file_system_type *get_fs_type(char *name)
{
	int a;
	
	if (!name)
		return &file_systems[0];
	for(a = 0 ; file_systems[a].read_super ; a++)
		if (!strcmp(name,file_systems[a].name))
			return(&file_systems[a]);
	return NULL;
}

void __wait_on_super(struct super_block * sb)
{
	struct wait_queue wait = { current, NULL };

	add_wait_queue(&sb->s_wait, &wait);
repeat:
	current->state = TASK_UNINTERRUPTIBLE;
	if (sb->s_lock) {
		schedule();
		goto repeat;
	}
	remove_wait_queue(&sb->s_wait, &wait);
	current->state = TASK_RUNNING;
}

void sync_supers(dev_t dev)
{
	struct super_block * sb;

	for (sb = super_blocks + 0 ; sb < super_blocks + NR_SUPER ; sb++) {
		if (!sb->s_dev)
			continue;
		if (dev && sb->s_dev != dev)
			continue;
		wait_on_super(sb);
		if (!sb->s_dev || !sb->s_dirt)
			continue;
		if (dev && (dev != sb->s_dev))
			continue;
		if (sb->s_op && sb->s_op->write_super)
			sb->s_op->write_super(sb);
	}
}

/* 获取相应设备的超级块号
 */
static struct super_block * get_super(dev_t dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_blocks;
	while (s < NR_SUPER+super_blocks)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			/* 然后再重头开始找 */
			s = 0+super_blocks;
		} else
			s++;
	return NULL;
}

/* 释放相应设备的超级块 */
void put_super(dev_t dev)
{
	struct super_block * sb;

	if (dev == ROOT_DEV) {
		printk("VFS: Root device %d/%d: prepare for armageddon\n",
							MAJOR(dev), MINOR(dev));
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_covered) {
		printk("VFS: Mounted device %d/%d - tssk, tssk\n",
						MAJOR(dev), MINOR(dev));
		return;
	}
	if (sb->s_op && sb->s_op->put_super)
		sb->s_op->put_super(sb);
}

/* 依次 循环处理文件系统类型，将读取到的文件系统超级块添加到数组super_blocks当中
  * name表示文件系统的名称 
  * 返回读取到的文件系统超级块  
  */
static struct super_block * read_super(dev_t dev,char *name,int flags,
				       void *data, int silent)
{
	struct super_block * s;
	struct file_system_type *type;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	s = get_super(dev);
	/* 如果找到了就直接返回 */
	if (s)
		return s;
	/* 获取文件系统类型结构
	 */
	if (!(type = get_fs_type(name))) {
		printk("VFS: on device %d/%d: get_fs_type(%s) failed\n",
						MAJOR(dev), MINOR(dev), name);
		return NULL;
	}
	/* 找到一个可用的超级块 */
	for (s = 0+super_blocks ;; s++) {
		if (s >= NR_SUPER+super_blocks)
			return NULL;
		if (!s->s_dev)
			break;
	}
	s->s_dev = dev;
	s->s_flags = flags;
	/* 然后通过对应文件系统类型的超级块读取函数来读取超级块
	 */
	if (!type->read_super(s,data, silent)) {
		s->s_dev = 0;
		return NULL;
	}
	/* 读取成功之后设置超级块的设备号
	 */
	s->s_dev = dev;
	s->s_covered = NULL;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	return s;
}

/*
 * Unnamed block devices are dummy devices used by virtual
 * filesystems which don't use real block-devices.  -- jrs
 */

static char unnamed_dev_in_use[256];

static dev_t get_unnamed_dev(void)
{
	static int first_use = 0;
	int i;

	if (first_use == 0) {
		first_use = 1;
		memset(unnamed_dev_in_use, 0, sizeof(unnamed_dev_in_use));
		unnamed_dev_in_use[0] = 1; /* minor 0 (nodev) is special */
	}
	for (i = 0; i < sizeof unnamed_dev_in_use/sizeof unnamed_dev_in_use[0]; i++) {
		if (!unnamed_dev_in_use[i]) {
			unnamed_dev_in_use[i] = 1;
			return (UNNAMED_MAJOR << 8) | i;
		}
	}
	return 0;
}

static void put_unnamed_dev(dev_t dev)
{
	if (!dev)
		return;
	if (!unnamed_dev_in_use[dev]) {
		printk("VFS: put_unnamed_dev: freeing unused device %d/%d\n",
							MAJOR(dev), MINOR(dev));
		return;
	}
	unnamed_dev_in_use[dev] = 0;
}

/* 执行正在的挂载 */
static int do_umount(dev_t dev)
{
	struct super_block * sb;
	int retval;
	
        /* 如果是根设备，则进行的是从新挂载  */
	if (dev==ROOT_DEV) {
		/* Special case for "unmounting" root.  We just try to remount
		   it readonly, and sync() the device. */
		if (!(sb=get_super(dev)))
			return -ENOENT;
                /* 如果根设备不是只读的 */
		if (!(sb->s_flags & MS_RDONLY)) {
			fsync_dev(dev);
			retval = do_remount_sb(sb, MS_RDONLY, 0);
			if (retval)
				return retval;
		}
		return 0;
	}
	if (!(sb=get_super(dev)) || !(sb->s_covered))
		return -ENOENT;
	if (!sb->s_covered->i_mount)
		printk("VFS: umount(%d/%d): mounted inode has i_mount=NULL\n",
							MAJOR(dev), MINOR(dev));
	if (!fs_may_umount(dev, sb->s_mounted))
		return -EBUSY;
	sb->s_covered->i_mount = NULL;
	iput(sb->s_covered);
	sb->s_covered = NULL;
	iput(sb->s_mounted);
	sb->s_mounted = NULL;
	if (sb->s_op && sb->s_op->write_super && sb->s_dirt)
		sb->s_op->write_super(sb);
	put_super(dev);
	return 0;
}

/*
 * Now umount can handle mount points as well as block devices.
 * This is important for filesystems which use unnamed block devices.
 *
 * There is a little kludge here with the dummy_inode.  The current
 * vfs release functions only use the r_dev field in the inode so
 * we give them the info they need without using a real inode.
 * If any other fields are ever needed by any block device release
 * functions, they should be faked here.  -- jrs
 */

/* 卸载文件系统 */
asmlinkage int sys_umount(char * name)
{
	struct inode * inode;
	dev_t dev;
	int retval;
	struct inode dummy_inode;
	struct file_operations * fops;

	if (!suser())
		return -EPERM;
	retval = namei(name,&inode);
	if (retval) {
		retval = lnamei(name,&inode);
		if (retval)
			return retval;
	}
	if (S_ISBLK(inode->i_mode)) {
		dev = inode->i_rdev;
		if (IS_NODEV(inode)) {
			iput(inode);
			return -EACCES;
		}
	} else {
		if (!inode || !inode->i_sb || inode != inode->i_sb->s_mounted) {
			iput(inode);
			return -EINVAL;
		}
		dev = inode->i_sb->s_dev;
		iput(inode);
		memset(&dummy_inode, 0, sizeof(dummy_inode));
		dummy_inode.i_rdev = dev;
		inode = &dummy_inode;
	}
        /* 设备的主设备号不能超过支持的最大主设备号 */
	if (MAJOR(dev) >= MAX_BLKDEV) {
		iput(inode);
		return -ENXIO;
	}
	if (!(retval = do_umount(dev)) && dev != ROOT_DEV) {
		fops = get_blkfops(MAJOR(dev));
		if (fops && fops->release)
			fops->release(inode,NULL);
		if (MAJOR(dev) == UNNAMED_MAJOR)
			put_unnamed_dev(dev);
	}
	if (inode != &dummy_inode)
		iput(inode);
	if (retval)
		return retval;
	fsync_dev(dev);
	return 0;
}

/*
 * do_mount() does the actual mounting after sys_mount has done the ugly
 * parameter parsing. When enough time has gone by, and everything uses the
 * new mount() parameters, sys_mount() can then be cleaned up.
 *
 * We cannot mount a filesystem if it has active, used, or dirty inodes.
 * We also have to flush all inode-data for this device, as the new mount
 * might need new info.
 */
/* 挂载文件系统
  * dev表示需要挂载的文件系统的设备号
  * dir表示需要挂载的目录
  * type表示挂载类型，如nfs，proc，ext2
  */
static int do_mount(dev_t dev, const char * dir, char * type, int flags, void * data)
{
	struct inode * dir_i;
	struct super_block * sb;
	int error;

	/* 得到对应路径的inode */
	error = namei(dir,&dir_i);
	if (error)
		return error;

	/* 正常情况下，没有其他进程或之前打开该inode的文件没有关闭，则不能挂载
	  * 也就是不能因为挂载，强行终止其他操作，或者是一个已经挂载的文件系统 
	  * 不能再让其他的文件系统挂载在它的上面，或者是循环挂载
	  */
	if (dir_i->i_count != 1 || dir_i->i_mount) {
		iput(dir_i);
		return -EBUSY;
	}
	/* 挂载必须要是一个目录，当然不能挂载到一个普通文件上 */
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!fs_may_mount(dev)) {
		iput(dir_i);
		return -EBUSY;
	}
	/* 读取挂载文件系统的超级块，
	  * type表示挂载的文件系统的名称，如nfs，proc，ext2等等
	  */
	sb = read_super(dev,type,flags,data,0);
	/* s_covered不为空表示已经有一个文件系统挂载在该目录，所以不能再次挂载 */
	if (!sb || sb->s_covered) {
		iput(dir_i);
		return -EBUSY;
	}
	/* 设置被覆盖的挂载点 */
	sb->s_covered = dir_i;
	dir_i->i_mount = sb->s_mounted;
	return 0;		/* we don't iput(dir_i) - see umount */
}


/*
 * Alters the mount flags of a mounted file system. Only the mount point
 * is used as a reference - file system type and the device are ignored.
 * FS-specific mount options can't be altered by remounting.
 */

/* 重新挂载超级块 */
static int do_remount_sb(struct super_block *sb, int flags, char *data)
{
	int retval;
	
	/* If we are remounting RDONLY, make sure there are no rw files open */
	if ((flags & MS_RDONLY) && !(sb->s_flags & MS_RDONLY))
		if (!fs_may_remount_ro(sb->s_dev))
			return -EBUSY;
	if (sb->s_op && sb->s_op->remount_fs) {
		retval = sb->s_op->remount_fs(sb, &flags, data);
		if (retval)
			return retval;
	}
	sb->s_flags = (sb->s_flags & ~MS_RMT_MASK) |
		(flags & MS_RMT_MASK);
	return 0;
}

static int do_remount(const char *dir,int flags,char *data)
{
	struct inode *dir_i;
	int retval;

	retval = namei(dir,&dir_i);
	if (retval)
		return retval;
	if (dir_i != dir_i->i_sb->s_mounted) {
		iput(dir_i);
		return -EINVAL;
	}
	retval = do_remount_sb(dir_i->i_sb, flags, data);
	iput(dir_i);
	return retval;
}

static int copy_mount_options (const void * data, unsigned long *where)
{
	int i;
	unsigned long page;
	struct vm_area_struct * vma;

	*where = 0;
	if (!data)
		return 0;

	for (vma = current->mmap ; ; ) {
		if (!vma ||
		    (unsigned long) data < vma->vm_start) {
			return -EFAULT;
		}
		if ((unsigned long) data < vma->vm_end)
			break;
		vma = vma->vm_next;
	}
	i = vma->vm_end - (unsigned long) data;
	if (PAGE_SIZE <= (unsigned long) i)
		i = PAGE_SIZE-1;
	if (!(page = __get_free_page(GFP_KERNEL))) {
		return -ENOMEM;
	}
	memcpy_fromfs((void *) page,data,i);
	*where = page;
	return 0;
}

/*
 * Flags is a 16-bit value that allows up to 16 non-fs dependent flags to
 * be given to the mount() call (ie: read-only, no-dev, no-suid etc).
 *
 * data is a (void *) that can point to any structure up to
 * PAGE_SIZE-1 bytes, which can contain arbitrary fs-dependent
 * information (or be NULL).
 *
 * NOTE! As old versions of mount() didn't use this setup, the flags
 * has to have a special 16-bit magic number in the hight word:
 * 0xC0ED. If this magic word isn't present, the flags and data info
 * isn't used, as the syscall assumes we are talking to an older
 * version that didn't understand them.
 */
asmlinkage int sys_mount(char * dev_name, char * dir_name, char * type,
	unsigned long new_flags, void * data)
{
	struct file_system_type * fstype;
	struct inode * inode;
	struct file_operations * fops;
	dev_t dev;
	int retval;
	char * t;
	unsigned long flags = 0;
	unsigned long page = 0;

	if (!suser())
		return -EPERM;
	if ((new_flags &
	     (MS_MGC_MSK | MS_REMOUNT)) == (MS_MGC_VAL | MS_REMOUNT)) {
		retval = copy_mount_options (data, &page);
		if (retval < 0)
			return retval;
		retval = do_remount(dir_name,
				    new_flags & ~MS_MGC_MSK & ~MS_REMOUNT,
				    (char *) page);
		free_page(page);
		return retval;
	}
	retval = copy_mount_options (type, &page);
	if (retval < 0)
		return retval;
	fstype = get_fs_type((char *) page);
	free_page(page);
	if (!fstype)		
		return -ENODEV;
	t = fstype->name;
	if (fstype->requires_dev) {
		retval = namei(dev_name,&inode);
		if (retval)
			return retval;
		if (!S_ISBLK(inode->i_mode)) {
			iput(inode);
			return -ENOTBLK;
		}
		if (IS_NODEV(inode)) {
			iput(inode);
			return -EACCES;
		}
		dev = inode->i_rdev;
		if (MAJOR(dev) >= MAX_BLKDEV) {
			iput(inode);
			return -ENXIO;
		}
	} else {
		if (!(dev = get_unnamed_dev()))
			return -EMFILE;
		inode = NULL;
	}
	fops = get_blkfops(MAJOR(dev));
	if (fops && fops->open) {
		retval = fops->open(inode,NULL);
		if (retval) {
			iput(inode);
			return retval;
		}
	}
	page = 0;
	if ((new_flags & MS_MGC_MSK) == MS_MGC_VAL) {
		flags = new_flags & ~MS_MGC_MSK;
		retval = copy_mount_options(data, &page);
		if (retval < 0) {
			iput(inode);
			return retval;
		}
	}
	retval = do_mount(dev,dir_name,t,flags,(void *) page);
	free_page(page);
	if (retval && fops && fops->release)
		fops->release(inode,NULL);
	iput(inode);
	return retval;
}

/* 挂载根文件系统
 */
void mount_root(void)
{
	struct file_system_type * fs_type;
	struct super_block * sb;
	struct inode * inode;

	/* 将super_blocks全部数据清0 */
	memset(super_blocks, 0, sizeof(super_blocks));
	fcntl_init_locks();
	if (MAJOR(ROOT_DEV) == FLOPPY_MAJOR) {
		printk(KERN_NOTICE "VFS: Insert root floppy and press ENTER\n");
		wait_for_keypress();
	}
	/*循环处理文件系统类型*/
	for (fs_type = file_systems; fs_type->read_super; fs_type++) {
		/* 表示是否需要设备，如果我ext2就是1，proc就是0 */
		if (!fs_type->requires_dev)
			continue;
		/* 读取每个文件系统的超级块 */
		sb = read_super(ROOT_DEV,fs_type->name,root_mountflags,NULL,1);
		if (sb) {
			/* 注意s_mounted是文件系统的根节点 */
			inode = sb->s_mounted;
			inode->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
			sb->s_covered = inode;
			sb->s_flags = root_mountflags;
                        /* 设置进程的工作目录的根目录 */
			current->pwd = inode;
			current->root = inode;
			printk ("VFS: Mounted root (%s filesystem)%s.\n",
				fs_type->name,
				(sb->s_flags & MS_RDONLY) ? " readonly" : "");
			return;
		}
	}
	panic("VFS: Unable to mount root");
}
