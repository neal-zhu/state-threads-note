/*
 * 事件系统的代码都相差不多，关于 epoll 的实现，只要记得同一个 fd 要保存其已经监听的事项
 * 计算出修改后的对应的 events 以及 op 即可
 * 但是 state-threads 中，事件系统与 vp, thread 耦合在了一起（因为其等待时间以及 epoll_wait 返回
 * 后要操作 IOQ，RUNQ 等队列)，难免显得有点不纯粹。也许可以通过类似提供回调函数等操作将其余其他模块解耦
 * 不过话说回来，有时候必要的耦合反而是合理的 tradeoff
 * 还有，这个接口使用的 struct pollfd，因为其具备足够的移植性
*/

#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include "common.h"


/* 文件描述符的事件信息 */
typedef struct _epoll_fd_data {
    int rd_ref_cnt;     /* 读事件引用计数 */
    int wr_ref_cnt;     /* 写事件引用计数 */
    int ex_ref_cnt;     /* except 引用计数 */
    int revents;        /* 触发的事件 */
} _epoll_fd_data_t;

static struct _st_epolldata {
    _epoll_fd_data_t *fd_data;      /* 文件描述符数组 */
    struct epoll_event *evtlist;    /* epoll 触发事件数组 */
    int fd_data_size;               /* 文件描述数组长度 */
    int evtlist_size;               /* epoll 触发事件数组长度 */
    int evtlist_cnt;                /* epoll 触发事件数 */
    int fd_hint;                    /* 创建 epoll 的 hint */
    int epfd;                       /* epoll 的句柄 */
    pid_t pid;                      /* 进程 id */
} *_st_epoll_data;

#ifndef ST_EPOLL_EVTLIST_SIZE
    /* Not a limit, just a hint */
    #define ST_EPOLL_EVTLIST_SIZE 4096
#endif

/* 一些宏定义 */
#define _ST_EPOLL_READ_CNT(fd)   (_st_epoll_data->fd_data[fd].rd_ref_cnt)
#define _ST_EPOLL_WRITE_CNT(fd)  (_st_epoll_data->fd_data[fd].wr_ref_cnt)
#define _ST_EPOLL_EXCEP_CNT(fd)  (_st_epoll_data->fd_data[fd].ex_ref_cnt)
#define _ST_EPOLL_REVENTS(fd)    (_st_epoll_data->fd_data[fd].revents)

#define _ST_EPOLL_READ_BIT(fd)   (_ST_EPOLL_READ_CNT(fd) ? EPOLLIN : 0)
#define _ST_EPOLL_WRITE_BIT(fd)  (_ST_EPOLL_WRITE_CNT(fd) ? EPOLLOUT : 0)
#define _ST_EPOLL_EXCEP_BIT(fd)  (_ST_EPOLL_EXCEP_CNT(fd) ? EPOLLPRI : 0)
#define _ST_EPOLL_EVENTS(fd) \
    (_ST_EPOLL_READ_BIT(fd)|_ST_EPOLL_WRITE_BIT(fd)|_ST_EPOLL_EXCEP_BIT(fd))

/* epoll 相关事件接口 */

/* 初始化 */
static int _st_epoll_init(void)
{
    int fdlim;
    int err = 0;
    int rv = 0;

    _st_epoll_data = (struct _st_epolldata *) calloc(1, sizeof(*_st_epoll_data));
    if (!_st_epoll_data)
        return -1;

    // 这个 hint 其实不是必须的，简单的使用 0 也是 ok 的
    fdlim = st_getfdlimit();
    _st_epoll_data->fd_hint = (fdlim > 0 && fdlim < ST_EPOLL_EVTLIST_SIZE) ? fdlim : ST_EPOLL_EVTLIST_SIZE;

    /* 创建 epoll 句柄 */
    if ((_st_epoll_data->epfd = epoll_create(_st_epoll_data->fd_hint)) < 0) {
        err = errno;
        rv = -1;
        goto cleanup_epoll;
    }
    fcntl(_st_epoll_data->epfd, F_SETFD, FD_CLOEXEC);
    _st_epoll_data->pid = getpid();

    /* 申请描述符数组 */
    _st_epoll_data->fd_data_size = _st_epoll_data->fd_hint;
    _st_epoll_data->fd_data = (_epoll_fd_data_t *)calloc(_st_epoll_data->fd_data_size, sizeof(_epoll_fd_data_t));
    if (!_st_epoll_data->fd_data) {
        err = errno;
        rv = -1;
        goto cleanup_epoll;
    }

    /* 申请事件数组 */
    _st_epoll_data->evtlist_size = _st_epoll_data->fd_hint;
    _st_epoll_data->evtlist = (struct epoll_event *)malloc(_st_epoll_data->evtlist_size * sizeof(struct epoll_event));
    if (!_st_epoll_data->evtlist) {
        err = errno;
        rv = -1;
    }

    /* 失败以后的清理工作 */
 cleanup_epoll:
    if (rv < 0) {
        if (_st_epoll_data->epfd >= 0)
            close(_st_epoll_data->epfd);
        free(_st_epoll_data->fd_data);
        free(_st_epoll_data->evtlist);
        free(_st_epoll_data);
        _st_epoll_data = NULL;
        errno = err;
    }

    return rv;
}

/* 扩展 epoll 中的描述符数组 */
static int _st_epoll_fd_data_expand(int maxfd)
{
    _epoll_fd_data_t *ptr;
    int n = _st_epoll_data->fd_data_size;

    /* 确定扩容目标大小 */
    while (maxfd >= n)
        n <<= 1;

    /* realloc 扩大数组 */
    ptr = (_epoll_fd_data_t *)realloc(_st_epoll_data->fd_data, n * sizeof(_epoll_fd_data_t));
    if (!ptr)
        return -1;

    memset(ptr + _st_epoll_data->fd_data_size, 0, (n - _st_epoll_data->fd_data_size) * sizeof(_epoll_fd_data_t));

    /* 更新数组指针的大小 */
    _st_epoll_data->fd_data = ptr;
    _st_epoll_data->fd_data_size = n;

    return 0;
}

/* 扩展事件数组 */
static void _st_epoll_evtlist_expand(void)
{
    struct epoll_event *ptr;
    int n = _st_epoll_data->evtlist_size;

    while (_st_epoll_data->evtlist_cnt > n)
        n <<= 1;

    ptr = (struct epoll_event *)realloc(_st_epoll_data->evtlist, n * sizeof(struct epoll_event));
    if (ptr) {
        _st_epoll_data->evtlist = ptr;
        _st_epoll_data->evtlist_size = n;
    }
}

/* 删除描述符数组 */
static void _st_epoll_pollset_del(struct pollfd *pds, int npds) {
    struct epoll_event ev;
    struct pollfd *pd;
    struct pollfd *epd = pds + npds;
    int old_events, events, op;

    /*
     * 这里没有处理删除失败的情况，因为删除失败的描述符要不然就会 close
     * 或者在 dispatch 中当其上的事件触发后被删除
     */
    for (pd = pds; pd < epd; pd++) {
        /* 计算新事件 flags */
        old_events = _ST_EPOLL_EVENTS(pd->fd);
        /* 相关引用计数 */
        if (pd->events & POLLIN)
            _ST_EPOLL_READ_CNT(pd->fd)--;
        if (pd->events & POLLOUT)
            _ST_EPOLL_WRITE_CNT(pd->fd)--;
        if (pd->events & POLLPRI)
            _ST_EPOLL_EXCEP_CNT(pd->fd)--;

        events = _ST_EPOLL_EVENTS(pd->fd);
        /*
         * 注意这里只有当 fd 上没有任何已触发事件的情况下才会去删除，这是为了这个函数可以在
         * dispatch 内部被调用
         */
        if (events != old_events && _ST_EPOLL_REVENTS(pd->fd) == 0) {
            /* 如果 events 已经为 0，那么删除否则则是修改 */
            op = events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            /* 调用 epoll 对其进行修改或删除,同时修改相应计数字段 */
            ev.events = events;
            ev.data.fd = pd->fd;
            if (epoll_ctl(_st_epoll_data->epfd, op, pd->fd, &ev) == 0 && op == EPOLL_CTL_DEL) {
                _st_epoll_data->evtlist_cnt--;
            }
        }
    }
}

/* 添加描述符数组 */
static int _st_epoll_pollset_add(struct pollfd *pds, int npds) {
    struct epoll_event ev;
    int i, fd;
    int old_events, events, op;

    /* 添加前，对输入进行尽可能多的校验 */
    for (i = 0; i < npds; i++) {
        fd = pds[i].fd;
        if (fd < 0 || !pds[i].events ||
            (pds[i].events & ~(POLLIN | POLLOUT | POLLPRI))) {
            errno = EINVAL;
            return -1;
        }
        /* 如果超过描述符数量上限，进行扩容 */
        if (fd >= _st_epoll_data->fd_data_size && _st_epoll_fd_data_expand(fd) < 0)
            return -1;
    }

    for (i = 0; i < npds; i++) {
        /* 计算新的 events */
        fd = pds[i].fd;
        old_events = _ST_EPOLL_EVENTS(fd);
        /* 相关引用计数 */
        if (pds[i].events & POLLIN)
            _ST_EPOLL_READ_CNT(fd)++;
        if (pds[i].events & POLLOUT)
            _ST_EPOLL_WRITE_CNT(fd)++;
        if (pds[i].events & POLLPRI)
            _ST_EPOLL_EXCEP_CNT(fd)++;

        events = _ST_EPOLL_EVENTS(fd);
        if (events != old_events) {
            /* 需要更新事件 */
            op = old_events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
            ev.events = events;
            ev.data.fd = fd;
            if (epoll_ctl(_st_epoll_data->epfd, op, fd, &ev) < 0 && (op != EPOLL_CTL_ADD || errno != EEXIST))
                break;
            if (op == EPOLL_CTL_ADD) {
                /* 如果是添加操作，还要更新相关的计数，并且必要的话进行扩容 */
                _st_epoll_data->evtlist_cnt++;
                if (_st_epoll_data->evtlist_cnt > _st_epoll_data->evtlist_size)
                    _st_epoll_evtlist_expand();
            }
        }
    }

    if (i < npds) {
        /* 没有全部处理成功 */
        int err = errno;
        /* 回滚之前的操作 */
        _st_epoll_pollset_del(pds, i + 1);
        errno = err;
        return -1;
    }

    return 0;
}

/* dispatch 只在没有可执行线程时执行，当其返回以后，应该会有线程重新处于可运行状态 */
static void _st_epoll_dispatch(void)
{
    st_utime_t min_timeout;
    _st_clist_t *q;
    _st_pollq_t *pq;
    struct pollfd *pds, *epds;
    struct epoll_event ev;
    int timeout, nfd, i, osfd, notify;
    int events, op;
    short revents;

    /* 根据休眠队列计算等待时间 */
    if (_ST_SLEEPQ == NULL) {
        timeout = -1;
    } else {
        // 设置超时时间，如果已经有超时的 sleep routine，设置为 0, 否则设置为下一个带 wake 的 routine 时间
        min_timeout = (_ST_SLEEPQ->due <= _ST_LAST_CLOCK) ? 0 : (_ST_SLEEPQ->due - _ST_LAST_CLOCK);
        timeout = (int) (min_timeout / 1000);
    }

    if (_st_epoll_data->pid != getpid()) {
        /* 可能调用了 fork，重新初始化一下 */
        close(_st_epoll_data->epfd);
        _st_epoll_data->epfd = epoll_create(_st_epoll_data->fd_hint);
        if (_st_epoll_data->epfd < 0) {
            return;
        }
        fcntl(_st_epoll_data->epfd, F_SETFD, FD_CLOEXEC);
        _st_epoll_data->pid = getpid();

        /* 把 IOQ 中的所有文件描述符添加到事件系统中 */
        memset(_st_epoll_data->fd_data, 0, _st_epoll_data->fd_data_size * sizeof(_epoll_fd_data_t));
        _st_epoll_data->evtlist_cnt = 0;
        for (q = _ST_IOQ.next; q != &_ST_IOQ; q = q->next) {
            pq = _ST_POLLQUEUE_PTR(q);
            _st_epoll_pollset_add(pq->pds, pq->npds);
        }
    }

    /* 等待 IO 事件发生 */
    nfd = epoll_wait(_st_epoll_data->epfd, _st_epoll_data->evtlist, _st_epoll_data->evtlist_size, timeout);

    if (nfd > 0) {
        /* 如果触发了 IO 事件 */
        for (i = 0; i < nfd; i++) {
            osfd = _st_epoll_data->evtlist[i].data.fd;
            _ST_EPOLL_REVENTS(osfd) = _st_epoll_data->evtlist[i].events;
            if (_ST_EPOLL_REVENTS(osfd) & (EPOLLERR | EPOLLHUP)) {
                /* 发生了错误 */
                _ST_EPOLL_REVENTS(osfd) |= _ST_EPOLL_EVENTS(osfd);
            }
        }

        /* 遍历 IOQ */
        for (q = _ST_IOQ.next; q != &_ST_IOQ; q = q->next) {
            pq = _ST_POLLQUEUE_PTR(q);
            notify = 0;
            epds = pq->pds + pq->npds;

            for (pds = pq->pds; pds < epds; pds++) {
                if (_ST_EPOLL_REVENTS(pds->fd) == 0) {
                    pds->revents = 0;
                    continue;
                }
                /* 计算触发事件的 events */
                osfd = pds->fd;
                events = pds->events;
                revents = 0;
                if ((events & POLLIN) && (_ST_EPOLL_REVENTS(osfd) & EPOLLIN))
                    revents |= POLLIN;
                if ((events & POLLOUT) && (_ST_EPOLL_REVENTS(osfd) & EPOLLOUT))
                    revents |= POLLOUT;
                if ((events & POLLPRI) && (_ST_EPOLL_REVENTS(osfd) & EPOLLPRI))
                    revents |= POLLPRI;
                if (_ST_EPOLL_REVENTS(osfd) & EPOLLERR)
                    revents |= POLLERR;
                if (_ST_EPOLL_REVENTS(osfd) & EPOLLHUP)
                    revents |= POLLHUP;

                pds->revents = revents;
                if (revents) {
                    notify = 1;
                }
            }
            if (notify) {
                /* 有 IO 事件发生, 从 IOQ 移除 */
                ST_REMOVE_LINK(&pq->links);
                pq->on_ioq = 0;
                /* 这个调用只会删除没有触发 IO 事件的描述符 */
                _st_epoll_pollset_del(pq->pds, pq->npds);

                /* 如果线程在休眠队列，将其唤醒 */
                if (pq->thread->flags & _ST_FL_ON_SLEEPQ)
                    _ST_DEL_SLEEPQ(pq->thread);
                pq->thread->state = _ST_ST_RUNNABLE;
                _ST_ADD_RUNQ(pq->thread);
            }
        }

        for (i = 0; i < nfd; i++) {
            /* 
             * 删除/修改 描述符，因为这个关注是一次性的，比如某个线程调用 read 一次，那么 read 触发
             * 之后我们是要将其从事件系统中删除的
             */
            osfd = _st_epoll_data->evtlist[i].data.fd;
            _ST_EPOLL_REVENTS(osfd) = 0;
            events = _ST_EPOLL_EVENTS(osfd);
            op = events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            ev.events = events;
            ev.data.fd = osfd;
            if (epoll_ctl(_st_epoll_data->epfd, op, osfd, &ev) == 0 && op == EPOLL_CTL_DEL) {
                _st_epoll_data->evtlist_cnt--;
            }
        }
    }
}

/* 类似于告知文件系统，我要添加对 osfd 的处理，请保证空间足够 */
static int _st_epoll_fd_new(int osfd)
{
    if (osfd >= _st_epoll_data->fd_data_size && _st_epoll_fd_data_expand(osfd) < 0)
        return -1;

    return 0;   
}

/* 关闭文件描述符 */
static int _st_epoll_fd_close(int osfd)
{
    /* 如果仍有引用，则返回错误 */
    if (_ST_EPOLL_READ_CNT(osfd) || _ST_EPOLL_WRITE_CNT(osfd) || _ST_EPOLL_EXCEP_CNT(osfd)) {
        errno = EBUSY;
        return -1;
    }

    return 0;
}

static int _st_epoll_fd_getlimit(void)
{
    /* 0 代表没有限制 */
    return 0;
}

static _st_eventsys_t _st_epoll_eventsys = {
    "epoll",
    _st_epoll_init,
    _st_epoll_dispatch,
    _st_epoll_pollset_add,
    _st_epoll_pollset_del,
    _st_epoll_fd_new,
    _st_epoll_fd_close,
    _st_epoll_fd_getlimit
};

/* 在 state-thread 中可以通过条件编译指定不同的 backend，我们直接指定为 epoll */
_st_eventsys_t *_st_eventsys = &_st_epoll_eventsys;