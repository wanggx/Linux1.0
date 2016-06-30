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
	void * addr;		//映射的地址
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

/* 主要作用统一修改进程的内核空间的映射
 */

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

/* 将vmalloc映射的内存给解除映射，并释放掉之前映射的物理页
 */
static int free_area_pages(unsigned long dindex, unsigned long index, unsigned long nr)
{
	unsigned long page, *pte;

	if (!(PAGE_PRESENT & (page = swapper_pg_dir[dindex])))
		return 0;
	page &= PAGE_MASK;
	pte = index + (unsigned long *) page;
	/*将页表中的页解除映射,并释放掉物理页*/
	do {
		unsigned long pg = *pte;
		*pte = 0;
		if (pg & PAGE_PRESENT)
			free_page(pg);
		pte++;
	} while (--nr);
	/* 此处判断该页表是否还有映射，也就是这个页表还是否有用，
	 * 如果该页表没有映射任何内存，则需要将该页表占用的一页内存
	 * 给释放掉，因为在alloc_area_pages函数中，如果在映射的时候
	 * 如果页表为空，则申请了一个新的页表，如果页表只使用了一项，
	 * 在释放的时候将页表中仅有的一项映射给解除了，那么该页表就
	 * 没有任何作用了，为了节约内存最好将该页表释放
	 */
	pte = (unsigned long *) page;
	for (nr = 0 ; nr < 1024 ; nr++, pte++)
		if (*pte)
			return 0;
	/* 因为映射的是内核空间，而所有的进程共享内核空间的，所以需要
	 * 将所有进程的内核空间映射更改
	 */
	set_pgdir(dindex,0);
	mem_map[MAP_NR(page)] = 1;
	free_page(page);
	invalidate();
	return 0;
}


/*  注意linux的内存分配，采用二级页表，dindex是4M的索引，即页目录表中的索引
  *  index即使页表中的索引，nr是从index索引处开始映射多少页的内存，也即申请了nr
  *  页物理内存,并映射到dindex,index...index+nr的位置上，此处注意swapper_pg_dir
  *  是内核页目录表，记录整个内存的使用情况
  */

static int alloc_area_pages(unsigned long dindex, unsigned long index, unsigned long nr)
{
	unsigned long page, *pte;

	page = swapper_pg_dir[dindex];
	/*如果该页表为空，则申请一个物理页作为页表*/
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
	//映射的是内核空间     为什么+TASK_SIZE ??????
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
	//从vmalloc分配的页表当中扫描,找到要释放的节点，并释放找到的节点
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

/* 用vamlloc两次分配的地址是不可能连续的，因为中间留有一个PAGE_SIZE大小的hole
 * 并且vamlloc分配的地址必须是整页的(大块内存)。实际上是用的物理内存是不连续的。kmalloc
 * 则分配的是小内存 ,do_area的时候+TASK_SIZE到内核空间 ????????
 */
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

	/* addr指向的是8MB的前一个位置，vmalloc分配的地址是从8MB开始的*/
	addr = (void *) ((high_memory + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1));
	/*此处为啥要加一个PAGE_SIZE   ????   */
	area->size = size + PAGE_SIZE;
	area->next = NULL;
	
	/* 注意此处代码比较精巧，第一次运行的时候for循环并不会执行，直到下一次即
	 * *p = area执行后，即vmlist=area，tmp->addr中地址是顺序存放的，并且是依次增大的
	 * 8MB开始     |----(已被使用)---|------(空闲大小为size+PAGE_SIZE)-----|----(已使用)----|....等等
	 */
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		/*找到一个可以存放size大小的区间，并将新area插入到vmlist当中*/
		if (size + (unsigned long) addr < (unsigned long) tmp->addr)
			break;
		//addr地址依次向后一下移动
		addr = (void *) (tmp->size + (unsigned long) tmp->addr);
	}
	//将申请的area连接到p所指节点后面，
	area->addr = addr;
	area->next = *p;
	*p = area;
	if (do_area(addr, size, alloc_area_pages)) {
		vfree(addr);
		return NULL;
	}
	return addr;
}


/* 返回实际读取数量
 * 从地址addr处，读取count字节存放到buf处，
 */
int vread(char *buf, char *addr, int count)
{
	struct vm_struct **p, *tmp;
	char *vaddr, *buf_start = buf;
	int n;
	//扫描vmlist
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		vaddr = (char *) tmp->addr;
		/*此处循环一定是在下面循环后面执行，因为链表中地址是从小到大排列的*/
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			put_fs_byte('\0', buf++), addr++, count--;
		}
		/*和vmalloc中的+PAGE_SIZE对应*/
		n = tmp->size - PAGE_SIZE;
		if (addr > vaddr)
			n -= addr - vaddr;
		/* 结合上面示意图，如果没有找到正确的节点，则会执行上面的if,此时
		 * n就会小于0,就不会执行下面的while，只有找到正确的节点才会执行
		 * while执行后会将剩下的所有字节读完，前提是count还大于0，如果读完之后
		 * count仍然大于0，则后面所有的内容都填充'\0',也就是执行上面的while
		 */
		while (--n >= 0) {
			if (count == 0)
				goto finished;
			put_fs_byte(*addr++, buf++), count--;
		}
	}
finished:
	return buf - buf_start;
}
