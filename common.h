#pragma once

#include <assert.h>
#include <stddef.h>
#include <ucontext.h>
#include <poll.h>
#include <stdio.h>

#include "public.h"

#define ST_BEGIN_MACRO {
#define ST_END_MACRO }

/* 注意，所有不在 public.h 文件中的 struct，函数都是 _ 开头的 */
/* 代表其是内部使用的，并不对外暴露，提供给调用者的接口仅为 public.h */

/*****************************************
 * 双向循环链表，就是一个很常见的双向链表实现
 */

typedef struct _st_clist {
  struct _st_clist *next;
  struct _st_clist *prev;
} _st_clist_t;

/* 将 _e 插入到节点 _l 前 */
#define ST_INSERT_BEFORE(_e, _l)   \
  ST_BEGIN_MACRO(_e)->next = (_l); \
  (_e)->prev = (_l)->prev;         \
  (_l)->prev->next = (_e);         \
  (_l)->prev = (_e);               \
  ST_END_MACRO

/* 将 _e 插入到节点 _l 后 */
#define ST_INSERT_AFTER(_e, _l)          \
  ST_BEGIN_MACRO(_e)->next = (_l)->next; \
  (_e)->prev = (_l);                     \
  (_l)->next->prev = (_e);               \
  (_l)->next = (_e);                     \
  ST_END_MACRO

/* 返回 _e 的后续节点 */
#define ST_NEXT_LINK(_e) ((_e)->next)

/* 将 _e 添加到链表 _l 的尾端 */
#define ST_APPEND_LINK(_e, _l) ST_INSERT_BEFORE(_e, _l)

/* 将 _e 添加到链表 _l 的开头 */
#define ST_INSERT_LINK(_e, _l) ST_INSERT_AFTER(_e, _l)

/* 获取链表的头尾元素 */
#define ST_LIST_HEAD(_l) (_l)->next
#define ST_LIST_TAIL(_l) (_l)->prev

/* 将 _e 从链表中移除 */
#define ST_REMOVE_LINK(_e)                     \
  ST_BEGIN_MACRO(_e)->prev->next = (_e)->next; \
  (_e)->next->prev = (_e)->prev;               \
  ST_END_MACRO

/* 链表是否为空 */
#define ST_CLIST_IS_EMPTY(_l) ((_l)->next == (_l))

/* 初始化链表 */
#define ST_INIT_CLIST(_l)          \
  ST_BEGIN_MACRO(_l)->next = (_l); \
  (_l)->prev = (_l);               \
  ST_END_MACRO

#define ST_INIT_STATIC_CLIST(_l) \
  { (_l), (_l) }

/*****************************************
 * 协程运行栈
 */

typedef void (*_st_destructor_t)(void *);

typedef struct _st_stack {
  _st_clist_t links;
  char *vaddr;      /* 栈占用的虚拟内存的开始字节 */
  int vaddr_size;   /* 栈占用的虚拟内存总量 */
  int stk_size;     /* 栈容量 */
  char *stk_bottom; /* 栈底部 */
  char *stk_top;    /* 栈顶 */
  void *sp;         /* 栈指针 */
} _st_stack_t;

/*****************************************
 * 条件变量
 */

typedef struct _st_cond {
  _st_clist_t wait_q; /* 等待条件变量的线程队列 */
} _st_cond_t;

/*****************************************
 * 线程
 */
typedef struct _st_thread _st_thread_t;

struct _st_thread {
  int state; /* 线程状态 */
  int flags; /* 线程 flags */

  void *(*start)(void *arg); /* 线程的 start 函数 */
  void *arg;                 /* start 函数的启动参数 */
  void *retval;              /* 线程 start 函数的返回结果 */

  _st_stack_t *stack; /* 线程执行栈 */

  _st_clist_t links;      /* run/sleep/zombie 队列指针 */
  _st_clist_t wait_links; /* mutex/condvar 等待队列指针 */

  st_utime_t due;      /* 线程的 sleep 结束时间 */
  _st_thread_t *left;  /* 超时堆 */
  _st_thread_t *right; /* -- see docs/timeout_heap.txt for details */
  int heap_index;

  void **private_data; /* 线程私有数据 */

  _st_cond_t *term; /* 线程结束时，join 使用的条件变量 */

  ucontext_t
      context; /* 线程上下文，源代码使用的是 jum_buf，我们使用 ucontext */
};

/*****************************************
 * 互斥锁
 */
typedef struct _st_mutex {
  _st_thread_t *owner; /* 获取了锁的线程 */
  _st_clist_t wait_q;  /* 等待获取锁的线程队列 */
} _st_mutex_t;

/*****************************************
 * poll 队列
 */
typedef struct _st_pollq {
  _st_clist_t links;    /* io 队列指针 */
  _st_thread_t *thread; /* 正在执行 polling 的 thread */
  struct pollfd *pds;   /* polling 的描述符数组 */
  int npds;             /* 数组长度 */
  int on_ioq;           /* Is it on ioq? */
} _st_pollq_t;

/*****************************************
 * 事件系统，可以理解为一个接口类，我们后面只会实现 epoll 的代码
 */
typedef struct _st_eventsys_ops {
  const char *name;                           /* 事件系统的名字 */
  int (*init)(void);                          /* 初始化 */
  void (*dispatch)(void);                     /* dispatch 用于分发事件 */
  int (*pollset_add)(struct pollfd *, int);   /* 添加一组描述符 */
  void (*pollset_del)(struct pollfd *, int);  /* 删除一组描述符 */
  int (*fd_new)(int);                         /* 向事件系统添加一个文件描述符 */
  int (*fd_close)(int);                       /* 关闭某文件描述 */
  int (*fd_getlimit)(void);                   /* 文件描述符的上限 */
} _st_eventsys_t;

/*****************************************
 * virtual processor
 */
typedef struct _st_vp {
  _st_thread_t *idle_thread; /* Idle thread */
  st_utime_t last_clock;     /* 最后一次调用 vp_check_clock() 时间 */

  _st_clist_t run_q;    /* 处于可运行态的线程队列 */
  _st_clist_t io_q;     /* 等待 IO 事件的线程队列 */
  _st_clist_t zombie_q; /* 僵尸线程队列 */
  int pagesize;

  _st_thread_t *sleep_q; /* 休眠线程堆 */
  int sleepq_size;       /* 休眠线程数 */
} _st_vp_t;

/*****************************************
 * 文件描述符的额外封装
 */
typedef struct _st_netfd {
  int osfd;                    /* 底层的操作系统文件描述符 */
  int inuse;                   /* 是否在使用 */
  void *private_data;          /* 私有数据 */
  _st_destructor_t destructor; /* 私有数据的析构函数 */
  void *aux_data;         /* 辅助数据，用于实现 serialize accept */
  struct _st_netfd *next; /* 用单链表组织该资源 */
} _st_netfd_t;

/*****************************************
 * Current vp, thread, and event system
 */
extern _st_vp_t _st_this_vp;
extern _st_thread_t *_st_this_thread;
extern _st_eventsys_t *_st_eventsys;

#define _ST_CURRENT_THREAD() (_st_this_thread)
#define _ST_SET_CURRENT_THREAD(_thread) (_st_this_thread = (_thread))

#define _ST_LAST_CLOCK (_st_this_vp.last_clock)

#define _ST_RUNQ (_st_this_vp.run_q)
#define _ST_IOQ (_st_this_vp.io_q)
#define _ST_ZOMBIEQ (_st_this_vp.zombie_q)

#define _ST_PAGE_SIZE (_st_this_vp.pagesize)

#define _ST_SLEEPQ (_st_this_vp.sleep_q)
#define _ST_SLEEPQ_SIZE (_st_this_vp.sleepq_size)

#define _ST_VP_IDLE() (*_st_eventsys->dispatch)()

/*****************************************
 * virtual processor 队列相关操作
 */
#define _ST_ADD_IOQ(_pq) ST_APPEND_LINK(&_pq.links, &_ST_IOQ)
#define _ST_DEL_IOQ(_pq) ST_REMOVE_LINK(&_pq.links)

#define _ST_ADD_RUNQ(_thr) ST_APPEND_LINK(&(_thr)->links, &_ST_RUNQ)
#define _ST_DEL_RUNQ(_thr) ST_REMOVE_LINK(&(_thr)->links)

#define _ST_ADD_SLEEPQ(_thr, _timeout) _st_add_sleep_q(_thr, _timeout)
#define _ST_DEL_SLEEPQ(_thr) _st_del_sleep_q(_thr)

#define _ST_ADD_ZOMBIEQ(_thr) ST_APPEND_LINK(&(_thr)->links, &_ST_ZOMBIEQ)
#define _ST_DEL_ZOMBIEQ(_thr) ST_REMOVE_LINK(&(_thr)->links)

/*****************************************
 * Thread states and flags
 */

/* 正在运行 */
#define _ST_ST_RUNNING 0
/* 可以运行 */
#define _ST_ST_RUNNABLE 1
/* 等待 IO 事件就绪 */
#define _ST_ST_IO_WAIT 2
/* 等待获取锁 */
#define _ST_ST_LOCK_WAIT 3
/* 等待条件变量 */
#define _ST_ST_COND_WAIT 4
/* 调用了 sleep */
#define _ST_ST_SLEEPING 5
/* 僵尸态，退出线程后，join 之前处于此状态 */
#define _ST_ST_ZOMBIE 6
/* 无限制休眠 */
#define _ST_ST_SUSPENDED 7

/* 原始线程(代表初始的用户线程) */
#define _ST_FL_PRIMORDIAL 0x01
/* 空闲线程 */
#define _ST_FL_IDLE_THREAD 0x02
/* 是否在 sleepq 中，注意不处于 sleeping 态的线程也可能处于 sleepq */
#define _ST_FL_ON_SLEEPQ 0x04
/* 被调用 st_thread_interrupt() 打断 */
#define _ST_FL_INTERRUPT 0x08
/* 等待超时 */
#define _ST_FL_TIMEDOUT 0x10

/*****************************************
 * 指针转型，因为 clist 使用的类似嵌套结构体的方式，我们需要可以用一个 clist
 * 指针转型为对应的真正数据结构
 */

#ifndef offsetof
#define offsetof(type, identifier) ((size_t) & (((type *)0)->identifier))
#endif

#define _ST_THREAD_PTR(_qp) \
  ((_st_thread_t *)((char *)(_qp)-offsetof(_st_thread_t, links)))

#define _ST_THREAD_WAITQ_PTR(_qp) \
  ((_st_thread_t *)((char *)(_qp)-offsetof(_st_thread_t, wait_links)))

#define _ST_THREAD_STACK_PTR(_qp) \
  ((_st_stack_t *)((char *)(_qp)-offsetof(_st_stack_t, links)))

#define _ST_POLLQUEUE_PTR(_qp) \
  ((_st_pollq_t *)((char *)(_qp)-offsetof(_st_pollq_t, links)))

/*****************************************
 * 常量
 */

#ifndef ST_UTIME_NO_TIMEOUT
#define ST_UTIME_NO_TIMEOUT ((st_utime_t)-1LL)
#endif

#define ST_DEFAULT_STACK_SIZE (128 * 1024) /* Includes register stack size */

#ifndef ST_KEYS_MAX
#define ST_KEYS_MAX 16
#endif

#ifndef ST_MIN_POLLFDS_SIZE
#define ST_MIN_POLLFDS_SIZE 64
#endif

/*****************************************
 * 线程上下文切换，这里是重点，我们改为使用
 * ucontext，移植性不足，但是可以避免汇编 我们删除了一些 debug 使用的 hook (比如
 * switch in/out callback)
 */

/* scheduler routine 使用的上下文 */
extern ucontext_t _st_schedule_context;

/*
 * 换出当前线程，进入调度器
 */
#define _ST_SWITCH_CONTEXT(_thread)                  \
  ST_BEGIN_MACRO                                     \
  swapcontext(&(_thread)->context, &_st_schedule_context); \
  ST_END_MACRO

/*
 * 恢复之前保存的一个线程的上下文
 */
#define _ST_RESTORE_CONTEXT(_thread) \
  ST_BEGIN_MACRO                     \
  _ST_SET_CURRENT_THREAD(_thread);   \
  swapcontext(&_st_schedule_context, &(_thread)->context); \
  ST_END_MACRO

/*
 * 初始化线程上下文
 */
#define _ST_INIT_CONTEXT(_thread, _main)                                                    \
  ST_BEGIN_MACRO                                                                            \
  getcontext(&(_thread)->context);                                                          \
  (_thread)->context.uc_stack.ss_sp = (_thread)->stack->sp;                                 \
  (_thread)->context.uc_stack.ss_size = (_thread)->stack->stk_top - (char*)(_thread)->stack->sp; \
  (_thread)->context.uc_link = &_st_schedule_context;                                             \
  makecontext(&(_thread)->context, _main, 0);                                               \
  ST_END_MACRO

/*****************************************
 * 一些函数的前置声明
 */

void _st_vp_schedule(void);
void _st_vp_check_clock(void);
void *_st_idle_thread_start(void *arg);
void _st_thread_main(void);
void _st_thread_cleanup(_st_thread_t *thread);
void _st_add_sleep_q(_st_thread_t *thread, st_utime_t timeout);
void _st_del_sleep_q(_st_thread_t *thread);
_st_stack_t *_st_stack_new(int stack_size);
void _st_stack_free(_st_stack_t *ts);
int _st_io_init(void);

st_utime_t st_utime(void);
_st_cond_t *st_cond_new(void);
int st_cond_destroy(_st_cond_t *cvar);
int st_cond_timedwait(_st_cond_t *cvar, st_utime_t timeout);
int st_cond_signal(_st_cond_t *cvar);
ssize_t st_read(_st_netfd_t *fd, void *buf, size_t nbyte, st_utime_t timeout);
ssize_t st_write(_st_netfd_t *fd, const void *buf, size_t nbyte,
                 st_utime_t timeout);
int st_poll(struct pollfd *pds, int npds, st_utime_t timeout);
_st_thread_t *st_thread_create(void *(*start)(void *arg), void *arg,
                               int joinable, int stk_size);