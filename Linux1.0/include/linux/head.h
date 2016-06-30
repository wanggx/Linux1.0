#ifndef _LINUX_HEAD_H
#define _LINUX_HEAD_H

typedef struct desc_struct {
	unsigned long a,b;
} desc_table[256];

/* 内核全局页目录表正好占用一页的内存，通过二级页表映射，
  * 可以寻址的范围是4GB
  */

extern unsigned long swapper_pg_dir[1024];
extern desc_table idt,gdt;

#define GDT_NUL 0
#define GDT_CODE 1
#define GDT_DATA 2
#define GDT_TMP 3

#define LDT_NUL 0
#define LDT_CODE 1
#define LDT_DATA 2

#endif
