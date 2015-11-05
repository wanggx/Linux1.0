/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 */

#include <asm/system.h>
#include <linux/config.h>

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/malloc.h>
#include <asm/segment.h>

struct vm_struct {
	unsigned long flags;
	void * addr;		//虚拟映射的地址
	unsigned long size;
	struct vm_struct * next;
};

static struct vm_struct * vmlist = NULL;

/* Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */
#define VMALLOC_OFFSET	(8*1024*1024)


static inline void set_pgdir(unsigned long dindex, unsigned long value)
{
	struct task_struct * p;
	/*将所有进程的页目录表中dindex项设置为value*/
	p = &init_task;
	do {
		((unsigned long *) p->tss.cr3)[dindex] = value;
		p = p->next_task;
	} while (p != &init_task);
}

//将vmalloc映射的内存给解除映射
static int free_area_pages(unsigned long dindex, unsigned long index, unsigned long nr)
{
	unsigned long page, *pte;

	if (!(PAGE_PRESENT & (page = swapper_pg_dir[dindex])))
		return 0;
	page &= PAGE_MASK;
	pte = index + (unsigned long *) page;
	do {
		unsigned long pg = *pte;
		*pte = 0;
		if (pg & PAGE_PRESENT)
			free_page(pg);
		pte++;
	} while (--nr);
	pte = (unsigned long *) page;
	for (nr = 0 ; nr < 1024 ; nr++, pte++)
		if (*pte)
			return 0;
	set_pgdir(dindex,0);
	mem_map[MAP_NR(page)] = 1;
	free_page(page);
	invalidate();
	return 0;
}


/*  注意linux的内存分配，采用二级页表，dindex是4M的索引，即页目录表中的索引
 *  index即使页表中的索引，nr是从index索引处开始映射多少页的内存，此处注意swapper_pg_dir
 *	是内核页目录表，记录整个内存的使用情况
 */

static int alloc_area_pages(unsigned long dindex, unsigned long index, unsigned long nr)
{
	unsigned long page, *pte;

	page = swapper_pg_dir[dindex];
	if (!page) {
		page = get_free_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;
		if (swapper_pg_dir[dindex]) {
			free_page(page);
			page = swapper_pg_dir[dindex];
		} else {
			mem_map[MAP_NR(page)] = MAP_PAGE_RESERVED;
			set_pgdir(dindex, page | PAGE_SHARED);
		}
	}
	page &= PAGE_MASK;
	pte = index + (unsigned long *) page;
	*pte = PAGE_SHARED;		/* remove a race with vfree() */
	do {
		unsigned long pg = get_free_page(GFP_KERNEL);

		if (!pg)
			return -ENOMEM;
		*pte = pg | PAGE_SHARED;
		pte++;
	} while (--nr);
	invalidate();
	return 0;
}

static int do_area(void * addr, unsigned long size,
	int (*area_fn)(unsigned long,unsigned long,unsigned long))
{
	unsigned long nr, dindex, index;
	//获取内存的页数
	nr = size >> PAGE_SHIFT;
	//映射的是内核空间
	dindex = (TASK_SIZE + (unsigned long) addr) >> 22;
	//index的范围在0-1024范围
	index = (((unsigned long) addr) >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
	while (nr > 0) {
		/* 判断当前页表还有多少项可以被映射   */
		unsigned long i = PTRS_PER_PAGE - index;

		if (i > nr)
			i = nr;
		nr -= i;
		if (area_fn(dindex, index, i))
			return -1;
		/*下一个映射页表的索引从0开始*/
		index = 0;
		/*增加页目录表中的索引*/
		dindex++;
	}
	return 0;
}

void vfree(void * addr)
{
	struct vm_struct **p, *tmp;

	if (!addr)
		return;
	//将释放的地址按页对齐
	if ((PAGE_SIZE-1) & (unsigned long) addr) {
		printk("Trying to vfree() bad address (%p)\n", addr);
		return;
	}
	//从vmalloc分配的页表当中扫描
	for (p = &vmlist ; (tmp = *p) ; p = &tmp->next) {
		if (tmp->addr == addr) {
			*p = tmp->next;
			do_area(tmp->addr, tmp->size, free_area_pages);
			kfree(tmp);
			return;
		}
	}
	printk("Trying to vfree() nonexistent vm area (%p)\n", addr);
}

void * vmalloc(unsigned long size)
{
	void * addr;
	struct vm_struct **p, *tmp, *area;
	//size地址对齐，不足页的作为一整页来处理，也就是一页的整数倍
	size = PAGE_ALIGN(size);
	if (!size || size > high_memory)
		return NULL;
	//从内核中分配一个结构体给area
	area = (struct vm_struct *) kmalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return NULL;
	addr = (void *) ((high_memory + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1));
	area->size = size + PAGE_SIZE;
	area->next = NULL;
	/* 注意此处代码比较精巧，第一次运行的时候for循环并不会执行，直到下一次即
	 * *p = area执行后，即vmlist=area
	 */
	
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		if (size + (unsigned long) addr < (unsigned long) tmp->addr)
			break;
		//addr地址依次向后一下移动
		addr = (void *) (tmp->size + (unsigned long) tmp->addr);
	}
	//将申请的area连接到vmlist的最后一个
	area->addr = addr;
	area->next = *p;
	*p = area;
	if (do_area(addr, size, alloc_area_pages)) {
		vfree(addr);
		return NULL;
	}
	return addr;
}


//返回实际读取数量
int vread(char *buf, char *addr, int count)
{
	struct vm_struct **p, *tmp;
	char *vaddr, *buf_start = buf;
	int n;
	//扫描vmlist
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		vaddr = (char *) tmp->addr;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			put_fs_byte('\0', buf++), addr++, count--;
		}
		n = tmp->size - PAGE_SIZE;
		if (addr > vaddr)
			n -= addr - vaddr;
		while (--n >= 0) {
			if (count == 0)
				goto finished;
			put_fs_byte(*addr++, buf++), count--;
		}
	}
finished:
	return buf - buf_start;
}
