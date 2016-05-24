#ifndef _LINUX_SEGMENT_H
#define _LINUX_SEGMENT_H

/* linux内核源代码情景分析
  * 每个段都是从0地址爱是的整个4GB虚存空间， 
  * 虚拟地址到线性地址的映射保持原值不变 
  */
#define KERNEL_CS	0x10
#define KERNEL_DS	0x18

#define USER_CS		0x23
#define USER_DS		0x2B

#endif
