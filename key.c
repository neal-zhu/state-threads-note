/*
 * state-thread thread local storage 实现
 * 比较奇怪的是，state-thread 中不同线程的线程独立数据，绑定的是一个全局的析构
 * 函数数组。而且 key 也是针对全局保存的，而不是线程之间各自独立
*/

#include "common.h"

/* 线程私有数据的析构函数 */
static _st_destructor_t _st_destructors[ST_KEYS_MAX];
static int key_max = 0;

/* 返回一个可用于存取线程私有数据的 key */
int st_key_create(int *keyp, _st_destructor_t destructor) {
    /* 超过上限 */
    if (key_max >= ST_KEYS_MAX) {
        errno = EAGAIN;
        return -1;
    }

    *keyp = key_max++;
    _st_destructors[*keyp] = destructor;

    return 0;
}

int st_key_getlimit() {
    return ST_KEYS_MAX;
}

/* 设置线程局部数据，key 必须是 st_key_create 返回的 */
int st_thread_setspecific(int key, void *value){ 
    _st_thread_t *me = _ST_CURRENT_THREAD();

    if (key < 0 || key >= ST_KEYS_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (value != me->private_data[key]) {
        /* 如果之前有设置数据，那么先析构 */
        if (me->private_data[key] && _st_destructors[key]) {
            (*_st_destructors[key])(me->private_data[key]);
        }
        me->private_data[key] = value;
    }

    return 0;
}

/* 获取线程局部数据，key 必须是 st_key_create 返回的 */
void *st_thread_getspecific(int key) {
    if (key < 0 || key >= key_max) {
        return NULL;
    }

    return ((_ST_CURRENT_THREAD())->private_data[key]);
}

/* 清理线程私有数据 */
void _st_thread_cleanup(_st_thread_t *thread) {
    int key;

    for (key = 0; key < key_max; key++) {
        if (thread->private_data[key] && _st_destructors[key]) {
            (*_st_destructors[key])(thread->private_data[key]);
            thread->private_data[key] = NULL;
        }
    }
}