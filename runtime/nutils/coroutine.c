#include "coroutine.h"

#include "runtime/processor.h"

coroutine_t *coroutine_create(void *fn, n_vec_t *args, uint64_t flag) {
    bool solo = FLAG(CO_FLAG_SOLO) & flag;
    return coroutine_new(fn, args, solo, false);
}

coroutine_t *coroutine_run(void *fn, n_vec_t *args, uint64_t flag) {
    coroutine_t *co = coroutine_create(fn, args, flag);

    coroutine_dispatch(co);

    return co;
}

void coroutine_yield() {
    processor_t *p = processor_get();
    co_yield_runnable(p, p->coroutine);
}

/**
 * repeat 为 0， 所以不会重复执行，所以不需要手动钓孙 uv_stop_timer 停止计时器
 * @param timer
 */
static void uv_on_timer(uv_timer_t *timer) {
    DEBUGF("[runtime.coroutine_on_timer] start, timer=%p, timer->data=%p", timer, timer->data);
    coroutine_t *co = timer->data;

    // - 标记 coroutine 并推送到可调度队列中等待 processor handle
    processor_t *p = co->p;
    assert(p);

    DEBUGF("[runtime.uv_on_timer] will push to runnable_list, p_index=%d, c=%d", p->index, co->status);

    co->status = CO_STATUS_RUNNABLE;
    runnable_push(p, co);
}

void coroutine_sleep(int64_t ms) {
    processor_t *p = processor_get();
    coroutine_t *co = coroutine_get();

    // - 初始化 libuv 定时器
    uv_timer_t *timer = NEW(uv_timer_t);
    uv_timer_init(p->uv_loop, timer);
    timer->data = co;

    // 设定定时器超时时间与回调
    uv_timer_start(timer, uv_on_timer, ms, 0);

    DEBUGF("[runtime.coroutine_sleep] start, uv_loop=%p, p_index=%d, timer=%p, timer_value=%lu", p->uv_loop, p->index, &timer,
           fetch_addr_value((addr_t)&timer));

    // 退出等待 io 事件就绪
    co_yield_waiting(p, co);

    DEBUGF("[runtime.coroutine_sleep] coroutine sleep completed");
    free(timer);
}