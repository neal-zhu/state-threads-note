/*
 * 调度和线程实现代码
*/

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* 全区变量 */
_st_vp_t _st_this_vp;             /* virtual processor 单例 */
_st_thread_t *_st_this_thread;    /* 当前运行的线程指针 */
int _st_active_count = 0;         /* 系统未结束运行的线程数 */

time_t _st_curr_time = 0;         /* 当前时间 */
st_utime_t _st_last_tset;         /* 上一次获取时间 */

/* poll 这些描述符 */
int st_poll(struct pollfd *pds, int npds, st_utime_t timeout) {
    struct pollfd *pd;
    struct pollfd *epd = pds + npds;
    _st_pollq_t pq;
    _st_thread_t *me = _ST_CURRENT_THREAD();
    int n;

    if (me->flags & _ST_FL_INTERRUPT) {
        /* 调用前被自己发送了信号，设置错误 */
        me->flags &= ~_ST_FL_INTERRUPT;
        errno = EINTR;
        return -1;
    }

    /* 向事件系统中添加描述符集合 */
    if ((*_st_eventsys->pollset_add)(pds, npds) < 0) {
        return -1;
    }

    /* 添加到 IO 队列 */
    pq.pds = pds;
    pq.npds = npds;
    pq.thread = me;
    pq.on_ioq = 1;
    _ST_ADD_IOQ(pq);

    if (timeout != ST_UTIME_NO_TIMEOUT) {
        /* 如果设置了超时，加入到休眠队列 */
        _ST_ADD_SLEEPQ(me, timeout);
    }
    me->state = _ST_ST_IO_WAIT;

    /* 让出 CPU */
    _ST_SWITCH_CONTEXT(me);

    /* 恢复执行了，检查我们是为什么返回 */
    n = 0;
    if (pq.on_ioq) {
        /* 还在 IOQ 中，说明要不然就是超时，要不然就是被打断，不管怎么样从 IOQ 删除 */
        _ST_DEL_IOQ(pq);
        (*_st_eventsys->pollset_del)(pds, npds);
    } else {
        /* 触发了 IO 事件， 先遍历看看有多少事件被触发 */
        for (pd = pds; pd != epd; pd++) {
            if (pd->events) {
                n++;
            }
        }
    }

    if (me->flags & _ST_FL_INTERRUPT) {
        /* 被打断 */
        me->flags &= ~_ST_FL_INTERRUPT;
        errno = EINTR;
        return -1;
    }

    return 0;
}

/* 与源代码不同，我们这里要是一个循环 */
void _st_vp_schedule() {
    _st_thread_t *thread;

    /* 只要还有未结束线程就一直执行 */
    while (_st_active_count) {
        if (!ST_CLIST_IS_EMPTY(&(_ST_RUNQ))) {
            /* 可运行队列中有线程 */
            thread = _ST_THREAD_PTR(_ST_RUNQ.next);
            _ST_DEL_RUNQ(thread);
        } else {
            /* 没有可运行线程，恢复执行 idle 线程 */
            thread = _st_this_vp.idle_thread;
        }
        /* thread 必然是可执行态 */
        assert(thread->state == _ST_ST_RUNNABLE);
        
        /* 恢复执行线程 */
        thread->state = _ST_ST_RUNNING;
        _ST_RESTORE_CONTEXT(thread);
    }

    /* 退出 */
    exit(0);
}

ucontext_t _st_schedule_context;
/* 使用 ucontext 的话我们需要单独 init scheduler 的上下文 */
static void _st_schedule_init() {
    /* 这里就使用一个静态 buffer 作为 scheduler 的栈 */
    static char buf[4095];
    getcontext(&_st_schedule_context);
    _st_schedule_context.uc_link = NULL;
    _st_schedule_context.uc_stack.ss_size = sizeof(buf);
    _st_schedule_context.uc_stack.ss_sp = buf;
    makecontext(&_st_schedule_context, _st_vp_schedule, 0);
}

/* 初始化 virtual processor */
int st_init() {
    _st_thread_t *thread;

    if (_st_active_count) {
        /* 初始化过了 */
        return 0;
    }

    _st_schedule_init();

    /* 初始化 IO 系统 */
    if (_st_io_init() < 0) {
        return -1;
    }

    /* 初始化一些队列 */
    memset(&_st_this_vp, 0, sizeof(_st_this_vp));
    ST_INIT_CLIST(&_ST_RUNQ);
    ST_INIT_CLIST(&_ST_IOQ);
    ST_INIT_CLIST(&_ST_ZOMBIEQ);

    /* 初始化事件系统 */
    if ((*_st_eventsys->init)() < 0) {
        return -1;
    }

    _st_this_vp.pagesize = getpagesize();
    _st_this_vp.last_clock = st_utime();

    /* 创建 idle 线程，没有其他可执行线程时，就会调度执行 idle */
    _st_this_vp.idle_thread = st_thread_create(_st_idle_thread_start, NULL, 0, 0);
    if (!_st_this_vp.idle_thread) {
        return -1;
    }
    _st_this_vp.idle_thread->flags = _ST_FL_IDLE_THREAD;
    /* idle 不算做活跃线程，只有用户调用 st_thread_create 创建的和用户本来的线程才算 active */
    _st_active_count--;
    _ST_DEL_RUNQ(_st_this_vp.idle_thread);

    /* 
     * 初始化原始线程，这个就是原始的用户线程，是真正的内核线程，其与其他线程的最大区别是
     * 1. 栈不需要额外申请
     * 2. 不需要指定线程函数
     * 所以我们不使用 st_thread_create 来创建，而是手动创建
     * */
    thread = (_st_thread_t*) calloc(1, sizeof(_st_thread_t) + sizeof(void*) * ST_KEYS_MAX);
    if (!thread) {
        _st_thread_cleanup(_st_this_vp.idle_thread);
        _st_stack_free(_st_this_vp.idle_thread->stack);
        return -1;
    }
    thread->private_data = (void**)(thread + 1);
    /* 原始线程最开始处于 RUNNING，在调用 state-thread 的阻塞接口后，会开始调度其他线程 */
    thread->state = _ST_ST_RUNNING;
    thread->flags = _ST_FL_PRIMORDIAL;
    _ST_SET_CURRENT_THREAD(thread);
    _st_active_count++;

    return 0;
}

/* idle 是操作系统常见的一个设计，其实直接在 schedule 进行这个操作也可以，但是引入 idle 以后会让调度职责更单一 */
void *_st_idle_thread_start(void *arg) {
    _st_thread_t *me = _ST_CURRENT_THREAD();

    for (; ;) {
        /* 一直等待 IO 或者超时发生 */
        _ST_VP_IDLE();

        /* 看看是否是休眠队列中有超时的线程 */
        _st_vp_check_clock();
        
        me->state = _ST_ST_RUNNABLE;
        _ST_ADD_RUNQ(me);
    }
}

/* 退出线程 */
void st_thread_exit(void *retval) {
    _st_thread_t *me = _ST_CURRENT_THREAD();

    me->retval = retval;
    _st_thread_cleanup(me);
    _st_active_count--;
    if (me->term) {
        /* 如果是 joinable 线程，不能直接销毁，要先进入 zombie 状态 */
        me->state = _ST_ST_ZOMBIE;
        _ST_ADD_ZOMBIEQ(me);

        /* 先通知可能在 join 本线程的线程 */
        st_cond_signal(me->term);

        /* 让出 CPU */
        _ST_SWITCH_CONTEXT(me);

        /* ok, 我们被 join 处理掉了，进行最后的清理工作 */
        st_cond_destroy(me->term);
        /* 线程会被复用，所以必须 reset 成员！*/
        me->term = NULL;
    }

    if (!(me->flags & _ST_FL_PRIMORDIAL)) {
        /* 
         * 如果不是原始线程，回收栈资源，注意，我们的 thread 对象其实就是占用了 stack 的部分空间
         * 所以回收了栈自然也就回收了 thread 对象，即 thread 对象本身也是复用的
         * */
        _st_stack_free(me->stack);
    }

    /* 重新调度 */
    _ST_SWITCH_CONTEXT(me);

    /* 永远不会返回 */
    assert(0);
}

/* join 线程，阻塞调用者，直到被 join 线程退出 */
int st_thread_join(_st_thread_t *thread, void **retvalp) {
    _st_cond_t *term = thread->term;

    if (term == NULL) {
        /* 不可以 join 一个 detach 的线程 */
        errno = EINVAL;
        return -1;
    }

    if (_ST_CURRENT_THREAD() == thread) {
        /* 自己不可以 join 自己 */
        errno = EDEADLOCK;
        return -1;
    }

    if (!ST_CLIST_IS_EMPTY(&term->wait_q)) {
        /* 已经有人在 join 该线程 */
        errno = EINVAL;
        return -1;
    }

    /* 等待被 join 线程变为 zombie 状态，注意之前提过用户态线程的条件变量 wait 不需要与 mutex 一起使用 */
    while (thread->state != _ST_ST_ZOMBIE) {
        if (st_cond_wait(term) != 0) {
            return -1;
        }
    }

    if (retvalp) {
        /* 传递返回值 */
        *retvalp = thread->retval;
    }

    /* 
     * 还记得上面的 exit 函数吧，被 join 的线程必须再次被调度，进行最后的扫尾工作，将其重新加入到可执行队列
     * 其实直接在这里做回收工作貌似也没有什么不可以的？
     * */
    thread->state = _ST_ST_RUNNABLE;
    _ST_ADD_RUNQ(thread);
    _ST_DEL_ZOMBIEQ(thread);

    return 0;
}

/* 真正的函数主体，为了在线程结束后执行必要的打扫工作 */
void _st_thread_main() {
    _st_thread_t *thread = _ST_CURRENT_THREAD();

    /* 执行线程主体函数 */
    thread->retval = (*thread->start)(thread->arg);

    /* 对于没有自己调用 st_thread_exit 的函数，在这里执行扫尾工作 */
    st_thread_exit(thread->retval);
}

/* 休眠队列是按照最小堆组织的，但是不是使用数组，而是树 */
static _st_thread_t **heap_insert(_st_thread_t *thread) {
    int target = thread->heap_index;
    int s = target;
    _st_thread_t **p = &_ST_SLEEPQ;  /* 最小的超时线程 */
    int bits = 0;
    int bit;
    int index = 1;

    while (s) {
        s >> 1;
        bits++;
    }

    /* 类似于执行 heap insert 的上溯过程 */
    for (bit = bits - 2; bit >= 0; bit--) {
        if (thread->due < (*p)->due) {
            /* 超时时间更少，要进行上移 */
            _st_thread_t *t = *p;
            thread->left = t->left;
            thread->right = t->right;
            *p = thread;
            thread->heap_index = index;
            thread = t;
        }
        index <<= 1;
        if (target & (1 << bit)) {
            p = &((*p)->right);
            index |= 1;
        } else {
            p = &((*p)->left);
        }
    }
    thread->heap_index = index;
    *p = thread;
    thread->left = thread->right = NULL;
    return p;
}

/* 从堆中删除 */
static void heap_delete(_st_thread_t *thread) {
    _st_thread_t *t, **p;
    int bits = 0;
    int s, bit;
    
    /* First find and unlink the last heap element */
    p = &_ST_SLEEPQ;
    s = _ST_SLEEPQ_SIZE;
    while (s) {
        s >>= 1;
        bits++;
    }
    for (bit = bits - 2; bit >= 0; bit--) {
        if (_ST_SLEEPQ_SIZE & (1 << bit)) {
            p = &((*p)->right);
        } else {
            p = &((*p)->left);
        }
    }
    t = *p;
    *p = NULL;
    --_ST_SLEEPQ_SIZE;
    if (t != thread) {
        /*
         * Insert the unlinked last element in place of the element we are deleting
         */
        t->heap_index = thread->heap_index;
        p = heap_insert(t);
        t = *p;
        t->left = thread->left;
        t->right = thread->right;
        
        /*
         * Reestablish the heap invariant.
         */
        for (;;) {
            _st_thread_t *y; /* The younger child */
            int index_tmp;
            if (t->left == NULL)
                break;
            else if (t->right == NULL)
                y = t->left;
            else if (t->left->due < t->right->due)
                y = t->left;
            else
                y = t->right;
            if (t->due > y->due) {
                _st_thread_t *tl = y->left;
                _st_thread_t *tr = y->right;
                *p = y;
                if (y == t->left) {
                    y->left = t;
                    y->right = t->right;
                    p = &y->left;
                } else {
                    y->left = t->left;
                    y->right = t;
                    p = &y->right;
                }
                t->left = tl;
                t->right = tr;
                index_tmp = t->heap_index;
                t->heap_index = y->heap_index;
                y->heap_index = index_tmp;
            } else {
                break;
            }
        }
    }
    thread->left = thread->right = NULL;
}

/* 添加休眠线程 */
void _st_add_sleep_q(_st_thread_t *thread, st_utime_t timeout)
{
    /* 注意这个时间是缓存 */
    thread->due = _ST_LAST_CLOCK + timeout;
    thread->flags |= _ST_FL_ON_SLEEPQ;
    thread->heap_index = ++_ST_SLEEPQ_SIZE;
    heap_insert(thread);
}

/* 从休眠队列删除 */
void _st_del_sleep_q(_st_thread_t *thread) {
    heap_delete(thread);
    thread->flags &= ~_ST_FL_ON_SLEEPQ;
}

/* 检查休眠队列 */
void _st_vp_check_clock() {
    _st_thread_t *thread;
    st_utime_t elapsed, now;

    now = st_utime();
    elapsed = now - _ST_LAST_CLOCK;
    _ST_LAST_CLOCK = now;

    if (_st_curr_time && now - _st_last_tset > 999000) {
        /* 每隔一段时间设置当前时间 */
        _st_curr_time = time(NULL);
        _st_last_tset = now;
    }

    /* 遍历所有休眠线程 */
    while (_ST_SLEEPQ != NULL) {
        thread = _ST_SLEEPQ;
        assert(thread->flags & _ST_FL_ON_SLEEPQ);
        if (thread->due > now) {
            /* 遇到了第一个未超时线程就退出 */
            break;
        }
        _ST_DEL_SLEEPQ(thread);

        /* 如果在等待条件变量，设置超时 flag 好让对方知道是超时 */
        if (thread->state == _ST_ST_COND_WAIT) {
            thread->flags |= _ST_FL_TIMEDOUT;
        }

        /* 绝不可能是 idle 线程在休眠 */
        assert(!(thread->flags & _ST_FL_IDLE_THREAD));
        /* 将超时线程恢复到可执行状态 */
        thread->state = _ST_ST_RUNNABLE;
        _ST_ADD_RUNQ(thread);
    }
}

/* 
 * 打断某线程(类似于真正线程被 os 信号打断)，让其从阻塞操作中恢复
 * 并且设置 flags |= _ST_FL_INTERRUPT，如果对非阻塞线程调用此函数
 * 会让其下一个阻塞调用直接返回错误 EINTR
 * */
void st_thread_interrupt(_st_thread_t *thread) {
    if (thread->state == _ST_ST_ZOMBIE) {
        /* 不能打断一个僵死线程 */
        return;
    }

    thread->flags |= _ST_FL_INTERRUPT;

    if (thread->state == _ST_ST_RUNNABLE || thread->state == _ST_ST_RUNNING) {
        /* 这两种状态的线程时非阻塞，直接返回即可 */
        return;
    }

    /* 如果在睡眠队列，将其从中删除 */
    if (thread->flags & _ST_FL_ON_SLEEPQ) {
        _ST_DEL_SLEEPQ(thread);
    }
    
    /* 让阻塞队列恢复为可运行态 */
    thread->state = _ST_ST_RUNNABLE;
    _ST_ADD_RUNQ(thread);
}

/* 创建线程 */
_st_thread_t *st_thread_create(void *(*start)(void *arg), void *arg, int joinable, int stk_size) {
    _st_thread_t *thread;
    _st_stack_t *stack;
    void **ptds;
    char *sp;

    if (stk_size == 0) {
        stk_size = ST_DEFAULT_STACK_SIZE;
    }
    /* 调整栈大小为 pagsize 整数倍 */
    stk_size = ((stk_size + _ST_PAGE_SIZE - 1) / _ST_PAGE_SIZE) * _ST_PAGE_SIZE;
    stack = _st_stack_new(stk_size);
    if (!stack) {
        return NULL;
    }

    /* 栈上分配的数据还包括 st_thread_t，线程私有数据数组 */
    sp = stack->stk_bottom;
    thread = (_st_thread_t*) sp;
    sp += sizeof(_st_thread_t);
    ptds = (void**) sp;
    sp += (ST_KEYS_MAX * sizeof(void*));
    stack->sp = sp;

    /* 保证栈地址 64-bytes 对齐 */
    if ((unsigned long)sp & 0x3f)
        sp = sp + (0x40 - ((unsigned long)sp & 0x3f));

    memset(thread, 0, sizeof(_st_thread_t));
    memset(ptds, 0, ST_KEYS_MAX * sizeof(void*));

    /* 设置字段 */
    thread->stack = stack;
    thread->start = start;
    thread->arg = arg;
    thread->private_data = ptds;

    /* 初始化线程上下文 */
    _ST_INIT_CONTEXT(thread, _st_thread_main);

    /* 新创建的线程都是可运行态 */
    thread->state = _ST_ST_RUNNABLE;
    _st_active_count++;
    _ST_ADD_RUNQ(thread);

    return thread;
}

/* 获取当前正在执行的线程 */
_st_thread_t *st_thread_self() {
    return _ST_CURRENT_THREAD();
}