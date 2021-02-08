#pragma once

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <errno.h>
#include <poll.h>

/* 对外暴露的所有接口 */

/* 一直等待在阻塞操作上，直到满足条件或者被中断 */
#ifndef ST_UTIME_NO_TIMEOUT
    #define ST_UTIME_NO_TIMEOUT ((st_utime_t) -1LL)
#endif

/* 不阻塞，没有事件发生的话立刻返回 */
#ifndef ST_UTIME_NO_WAIT
    #define ST_UTIME_NO_WAIT 0
#endif

/* 与 c++ 兼容 */
#ifdef __cplusplus
extern "C" {
#endif

/* 一些 typedef，注意对外暴露的都是一些 struct pointer */
typedef unsigned long long  st_utime_t;
typedef struct _st_thread   *st_thread_t;
typedef struct _st_cond     *st_cond_t;
typedef struct _st_mutex    *st_mutex_t;
typedef struct _st_netfd    *st_netfd_t;

/* 初始化 state-threads 系统，使用前必须先调用本函数 */
extern int st_init();
/* 获取最大打开文件数限制 */
extern int st_getfdlimit();

/* 获取当前线程的句柄 */
extern st_thread_t st_thread_self();
/* 退出当前线程，并且将 retval 作为返回值 */
extern void st_thread_exit(void *retval);
/* 等待一个 joinable 的线程结束，并且将 retvalp 设置为其返回值 */
extern int st_thread_join(st_thread_t thread, void **retvalp);
/* 打断某个陷入阻塞的线程(注意这里的阻塞是指调用 state-threads 提供的阻塞接口) */
extern void st_thread_interrupt(st_thread_t thread);
/* 创建线程 */
extern st_thread_t st_thread_create(void *(*start)(void*), void *arg, int joinable, int stack_size);
/* 启动栈地址随机机制 */
extern int st_randomize_stacks(int on);
/* 设置时间获取函数 */
extern int st_set_utime_function(st_utime_t (*func)());

/* 时间相关函数族 */
extern st_utime_t st_utime();
extern st_utime_t st_utime_last_clock();
extern time_t st_time(void);
extern int st_usleep(st_utime_t usecs);
extern int st_sleep(int secs);
extern int st_timecache_set(int on);

/* 同步原语相关函数 */
extern st_cond_t st_cond_new(void);
extern int st_cond_destroy(st_cond_t cvar);
extern int st_cond_timedwait(st_cond_t cvar, st_utime_t timeout);
extern int st_cond_wait(st_cond_t cvar);
extern int st_cond_signal(st_cond_t cvar);
extern int st_cond_broadcast(st_cond_t cvar);
extern st_mutex_t st_mutex_new(void);
extern int st_mutex_destroy(st_mutex_t lock);
extern int st_mutex_lock(st_mutex_t lock);
extern int st_mutex_unlock(st_mutex_t lock);
extern int st_mutex_trylock(st_mutex_t lock);

/* tls(thread local storage) 相关函数 */
extern int st_key_create(int *keyp, void (*destructor)(void *));
extern int st_key_getlimit(void);
extern int st_thread_setspecific(int key, void *value);
extern void *st_thread_getspecific(int key);

/* IO 相关函数 */
extern st_netfd_t st_netfd_open(int osfd);
extern st_netfd_t st_netfd_open_socket(int osfd);
extern void st_netfd_free(st_netfd_t fd);
extern int st_netfd_close(st_netfd_t fd);
extern int st_netfd_fileno(st_netfd_t fd);
extern void st_netfd_setspecific(st_netfd_t fd, void *value, void (*destructor)(void *));
extern void *st_netfd_getspecific(st_netfd_t fd);
extern int st_netfd_serialize_accept(st_netfd_t fd);
extern int st_netfd_poll(st_netfd_t fd, int how, st_utime_t timeout);

/* 下面的 st-xx 函数，基本可以认为是等同系统调用 xx */
extern int st_poll(struct pollfd *pds, int npds, st_utime_t timeout);
extern st_netfd_t st_accept(st_netfd_t fd, struct sockaddr *addr, int *addrlen, st_utime_t timeout);
extern int st_connect(st_netfd_t fd, const struct sockaddr *addr, int addrlen, st_utime_t timeout);
extern ssize_t st_read(st_netfd_t fd, void *buf, size_t nbyte, st_utime_t timeout);
extern ssize_t st_read_fully(st_netfd_t fd, void *buf, size_t nbyte, st_utime_t timeout);
extern int st_read_resid(st_netfd_t fd, void *buf, size_t *resid, st_utime_t timeout);
extern ssize_t st_readv(st_netfd_t fd, const struct iovec *iov, int iov_size, st_utime_t timeout);
extern int st_readv_resid(st_netfd_t fd, struct iovec **iov, int *iov_size, st_utime_t timeout);
extern ssize_t st_write(st_netfd_t fd, const void *buf, size_t nbyte, st_utime_t timeout);
extern int st_write_resid(st_netfd_t fd, const void *buf, size_t *resid, st_utime_t timeout);
extern ssize_t st_writev(st_netfd_t fd, const struct iovec *iov, int iov_size, st_utime_t timeout);
extern int st_writev_resid(st_netfd_t fd, struct iovec **iov, int *iov_size, st_utime_t timeout);
extern int st_recvfrom(st_netfd_t fd, void *buf, int len, struct sockaddr *from, int *fromlen, st_utime_t timeout);
extern int st_sendto(st_netfd_t fd, const void *msg, int len, const struct sockaddr *to, int tolen, st_utime_t timeout);
extern int st_recvmsg(st_netfd_t fd, struct msghdr *msg, int flags, st_utime_t timeout);
extern int st_sendmsg(st_netfd_t fd, const struct msghdr *msg, int flags, st_utime_t timeout);

#include <sys/socket.h>
struct st_mmsghdr {
   struct msghdr msg_hdr;  /* Message header */
   unsigned int  msg_len;  /* Number of bytes transmitted */
};
extern int st_sendmmsg(st_netfd_t fd, struct st_mmsghdr *msgvec, unsigned int vlen, int flags, st_utime_t timeout);

extern st_netfd_t st_open(const char *path, int oflags, mode_t mode);

#ifdef __cplusplus
}
#endif