#ifndef NATURE_PROCESSOR_H
#define NATURE_PROCESSOR_H

#include <stdint.h>
#include <uv.h>

#include "nutils/errort.h"
#include "nutils/vec.h"
#include "runtime.h"

extern int cpu_count;
extern slice_t *share_processor_list; // 共享协程列表的数量一般就等于线程数量
extern linked_t *solo_processor_list; // 独享协程列表其实就是多线程
extern int solo_processor_count;      // 累计数量
extern uv_key_t tls_processor_key;
extern uv_key_t tls_coroutine_key;

// processor gc_finished 后新产生的 shade ptr 会存入到该全局工作队列中，在 gc_mark_done 阶段进行单线程处理
extern linked_t *global_gc_worklist; // 全局 gc worklist
extern mutex_t *global_gc_locker;    // 全局 gc locker

extern bool processor_need_stw;  // 全局 STW 标识
extern bool processor_need_exit; // 全局 STW 标识

extern void async_preempt() __asm__("async_preempt");

void co_preempt_yield();

/**
 * 一旦 thread_locker 被 sysmon 持有，则 can_preempt 的值无法修改
 * 因为采取了先修改 can_preempt 再离开 sysmon 的策略，所以此时无法切换协程
 * @param p
 * @param v
 */
static inline void set_can_preempt(processor_t *p, bool v) {
//    DEBUGF("[runtime.set_can_preempt.thread_locker] start, p_index_%d=%d, will set can_preempt=%d", p->share, p->index,
//           v);
    mutex_lock(p->thread_locker);
    p->can_preempt = v;
    mutex_unlock(p->thread_locker);
//    DEBUGF("[runtime.set_can_preempt.thread_locker] end, p_index_%d=%d, set can_preempt=%d success", p->share,
//           p->index, v);
}

/**
 * yield 统一入口, 避免直接调用 aco_yield
 */
static inline void _co_yield(processor_t *p, coroutine_t *co) {
    assert(p);
    assert(co);

    set_can_preempt(p, false); // 即将切换到 runtime，不再允许抢占

    aco_yield1(co->aco);

    if (!co->gc_work) {
        set_can_preempt(p, true); // 回到用户态，允许抢占
    }
}

static inline void co_yield_runnable(processor_t *p, coroutine_t *co) {
    DEBUGF("[runtime.co_yield_runnable] start")
    assert(p);
    assert(co);

    co->status = CO_STATUS_RUNNABLE;
    linked_push(p->runnable_list, co);
    DEBUGF("[runtime.co_yield_runnable] p_index_%d=%d, co=%p, co_status=%d, will yield", p->share, p->index, co,
           co->status);
    _co_yield(p, co);
    DEBUGF("[runtime.co_yield_runnable] p_index_%d=%d, co=%p, co_status=%d, yield resume", p->share, p->index, co,
           co->status);
}

static inline void co_yield_waiting(processor_t *p, coroutine_t *co) {
    assert(p);
    assert(co);

    co->status = CO_STATUS_WAITING;
    _co_yield(p, co);
}

// locker
void *global_gc_worklist_pop();

// 调度器每一次处理都会先调用 need_stw 判断是否存在 STW 需求
bool processor_get_stw();

void processor_stop_the_world();

void processor_start_the_world();

bool processor_all_safe();

void processor_wait_all_safe();

void wait_all_gc_work_finished();

/**
 *  processor 停止调度
 */
bool processor_get_exit();

void processor_set_exit();

/**
 * 阻塞特定时间的网络 io 时间, 如果有 io 事件就绪则立即返回
 * uv_run 有三种模式
 * UV_RUN_DEFAULT: 持续阻塞不返回, 可以使用 uv_stop 安全退出
 * UV_RUN_ONCE: 处理至少活跃的 fd 后返回, 如果没有活跃的 fd 则一直阻塞
 * UV_RUN_NOWAIT: 不阻塞, 如果没有事件就绪则立即返回
 * @param timeout_ms
 * @return
 */
int io_run(processor_t *p, uint64_t timeout_ms);

/**
 * runtime_main 会负责调用该方法，该方法读取 cpu 的核心数，然后初始化对应数量的 share_processor
 * 每个 share_processor 可以通过 void* arg 直接找到自己的 share_processor_t 对象
 */
void processor_init();

processor_t *processor_new(int index);

void processor_free(processor_t *);

/**
 * @param fn
 * @param args 元素的类型是 n_union_t 联合类型
 * @param solo
 * @return
 */
coroutine_t *coroutine_new(void *fn, n_vec_t *args, bool solo, bool main);

/**
 * 为 coroutine 选择合适的 processor 绑定，如果是独享 coroutine 则创建一个 solo processor
 */
void coroutine_dispatch(coroutine_t *co);

/**
 * 有 processor_run 调用
 * coroutine 的本质是一个指针，指向了需要执行的代码的 IP 地址。 (aco_create 会绑定对应的 fn)
 */
void coroutine_resume(processor_t *p, coroutine_t *co);

/**
 * 正常需要根据线程 id 返回，第一版返回 id 就行了
 * 第一版总是返回 processor_main
 * @return
 */
processor_t *processor_get();

coroutine_t *coroutine_get();

void pre_tpl_hook(char *target);

#endif // NATURE_PROCESSOR_H
