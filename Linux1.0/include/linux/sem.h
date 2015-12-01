#ifndef _LINUX_SEM_H
#define _LINUX_SEM_H
#include <linux/ipc.h>

/* semop flags */
#define SEM_UNDO        010000  /* undo the operation on exit */

/* semctl Command Definitions. */
#define GETPID  11       /* get sempid */
#define GETVAL  12       /* get semval */
#define GETALL  13       /* get all semval's */
#define GETNCNT 14       /* get semncnt */
#define GETZCNT 15       /* get semzcnt */
#define SETVAL  16       /* set semval */
#define SETALL  17       /* set all semval's */

/* One semid data structure for each set of semaphores in the system. */
struct semid_ds {
  struct ipc_perm sem_perm;       /* permissions .. see ipc.h */
  time_t          sem_otime;      /* last semop time */
  time_t          sem_ctime;      /* last change time */
  struct sem      *sem_base;      /* ptr to first semaphore in array */
  /* 等待信号资源的进程队列 */
  struct wait_queue *eventn;
  /* 等到信号值为0的进程等待队列 */
  struct wait_queue *eventz;     
  struct sem_undo  *undo;	  /* undo requests on this array */
  ushort          sem_nsems;      /* no. of semaphores in array */ /* 信号量集中信号量的数量 */
};


/* One semaphore structure for each semaphore in the system. */
struct sem {
/* 最后一次调用sem_op的进程ID */
  short   sempid;         /* pid of last operation */
/* 当前信号量的值 */
  ushort  semval;         /* current value */
/* 如果当前信号量值semval不足sem_op准备减去的信号量数值
 * 该进程就阻塞，semncnt自加1，当当前信号值足够了，进程解锁
 * semncnt自动减1，等待该信号量值大于当前值的进程数（一有进程释放资源 就被唤醒）
 */
  ushort  semncnt;        /* num procs awaiting increase in semval */
/* 等待该信号量值等于0的进程数 */
  ushort  semzcnt;        /* num procs awaiting semval = 0 */
};

/* semop system calls takes an array of these.*/
/* 	（1）若sem_op为正，这对应于进程释放占用的资源数。sem_op值加到信号量的值上。（V操作）
	（2）若sem_op为负,这表示要获取该信号量控制的资源数。信号量值减去sem_op的绝对值。（P操作）
	（3）若sem_op为0,这表示调用进程希望等待到该信号量值变成0
 */
struct sembuf {
  ushort  sem_num;        /* semaphore index in array */
  short   sem_op;         /* semaphore operation */
  short   sem_flg;        /* operation flags */
};

/* arg for semctl system calls. */
union semun {
  int val;               /* value for SETVAL */
  struct semid_ds *buf;  /* buffer for IPC_STAT & IPC_SET */
  ushort *array;         /* array for GETALL & SETALL */
};


struct  seminfo {
    int semmap; 
    int semmni; 
    int semmns; 
    int semmnu; 
    int semmsl; 
    int semopm; 
    int semume; 
    int semusz; 
    int semvmx; 
    int semaem; 
};

#define SEMMNI  128             /* ?  max # of semaphore identifiers */
#define SEMMSL  32              /* <= 512 max num of semaphores per id */
#define SEMMNS  (SEMMNI*SEMMSL) /* ? max # of semaphores in system */
#define SEMOPM  32	        /* ~ 100 max num of ops per semop call */
#define SEMVMX  32767           /* semaphore maximum value */

/* unused */
#define SEMUME  SEMOPM          /* max num of undo entries per process */
#define SEMMNU  SEMMNS          /* num of undo structures system wide */
#define SEMAEM  (SEMVMX >> 1)   /* adjust on exit max value */
#define SEMMAP  SEMMNS          /* # of entries in semaphore map */
#define SEMUSZ  20		/* sizeof struct sem_undo */ 

#ifdef __KERNEL__
/* ipcs ctl cmds */
#define SEM_STAT 18	
#define SEM_INFO 19

/* per process undo requests */
/* this gets linked into the task_struct */

/* 程序结束时(不论正常或不正常)，
 * 保证信号值会被重设为semop()调用前的值。
 * 这样做的目的在于避免程序在异常情况下结束时未将锁定的资源解锁，
 * 造成该资源永远锁定。是信号量很重要的一个参数，但是使用它一定要注意
 */
struct sem_undo {
    struct sem_undo *proc_next;  /*指向进程undo链表中的下一个节点*/
    struct sem_undo *id_next;	/*指向信号集的undo链表中的下一个节点*/
    int    semid;	    /* 信号集ID */
	/* 主要用于undo,表示之前最后一次信号量的值  */
    short  semadj; 		/* semval adjusted by exit */
    ushort sem_num; 		/* semaphore index in array semid */
};      

#endif /* __KERNEL__ */

#endif /* _LINUX_SEM_H */
