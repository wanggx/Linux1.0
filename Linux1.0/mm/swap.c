/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * This file should contain most things doing the swapping from/to disk.
 * Started 18.12.91
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>

#include <asm/system.h> /* for cli()/sti() */
#include <asm/bitops.h>

/* 最大交换文件数量*/
#define MAX_SWAPFILES 8

#define SWP_USED	1
#define SWP_WRITEOK	3

/* 前12位表示偏移量，后面的位数用来表示type，
 * 也就是swap_info数组中的索引
 */
#define SWP_TYPE(entry) (((entry) & 0xfe) >> 1)
#define SWP_OFFSET(entry) ((entry) >> PAGE_SHIFT)
#define SWP_ENTRY(type,offset) (((type) << 1) | ((offset) << PAGE_SHIFT))


/* 只有sys_swapon函数才会增加该变量，表示系统中交换文件的数量 */
static int nr_swapfiles = 0;

/* 等待操作swap_info的队列 */
static struct wait_queue * lock_queue = NULL;

static struct swap_info_struct {
	unsigned long flags;		/* 交换分区的标记，表示是否可已用 */
	struct inode * swap_file;	/* 交换文件对应的inode */
	unsigned int swap_device;   /* 交换文件所在的设备号*/
	unsigned char * swap_map;
	unsigned char * swap_lockmap;  /* 一页的内存 */
	int pages;				/*  交换文件中页的数量 */
	/*记录swap_lockmap中第一个位图不为0的位索引*/
	int lowest_bit;		
	/*记录swap_lockmap中最后一个位图不为0的位索引*/
	int highest_bit;
	/*记录swap_lockmap中最后一个位图不为0的位下一个位置的索引*/
	unsigned long max;
} swap_info[MAX_SWAPFILES];

extern unsigned long free_page_list;
extern int shm_swap (int);

/*
 * The following are used to make sure we don't thrash too much...
 * NOTE!! NR_LAST_FREE_PAGES must be a power of 2...
 */

/* 记录被分配出去的空闲内存
 *
 */
#define NR_LAST_FREE_PAGES 32

/* 记录系统中最后被分配出去的32块物理内存，在交换出内存时会做判断 */
static unsigned long last_free_pages[NR_LAST_FREE_PAGES] = {0,};

void rw_swap_page(int rw, unsigned long entry, char * buf)
{
	unsigned long type, offset;
	struct swap_info_struct * p;

	/* 从entry中获取swap_info数组的索引 */
	type = SWP_TYPE(entry);
	if (type >= nr_swapfiles) {
		printk("Internal error: bad swap-device\n");
		return;
	}
	p = &swap_info[type];
	/* 获取相应的偏移量 */
	offset = SWP_OFFSET(entry);
	/* 如果超过最大值则失败返回 */
	if (offset >= p->max) {
		printk("rw_swap_page: weirdness\n");
		return;
	}
	if (!(p->flags & SWP_USED)) {
		printk("Trying to swap to unused swap-device\n");
		return;
	}
	/* 如果原来就是1，则表示已被占用，需要等待*/
	while (set_bit(offset,p->swap_lockmap))
		sleep_on(&lock_queue);
	if (rw == READ)
		kstat.pswpin++;
	else
		kstat.pswpout++;
	if (p->swap_device) {
		ll_rw_page(rw,p->swap_device,offset,buf);
	} else if (p->swap_file) {
		/* 磁盘设备的逻辑块号，
		  * 使用交换文件的逻辑块号来映射的
		  */
		unsigned int zones[8];
		unsigned int block;
		int i, j;

		/* 获取交换文件读取位置的逻辑块号*/
		block = offset << (12 - p->swap_file->i_sb->s_blocksize_bits);

		/* 因为PAGE_SIZE是内存的一页大小，而s_blocksize是磁盘块一块的大小 */
		for (i=0, j=0; j< PAGE_SIZE ; i++, j +=p->swap_file->i_sb->s_blocksize)
			if (!(zones[i] = bmap(p->swap_file,block++))) {
				printk("rw_swap_page: bad swap file\n");
				return;
			}
		ll_rw_swap_file(rw,p->swap_file->i_dev, zones, i,buf);
	} else
		printk("re_swap_page: no swap file or device\n");
	if (offset && !clear_bit(offset,p->swap_lockmap))
		printk("rw_swap_page: lock already cleared\n");
	wake_up(&lock_queue);
}

/* 获取一个合适的交换页 */
unsigned int get_swap_page(void)
{
	struct swap_info_struct * p;
	unsigned int offset, type;

	p = swap_info;
	for (type = 0 ; type < nr_swapfiles ; type++,p++) {
		/* 需要是可以写的交换文件，如果是SWP_USED标记，则表示还没有准备好 */
		if ((p->flags & SWP_WRITEOK) != SWP_WRITEOK)
			continue;
		for (offset = p->lowest_bit; offset <= p->highest_bit ; offset++) {
			if (p->swap_map[offset])
				continue;
			/* 设置页的标记，同时减小总共交换页的数量 */
			p->swap_map[offset] = 1;
			nr_swap_pages--;
			if (offset == p->highest_bit)
				p->highest_bit--;
			p->lowest_bit = offset;
			return SWP_ENTRY(type,offset);
		}
	}
	return 0;
}

/* 在拷贝进程内存页表的时候，如果判断内存的页在交换区当中，
 * 则在交换区中拷贝一页，此处的拷贝就是将原来地址对应的
 * 交换区的引用计数加1，
 */
unsigned long swap_duplicate(unsigned long entry)
{
	struct swap_info_struct * p;
	unsigned long offset, type;

	if (!entry)
		return 0;
	offset = SWP_OFFSET(entry);
	type = SWP_TYPE(entry);
	if (type == SHM_SWP_TYPE)
		return entry;
	if (type >= nr_swapfiles) {
		printk("Trying to duplicate nonexistent swap-page\n");
		return 0;
	}
	p = type + swap_info;
	if (offset >= p->max) {
		printk("swap_free: weirdness\n");
		return 0;
	}
	if (!p->swap_map[offset]) {
		printk("swap_duplicate: trying to duplicate unused page\n");
		return 0;
	}
	/* 增加了映射的引用计数，之后仍然将原来的entry返回*/
	p->swap_map[offset]++;
	return entry;
}

/* 释放swap_info中的信息，同时增加交换页的数量
 * 该函数和get_swap_page正好相反
 */
void swap_free(unsigned long entry)
{
	struct swap_info_struct * p;
	unsigned long offset, type;

	if (!entry)
		return;
	type = SWP_TYPE(entry);
	if (type == SHM_SWP_TYPE)
		return;
	if (type >= nr_swapfiles) {
		printk("Trying to free nonexistent swap-page\n");
		return;
	}
	p = & swap_info[type];
	offset = SWP_OFFSET(entry);
	if (offset >= p->max) {
		printk("swap_free: weirdness\n");
		return;
	}
	if (!(p->flags & SWP_USED)) {
		printk("Trying to free swap from unused swap-device\n");
		return;
	}
	while (set_bit(offset,p->swap_lockmap))
		sleep_on(&lock_queue);
	if (offset < p->lowest_bit)
		p->lowest_bit = offset;
	if (offset > p->highest_bit)
		p->highest_bit = offset;
	if (!p->swap_map[offset])
		printk("swap_free: swap-space map bad (entry %08lx)\n",entry);
	else
		if (!--p->swap_map[offset])
			nr_swap_pages++;
	if (!clear_bit(offset,p->swap_lockmap))
		printk("swap_free: lock already cleared\n");
	wake_up(&lock_queue);
}

/* 和swap_out正好相反，从交换区中把数据交换到内存当中 
 * table_ptr代表物理页的地址
 */
void swap_in(unsigned long *table_ptr)
{
	unsigned long entry;
	unsigned long page;

	entry = *table_ptr;
	if (PAGE_PRESENT & entry) {
		printk("trying to swap in present page\n");
		return;
	}
	if (!entry) {
		printk("No swap page in swap_in\n");
		return;
	}
	if (SWP_TYPE(entry) == SHM_SWP_TYPE) {
		shm_no_page ((unsigned long *) table_ptr);
		return;
	}
	/* 为什么要先去内核申请物理页?*/
	if (!(page = get_free_page(GFP_KERNEL))) {
		oom(current);
		page = BAD_PAGE;
	} else	
		read_swap_page(entry, (char *) page);
	if (*table_ptr != entry) {
		free_page(page);
		return;
	}
	/* 设置表项和内存页的映射关系*/
	*table_ptr = page | (PAGE_DIRTY | PAGE_PRIVATE);
	swap_free(entry);
}


/* 将table_ptr指向的物理页交换出去 */
static inline int try_to_swap_out(unsigned long * table_ptr)
{
	int i;
	unsigned long page;
	unsigned long entry;

	page = *table_ptr;
	if (!(PAGE_PRESENT & page))
		return 0;
	if (page >= high_memory)
		return 0;
	if (mem_map[MAP_NR(page)] & MAP_PAGE_RESERVED)
		return 0;
	if (PAGE_ACCESSED & page) {
		*table_ptr &= ~PAGE_ACCESSED;
		return 0;
	}

	/* 如果该页内存时最近被分配的32块内存，则不交换*/
	for (i = 0; i < NR_LAST_FREE_PAGES; i++)
		if (last_free_pages[i] == (page & PAGE_MASK))
			return 0;

	/* 如果也是脏的，则将物理页中的数据写到磁盘中的交换区,
	 * 如果物理页不是脏的，则直接将物理页给释放掉，如果拥有
	 * 该物理页的进程访问该页时，会不会产生缺页中断?
	 */
	if (PAGE_DIRTY & page) {
		page &= PAGE_MASK;
		if (mem_map[MAP_NR(page)] != 1)
			return 0;
		if (!(entry = get_swap_page()))
			return 0;
		/* 注意在do_no_page函数当中会判断二级页表
		 * 中的值是否为0，如果不等于0，也就是下面这句话，
		 * 则表示该页内存已被交换到交换区
		 */
		*table_ptr = entry;
		invalidate();
		write_swap_page(entry, (char *) page);
		free_page(page);
		return 1;
	}
	page &= PAGE_MASK;
	/* 注意这一句非常重要，当把进程的对应对应内存释放后，
	 * 也相应的将页的索引值页给设置为0，当进程再次访问时
	 * 就会重新映射内存 
	 */
	*table_ptr = 0;
	invalidate();
	free_page(page);
	return 1 + mem_map[MAP_NR(page)];
}

/*
 * sys_idle() does nothing much: it just searches for likely candidates for
 * swapping out or forgetting about. This speeds up the search when we
 * actually have to swap.
 */
asmlinkage int sys_idle(void)
{
	need_resched = 1;
	return 0;
}

/*
 * A new implementation of swap_out().  We do not swap complete processes,
 * but only a small number of blocks, before we continue with the next
 * process.  The number of blocks actually swapped is determined on the
 * number of page faults, that this process actually had in the last time,
 * so we won't swap heavily used processes all the time ...
 *
 * Note: the priority argument is a hint on much CPU to waste with the
 *       swap block search, not a hint, of how much blocks to swap with
 *       each process.
 *
 * (C) 1993 Kai Petzke, wpp@marie.physik.tu-berlin.de
 */
#ifdef NEW_SWAP    /*这个宏被定义*/
/*
 * These are the miminum and maximum number of pages to swap from one process,
 * before proceeding to the next:
 */
#define SWAP_MIN	4
#define SWAP_MAX	32

/*
 * The actual number of pages to swap is determined as:
 * SWAP_RATIO / (number of recent major page faults)
 */
#define SWAP_RATIO	128

static int swap_out(unsigned int priority)
{
    static int swap_task;
    int table;
    int page;
    long pg_table;
    int loop;
    int counter = NR_TASKS * 2 >> priority;
    struct task_struct *p;

    counter = NR_TASKS * 2 >> priority;
	/* 优先级越高，循环尝试的次数越少 */
    for(; counter >= 0; counter--, swap_task++) {
	/*
	 * Check that swap_task is suitable for swapping.  If not, look for
	 * the next suitable process.
	 */
		loop = 0;
		while(1) {
	    	if(swap_task >= NR_TASKS) {
				swap_task = 1;
				if(loop)
		    		/* all processes are unswappable or already swapped out */
		    		return 0;
				loop = 1;
	    	}

	    	p = task[swap_task];
			/* 如果进程是可交换的，并且在主存中有内存，则该进程是可以被交换到交换区的 */
	    	if(p && p->swappable && p->rss)
				break;

			/* 查找下一个进程 */
	    	swap_task++;
		}

		/*
		 * Determine the number of pages to swap from this process.
		 */
		/* 计算进程有多少页需要被交换出去
		 */
		if(! p->swap_cnt) {
		    p->dec_flt = (p->dec_flt * 3) / 4 + p->maj_flt - p->old_maj_flt;
		    p->old_maj_flt = p->maj_flt;

		    if(p->dec_flt >= SWAP_RATIO / SWAP_MIN) {
				p->dec_flt = SWAP_RATIO / SWAP_MIN;
				p->swap_cnt = SWAP_MIN;
		    } else if(p->dec_flt <= SWAP_RATIO / SWAP_MAX)
				p->swap_cnt = SWAP_MAX;
		    else
				p->swap_cnt = SWAP_RATIO / p->dec_flt;
		}

		/*
		 * Go through process' page directory.
		 */
		for(table = p->swap_table; table < 1024; table++) {
			/* 获取页表的首地址 */
		    pg_table = ((unsigned long *) p->tss.cr3)[table];
		    if(pg_table >= high_memory)
			    continue;
		    if(mem_map[MAP_NR(pg_table)] & MAP_PAGE_RESERVED)
			    continue;
		    if(!(PAGE_PRESENT & pg_table)) {
			    printk("swap_out: bad page-table at pg_dir[%d]: %08lx\n",
				    table, pg_table);
			    ((unsigned long *) p->tss.cr3)[table] = 0;
			    continue;
		    }
		    pg_table &= 0xfffff000;

		    /*
		      * Go through this page table.
		      */
		    /* 循环处理页表中的每一页 */
		    for(page = p->swap_page; page < 1024; page++) {
				switch(try_to_swap_out(page + (unsigned long *) pg_table)) {
				    case 0:
						break;

				    case 1:
						p->rss--;
						/* continue with the following page the next time */
						/* 下次收缩就是在进程的下一个页表的下一个页，
						 * 当进程一直在等待的时候，收缩内存的操作可能已经被执行了好多次
						 */
						p->swap_table = table;
						p->swap_page  = page + 1;
						if((--p->swap_cnt) == 0)
						    swap_task++;
						return 1;

				    default:
						p->rss--;
						break;
				}
		    }
		    p->swap_page = 0;
		}

		/*
		 * Finish work with this process, if we reached the end of the page
		 * directory.  Mark restart from the beginning the next time.
		 */
		p->swap_table = 0;
    }
    return 0;
}

#else /* old swapping procedure */

/*
 * Go through the page tables, searching for a user page that
 * we can swap out.
 * 
 * We now check that the process is swappable (normally only 'init'
 * is un-swappable), allowing high-priority processes which cannot be
 * swapped out (things like user-level device drivers (Not implemented)).
 */
static int swap_out(unsigned int priority)
{
	static int swap_task = 1;
	static int swap_table = 0;
	static int swap_page = 0;
	int counter = NR_TASKS*8;
	int pg_table;
	struct task_struct * p;

	counter >>= priority;
check_task:
	if (counter-- < 0)
		return 0;
	if (swap_task >= NR_TASKS) {
		swap_task = 1;
		goto check_task;
	}
	p = task[swap_task];
	if (!p || !p->swappable) {
		swap_task++;
		goto check_task;
	}
check_dir:
	if (swap_table >= PTRS_PER_PAGE) {
		swap_table = 0;
		swap_task++;
		goto check_task;
	}
	pg_table = ((unsigned long *) p->tss.cr3)[swap_table];
	if (pg_table >= high_memory || (mem_map[MAP_NR(pg_table)] & MAP_PAGE_RESERVED)) {
		swap_table++;
		goto check_dir;
	}
	if (!(PAGE_PRESENT & pg_table)) {
		printk("bad page-table at pg_dir[%d]: %08x\n",
			swap_table,pg_table);
		((unsigned long *) p->tss.cr3)[swap_table] = 0;
		swap_table++;
		goto check_dir;
	}
	pg_table &= PAGE_MASK;
check_table:
	if (swap_page >= PTRS_PER_PAGE) {
		swap_page = 0;
		swap_table++;
		goto check_dir;
	}
	switch (try_to_swap_out(swap_page + (unsigned long *) pg_table)) {
		case 0: break;
		case 1: p->rss--; return 1;
		default: p->rss--;
	}
	swap_page++;
	goto check_table;
}

#endif


/* 该函数在内核通过__get_free_page函数申请内存时，
 * 如果内存不够，则将部分内存交换到交换区当中，
 * 注意交换内存的顺序
 */
static int try_to_free_page(void)
{
	int i=6;

	while (i--) {
		if (shrink_buffers(i))
			return 1;
		if (shm_swap(i))
			return 1;
		if (swap_out(i))
			return 1;
	}
	return 0;
}

/*
 * Note that this must be atomic, or bad things will happen when
 * pages are requested in interrupts (as malloc can do). Thus the
 * cli/sti's.
 */

/* 将地址addr添加到空闲物理页链表的链首
 */
static inline void add_mem_queue(unsigned long addr, unsigned long * queue)
{
	addr &= PAGE_MASK;
	*(unsigned long *) addr = *queue;
	*queue = addr;
}

/*
 * Free_page() adds the page to the free lists. This is optimized for
 * fast normal cases (no error jumps taken normally).
 *
 * The way to optimize jumps for gcc-2.2.2 is to:
 *  - select the "normal" case and put it inside the if () { XXX }
 *  - no else-statements if you can avoid them
 *
 * With the above two rules, you get a straight-line execution path
 * for the normal case, giving better asm-code.
 */

/* 释放物理内存页，如果mem_map中引用计数大于1，则最后的结果仅仅是引用计数减1
 **/
void free_page(unsigned long addr)
{
	if (addr < high_memory) {
		unsigned short * map = mem_map + MAP_NR(addr);

		if (*map) {
			/*如果不是保留的页，则释放*/
			if (!(*map & MAP_PAGE_RESERVED)) {
				unsigned long flag;
				/* 保存现场，也就是保存原来CPSR的值
				 * 然后再禁止中断，当需要开中断时，直接restore_flags即可，
				 * 可以去查查中断的过程
				 */
				save_flags(flag);
				cli();
				/* 如果该页的引用计数不等于0,则释放，只有在释放的时候才会减小
				 * 物理内存页的引用计数
				 */
				if (!--*map) {
					/* 系统中要留有一定的内存，当在释放内存的时候，
					 * 如果nr_secondary_pages小于MAX_SECONDARY_PAGES，
					 * 则将空闲物理页添加到secondary_page_list当中
					 */
					if (nr_secondary_pages < MAX_SECONDARY_PAGES) {
						add_mem_queue(addr,&secondary_page_list);
						nr_secondary_pages++;
						restore_flags(flag);
						return;
					}
					/*将其添加到空闲链表中*/
					add_mem_queue(addr,&free_page_list);
					nr_free_pages++;
				}
				restore_flags(flag);
			}
			return;
		}
		printk("Trying to free free memory (%08lx): memory probabably corrupted\n",addr);
		printk("PC = %08lx\n",*(((unsigned long *)&addr)-1));
		return;
	}
}

/*
 * This is one ugly macro, but it simplifies checking, and makes
 * this speed-critical place reasonably fast, especially as we have
 * to do things with the interrupt flag etc.
 *
 * Note that this #define is heavily optimized to give fast code
 * for the normal case - the if-statements are ordered so that gcc-2.2.2
 * will make *no* jumps for the normal code. Don't touch unless you
 * know what you are doing.
 */

/* queue空闲链表的链首，nr空闲块的数量
 */
#define REMOVE_FROM_MEM_QUEUE(queue,nr) \
	cli(); \
	/* 如果空闲链首不为NULL，则将第一个空闲页给返回 */
	if ((result = queue) != 0) { \
		/* 页的地址一定要是页对齐的，并且不能超过系统当前的最大内存 */
		if (!(result & ~PAGE_MASK) && result < high_memory) { \
			/* 移动空闲链表的首部 */
			queue = *(unsigned long *) result; \
			/* 首先要判断该页没有被用到 */
			if (!mem_map[MAP_NR(result)]) { \
				/* 设置空闲页的引用计数，同时减小空闲页的数量 */
				mem_map[MAP_NR(result)] = 1; \
				nr--; \
				/* 此处记录最后被分配到出去的内存，总共记录了32块，
				 * 记录的原因就是当系统内存吃紧时会将一部分内存给交换出去
				 * 如果需要交换出去的内存在最后被分配出去的32块当中，则不会被
				 * 交换出去，这个取决于内存的调度策略，因为刚被分配出去就又
				 * 要交换出去，显然是很不合理的，详见详见try_to_swap_out函数
				 */
				last_free_pages[index = (index + 1) & (NR_LAST_FREE_PAGES - 1)] = result; \
				restore_flags(flag); \
				return result; \
			} \
			printk("Free page %08lx has mem_map = %d\n", \
				result,mem_map[MAP_NR(result)]); \
		} else \
			printk("Result = 0x%08lx - memory map destroyed\n", result); \
		queue = 0; \
		nr = 0; \
	} else if (nr) { \
		printk(#nr " is %d, but " #queue " is empty\n",nr); \
		nr = 0; \
	} \
	restore_flags(flag)

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 *
 * Note that this is one of the most heavily called functions in the kernel,
 * so it's a bit timing-critical (especially as we have to disable interrupts
 * in it). See the above macro which does most of the work, and which is
 * optimized for a fast normal path of execution.
 */
/* 注意该函数返回的物理页中的数据并没有清0
  * 注意和get_free_page函数区别
  */
unsigned long __get_free_page(int priority)
{
	extern unsigned long intr_count;
	unsigned long result, flag;
	static unsigned long index = 0;

	/* this routine can be called at interrupt time via
	   malloc.  We want to make sure that the critical
	   sections of code have interrupts disabled. -RAB
	   Is this code reentrant? */

	if (intr_count && priority != GFP_ATOMIC) {
		printk("gfp called nonatomically from interrupt %08lx\n",
			((unsigned long *)&priority)[-1]);
		priority = GFP_ATOMIC;
	}
	save_flags(flag);
repeat:
	REMOVE_FROM_MEM_QUEUE(free_page_list,nr_free_pages);
	if (priority == GFP_BUFFER)
		return 0;
	/* 原子请求是不能被阻塞的，如处理中断和临界区时,
	 * 如果没有足够的空闲页，则直接返回失败
	 */
	if (priority != GFP_ATOMIC)
		/*看能不能释放一些缓存，如果释放成功，则继续尝试*/
		if (try_to_free_page())
			goto repeat;
	REMOVE_FROM_MEM_QUEUE(secondary_page_list,nr_secondary_pages);
	return 0;
}

/*
 * Trying to stop swapping from a file is fraught with races, so
 * we repeat quite a bit here when we have to pause. swapoff()
 * isn't exactly timing-critical, so who cares?
 */
static int try_to_unuse(unsigned int type)
{
	int nr, pgt, pg;
	unsigned long page, *ppage;
	unsigned long tmp = 0;
	struct task_struct *p;

	nr = 0;
/*
 * When we have to sleep, we restart the whole algorithm from the same
 * task we stopped in. That at least rids us of all races.
 */
repeat:
	for (; nr < NR_TASKS ; nr++) {
		p = task[nr];
		if (!p)
			continue;
		for (pgt = 0 ; pgt < PTRS_PER_PAGE ; pgt++) {
			ppage = pgt + ((unsigned long *) p->tss.cr3);
			page = *ppage;
			if (!page)
				continue;
			if (!(page & PAGE_PRESENT) || (page >= high_memory))
				continue;
			if (mem_map[MAP_NR(page)] & MAP_PAGE_RESERVED)
				continue;
			ppage = (unsigned long *) (page & PAGE_MASK);	
			for (pg = 0 ; pg < PTRS_PER_PAGE ; pg++,ppage++) {
				page = *ppage;
				if (!page)
					continue;
				if (page & PAGE_PRESENT)
					continue;
				if (SWP_TYPE(page) != type)
					continue;
				if (!tmp) {
					if (!(tmp = __get_free_page(GFP_KERNEL)))
						return -ENOMEM;
					goto repeat;
				}
				read_swap_page(page, (char *) tmp);
				if (*ppage == page) {
					*ppage = tmp | (PAGE_DIRTY | PAGE_PRIVATE);
					++p->rss;
					swap_free(page);
					tmp = 0;
				}
				goto repeat;
			}
		}
	}
	free_page(tmp);
	return 0;
}

asmlinkage int sys_swapoff(const char * specialfile)
{
	struct swap_info_struct * p;
	struct inode * inode;
	unsigned int type;
	int i;

	if (!suser())
		return -EPERM;
	i = namei(specialfile,&inode);
	if (i)
		return i;
	p = swap_info;
	for (type = 0 ; type < nr_swapfiles ; type++,p++) {
		if ((p->flags & SWP_WRITEOK) != SWP_WRITEOK)
			continue;
		if (p->swap_file) {
			if (p->swap_file == inode)
				break;
		} else {
			if (!S_ISBLK(inode->i_mode))
				continue;
			if (p->swap_device == inode->i_rdev)
				break;
		}
	}
	iput(inode);
	if (type >= nr_swapfiles)
		return -EINVAL;
	p->flags = SWP_USED;
	i = try_to_unuse(type);
	if (i) {
		p->flags = SWP_WRITEOK;
		return i;
	}
	nr_swap_pages -= p->pages;
	iput(p->swap_file);
	p->swap_file = NULL;
	p->swap_device = 0;
	vfree(p->swap_map);
	p->swap_map = NULL;
	free_page((long) p->swap_lockmap);
	p->swap_lockmap = NULL;
	p->flags = 0;
	return 0;
}

/*
 * Written 01/25/92 by Simmule Turner, heavily changed by Linus.
 *
 * The swapon system call
 */

/* 参数是一个特殊的文件路径
 */
asmlinkage int sys_swapon(const char * specialfile)
{
	struct swap_info_struct * p;
	struct inode * swap_inode;
	unsigned int type;
	int i,j;
	int error;

	if (!suser())
		return -EPERM;
	p = swap_info;
	/* 从swap_info中找到一个合适的type，起始也就是一个索引 */
	for (type = 0 ; type < nr_swapfiles ; type++,p++)
		if (!(p->flags & SWP_USED))
			break;
	if (type >= MAX_SWAPFILES)
		return -EPERM;
	if (type >= nr_swapfiles)
		nr_swapfiles = type+1;
	/* 设置SWP_USED标记 */
	p->flags = SWP_USED;
	p->swap_file = NULL;
	p->swap_device = 0;
	p->swap_map = NULL;
	p->swap_lockmap = NULL;
	p->lowest_bit = 0;
	p->highest_bit = 0;
	/* 此处是1，在读取的时候读取的索引就是0*/
	p->max = 1;
	/* 获取该路径对应的文件的inode */
	error = namei(specialfile,&swap_inode);
	if (error)
		goto bad_swap;
	error = -EBUSY;
	/* 打开的交换文件的引用计数必须是1 */
	if (swap_inode->i_count != 1)
		goto bad_swap;
	error = -EINVAL;
	/* 如果是块文件 */
	if (S_ISBLK(swap_inode->i_mode)) {
		p->swap_device = swap_inode->i_rdev;
		iput(swap_inode);
		error = -ENODEV;
		if (!p->swap_device)
			goto bad_swap;
		error = -EBUSY;
		for (i = 0 ; i < nr_swapfiles ; i++) {
			if (i == type)
				continue;
			if (p->swap_device == swap_info[i].swap_device)
				goto bad_swap;
		}
	} else if (S_ISREG(swap_inode->i_mode))
		p->swap_file = swap_inode;
	else
		goto bad_swap;
	p->swap_lockmap = (unsigned char *) get_free_page(GFP_USER);
	if (!p->swap_lockmap) {
		printk("Unable to start swapping: out of memory :-)\n");
		error = -ENOMEM;
		goto bad_swap;
	}
	read_swap_page(SWP_ENTRY(type,0), (char *) p->swap_lockmap);
	/* 页的最后10个字节有签名 */
	if (memcmp("SWAP-SPACE",p->swap_lockmap+4086,10)) {
		printk("Unable to find swap-space signature\n");
		error = -EINVAL;
		goto bad_swap;
	}
	memset(p->swap_lockmap+PAGE_SIZE-10,0,10);
	j = 0;
	p->lowest_bit = 0;
	p->highest_bit = 0;
	/* 循环扫描锁位图*/
	for (i = 1 ; i < 8*PAGE_SIZE ; i++) {
		if (test_bit(i,p->swap_lockmap)) {
			if (!p->lowest_bit)
				p->lowest_bit = i;
			p->highest_bit = i;
			p->max = i+1;
			j++;
		}
	}
	if (!j) {
		printk("Empty swap-file\n");
		error = -EINVAL;
		goto bad_swap;
	}
	/* 分配一段大小为p->max的线性地址*/
	p->swap_map = (unsigned char *) vmalloc(p->max);
	if (!p->swap_map) {
		error = -ENOMEM;
		goto bad_swap;
	}
	/* 再次扫描swap_lockmap中的值
	  * 同时设置swap_map的值*/
	for (i = 1 ; i < p->max ; i++) {
		if (test_bit(i,p->swap_lockmap))
			p->swap_map[i] = 0;
		else
			p->swap_map[i] = 0x80;
	}
	p->swap_map[0] = 0x80;
	memset(p->swap_lockmap,0,PAGE_SIZE);
	p->flags = SWP_WRITEOK;
	p->pages = j;
	nr_swap_pages += j;
	printk("Adding Swap: %dk swap-space\n",j<<2);
	return 0;
bad_swap:
	free_page((long) p->swap_lockmap);
	vfree(p->swap_map);
	iput(p->swap_file);
	p->swap_device = 0;
	p->swap_file = NULL;
	p->swap_map = NULL;
	p->swap_lockmap = NULL;
	p->flags = 0;
	return error;
}

void si_swapinfo(struct sysinfo *val)
{
	unsigned int i, j;

	val->freeswap = val->totalswap = 0;
	for (i = 0; i < nr_swapfiles; i++) {
		if (!(swap_info[i].flags & SWP_USED))
			continue;
		for (j = 0; j < swap_info[i].max; ++j)
			switch (swap_info[i].swap_map[j]) {
				case 128:
					continue;
				case 0:
					++val->freeswap;
				default:
					++val->totalswap;
			}
	}
	val->freeswap <<= PAGE_SHIFT;
	val->totalswap <<= PAGE_SHIFT;
	return;
}
