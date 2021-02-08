/*
 * IO 系统实现，主要提供一些类似系统调用的 IO 操作，所有的线程都应该只使用 state-thread
 * 提供的阻塞操作，而不能使用底层的阻塞系统调用
 * 对于非阻塞 IO 的实现，一个惯有套路就是先直接调用底层系统调用，如果返回类似 EAGAIN 的错误
 * 那么注册到事件系统中，等待通知
*/

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "common.h"

/* 辅助宏，代表文件描述符上的 IO 事件未就绪 */
#if EAGAIN != EWOULDBLOCK
    #define _IO_NOT_READY_ERROR  ((errno == EAGAIN) || (errno == EWOULDBLOCK))
#else
    #define _IO_NOT_READY_ERROR  (errno == EAGAIN)
#endif

#define _LOCAL_MAXIOV  16

/* 文件描述也用一个双向链表管理 */
static _st_netfd_t *_st_netfd_freelist = NULL;
/* 系统文件描述符上限 */
static int _st_osfd_limit = -1;

int _st_io_init() {
    struct sigaction sigact;
    struct rlimit rlim;
    int fdlim;

    /* 忽略 SIGPIPE，这种错误在网络编程中是很常见的 */
    sigact.sa_handler = SIG_IGN;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    if (sigaction(SIGPIPE, &sigact, NULL) < 0)
        return -1;

    /* 获取系统最大打开文件数限制 */
    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
        return -1;

    /* 根据事件系统修改最大打开文件数限制 */
    fdlim = (*_st_eventsys->fd_getlimit)();
    if (fdlim > 0 && rlim.rlim_max > (rlim_t) fdlim) {
        rlim.rlim_max = fdlim;
    }
    
    rlim.rlim_cur = rlim.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rlim) < 0)
        return -1;
    _st_osfd_limit = (int) rlim.rlim_max;

    return 0;
}

int st_getfdlimit(void) {
    return _st_osfd_limit;
}

/* 销毁 fd */
void st_netfd_free(_st_netfd_t *fd) {
    if (!fd->inuse)
        return;

    fd->inuse = 0;
    if (fd->private_data && fd->destructor)
        (*(fd->destructor))(fd->private_data);
    fd->private_data = NULL;
    fd->destructor = NULL;
    fd->next = _st_netfd_freelist;
    _st_netfd_freelist = fd;
}

/* 创建 netfd，注意此时的底层系统文件描述符是打开状态，netfd 是系统描述符上的一层 wrapper*/
static _st_netfd_t *_st_netfd_new(int osfd, int nonblock, int is_socket) {
    _st_netfd_t *fd;
    int flags = 1;

    /* 调用事件系统，添加 osfd */
    if ((*_st_eventsys->fd_new)(osfd) < 0)
        return NULL;

    /* 复用/构造 netfd 结构 */
    if (_st_netfd_freelist) {
        fd = _st_netfd_freelist;
        _st_netfd_freelist = _st_netfd_freelist->next;
    } else {
        fd = calloc(1, sizeof(_st_netfd_t));
        if (!fd)
            return NULL;
    }

    /* 初始化字段 */
    fd->osfd = osfd;
    fd->inuse = 1;
    fd->next = NULL;
    
    if (nonblock) {
        /* 设置非阻塞，注意，IO 复用模型一定要配合非阻塞 IO！没有任何道理使用阻塞 IO */
        if (is_socket && ioctl(osfd, FIONBIO, &flags) != -1)
            return fd;
        if ((flags = fcntl(osfd, F_GETFL, 0)) < 0 ||
            fcntl(osfd, F_SETFL, flags | O_NONBLOCK) < 0) {
            st_netfd_free(fd);
            return NULL;
        }
    }

    return fd;
}

_st_netfd_t *st_netfd_open(int osfd) {
    return _st_netfd_new(osfd, 1, 0);
}


_st_netfd_t *st_netfd_open_socket(int osfd) {
    return _st_netfd_new(osfd, 1, 1);
}

/* 关闭文件描述符 */
int st_netfd_close(_st_netfd_t *fd) {
    /* 从事件系统删除 */
    if ((*_st_eventsys->fd_close)(fd->osfd) < 0)
        return -1;
    
    st_netfd_free(fd);
    /* 关闭底层的系统文件描述符 */
    return close(fd->osfd);
}

/* 获取底层文件描述符 */
int st_netfd_fileno(_st_netfd_t *fd) {
    return (fd->osfd);
}

/* 设置文件描述的私有数据 */
void st_netfd_setspecific(_st_netfd_t *fd, void *value, _st_destructor_t destructor) {
  if (value != fd->private_data) {
    /* 之前有私有数据的话，进行析构 */
    if (fd->private_data && fd->destructor)
      (*(fd->destructor))(fd->private_data);
  }
  fd->private_data = value;
  fd->destructor = destructor;
}

/* 获取文件描述符的私有数据 */
void *st_netfd_getspecific(_st_netfd_t *fd) {
    return (fd->private_data);
}

/*
 * 等待单个文件描述符上的 IO 事件就绪
 */
int st_netfd_poll(_st_netfd_t *fd, int how, st_utime_t timeout) {
    struct pollfd pd;
    int n;
    
    pd.fd = fd->osfd;
    pd.events = (short) how;
    pd.revents = 0;
    
    if ((n = st_poll(&pd, 1, timeout)) < 0)
        return -1;
    if (n == 0) {
        /* 超时 */
        errno = ETIME;
        return -1;
    }
    if (pd.revents & POLLNVAL) {
        /* 出错了，传入的文件描述符无效 */
        errno = EBADF;
        return -1;
    }
    
    return 0;
}

/* 
 * accept，注意 state-thread 中提到了有些 os 要求不同进程对同一个文件描述符是不可以并发的调用
 * accept 的，所以其使用 pipe 作为一个进程间的同步工具，保证任意时刻只有一个进程可以 accpet
 * 我们这里省去了这部分的代码
 */
_st_netfd_t *st_accept(_st_netfd_t *fd, struct sockaddr *addr, int *addrlen, st_utime_t timeout) {
    int osfd, err;
    _st_netfd_t *newfd;
    
    /* 先直接调用底层 accept 函数 */
    while ((osfd = accept(fd->osfd, addr, (socklen_t *)addrlen)) < 0) {
        if (errno == EINTR)
            /* 被系统信号中断，重试即可 */
            continue;
        if (!_IO_NOT_READY_ERROR)
            /* 返回了非阻塞以外的错误，返回空指针 */
            return NULL;
        /* 代表读事件为就绪，等待其就绪 */
        if (st_netfd_poll(fd, POLLIN, timeout) < 0)
            return NULL;
    }
    
    /* 构建新的文件描述符对象 */
    newfd = _st_netfd_new(osfd, 1, 1);
    if (!newfd) {
        /* 要暂时保存一下错误，防止 close 设置了 errno */
        err = errno;
        close(osfd);
        errno = err;
    }
    
    return newfd;
}

/* connect，非阻塞的 connect 其实比较难实现 看后面的具体注释 */
int st_connect(_st_netfd_t *fd, const struct sockaddr *addr, int addrlen, st_utime_t timeout) {
    int n, err = 0;
    
    /* 直接调用 connect 系统调用 */
    while (connect(fd->osfd, addr, addrlen) < 0) {
        if (errno != EINTR) {
            /*
             * 在一些 os 上，如果 connect() 是在内核 bind 之后被信号中断的(errno == EINTR)
             * 那么随后的一次 connect() 调用会返回 EADDRINUSE 的错误.所以如果这不是第一次调用
             * connect()，而且上次失败的原因是 EINTR，就忽略 EADDRINUSE 的错误
             */
            if (errno != EINPROGRESS && (errno != EADDRINUSE || err == 0))
                return -1;
            /* 等待描述符可写，这个应该在建立连接后会立马触发(但是不代表一定成功 connect()) */
            if (st_netfd_poll(fd, POLLOUT, timeout) < 0)
                return -1;
            /* 查看 socket opt，确定是否成功连接 */
            n = sizeof(int);
            if (getsockopt(fd->osfd, SOL_SOCKET, SO_ERROR, (char *)&err, (socklen_t *)&n) < 0)
                return -1;
            if (err) {
                errno = err;
                return -1;
            }
            break;
        }
        err = 1;
    }
    return 0;
}

/* 后面的都是一些读写套接字的函数，思路大同小异，这里不再赘述 */
ssize_t st_read(_st_netfd_t *fd, void *buf, size_t nbyte, st_utime_t timeout) {
    ssize_t n;
    
    while ((n = read(fd->osfd, buf, nbyte)) < 0) {
        if (errno == EINTR)
            continue;
        if (!_IO_NOT_READY_ERROR)
            return -1;
        /* Wait until the socket becomes readable */
        if (st_netfd_poll(fd, POLLIN, timeout) < 0)
            return -1;
    }
    
    return n;
}


int st_read_resid(_st_netfd_t *fd, void *buf, size_t *resid, st_utime_t timeout)
{
    struct iovec iov, *riov;
    int riov_size, rv;
    
    iov.iov_base = buf;
    iov.iov_len = *resid;
    riov = &iov;
    riov_size = 1;
    rv = st_readv_resid(fd, &riov, &riov_size, timeout);
    *resid = iov.iov_len;
    return rv;
}


ssize_t st_readv(_st_netfd_t *fd, const struct iovec *iov, int iov_size, st_utime_t timeout)
{
    ssize_t n;
    
    while ((n = readv(fd->osfd, iov, iov_size)) < 0) {
        if (errno == EINTR)
            continue;
        if (!_IO_NOT_READY_ERROR)
            return -1;
        /* Wait until the socket becomes readable */
        if (st_netfd_poll(fd, POLLIN, timeout) < 0)
            return -1;
    }
    
    return n;
}

int st_readv_resid(_st_netfd_t *fd, struct iovec **iov, int *iov_size, st_utime_t timeout)
{
    ssize_t n;
    
    while (*iov_size > 0) {
        if (*iov_size == 1)
            n = read(fd->osfd, (*iov)->iov_base, (*iov)->iov_len);
        else
            n = readv(fd->osfd, *iov, *iov_size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!_IO_NOT_READY_ERROR)
                return -1;
        } else if (n == 0)
            break;
        else {
            while ((size_t) n >= (*iov)->iov_len) {
                n -= (*iov)->iov_len;
                (*iov)->iov_base = (char *) (*iov)->iov_base + (*iov)->iov_len;
                (*iov)->iov_len = 0;
                (*iov)++;
                (*iov_size)--;
                if (n == 0)
                    break;
            }
            if (*iov_size == 0)
                break;
            (*iov)->iov_base = (char *) (*iov)->iov_base + n;
            (*iov)->iov_len -= n;
        }
        /* Wait until the socket becomes readable */
        if (st_netfd_poll(fd, POLLIN, timeout) < 0)
            return -1;
    }
    
    return 0;
}


ssize_t st_read_fully(_st_netfd_t *fd, void *buf, size_t nbyte, st_utime_t timeout)
{
    size_t resid = nbyte;
    return st_read_resid(fd, buf, &resid, timeout) == 0 ?
    (ssize_t) (nbyte - resid) : -1;
}


int st_write_resid(_st_netfd_t *fd, const void *buf, size_t *resid, st_utime_t timeout)
{
    struct iovec iov, *riov;
    int riov_size, rv;
    
    iov.iov_base = (void *) buf;        /* we promise not to modify buf */
    iov.iov_len = *resid;
    riov = &iov;
    riov_size = 1;
    rv = st_writev_resid(fd, &riov, &riov_size, timeout);
    *resid = iov.iov_len;
    return rv;
}


ssize_t st_write(_st_netfd_t *fd, const void *buf, size_t nbyte, st_utime_t timeout)
{
    size_t resid = nbyte;
    return st_write_resid(fd, buf, &resid, timeout) == 0 ?
    (ssize_t) (nbyte - resid) : -1;
}


ssize_t st_writev(_st_netfd_t *fd, const struct iovec *iov, int iov_size, st_utime_t timeout)
{
    ssize_t n, rv;
    size_t nleft, nbyte;
    int index, iov_cnt;
    struct iovec *tmp_iov;
    struct iovec local_iov[_LOCAL_MAXIOV];
    
    /* Calculate the total number of bytes to be sent */
    nbyte = 0;
    for (index = 0; index < iov_size; index++)
        nbyte += iov[index].iov_len;
    
    rv = (ssize_t)nbyte;
    nleft = nbyte;
    tmp_iov = (struct iovec *) iov;    /* we promise not to modify iov */
    iov_cnt = iov_size;
    
    while (nleft > 0) {
        if (iov_cnt == 1) {
            if (st_write(fd, tmp_iov[0].iov_base, nleft, timeout) != (ssize_t) nleft)
                rv = -1;
            break;
        }
        if ((n = writev(fd->osfd, tmp_iov, iov_cnt)) < 0) {
            if (errno == EINTR)
                continue;
            if (!_IO_NOT_READY_ERROR) {
                rv = -1;
                break;
            }
        } else {
            if ((size_t) n == nleft)
                break;
            nleft -= n;
            /* Find the next unwritten vector */
            n = (ssize_t)(nbyte - nleft);
            for (index = 0; (size_t) n >= iov[index].iov_len; index++)
                n -= iov[index].iov_len;
            
            if (tmp_iov == iov) {
                /* Must copy iov's around */
                if (iov_size - index <= _LOCAL_MAXIOV) {
                    tmp_iov = local_iov;
                } else {
                    tmp_iov = calloc(1, (iov_size - index) * sizeof(struct iovec));
                    if (tmp_iov == NULL)
                        return -1;
                }
            }
            
            /* Fill in the first partial read */
            tmp_iov[0].iov_base = &(((char *)iov[index].iov_base)[n]);
            tmp_iov[0].iov_len = iov[index].iov_len - n;
            index++;
            /* Copy the remaining vectors */
            for (iov_cnt = 1; index < iov_size; iov_cnt++, index++) {
                tmp_iov[iov_cnt].iov_base = iov[index].iov_base;
                tmp_iov[iov_cnt].iov_len = iov[index].iov_len;
            }
        }
        /* Wait until the socket becomes writable */
        if (st_netfd_poll(fd, POLLOUT, timeout) < 0) {
            rv = -1;
            break;
        }
    }
    
    if (tmp_iov != iov && tmp_iov != local_iov)
        free(tmp_iov);
    
    return rv;
}


int st_writev_resid(_st_netfd_t *fd, struct iovec **iov, int *iov_size, st_utime_t timeout)
{
    ssize_t n;
    
    while (*iov_size > 0) {
        if (*iov_size == 1)
            n = write(fd->osfd, (*iov)->iov_base, (*iov)->iov_len);
        else
            n = writev(fd->osfd, *iov, *iov_size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!_IO_NOT_READY_ERROR)
                return -1;
        } else {
            while ((size_t) n >= (*iov)->iov_len) {
                n -= (*iov)->iov_len;
                (*iov)->iov_base = (char *) (*iov)->iov_base + (*iov)->iov_len;
                (*iov)->iov_len = 0;
                (*iov)++;
                (*iov_size)--;
                if (n == 0)
                    break;
            }
            if (*iov_size == 0)
                break;
            (*iov)->iov_base = (char *) (*iov)->iov_base + n;
            (*iov)->iov_len -= n;
        }
        /* Wait until the socket becomes writable */
        if (st_netfd_poll(fd, POLLOUT, timeout) < 0)
            return -1;
    }
    
    return 0;
}


/*
 * Simple I/O functions for UDP.
 */
int st_recvfrom(_st_netfd_t *fd, void *buf, int len, struct sockaddr *from, int *fromlen, st_utime_t timeout)
{
    int n;
    
    while ((n = recvfrom(fd->osfd, buf, len, 0, from, (socklen_t *)fromlen)) < 0) {
        if (errno == EINTR)
            continue;
        if (!_IO_NOT_READY_ERROR)
            return -1;
        /* Wait until the socket becomes readable */
        if (st_netfd_poll(fd, POLLIN, timeout) < 0)
            return -1;
    }
    
    return n;
}


int st_sendto(_st_netfd_t *fd, const void *msg, int len, const struct sockaddr *to, int tolen, st_utime_t timeout)
{
    int n;
    
    while ((n = sendto(fd->osfd, msg, len, 0, to, tolen)) < 0) {
        if (errno == EINTR)
            continue;
        if (!_IO_NOT_READY_ERROR)
            return -1;
        /* Wait until the socket becomes writable */
        if (st_netfd_poll(fd, POLLOUT, timeout) < 0)
            return -1;
    }
    
    return n;
}


int st_recvmsg(_st_netfd_t *fd, struct msghdr *msg, int flags, st_utime_t timeout)
{
    int n;
    
    while ((n = recvmsg(fd->osfd, msg, flags)) < 0) {
        if (errno == EINTR)
            continue;
        if (!_IO_NOT_READY_ERROR)
            return -1;
        /* Wait until the socket becomes readable */
        if (st_netfd_poll(fd, POLLIN, timeout) < 0)
            return -1;
    }
    
    return n;
}


int st_sendmsg(_st_netfd_t *fd, const struct msghdr *msg, int flags, st_utime_t timeout)
{
    int n;
    
    while ((n = sendmsg(fd->osfd, msg, flags)) < 0) {
        if (errno == EINTR)
            continue;
        if (!_IO_NOT_READY_ERROR)
            return -1;
        /* Wait until the socket becomes writable */
        if (st_netfd_poll(fd, POLLOUT, timeout) < 0)
            return -1;
    }
    
    return n;
}

int st_sendmmsg(st_netfd_t fd, struct st_mmsghdr *msgvec, unsigned int vlen, int flags, st_utime_t timeout)
{
#if defined(MD_HAVE_SENDMMSG) && defined(_GNU_SOURCE)
    int n;
    int left;
    struct mmsghdr *p;

    left = (int)vlen;
    while (left > 0) {
        p = (struct mmsghdr*)msgvec + (vlen - left);

        if ((n = sendmmsg(fd->osfd, p, left, flags)) < 0) {
            if (errno == EINTR)
                continue;
            if (!_IO_NOT_READY_ERROR)
                break;
            /* Wait until the socket becomes writable */
            if (st_netfd_poll(fd, POLLOUT, timeout) < 0)
                break;
        }

        left -= n;
    }

    // An error is returned only if no datagrams could be sent.
    if (left == (int)vlen) {
        return n;
    }
    return (int)vlen - left;
#else
    struct st_mmsghdr *p;
    int i, n;

    // @see http://man7.org/linux/man-pages/man2/sendmmsg.2.html
    for (i = 0; i < (int)vlen; ++i) {
        p = msgvec + i;
        n = st_sendmsg(fd, &p->msg_hdr, flags, timeout);
        if (n < 0) {
            // An error is returned only if no datagrams could be sent.
            if (i == 0) {
                return n;
            }
            return i + 1;
        }

        p->msg_len = n;
    }

    // Returns the number of messages sent from msgvec; if this is less than vlen, the caller can retry with a
    // further sendmmsg() call to send the remaining messages.
    return vlen;
#endif
}


/*
 * To open FIFOs or other special files.
 */
_st_netfd_t *st_open(const char *path, int oflags, mode_t mode)
{
    int osfd, err;
    _st_netfd_t *newfd;
    
    while ((osfd = open(path, oflags | O_NONBLOCK, mode)) < 0) {
        if (errno != EINTR)
            return NULL;
    }
    
    newfd = _st_netfd_new(osfd, 0, 0);
    if (!newfd) {
        err = errno;
        close(osfd);
        errno = err;
    }
    
    return newfd;
}

