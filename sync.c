/*
 * 同步原语相关实现，这里有几点值得注意的:
 * 1. state-thread 的条件变量的 wait 并不要求必须与 mutex 一起使用
 *    因为其可以保证 check 条件与 wait 是原子操作，但是如果调用者在
 *    check 后，又做了任何可能导致阻塞的操作，则可能会有问题
 * 2. 任何阻塞操作最终都会导致让出 CPU 去调度其他可执行线程
*/

#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

#include "common.h"

extern time_t _st_curr_time;
extern st_utime_t _st_last_tset;
extern int _st_active_count;

static st_utime_t (*_st_utime)() = NULL;

/* 获取时间，state-thread 提供了定制化这个函数的接口，方便用户可以使用更高效的时间函数 */
st_utime_t st_utime(void) {
    if (_st_utime == NULL) {
        struct timeval tv;
        (void) gettimeofday(&tv, NULL);
        return (tv.tv_sec * 1000000LL + tv.tv_usec);
    }
    
    return (*_st_utime)();
}

/* 设置时间获取函数 */
int st_set_utime_function(st_utime_t (*func)(void)) {
    if (_st_active_count) {
        errno = EINVAL;
        return -1;
    }
    
    _st_utime = func;
    
    return 0;
}

st_utime_t st_utime_last_clock(void) {
    return _ST_LAST_CLOCK;
}

/* 设置时间缓存，如果是 on 会更新缓存的时间字段 */
int st_timecache_set(int on)
{
    int wason = (_st_curr_time) ? 1 : 0;
    
    if (on) {
        _st_curr_time = time(NULL);
        _st_last_tset = st_utime();
    } else
        _st_curr_time = 0;
    
    return wason;
}

/* 获取当前时间 */
time_t st_time() {
    if (_st_curr_time)
        return _st_curr_time;
    
    return time(NULL);
}

/* sleep us */
int st_usleep(st_utime_t usecs) {
    _st_thread_t *me = _ST_CURRENT_THREAD();

    if (me->flags & _ST_FL_INTERRUPT) {
        /*
         * 如果线程被调用过 st_thread_interrupt，清除对应 bit
         * 设置 errno 表明我们是被 "信号" 中断，返回 -1
        */
        errno = EINTR;
        return -1;
    }

    if (usecs != ST_UTIME_NO_TIMEOUT) {
        /* 如果不是让一直 sleep，设置 state 放入休眠队列 */
        me->state  = _ST_ST_SLEEPING;
        _ST_ADD_SLEEPQ(me, usecs);
    } else {
        /* 一直休眠 */
        me->state = _ST_ST_SUSPENDED;
    }

    /* 让出 CPU */
    _ST_SWITCH_CONTEXT(me);

    /* 恢复执行 */
    if (me->flags & _ST_FL_INTERRUPT) {
        /* 同上被信号打断 */
        me->flags &= ~_ST_FL_INTERRUPT;
        errno = EINTR;
        return -1;
    }

    return 0;
}

int st_sleep(int secs) {
    return st_usleep((secs >= 0) ? secs * (st_utime_t)1000000LL : ST_UTIME_NO_TIMEOUT);
}

/*****************************************
 * 条件变量相关函数
 */

/* 构造一个条件变量 */
_st_cond_t *st_cond_new() {
    _st_cond_t *cvar;

    cvar = (_st_cond_t*) calloc(1, sizeof(_st_cond_t));
    if (cvar) {
        ST_INIT_CLIST(&cvar->wait_q);
    }

    return cvar;
}

/* 释放一个条件变量 */
int st_cond_destroy(_st_cond_t *cvar) {
    if (!ST_CLIST_IS_EMPTY(&cvar->wait_q)) {
        /* 如果还有等待条件变量的线程则设置 EBUSY 的错误 */
        errno = EBUSY;
        return -1;
    }

    free(cvar);

    return 0;
}

/* 有超时的等待条件变量 */
int st_cond_timedwait(_st_cond_t *cvar, st_utime_t timeout) {
    _st_thread_t *me = _ST_CURRENT_THREAD();
    int rv;

    if (me->flags & _ST_FL_INTERRUPT) {
        /* 信号中断，后续都不再赘述 */
        errno = EINTR;
        return -1;
    }

    /* 设置线程状态，将线程放入条件变量的等待队列 */
    me->state = _ST_ST_COND_WAIT;
    ST_APPEND_LINK(&me->wait_links, &cvar->wait_q);

    if (timeout != ST_UTIME_NO_TIMEOUT) {
        /* 
         * 如果设置了超时，那么加入到睡眠队列，这里我们就看到了有些线程虽然没有
         * 自己调用 sleep 函数，但是还是可能被加入等待队列，不过这种线程的 state
         * 不会是 _ST_ST_SLEEPING
        */
       _ST_ADD_SLEEPQ(me, timeout);
    }

    /* 让出 CPU */
    _ST_SWITCH_CONTEXT(me);

    /* 
     * 恢复执行，要么是条件变量上调用了 signal 或者 broadcast 要不就是超时
     * 不管哪种情况，都将线程从条件变量的等待队列中移除
     * */
    ST_REMOVE_LINK(&me->wait_links);
    rv = 0;

    if (me->flags & _ST_FL_INTERRUPT) {
        /* 被信号中断 */
        me->flags &= ~_ST_FL_INTERRUPT;
        errno = EINTR;
        rv = -1;
    }

    if (me->flags & _ST_FL_TIMEDOUT) {
        /* 返回是因为超时 */
        errno = ETIME;
        rv = -1;
    }

    return rv;
}

/* 无超时等待 */
int st_cond_wait(_st_cond_t *cvar) {
    return st_cond_timedwait(cvar, ST_UTIME_NO_TIMEOUT);
}

/* 通知等待条件变量的线程 */
static int _st_cond_signal(_st_cond_t *cvar, int broadcast) {
    _st_thread_t *thread;
    _st_clist_t *q;

    /* 遍历所有的等待线程 */
    for (q = cvar->wait_q.next; q != &cvar->wait_q; q = q->next) {
        thread = _ST_THREAD_WAITQ_PTR(q);
        if (thread->state == _ST_ST_COND_WAIT) {
            /* 如果线程处于等待条件变量的状态，从等待队列删除(我实在没想到为什么会不是这个状态) */
            _ST_DEL_SLEEPQ(thread);
        }

        /* 线程重新变为可执行 */
        thread->state = _ST_ST_RUNNABLE;
        _ST_ADD_RUNQ(thread);
        if (!broadcast) {
            /* 如果不是广播，通知第一个线程以后就退出 */
            break;
        }
    }

    return 0;
}

int st_cond_signal(_st_cond_t *cvar) {
    return _st_cond_signal(cvar, 0);
}

int st_cond_broadcast(_st_cond_t *cvar) {
    return _st_cond_signal(cvar, 1);
}

/*****************************************
 * Mutex functions
 */

/* 创建 mutex */
_st_mutex_t *st_mutex_new() {
    _st_mutex_t *lock;

    lock = (_st_mutex_t*) calloc(1, sizeof(_st_mutex_t));
    if (lock) {
        ST_INIT_CLIST(&lock->wait_q);
    }

    return lock;
}

/* 销毁 mutex */
int st_mutex_destroy(_st_mutex_t *lock) {
    if (lock->owner != NULL || !ST_CLIST_IS_EMPTY(&lock->wait_q)) {
        /* 如果有线程持锁或者有线程阻塞在 lock 上，返回错误 */
        errno = EBUSY;
        return -1;
    }

    free(lock);

    return 0;
}

/* 阻塞式加锁 */
int st_mutex_lock(_st_mutex_t *lock) {
    _st_thread_t *me = _ST_CURRENT_THREAD();

    if (me->flags & _ST_FL_INTERRUPT) {
        errno = EINTR;
        return -1;
    }

    if (lock->owner == NULL) {
        /* 当前无人加锁，成功 */
        lock->owner = me;
        return 0;
    }

    if (lock->owner == me) {
        /* 非递归锁，不可重复加 */
        errno = EDEADLK;
        return -1;
    }

    /* 设置状态，加入到 lock 的等待队列 */
    me->state = _ST_ST_LOCK_WAIT;
    ST_APPEND_LINK(&me->wait_links, &lock->wait_q);

    /* 让出 CPU */
    _ST_SWITCH_CONTEXT(me);

    /* 恢复执行，从等待队列移除 */
    ST_REMOVE_LINK(&me->wait_links);

    if (me->flags & _ST_FL_INTERRUPT && lock->owner != me) {
        /* 如果我们是被信号打断，而且 owner 不是自己，设置错误 */
        me->flags &= ~_ST_FL_INTERRUPT;
        errno = EINTR;
        return -1;
    }

    /* 加锁成功，对 me 的修改是在 unlock 做的 */
    return 0;
}

int st_mutex_unlock(_st_mutex_t *lock) {
    _st_thread_t *thread;
    _st_clist_t *q;

    if (lock->owner != _ST_CURRENT_THREAD()) {
        /* 没有持有锁，就不准释放锁 */
        errno = EPERM;
        return -1;
    }

    for (q = lock->wait_q.next; q != &lock->wait_q; q = q->next) {
        thread = _ST_THREAD_WAITQ_PTR(q);
        if (thread->state == _ST_ST_LOCK_WAIT) {
            /* 如果是在等待锁，将锁的所有权给第一个 thread */
            lock->owner = thread;
            thread->state = _ST_ST_RUNNABLE;
            _ST_ADD_RUNQ(thread);
            return 0;
        }
    }

    /* 没有人在等待加锁 */
    lock->owner = NULL;
    return 0;
}

/* 非阻塞加锁 */
int st_mutex_trylock(_st_mutex_t *lock)
{
    if (lock->owner != NULL) {
        /* 名花有主,返回错误 */
        errno = EBUSY;
        return -1;
    }
    
    /* 成功 */
    lock->owner = _ST_CURRENT_THREAD();
    
    return 0;
}