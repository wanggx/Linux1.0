/* interrupt.h */
#ifndef _LINUX_INTERRUPT_H
#define _LINUX_INTERRUPT_H

struct bh_struct {
	void (*routine)(void *);
	void *data;
};

extern unsigned long bh_active;
extern unsigned long bh_mask;
extern struct bh_struct bh_base[32];

/* Who gets which entry in bh_base.  Things which will occur most often
   should come first. */
enum {
	TIMER_BH = 0,
	CONSOLE_BH,
	SERIAL_BH,
	TTY_BH,                 /* 终端 */
	INET_BH,
	KEYBOARD_BH    /* 键盘 */
};

/* orl位或运算，将第nr位置为1 */
extern inline void mark_bh(int nr)
{
	__asm__ __volatile__("orl %1,%0":"=m" (bh_active):"ir" (1<<nr));
}

/* andl位与运算，将第nr位置0 */
extern inline void disable_bh(int nr)
{
	__asm__ __volatile__("andl %1,%0":"=m" (bh_mask):"ir" (~(1<<nr)));
}

/* 禁用中断下半部分 */
extern inline void enable_bh(int nr)
{
	__asm__ __volatile__("orl %1,%0":"=m" (bh_mask):"ir" (1<<nr));
}

#endif
