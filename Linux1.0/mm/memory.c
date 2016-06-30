/*
 *  linux/mm/memory.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

/*
 * Real VM (paging to/from disk) started 18.12.91. Much more work and
 * thought has to go into this. Oh, well..
 * 19.12.91  -  works, somewhat. Sometimes I get faults, don't know why.
 *		Found it. Everything seems to work now.
 * 20.12.91  -  Ok, making the swap-device changeable like the root.
 */

#include <asm/system.h>
#include <linux/config.h>

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>


//mem_init中传递的end_mem，即内核支持的最大物理地址
unsigned long high_memory = 0;

/*pg0映射物理地址的0-4MB范围*/
extern unsigned long pg0[1024];		/* page table for 0-4MB for everybody */

extern void sound_mem_init(void);
extern void die_if_kernel(char *,struct pt_regs *,long);

/* 交换分区中的页的数量 */
int nr_swap_pages = 0;

/* 内核空闲链表的链首和空闲块的数量*/
int nr_free_pages = 0;
unsigned long free_page_list = 0;
/*
 * The secondary free_page_list is used for malloc() etc things that
 * may need pages during interrupts etc. Normal get_free_page() operations
 * don't touch it, so it stays as a kind of "panic-list", that can be
 * accessed when all other mm tricks have failed.
 */
int nr_secondary_pages = 0;
/* 内核为原子内存分配预留的空闲页 */
unsigned long secondary_page_list = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl": :"S" (from),"D" (to),"c" (1024):"cx","di","si")

unsigned short * mem_map = NULL;

#define CODE_SPACE(addr,p) ((addr) < (p)->end_code)

/*
 * oom() prints a message (so that the user knows why the process died),
 * and gives the process an untrappable SIGSEGV.
 */
void oom(struct task_struct * task)
{
	printk("\nout of memory\n");
	task->sigaction[SIGKILL-1].sa_handler = NULL;
	task->blocked &= ~(1<<(SIGKILL-1));
	send_sig(SIGKILL,task,1);
}

/* 释放一个页表的内存，也就是4MB
 * page_dir是页表地址的地址
 **/
static void free_one_table(unsigned long * page_dir)
{
	int j;
	unsigned long pg_table = *page_dir;
	unsigned long * page_table;

	if (!pg_table)
		return;
	*page_dir = 0;
	if (pg_table >= high_memory || !(pg_table & PAGE_PRESENT)) {
		printk("Bad page table: [%p]=%08lx\n",page_dir,pg_table);
		return;
	}
	/*如果是保留页，则不做处理*/
	if (mem_map[MAP_NR(pg_table)] & MAP_PAGE_RESERVED)
		return;
	/*获取页表首地址，循环处理页表中的每一项*/
	page_table = (unsigned long *) (pg_table & PAGE_MASK);
	for (j = 0 ; j < PTRS_PER_PAGE ; j++,page_table++) {
		unsigned long pg = *page_table;
		
		if (!pg)
			continue;
		/*清空页表项的值*/
		*page_table = 0;
		/* 注意此处有点非常重要
		 * 当访问的内存不在主存中时，在交换区中，则需要做另外的处理
		 * PAGE_PRESENT表示内存在主存中
		 */
		if (pg & PAGE_PRESENT)
			free_page(PAGE_MASK & pg);
		else
			swap_free(pg);
	}
	/*释放页表所占用的空间*/
	free_page(PAGE_MASK & pg_table);
}

/*
 * This function clears all user-level page tables of a process - this
 * is needed by execve(), so that old pages aren't in the way. Note that
 * unlike 'free_page_tables()', this function still leaves a valid
 * page-table-tree in memory: it just removes the user pages. The two
 * functions are similar, but there is a fundamental difference.
 */

/* 只保留进程的内核态页表，释放所有用户态页表
 */

void clear_page_tables(struct task_struct * tsk)
{
	int i;
	unsigned long pg_dir;
	unsigned long * page_dir;

	if (!tsk)
		return;
	if (tsk == task[0])
		panic("task[0] (swapper) doesn't support exec()\n");
	/*每个进程的页目录表存放在tast_struct->tss.cr3当中*/
	pg_dir = tsk->tss.cr3;
	page_dir = (unsigned long *) pg_dir;
	if (!page_dir || page_dir == swapper_pg_dir) {
		printk("Trying to clear kernel page-directory: not good\n");
		return;
	}
	if (mem_map[MAP_NR(pg_dir)] > 1) {
		unsigned long * new_pg;

		/* 申请一个新的页目录表，
		 * 将进程的内核态地址空间拷贝到新的页面当中，
		 * 将用户态0-3GB的地址空间释放，然后将新的页目录表
		 * 赋值给cr3,并释放原来的页目录表，此函数主要用在exev函数族当中
		 */
		if (!(new_pg = (unsigned long*) get_free_page(GFP_KERNEL))) {
			oom(tsk);
			return;
		}
		for (i = 768 ; i < 1024 ; i++)
			new_pg[i] = page_dir[i];
		free_page(pg_dir);
		tsk->tss.cr3 = (unsigned long) new_pg;
		return;
	}
	for (i = 0 ; i < 768 ; i++,page_dir++)
		free_one_table(page_dir);
	invalidate();
	return;
}

/*
 * This function frees up all page tables of a process when it exits.
 */
void free_page_tables(struct task_struct * tsk)
{
	int i;
	unsigned long pg_dir;
	unsigned long * page_dir;

	if (!tsk)
		return;
	if (tsk == task[0]) {
		printk("task[0] (swapper) killed: unable to recover\n");
		panic("Trying to free up swapper memory space");
	}
	pg_dir = tsk->tss.cr3;
	if (!pg_dir || pg_dir == (unsigned long) swapper_pg_dir) {
		printk("Trying to free kernel page-directory: not good\n");
		return;
	}
	tsk->tss.cr3 = (unsigned long) swapper_pg_dir;
	if (tsk == current)
		__asm__ __volatile__("movl %0,%%cr3": :"a" (tsk->tss.cr3));

	/* 如果页目录表的引用计数大于1，可以这么理解，比如说，两个进程都指向了一个页目录表
	 * 释放一个进程的页表，则该页表的计数减1，并不做任何处理返回，如果只被一个进程所用
	 * 则需要将页目录表所映射的内存都释放，可以看函数clone_page_tables的实现
	 */
	if (mem_map[MAP_NR(pg_dir)] > 1) {
		free_page(pg_dir);
		return;
	}
	page_dir = (unsigned long *) pg_dir;
	for (i = 0 ; i < PTRS_PER_PAGE ; i++,page_dir++)
		free_one_table(page_dir);
	free_page(pg_dir);
	invalidate();
}

/*
 * clone_page_tables() clones the page table for a process - both
 * processes will have the exact same pages in memory. There are
 * probably races in the memory management with cloning, but we'll
 * see..
 */


int clone_page_tables(struct task_struct * tsk)
{
	unsigned long pg_dir;

	pg_dir = current->tss.cr3;
	mem_map[MAP_NR(pg_dir)]++;
	tsk->tss.cr3 = pg_dir;
	return 0;
}

/*
 * copy_page_tables() just copies the whole process memory range:
 * note the special handling of RESERVED (ie kernel) pages, which
 * means that they are always shared by all processes.
 */
int copy_page_tables(struct task_struct * tsk)
{
	int i;
	unsigned long old_pg_dir, *old_page_dir;
	unsigned long new_pg_dir, *new_page_dir;

	/* 申请一个新的页目录表
	 */
	if (!(new_pg_dir = get_free_page(GFP_KERNEL)))
		return -ENOMEM;
	/* 获取当前进程的页目录表，设置新进程的页目录表
	 */
	old_pg_dir = current->tss.cr3;
	tsk->tss.cr3 = new_pg_dir;
	old_page_dir = (unsigned long *) old_pg_dir;
	new_page_dir = (unsigned long *) new_pg_dir;
	/* 开始循环复制进程的页目录表，因为每页4kb,每个页表项4byte，总共1024个页表
	 */
	for (i = 0 ; i < PTRS_PER_PAGE ; i++,old_page_dir++,new_page_dir++) {
		int j;
		unsigned long old_pg_table, *old_page_table;
		unsigned long new_pg_table, *new_page_table;

		old_pg_table = *old_page_dir;
		if (!old_pg_table)
			continue;
		if (old_pg_table >= high_memory || !(old_pg_table & PAGE_PRESENT)) {
			printk("copy_page_tables: bad page table: "
				"probable memory corruption");
			*old_page_dir = 0;
			continue;
		}
		/* 如果是内核空间则直接赋值即可
		 */
		if (mem_map[MAP_NR(old_pg_table)] & MAP_PAGE_RESERVED) {
			*new_page_dir = old_pg_table;
			continue;
		}
		/* 申请一个新的页表
		 */
		if (!(new_pg_table = get_free_page(GFP_KERNEL))) {
			free_page_tables(tsk);
			return -ENOMEM;
		}
		old_page_table = (unsigned long *) (PAGE_MASK & old_pg_table);
		new_page_table = (unsigned long *) (PAGE_MASK & new_pg_table);
		for (j = 0 ; j < PTRS_PER_PAGE ; j++,old_page_table++,new_page_table++) {
			unsigned long pg;
			pg = *old_page_table;
			/* 如果是等于0，则说明该页内存还没有被映射，也就不存在拷贝了
			 * 在下一个if当中如果内存有映射了，但是不在内存当中，则说明在
			 * 交换区当中，那么就需要在交换区当中拷贝
			 */
			if (!pg)
				continue;
			if (!(pg & PAGE_PRESENT)) {
				*new_page_table = swap_duplicate(pg);
				continue;
			}
			if ((pg & (PAGE_RW | PAGE_COW)) == (PAGE_RW | PAGE_COW))
				pg &= ~PAGE_RW;
			/* 此处并没有去申请一个新的内存，实际是父进程和子进程在共享内存，如果要写再去复制
			 * 也就是写时复制(copy-on-write),同时增加了物理页mem_map的引用计数，在__verify_write中在
			 * 调用free_page减小引用计数
			 */
			*new_page_table = pg;
			if (mem_map[MAP_NR(pg)] & MAP_PAGE_RESERVED)
				continue;
			*old_page_table = pg;
			mem_map[MAP_NR(pg)]++;
		}
		*new_page_dir = new_pg_table | PAGE_TABLE;
	}
	invalidate();
	return 0;
}

/*
 * a more complete version of free_page_tables which performs with page
 * granularity.
 */

/* 将以from为起始地址，大小为size的地址空间从进程的
 * 二级页表中删除
 */
int unmap_page_range(unsigned long from, unsigned long size)
{
	unsigned long page, page_dir;
	unsigned long *page_table, *dir;
	unsigned long poff, pcnt, pc;

	if (from & ~PAGE_MASK) {
		printk("unmap_page_range called with wrong alignment\n");
		return -EINVAL;
	}
	size = (size + ~PAGE_MASK) >> PAGE_SHIFT;
	dir = PAGE_DIR_OFFSET(current->tss.cr3,from);
	poff = (from >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
	if ((pcnt = PTRS_PER_PAGE - poff) > size)
		pcnt = size;

	for ( ; size > 0; ++dir, size -= pcnt,
	     pcnt = (size > PTRS_PER_PAGE ? PTRS_PER_PAGE : size)) {
		if (!(page_dir = *dir))	{
			poff = 0;
			continue;
		}
		if (!(page_dir & PAGE_PRESENT)) {
			printk("unmap_page_range: bad page directory.");
			continue;
		}
		page_table = (unsigned long *)(PAGE_MASK & page_dir);
		if (poff) {
			page_table += poff;
			poff = 0;
		}
		for (pc = pcnt; pc--; page_table++) {
			if ((page = *page_table) != 0) {
				*page_table = 0;
				if (1 & page) {
					if (!(mem_map[MAP_NR(page)] & MAP_PAGE_RESERVED))
						if (current->rss > 0)
							--current->rss;
					free_page(PAGE_MASK & page);
				} else
					swap_free(page);
			}
		}
		if (pcnt == PTRS_PER_PAGE) {
			*dir = 0;
			free_page(PAGE_MASK & page_dir);
		}
	}
	invalidate();
	return 0;
}

/* 注意和上面unmap_page_range函数差别，
 * 在将二级页表中的地址映射给取消之后，
 * 将相应的项设置为mask值，mask的值代表在
 * 进程操作当前线性地址对应的物理页时，应该
 * 采取咋样的操作，如线性地址共享
 */
int zeromap_page_range(unsigned long from, unsigned long size, int mask)
{
	unsigned long *page_table, *dir;
	unsigned long poff, pcnt;
	unsigned long page;

	if (mask) {
		if ((mask & (PAGE_MASK|PAGE_PRESENT)) != PAGE_PRESENT) {
			printk("zeromap_page_range: mask = %08x\n",mask);
			return -EINVAL;
		}
		mask |= ZERO_PAGE;
	}
	if (from & ~PAGE_MASK) {
		printk("zeromap_page_range: from = %08lx\n",from);
		return -EINVAL;
	}
	/*获取在页目录表中的位置*/
	dir = PAGE_DIR_OFFSET(current->tss.cr3,from);
	size = (size + ~PAGE_MASK) >> PAGE_SHIFT;
	/*获取在页表中的位置*/
	poff = (from >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
	if ((pcnt = PTRS_PER_PAGE - poff) > size)
		pcnt = size;

	while (size > 0) {
		/*如果当前不在内存当中*/
		if (!(PAGE_PRESENT & *dir)) {
				/* clear page needed here?  SRB. */
			if (!(page_table = (unsigned long*) get_free_page(GFP_KERNEL))) {
				invalidate();
				return -ENOMEM;
			}
			if (PAGE_PRESENT & *dir) {
				free_page((unsigned long) page_table);
				page_table = (unsigned long *)(PAGE_MASK & *dir++);
			} else
				*dir++ = ((unsigned long) page_table) | PAGE_TABLE;
		} else {
			/*获取页目录表中页表的位置*/
			page_table = (unsigned long *)(PAGE_MASK & *dir++);
		}
		page_table += poff;
		poff = 0;
		for (size -= pcnt; pcnt-- ;) {
			if ((page = *page_table) != 0) {
				*page_table = 0;
				if (page & PAGE_PRESENT) {
					if (!(mem_map[MAP_NR(page)] & MAP_PAGE_RESERVED))
						if (current->rss > 0)
							--current->rss;
					free_page(PAGE_MASK & page);
				} else
					swap_free(page);
			}
			*page_table++ = mask;
		}
		pcnt = (size > PTRS_PER_PAGE ? PTRS_PER_PAGE : size);
	}
	invalidate();
	return 0;
}

/*
 * maps a range of physical memory into the requested pages. the old
 * mappings are removed. any references to nonexistent pages results
 * in null mappings (currently treated as "copy-on-access")
 */
int remap_page_range(unsigned long from, unsigned long to, unsigned long size, int mask)
{
	unsigned long *page_table, *dir;
	unsigned long poff, pcnt;
	unsigned long page;

	if (mask) {
		if ((mask & (PAGE_MASK|PAGE_PRESENT)) != PAGE_PRESENT) {
			printk("remap_page_range: mask = %08x\n",mask);
			return -EINVAL;
		}
	}
	if ((from & ~PAGE_MASK) || (to & ~PAGE_MASK)) {
		printk("remap_page_range: from = %08lx, to=%08lx\n",from,to);
		return -EINVAL;
	}
	dir = PAGE_DIR_OFFSET(current->tss.cr3,from);
	size = (size + ~PAGE_MASK) >> PAGE_SHIFT;
	poff = (from >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
	if ((pcnt = PTRS_PER_PAGE - poff) > size)
		pcnt = size;

	while (size > 0) {
		if (!(PAGE_PRESENT & *dir)) {
			/* clearing page here, needed?  SRB. */
			if (!(page_table = (unsigned long*) get_free_page(GFP_KERNEL))) {
				invalidate();
				return -1;
			}
			*dir++ = ((unsigned long) page_table) | PAGE_TABLE;
		}
		else
			page_table = (unsigned long *)(PAGE_MASK & *dir++);
		if (poff) {
			page_table += poff;
			poff = 0;
		}

		for (size -= pcnt; pcnt-- ;) {
			if ((page = *page_table) != 0) {
				*page_table = 0;
				if (PAGE_PRESENT & page) {
					if (!(mem_map[MAP_NR(page)] & MAP_PAGE_RESERVED))
						if (current->rss > 0)
							--current->rss;
					free_page(PAGE_MASK & page);
				} else
					swap_free(page);
			}

			/*
			 * the first condition should return an invalid access
			 * when the page is referenced. current assumptions
			 * cause it to be treated as demand allocation in some
			 * cases.
			 */
			if (!mask)
				*page_table++ = 0;	/* not present */
			else if (to >= high_memory)
				*page_table++ = (to | mask);
			else if (!mem_map[MAP_NR(to)])
				*page_table++ = 0;	/* not present */
			else {
				*page_table++ = (to | mask);
				if (!(mem_map[MAP_NR(to)] & MAP_PAGE_RESERVED)) {
					++current->rss;
					mem_map[MAP_NR(to)]++;
				}
			}
			to += PAGE_SIZE;
		}
		pcnt = (size > PTRS_PER_PAGE ? PTRS_PER_PAGE : size);
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
unsigned long put_page(struct task_struct * tsk,unsigned long page,
	unsigned long address,int prot)
{
	unsigned long *page_table;

	if ((prot & (PAGE_MASK|PAGE_PRESENT)) != PAGE_PRESENT)
		printk("put_page: prot = %08x\n",prot);
	if (page >= high_memory) {
		printk("put_page: trying to put page %08lx at %08lx\n",page,address);
		return 0;
	}
	/*  此处的address是线性地址,此处一定要想清楚，
	 *  Linux的内存分配采用的是段页式的，逻辑地址---(分段)---线性地址----(分页)-----物理地址，
	 *  然后找到他在页目录表的所在的项，也就是页表的地址
	 */
	page_table = PAGE_DIR_OFFSET(tsk->tss.cr3,address);
	if ((*page_table) & PAGE_PRESENT)
		page_table = (unsigned long *) (PAGE_MASK & *page_table);
	else {
		printk("put_page: bad page directory entry\n");
		oom(tsk);
		*page_table = BAD_PAGETABLE | PAGE_TABLE;
		return 0;
	}
	/*取出地址在页表中的位置*/
	page_table += (address >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
	/* 如果页表已经被映射了，则将之前的映射取消
	 * 此处有一个问题，映射取消时，为啥没有释放被映射的物理内存?
	 * 或者减小mem_map引用计数
	 */
	if (*page_table) {
		printk("put_page: page already exists\n");
		*page_table = 0;
		invalidate();
	}
	*page_table = page | prot;
/* no need for invalidate */
	return page;
}

/*
 * The previous function doesn't work very well if you also want to mark
 * the page dirty: exec.c wants this, as it has earlier changed the page,
 * and we want the dirty-status to be correct (for VM). Thus the same
 * routine, but this time we mark it dirty too.
 */

/* page表示物理地址
  * address表示线性地址 
  * 将address线性地址对应原物理页解除映射，并重新映射到page指向的物理页 
  * 并且设置页为脏 
  */
unsigned long put_dirty_page(struct task_struct * tsk, unsigned long page, unsigned long address)
{
	unsigned long tmp, *page_table;

	if (page >= high_memory)
		printk("put_dirty_page: trying to put page %08lx at %08lx\n",page,address);
	if (mem_map[MAP_NR(page)] != 1)
		printk("mem_map disagrees with %08lx at %08lx\n",page,address);
        /* 根据线性地址计算线性地址对应的页目录表项的物理地址 */
	page_table = PAGE_DIR_OFFSET(tsk->tss.cr3,address);
	if (PAGE_PRESENT & *page_table)
		page_table = (unsigned long *) (PAGE_MASK & *page_table);
	else {
		/*如果当前的页表项不在内存中,则重新分配一个空闲的内存页*/
		if (!(tmp = get_free_page(GFP_KERNEL)))
			return 0;
		/*此处为啥又要判断一次?*/
		if (PAGE_PRESENT & *page_table) {
			free_page(tmp);
			page_table = (unsigned long *) (PAGE_MASK & *page_table);
		} else {
			*page_table = tmp | PAGE_TABLE;
			page_table = (unsigned long *) tmp;
		}
	}
        /* 取线性地址对应的页表中的页表项 */
	page_table += (address >> PAGE_SHIFT) & (PTRS_PER_PAGE-1);
	if (*page_table) {
		printk("put_dirty_page: page already exists\n");
                /* 这是页表项的映射为0，也就是该页表项不指向任何页 */
		*page_table = 0;
		invalidate();
	}
	/*设置页标记为脏和私有*/
	*page_table = page | (PAGE_DIRTY | PAGE_PRIVATE);
/* no need for invalidate */
	return page;
}

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * Note that we do many checks twice (look at do_wp_page()), as
 * we have to be careful about race-conditions.
 *
 * Goto-purists beware: the only reason for goto's here is that it results
 * in better assembly code.. The "default" path will see no jumps at all.
 */



/* 内存写保护函数
 */
static void __do_wp_page(unsigned long error_code, unsigned long address,
	struct task_struct * tsk, unsigned long user_esp)
{
	unsigned long *pde, pte, old_page, prot;
	unsigned long new_page;

	new_page = __get_free_page(GFP_KERNEL);
	pde = PAGE_DIR_OFFSET(tsk->tss.cr3,address);
	pte = *pde;
	/*写保护内存对应映射的页表不在内存时，不做任何处理*/
	if (!(pte & PAGE_PRESENT))
		goto end_wp_page;
	if ((pte & PAGE_TABLE) != PAGE_TABLE || pte >= high_memory)
		goto bad_wp_pagetable;
	pte &= PAGE_MASK;
	pte += PAGE_PTR(address);
	/*当要写保护的内存不在内存当中时，则不做任何处理*/
	old_page = *(unsigned long *) pte;
	if (!(old_page & PAGE_PRESENT))
		goto end_wp_page;
	if (old_page >= high_memory)
		goto bad_wp_page;
	if (old_page & PAGE_RW)
		goto end_wp_page;
	tsk->min_flt++;
	prot = (old_page & ~PAGE_MASK) | PAGE_RW;
	old_page &= PAGE_MASK;
	if (mem_map[MAP_NR(old_page)] != 1) {
		if (new_page) {
			if (mem_map[MAP_NR(old_page)] & MAP_PAGE_RESERVED)
				++tsk->rss;
			/*将物理页数据拷贝*/
			copy_page(old_page,new_page);
			*(unsigned long *) pte = new_page | prot;
			/* 因为之前两个地址都映射到同一个物理地址
			 * 现在将同一个物理页给拷贝一份，则原来物理页的引用计数将会减1
			 * 也就是这里执行free_page的原因
			 */
			free_page(old_page);
			invalidate();
			return;
		}
		free_page(old_page);
		oom(tsk);
		*(unsigned long *) pte = BAD_PAGE | prot;
		invalidate();
		return;
	}
	*(unsigned long *) pte |= PAGE_RW;
	invalidate();
	if (new_page)
		free_page(new_page);
	return;
	/*设置页表项对应的页是坏的页，同时发送杀死进程的信号*/
bad_wp_page:
	printk("do_wp_page: bogus page at address %08lx (%08lx)\n",address,old_page);
	*(unsigned long *) pte = BAD_PAGE | PAGE_SHARED;
	send_sig(SIGKILL, tsk, 1);
	goto end_wp_page;
	/*设置页目录项对应的页表时坏的，并且发送杀死进程的信号*/
bad_wp_pagetable:
	printk("do_wp_page: bogus page-table at address %08lx (%08lx)\n",address,pte);
	*pde = BAD_PAGETABLE | PAGE_TABLE;
	send_sig(SIGKILL, tsk, 1);
end_wp_page:
	if (new_page)
		free_page(new_page);
	return;
}

/*
 * check that a page table change is actually needed, and call
 * the low-level function only in that case..
 */
void do_wp_page(unsigned long error_code, unsigned long address,
	struct task_struct * tsk, unsigned long user_esp)
{
	unsigned long page;
	unsigned long * pg_table;

	pg_table = PAGE_DIR_OFFSET(tsk->tss.cr3,address);
	page = *pg_table;
	if (!page)
		return;
	if ((page & PAGE_PRESENT) && page < high_memory) {
		/* PAGE_PTR(address)找到线性地址在页表中的偏移量 */
		pg_table = (unsigned long *) ((page & PAGE_MASK) + PAGE_PTR(address));
		page = *pg_table;
		/*如果当前页不在内存或者该页本来就是共享读写的，就直接返回*/
		if (!(page & PAGE_PRESENT))
			return;
		if (page & PAGE_RW)
			return;
		/*如果不是写时复制*/
		if (!(page & PAGE_COW)) {
			if (user_esp && tsk == current) {
				current->tss.cr2 = address;
				current->tss.error_code = error_code;
				current->tss.trap_no = 14;   /*14就是也错误处理*/
				send_sig(SIGSEGV, tsk, 1);
				return;
			}
		}
		if (mem_map[MAP_NR(page)] == 1) {
			*pg_table |= PAGE_RW | PAGE_DIRTY;
			invalidate();
			return;
		}
		__do_wp_page(error_code, address, tsk, user_esp);
		return;
	}
	printk("bad page directory entry %08lx\n",page);
	*pg_table = 0;
}


/* 此操作主要针对COW，进程在fork的时候，并没有将父进程的，物理内存页拷贝，
 * 只有在写的时候，才会去拷贝，当其中一个进程需要写时，就将所在的物理页拷贝一份*/
int __verify_write(unsigned long start, unsigned long size)
{
	size--;
	size += start & ~PAGE_MASK;
	size >>= PAGE_SHIFT;
	start &= PAGE_MASK;
	/*一次处理所有页*/
	do {
		do_wp_page(1,start,current,0);
		start += PAGE_SIZE;
	} while (size--);
	return 0;
}

/* 申请一个空的物理内存页，然后把该物理页映射到address表示的线性地址所在的页*/
static inline void get_empty_page(struct task_struct * tsk, unsigned long address)
{
	unsigned long tmp;

	if (!(tmp = get_free_page(GFP_KERNEL))) {
		oom(tsk);
		tmp = BAD_PAGE;
	}
	if (!put_page(tsk,tmp,address,PAGE_PRIVATE))
		free_page(tmp);
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable or library.
 *
 * We may want to fix this to allow page sharing for PIC pages at different
 * addresses so that ELF will really perform properly. As long as the vast
 * majority of sharable libraries load at fixed addresses this is not a
 * big concern. Any sharing of pages between the buffer cache and the
 * code space reduces the need for this as well.  - ERY
 */

/* 将进程p的线性地址共享到进程tsk的线性地址,
 * 同时tsk的address线性地址映射到物理地址newpage处
 * 如果页时非PAGE_RW得，则仅仅增加引用计数而已
 */
static int try_to_share(unsigned long address, struct task_struct * tsk,
	struct task_struct * p, unsigned long error_code, unsigned long newpage)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;

	/*获取同一个线性地址在两个不同进程当中的页目录项地址*/
	from_page = (unsigned long)PAGE_DIR_OFFSET(p->tss.cr3,address);
	to_page = (unsigned long)PAGE_DIR_OFFSET(tsk->tss.cr3,address);
/* is there a page-directory at from ? */
	from = *(unsigned long *) from_page;
	if (!(from & PAGE_PRESENT))
		return 0;
	from &= PAGE_MASK;
	/*找到要共享页在页表中的地址*/
	from_page = from + PAGE_PTR(address);
	from = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((from & (PAGE_PRESENT | PAGE_DIRTY)) != PAGE_PRESENT)
		return 0;
	if (from >= high_memory)
		return 0;
	/*如果要共享的页不在内存中则不行*/
	if (mem_map[MAP_NR(from)] & MAP_PAGE_RESERVED)
		return 0;
/* is the destination ok? */
	to = *(unsigned long *) to_page;
	if (!(to & PAGE_PRESENT))
		return 0;
	to &= PAGE_MASK;
	to_page = to + PAGE_PTR(address);
	/* 如果to_page已经对应了物理页，则无法共享，不能释放该内存在映射，
	 * 因为这时根本就不知道该物理页里存放的是什么数据*/
	if (*(unsigned long *) to_page)
		return 0;
/* share them if read - do COW immediately otherwise */
	if (error_code & PAGE_RW) {
		if(!newpage)	/* did the page exist?  SRB. */
			return 0;
		copy_page((from & PAGE_MASK),newpage);
		to = newpage | PAGE_PRIVATE;
	} else {
		mem_map[MAP_NR(from)]++;
		from &= ~PAGE_RW;
		to = from;
		if(newpage)	/* only if it existed. SRB. */
			free_page(newpage);
	}
	*(unsigned long *) from_page = from;
	*(unsigned long *) to_page = to;
	invalidate();
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */

/* area是需要共享的虚拟地址
 * tsk是需要被共享的进程
 * address是线性地址
 * newpage是物理地址
 */
int share_page(struct vm_area_struct * area, struct task_struct * tsk,
	struct inode * inode,
	unsigned long address, unsigned long error_code, unsigned long newpage)
{
	struct task_struct ** p;

	if (!inode || inode->i_count < 2 || !area->vm_ops)
		return 0;
	/*依次扫描进程列表*/
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (tsk == *p)
			continue;
		/*如果和进程可执行文件对应的inode节点不一样*/
		if (inode != (*p)->executable) {
			if(!area) continue;
			/* Now see if there is something in the VMM that
			   we can share pages with */
			if(area){
			  struct vm_area_struct * mpnt;
			  /*扫描进程的虚拟地址空间*/
			  for (mpnt = (*p)->mmap; mpnt; mpnt = mpnt->vm_next) {
			  	/* 虚拟地址的操作函数，i节点号，设备号相同
				 */
			    if (mpnt->vm_ops == area->vm_ops &&
			       mpnt->vm_inode->i_ino == area->vm_inode->i_ino&&
			       mpnt->vm_inode->i_dev == area->vm_inode->i_dev){
			      if (mpnt->vm_ops->share(mpnt, area, address))
				break;
			    };
			  };
			  if (!mpnt) continue;  /* Nope.  Nuthin here */
			};
		}
		if (try_to_share(address,tsk,*p,error_code,newpage))
			return 1;
	}
	return 0;
}

/*
 * fill in an empty page-table if none exists.
 */

/* 在线性地址address对应的页目录项分配一个页表，
 * 如果已经分配了，则直接返回，如果没有分配，则申请
 * 一页新的内存作为页表，并将该页内存的赋给页目录项
 * 注意此处仅仅是分配了一个页表，而也表项对应的物理并没有
 */
static inline unsigned long get_empty_pgtable(struct task_struct * tsk,unsigned long address)
{
	unsigned long page;
	unsigned long *p;

	p = PAGE_DIR_OFFSET(tsk->tss.cr3,address);
	if (PAGE_PRESENT & *p)
		return *p;
	if (*p) {
		printk("get_empty_pgtable: bad page-directory entry \n");
		*p = 0;
	}
	page = get_free_page(GFP_KERNEL);
	/*找到页目录项的位置*/
	p = PAGE_DIR_OFFSET(tsk->tss.cr3,address);
	/*如果当前页目录项已经在内存当中，也就是说已经存在映射了
	 *那么直接返回已经映射的页表的地址
	 */
	if (PAGE_PRESENT & *p) {
		free_page(page);
		return *p;
	}
	if (*p) {
		printk("get_empty_pgtable: bad page-directory entry \n");
		*p = 0;
	}
	/*如果还没有映射则将新申请的一页内存(作为页表)地址赋给页目录项*/
	if (page) {
		*p = page | PAGE_TABLE;
		return *p;
	}
	oom(current);
	*p = BAD_PAGETABLE | PAGE_TABLE;
	return 0;
}

/* 要访问的地址不在内存当中
 */
void do_no_page(unsigned long error_code, unsigned long address,
	struct task_struct *tsk, unsigned long user_esp)
{
	unsigned long tmp;
	unsigned long page;
	struct vm_area_struct * mpnt;

	/* 获取映射的物理页，如果失败了，则不做处理
	 * 如果成功了，则会继续处理页表项对应的物理页
	 */
	page = get_empty_pgtable(tsk,address);
	if (!page)
		return;
	page &= PAGE_MASK;
	page += PAGE_PTR(address);
	tmp = *(unsigned long *) page;
	/*如果物理页已经在内存当中，则不做任何处理*/
	if (tmp & PAGE_PRESENT)
		return;
	/*增加进程在内核中占用的物理页的数量*/
	++tsk->rss;
	/*如果缺页的内存在交换区中则将交换区中的内存交换到内存*/
	if (tmp) {
		++tsk->maj_flt;
		/*注意此处的page是页表项的地址*/
		swap_in((unsigned long *) page);
		return;
	}
	/* vm_area_struct中的地址是和4KB对齐的
	 */
	address &= 0xfffff000;
	tmp = 0;
	/* 此处注意虚拟地址链表是按照地址大小顺序来排列的，目前版本内核是链表的，
	 * 在高版本内核中是二叉树结构(AVL)，
	 */
	for (mpnt = tsk->mmap; mpnt != NULL; mpnt = mpnt->vm_next) {
		if (address < mpnt->vm_start)
			break;
		if (address >= mpnt->vm_end) {
			tmp = mpnt->vm_end;
			continue;
		}
		if (!mpnt->vm_ops || !mpnt->vm_ops->nopage) {
			++tsk->min_flt;
			get_empty_page(tsk,address);
			return;
		}
		mpnt->vm_ops->nopage(error_code, mpnt, address);
		return;
	}
	/*不是当前进程就好说了，直接给tsk分配一页物理内存*/
	if (tsk != current)
		goto ok_no_page;
	if (address >= tsk->end_data && address < tsk->brk)
		goto ok_no_page;
	if (mpnt && mpnt == tsk->stk_vma &&
	    address - tmp > mpnt->vm_start - address &&
	    tsk->rlim[RLIMIT_STACK].rlim_cur > mpnt->vm_end - address) {
		mpnt->vm_start = address;
		goto ok_no_page;
	}
	/*cr2记录缺页地址*/
	tsk->tss.cr2 = address;
	current->tss.error_code = error_code;
	current->tss.trap_no = 14;
	/*发送段错误信号，杀死进程*/
	send_sig(SIGSEGV,tsk,1);
	if (error_code & 4)	/* user level access? */
		return;
ok_no_page:
	++tsk->min_flt;
	get_empty_page(tsk,address);
}

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 */
 
asmlinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
	unsigned long address;
	unsigned long user_esp = 0;
	unsigned int bit;

	/* get the address */
	/*从cr2中读取引起页错误的地址*/
	__asm__("movl %%cr2,%0":"=r" (address));
	/*如果是用户空间*/
	if (address < TASK_SIZE) {
		if (error_code & 4) {	/* user mode access? */
			if (regs->eflags & VM_MASK) {
				bit = (address - 0xA0000) >> PAGE_SHIFT;
				if (bit < 32)
					current->screen_bitmap |= 1 << bit;
			} else 
				user_esp = regs->esp;
		}
		if (error_code & 1)
			do_wp_page(error_code, address, current, user_esp);
		else
			do_no_page(error_code, address, current, user_esp);
		return;
	}
	address -= TASK_SIZE;
	if (wp_works_ok < 0 && address == 0 && (error_code & PAGE_PRESENT)) {
		wp_works_ok = 1;
		pg0[0] = PAGE_SHARED;
		printk("This processor honours the WP bit even when in supervisor mode. Good.\n");
		return;
	}
	if (address < PAGE_SIZE) {
		printk("Unable to handle kernel NULL pointer dereference");
		pg0[0] = PAGE_SHARED;
	} else
		printk("Unable to handle kernel paging request");
	printk(" at address %08lx\n",address);
	die_if_kernel("Oops", regs, error_code);
	do_exit(SIGKILL);
}

/*
 * BAD_PAGE is the page that is used for page faults when linux
 * is out-of-memory. Older versions of linux just did a
 * do_exit(), but using this instead means there is less risk
 * for a process dying in kernel mode, possibly leaving a inode
 * unused etc..
 *
 * BAD_PAGETABLE is the accompanying page-table: it is initialized
 * to point to BAD_PAGE entries.
 *
 * ZERO_PAGE is a special page that is used for zero-initialized
 * data and COW.
 */
unsigned long __bad_pagetable(void)
{
	extern char empty_bad_page_table[PAGE_SIZE];

	__asm__ __volatile__("cld ; rep ; stosl":
		:"a" (BAD_PAGE + PAGE_TABLE),
		 "D" ((long) empty_bad_page_table),
		 "c" (PTRS_PER_PAGE)
		:"di","cx");
	return (unsigned long) empty_bad_page_table;
}

unsigned long __bad_page(void)
{
	extern char empty_bad_page[PAGE_SIZE];

	__asm__ __volatile__("cld ; rep ; stosl":
		:"a" (0),
		 "D" ((long) empty_bad_page),
		 "c" (PTRS_PER_PAGE)
		:"di","cx");
	return (unsigned long) empty_bad_page;
}

unsigned long __zero_page(void)
{
	extern char empty_zero_page[PAGE_SIZE];

	__asm__ __volatile__("cld ; rep ; stosl":
		:"a" (0),
		 "D" ((long) empty_zero_page),
		 "c" (PTRS_PER_PAGE)
		:"di","cx");
	return (unsigned long) empty_zero_page;
}

void show_mem(void)
{
	int i,free = 0,total = 0,reserved = 0;
	int shared = 0;

	printk("Mem-info:\n");
	printk("Free pages:      %6dkB\n",nr_free_pages<<(PAGE_SHIFT-10));
	printk("Secondary pages: %6dkB\n",nr_secondary_pages<<(PAGE_SHIFT-10));
	printk("Free swap:       %6dkB\n",nr_swap_pages<<(PAGE_SHIFT-10));
	i = high_memory >> PAGE_SHIFT;
	while (i-- > 0) {
		total++;
		if (mem_map[i] & MAP_PAGE_RESERVED)
			reserved++;
		else if (!mem_map[i])
			free++;
		else
			shared += mem_map[i]-1;
	}
	printk("%d pages of RAM\n",total);
	printk("%d free pages\n",free);
	printk("%d reserved pages\n",reserved);
	printk("%d pages shared\n",shared);
	show_buffers();
}

/*
 * paging_init() sets up the page tables - note that the first 4MB are
 * already mapped by head.S.
 *
 * This routines also unmaps the page at virtual kernel address 0, so
 * that we can trap those pesky NULL-reference errors in the kernel.
 */

/* 将内核中可用的地址空间映射到swapper_pg_dir页目录表对应的
  * 0地址处和3g地址处，也就是swapper_pg_dir的第0项和第768项处有 
  * 若干个页表的映射相同，也就是将系统中所有可用内存映射到swapper_pg_dir 
  * 当中，并作为内核的全局页目录表，在映射的时候会分配若干的一级页表， 
  * start_mem------一级页表-------------- end_mem 
  * 最后返回的就是一级页表之后的首地址 
  */
unsigned long paging_init(unsigned long start_mem, unsigned long end_mem)
{
	unsigned long * pg_dir;
	unsigned long * pg_table;
	unsigned long tmp;
	unsigned long address;

/*
 * Physical page 0 is special; it's not touched by Linux since BIOS
 * and SMM (for laptops with [34]86/SL chips) may need it.  It is read
 * and write protected to detect null pointer references in the
 * kernel.
 */
#if 0
	memset((void *) 0, 0, PAGE_SIZE);
#endif
	start_mem = PAGE_ALIGN(start_mem);
	/*此处的address应该是物理地址，映射的是进程的内核空间,而
	 *内核程序是可以任意访问物理内存的任何位置的*/
	address = 0;
	pg_dir = swapper_pg_dir;
	while (address < end_mem) {
		/*在二级页表当中，页目录表中的768项正好是3GB的位置，在每个进程的地址
		  *空间当中，0-3GB是进程的用户地址空间，3-4GB是进程的内核空间*/
		tmp = *(pg_dir + 768);		/* at virtual addr 0xC0000000 */
		/* 如果页表项为空，则从start_mem位置开始分配一个页表，而一个页表
		 * 具有1024项，正好一页大小，然后将这一页表的地址赋给页目录项
		 */
		if (!tmp) {
			tmp = start_mem | PAGE_TABLE;
			*(pg_dir + 768) = tmp;
                        /* 为了映射swapper_pg_dir时用掉的页表，
                          *  最后将用掉的页表的最后一个地址给返回
                          */
			start_mem += PAGE_SIZE;
		}
                /* 在映射0x00000的地址时，会擦除之前的映射 */
		*pg_dir = tmp;			/* also map it in at 0x0000000 for init */
		pg_dir++;
		/*获取页表的地址*/
		pg_table = (unsigned long *) (tmp & PAGE_MASK);
		/*循环处理一个页表*/
		for (tmp = 0 ; tmp < PTRS_PER_PAGE ; tmp++,pg_table++) {
			if (address < end_mem)
				*pg_table = address | PAGE_SHARED;
			else
				*pg_table = 0;
			address += PAGE_SIZE;
		}
	}
	invalidate();
	return start_mem;
}


/* 内存初始化
 * start_low_mem是从640KB处开始的
 */
void mem_init(unsigned long start_low_mem,
	      unsigned long start_mem, unsigned long end_mem)
{
	int codepages = 0;
	int reservedpages = 0;
	int datapages = 0;
	unsigned long tmp;
	unsigned short * p;
	extern int etext;

	cli();
	end_mem &= PAGE_MASK;
	high_memory = end_mem;
	start_mem +=  0x0000000f;
	start_mem &= ~0x0000000f;
	tmp = MAP_NR(end_mem);
	/* mem_map放在start_mem的起始处，占用空间是根据物理地址的总页数
	 * 来确定的，mem_map是用来记录每页物理内存的使用情况
	 **/
	mem_map = (unsigned short *) start_mem;
	p = mem_map + tmp;
	start_mem = (unsigned long) p;
	/*将所有的内存页状态设置为MAP_PAGE_RESERVED，应该和COW有关*/
	while (p > mem_map)
		*--p = MAP_PAGE_RESERVED;
	start_low_mem = PAGE_ALIGN(start_low_mem);
	start_mem = PAGE_ALIGN(start_mem);
	/* 如果start_low_mem小于1M则640KB-1MB留给显存了。0xA0000=640KB */
	while (start_low_mem < 0xA0000) {
		mem_map[MAP_NR(start_low_mem)] = 0;
		start_low_mem += PAGE_SIZE;
	}
        /* 将可用内存区的页表数组项都设置为0，表示为空闲 */
	while (start_mem < end_mem) {
		mem_map[MAP_NR(start_mem)] = 0;
		start_mem += PAGE_SIZE;
	}
	/* 上面第一个循环将所有的物理页表设置为MAP_PAGE_RESERVED，
	 *  而在后面的两个循环中，将想要的物理页表设置为0，也即是主存区
	 */
#ifdef CONFIG_SOUND
	sound_mem_init();
#endif
	/* 此段代码非常重要，涉及到kamlloc
	 * free_page_list作为空闲物理页表的链首
	 * nr_free_pages代表空闲物理页表的数量
	 **/

	free_page_list = 0;
	nr_free_pages = 0;
	for (tmp = 0 ; tmp < end_mem ; tmp += PAGE_SIZE) {
		/*如果mem_map的标记大于0，则表示该页表已被使用*/
		if (mem_map[MAP_NR(tmp)]) {
			/*如果实在显存区*/
			if (tmp >= 0xA0000 && tmp < 0x100000)
				reservedpages++;
			/*此处etext应该是内核代码段*/
			else if (tmp < (unsigned long) &etext)
				codepages++;
			else
				datapages++;
			continue;
		}
		/* 如果物理页是空闲的，则将该空闲页表添加到free_page_list当中
		  * 放在链首，同时增加nr_free_pages
		  */
		*(unsigned long *) tmp = free_page_list;
		free_page_list = tmp;
		nr_free_pages++;
	}
	tmp = nr_free_pages << PAGE_SHIFT;
	printk("Memory: %luk/%luk available (%dk kernel code, %dk reserved, %dk data)\n",
		tmp >> 10,
		end_mem >> 10,
		codepages << (PAGE_SHIFT-10),
		reservedpages << (PAGE_SHIFT-10),
		datapages << (PAGE_SHIFT-10));
/* test if the WP bit is honoured in supervisor mode */
	wp_works_ok = -1;
	pg0[0] = PAGE_READONLY;
	invalidate();
	__asm__ __volatile__("movb 0,%%al ; movb %%al,0": : :"ax", "memory");
	pg0[0] = 0;
	invalidate();
	/*开始启动分页功能*/
	if (wp_works_ok < 0)
		wp_works_ok = 0;
	return;
}

void si_meminfo(struct sysinfo *val)
{
	int i;

	i = high_memory >> PAGE_SHIFT;
	val->totalram = 0;
	val->freeram = 0;
	val->sharedram = 0;
	val->bufferram = buffermem;
	while (i-- > 0)  {
		if (mem_map[i] & MAP_PAGE_RESERVED)
			continue;
		val->totalram++;
		if (!mem_map[i]) {
			val->freeram++;
			continue;
		}
		val->sharedram += mem_map[i]-1;
	}
	val->totalram <<= PAGE_SHIFT;
	val->freeram <<= PAGE_SHIFT;
	val->sharedram <<= PAGE_SHIFT;
	return;
}


/* This handles a generic mmap of a disk file */
void file_mmap_nopage(int error_code, struct vm_area_struct * area, unsigned long address)
{
	struct inode * inode = area->vm_inode;
	unsigned int block;
	unsigned long page;
	int nr[8];
	int i, j;
	int prot = area->vm_page_prot;

	address &= PAGE_MASK;
	/*因为文件映射都是从虚拟地址段的开始地址处开始映射的，
	 *而映射的文件可以从第多少个字节处开始，所以在文件中总的偏移量为
	 *一下计算
	 */
	block = address - area->vm_start + area->vm_offset;

	/*获取偏移块号
	 */
	block >>= inode->i_sb->s_blocksize_bits;

	page = get_free_page(GFP_KERNEL);
	if (share_page(area, area->vm_task, inode, address, error_code, page)) {
		++area->vm_task->min_flt;
		return;
	}

	++area->vm_task->maj_flt;
	if (!page) {
		oom(current);
		put_page(area->vm_task, BAD_PAGE, address, PAGE_PRIVATE);
		return;
	}
	for (i=0, j=0; i< PAGE_SIZE ; j++, block++, i += inode->i_sb->s_blocksize)
		nr[j] = bmap(inode,block);
	if (error_code & PAGE_RW)
		prot |= PAGE_RW | PAGE_DIRTY;
	/* 注意nr有8个元素，其中都是物理设备的逻辑块号
	 */
	page = bread_page(page, inode->i_dev, nr, inode->i_sb->s_blocksize, prot);

	if (!(prot & PAGE_RW)) {
		if (share_page(area, area->vm_task, inode, address, error_code, page))
			return;
	}
	if (put_page(area->vm_task,page,address,prot))
		return;
	free_page(page);
	oom(current);
}


/* 将虚拟地址空间对应的文件映射写回设备
 * 在将文件mmap到虚拟地址空间时，一个地址段对应
 * 一个文件，所以在某一段即将释放时，直接将虚拟段
 * 对应的文件给写回即可。
 */

void file_mmap_free(struct vm_area_struct * area)
{
	if (area->vm_inode)
		iput(area->vm_inode);
#if 0
	if (area->vm_inode)
		printk("Free inode %x:%d (%d)\n",area->vm_inode->i_dev, 
				 area->vm_inode->i_ino, area->vm_inode->i_count);
#endif
}

/*
 * Compare the contents of the mmap entries, and decide if we are allowed to
 * share the pages
 */

/* 判断虚拟地址空间是否可以共享
 * 只是进行了条件判断，并没有做任何的处理
 */
int file_mmap_share(struct vm_area_struct * area1, 
		    struct vm_area_struct * area2, 
		    unsigned long address)
{
	if (area1->vm_inode != area2->vm_inode)
		return 0;
	if (area1->vm_start != area2->vm_start)
		return 0;
	if (area1->vm_end != area2->vm_end)
		return 0;
	if (area1->vm_offset != area2->vm_offset)
		return 0;
	if (area1->vm_page_prot != area2->vm_page_prot)
		return 0;
	return 1;
}


/* 虚拟内存操作函数 */
struct vm_operations_struct file_mmap = {
	NULL,			/* open */
	file_mmap_free,		/* close */
	file_mmap_nopage,	/* nopage */
	NULL,			/* wppage */
	file_mmap_share,	/* share */
	NULL,			/* unmap */
};
