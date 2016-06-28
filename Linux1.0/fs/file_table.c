/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/fs.h>
#include <linux/string.h>
#include <linux/mm.h>

struct file * first_file;
int nr_files = 0;

/* 将file插入到链尾
 **/
static void insert_file_free(struct file *file)
{
	file->f_next = first_file;
	file->f_prev = first_file->f_prev;
	file->f_next->f_prev = file;
	file->f_prev->f_next = file;
	first_file = file;
}

/* 将文件指针从空闲链表中移除，并始终让first_file指向链首
 * 并将移除的file前后指向都指空
 */
static void remove_file_free(struct file *file)
{
	if (first_file == file)
		first_file = first_file->f_next;
	if (file->f_next)
		file->f_next->f_prev = file->f_prev;
	if (file->f_prev)
		file->f_prev->f_next = file->f_next;
	file->f_next = file->f_prev = NULL;
}

/* 将file放在以first_file为首的末尾     */
static void put_last_free(struct file *file)
{
	remove_file_free(file);
	file->f_prev = first_file->f_prev;
	file->f_prev->f_next = file;
	file->f_next = first_file;
	file->f_next->f_prev = file;
}

void grow_files(void)
{
	struct file * file;
	int i;

	file = (struct file *) get_free_page(GFP_KERNEL);

	if (!file)
		return;

	nr_files+=i= PAGE_SIZE/sizeof(struct file);

	if (!first_file)
		file->f_next = file->f_prev = first_file = file++, i--;

	for (; i ; i--)
		insert_file_free(file++);
}

unsigned long file_table_init(unsigned long start, unsigned long end)
{
	first_file = NULL;
	return start;
}

/* 获取一个空的文件描述符，注意增长的文件描述符
 * 并没有被释放，但是增长的总数不会超过NR_FILE
 * 释放文件描述符就是将f->f_count=0,然后循环查找f_count=0的
 * 描述符重复利用，该函数非阻塞，如果内存中使用的file
 * 数量大于NR_FILE，则返回失败
 */
struct file * get_empty_filp(void)
{
	int i;
	struct file * f;

	if (!first_file)
		grow_files();
repeat:
	for (f = first_file, i=0; i < nr_files; i++, f = f->f_next)
		if (!f->f_count) {
			remove_file_free(f);
			memset(f,0,sizeof(*f));
			put_last_free(f);
			f->f_count = 1;
			return f;
		}
	if (nr_files < NR_FILE) {
		grow_files();
		goto repeat;
	}
	return NULL;
}

