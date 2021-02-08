/**
 * 栈实现代码。state-thread 的栈用一个全局的链表管理，当线程数较多（能低成本的创建很多线程，本身
 * 就是协程库存在的最主要意义之一) 且线程指定栈大小不一示，很可能造成内存浪费 + 性能问题（线性查找一个
 * 全局 list)。可能采取的优化措施：
 * 1. 采用多个 slots 分别管理某个大小的 stack 集合，可能更好，实现也并不复杂
*/

#include <sys/mman.h>
#include <stdlib.h>

#include "common.h"

/* 在栈的可用空间的头尾两端会保留 REDZONE 大小的空闲内存 */
#define REDZONE _ST_PAGE_SIZE

/* 栈对象用一个全局的 free list 管理，对资源进行复用 */
/* 注意这个链表什么的是一个 _st_clist_t 而不是 _st_stack_t */
static _st_clist_t _st_free_stacks = ST_INIT_STATIC_CLIST(&_st_free_stacks);
static int _st_num_free_stacks = 0;
static int _st_randomize_stacks = 0;

static char *_st_new_stk_segment(int size);

/* 申请一个 stack_size 大小的栈(返回的栈可用空间可能超过 stack_size) */
_st_stack_t *_st_stack_new(int stack_size) {
    _st_clist_t *qp;
    _st_stack_t *ts;
    int extra;

    /* 遍历空闲链表，看看是否有大小满足要求的栈 */
    for (qp = _st_free_stacks.next; qp != &_st_free_stacks; qp = qp->next) {
        // 转型得到栈对象
        ts = _ST_THREAD_STACK_PTR(qp);
        if (ts->stk_size >= stack_size) {
            /* ok，找到了可以复用的栈，first fit 算法 */
            ST_REMOVE_LINK(&ts->links);
            _st_num_free_stacks--;
            ts->links.prev = ts->links.next = NULL;
            return ts;
        }
    }

    /* 没有满足大小的栈，创建一个 */
    if ((ts = (_st_stack_t*) calloc(1, sizeof(_st_stack_t))) == NULL) {
        return NULL;
    }
    extra = _st_randomize_stacks ? _ST_PAGE_SIZE : 0;
    // vaddr 包含: 栈，栈前后的 REDZONE，如果开启了随机栈地址，还需要额外申请一个 page
    ts->vaddr_size = stack_size + REDZONE*2 + extra;
    ts->vaddr = _st_new_stk_segment(ts->vaddr_size);
    if (!ts->vaddr) {
        free(ts);
        return NULL;
    }
    // 设置栈相关数据
    ts->stk_size = stack_size;
    ts->stk_bottom = ts->vaddr + REDZONE;
    ts->stk_top = ts->stk_bottom + stack_size;

    if (extra) {
        /* 栈地址随机，取个 0~0xf 的随机数，将栈上移 offset */
        long offset = (random() % extra) & ~0xf;
        ts->stk_bottom += offset;
        ts->stk_top += offset;
    }

    return ts;
}

/* 释放 ts */
void _st_stack_free(_st_stack_t *ts) {
    if (!ts) {
        return;
    }

    /* 放回全局的空闲链表中 */
    ST_APPEND_LINK(&ts->links, _st_free_stacks.prev);
    _st_num_free_stacks++;
}

/* 为栈申请内存 */
static char *_st_new_stk_segment(int size) {
    /* 使用 mmap 分配内存 */
    static int zero_fd = -1;
    int mmap_flags = MAP_PRIVATE | MAP_ANON;
    void *vaddr;

    vaddr = mmap(NULL, size, PROT_READ | PROT_WRITE, mmap_flags, zero_fd, 0);
    if (vaddr == (void*)MAP_FAILED) {
        return NULL;
    }

    // 将两块 REDZONE 设为不可访问防止栈溢出等问题
    mprotect(vaddr, REDZONE, PROT_NONE);
    mprotect((char*)vaddr + size - REDZONE, REDZONE, PROT_NONE);

    return (char*) vaddr;
}

/* 开启随机栈地址机制 */
int st_randomize_stacks(int on)
{
    int wason = _st_randomize_stacks;
    
    _st_randomize_stacks = on;
    if (on)
        srandom((unsigned int) st_utime());
    
    return wason;
}
