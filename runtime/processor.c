#include "processor.h"

#include <ucontext.h>

#include "runtime.h"

int cpu_count;
bool processor_need_exit;

processor_t *share_processor_index[1024] = {0};
processor_t *share_processor_list; // 共享协程列表的数量一般就等于线程数量
processor_t *solo_processor_list;  // 独享协程列表其实就是多线程
mutex_t solo_processor_locker;     // 删除 solo processor 需要先获取该锁

int solo_processor_count; // 累计数量
rt_linked_t global_gc_worklist;
mutex_t global_gc_locker; // 全局 gc locker

uv_key_t tls_processor_key = 0;
uv_key_t tls_coroutine_key = 0;

fixalloc_t coroutine_alloc;
fixalloc_t processor_alloc;
mutex_t cp_alloc_locker;

// 这里直接引用了 main 符号进行调整，ct 不需要在寻找 main 对应到函数位置了
extern int main();

__attribute__((optimize(0))) void debug_ret(uint64_t rbp, uint64_t ret_addr) {
    // 这里也不安全了呀，毕竟没有回去。。所以为什么会超时,为啥还需要抢占。这 co 都换新了吧
    // DEBUGF("[runtime.debug_ret] rbp=%p, ret_addr=%p", (void *)rbp, (void *)ret_addr);
}

/**
 * 在 user_code 期间的超时抢占
 */
__attribute__((optimize(0))) void co_preempt_yield() {
    RDEBUGF("[runtime.co_preempt_yield] start, %lu", (uint64_t)uv_thread_self());

    processor_t *p = processor_get();
    assert(p);
    coroutine_t *co = p->coroutine;
    assert(co);

    DEBUGF("[runtime.co_preempt_yield] p_index_%d=%d(%d), co=%p, p_status=%d, scan_ret_addr=%p, scan_offset=%lu, will yield", p->share,
           p->index, p->status, co, co->status, (void *)co->scan_ret_addr, co->scan_offset);

    p->status = P_STATUS_PREEMPT;

    mutex_lock(&p->co_locker);
    co->status = CO_STATUS_RUNNABLE;
    rt_linked_push(&p->runnable_list, co);
    mutex_unlock(&p->co_locker);

    DEBUGF("[runtime.co_preempt_yield.thread_locker] co=%p push and update status success", co);
    _co_yield(p, co);

    assert(p->status == P_STATUS_RUNNABLE);

    // 接下来将直接 return 到用户态，不经过 post_tpl_hook, 所以直接更新为允许抢占
    // yield 切换回了用户态，此时允许抢占，所以不能再使用 RDEBUG, 而是 DEBUG
    DEBUGF("[runtime.co_preempt_yield] yield resume end, will set running, p_index_%d=%d, p_status=%d co=%p, p->co=%p", p->share, p->index,
           p->status, co, p->coroutine);

    co_set_status(p, co, CO_STATUS_RUNNING);
    processor_set_status(p, P_STATUS_RUNNING);
}

/**
 * 不能在信号处理函数中调用不可重入函数，包括 malloc/free/printf 等
 * @param sig
 * @param info
 * @param ucontext
 */
__attribute__((optimize(0))) static void thread_handle_sig(int sig, siginfo_t *info, void *ucontext) {
    ucontext_t *ctx = ucontext;
    processor_t *p = processor_get();
    assert(p);
    coroutine_t *co = p->coroutine;
    assert(co);

#ifdef __x86_64__
    // int REG_RBP = 10;
    int REG_RSP = 15;
    int REG_RIP = 16;
    uint64_t *rsp = (uint64_t *)ctx->uc_mcontext.gregs[REG_RSP];
    // return addr, 这个是 async_preempt 返回后应该去的地方
    int64_t rip = ctx->uc_mcontext.gregs[REG_RIP];

    // rip 已经存到了 rsp 里面,只要能定位到 bp_offset 就行了。
    fndef_t *fn = find_fn(rip);
    if (fn) {
        // 基于当前 rsp scan
        uint64_t sp_addr = (uint64_t)rsp;
        co->scan_ret_addr = rip;
        co->scan_offset = (uint64_t)co->p->share_stack.align_retptr - sp_addr;
    } else {
        // c 语言段被抢占，采取保守的扫描策略(使用 ret_addr = 0 来识别)
        co->scan_ret_addr = 0;
        co->scan_offset = (uint64_t)co->p->share_stack.align_retptr - (uint64_t)rsp;
    }

    // 由于被抢占的函数可以会在没有 sub 保留 rsp 的情况下使用 rsp-0x10 这样的空间地址
    // 所以需要为 rsp 预留足够的栈空间给被抢占的函数, 避免后续的操作污染被抢占的函数
    rsp -= 128; // 一个指针是 8byte, 所以这里是 128 * 8 = 1024 个字节

    // push rip
    rsp--; // 栈中预留返回地址
    *rsp = rip;

    RDEBUGF("[runtime.thread_handle_sig] rip=%p save to %p, co=%p, scan_ret_addr=%p, scan_offset=%lu, fn=%p", (void *)rip, rsp, co,
            (void *)co->scan_ret_addr, co->scan_offset, fn);

    ctx->uc_mcontext.gregs[REG_RSP] = (int64_t)rsp;
    ctx->uc_mcontext.gregs[REG_RIP] = (int64_t)async_preempt;
#elif
#endif
}

static void processor_uv_close(processor_t *p) {
    // 关闭 uv_loop
    RDEBUGF("[runtime.processor_uv_close] will close loop=%p, loop_req_count=%u, p_index_%d=%d", &p->uv_loop, p->uv_loop.active_reqs.count,
            p->share, p->index);
    uv_close((uv_handle_t *)&p->timer, NULL); // io_run 等待 close 完成！

    uv_run(&p->uv_loop, UV_RUN_DEFAULT); // 等待上面注册的 uv_close 完成

    int result = uv_loop_close(&p->uv_loop);

    if (result != 0) {
        RDEBUGF("[runtime.processor_uv_close] uv loop close failed, code=%d, msg=%s, p_index_%d=%d", result, uv_strerror(result), p->share,
                p->index);
        assert(false && "uv loop close failed");
    }

    RDEBUGF("[runtime.processor_uv_close] processor uv close success p_index_%d=%d", p->share, p->index);
}

static void coroutine_wrapper() {
    coroutine_t *co = aco_get_arg();
    assert(co);
    processor_t *p = co->p;
    assert(p);

    DEBUGF("[runtime.coroutine_wrapper] p_index_%d=%d(%d) co=%p, main=%d, gc_work=%d", p->share, p->index, p->status, co, co->main,
           co->gc_work);

    co_set_status(p, co, CO_STATUS_RUNNING);
    processor_set_status(p, P_STATUS_RUNNING);

    // 调用并处理请求参数 TODO 改成内联汇编实现，需要 #ifdef 判定不通架构
    ((void_fn_t)co->fn)();

    // 更新到 syscall 就不可抢占
    processor_set_status(p, P_STATUS_TPLCALL);

    if (co->main) {
        // 通知所有协程退出
        processor_set_exit();
        DEBUGF("[runtime.coroutine_wrapper] co=%p, main coroutine exit, set processor_need_exit=true", co);
    }

    co_set_status(p, co, CO_STATUS_DEAD);
    DEBUGF("[runtime.coroutine_wrapper] co=%p will dead", co);
    aco_exit1(&co->aco);
}

/**
 * 必须在用户态/或者 processor 还没有开始 run 之前初始化
 * @param p
 * @param co
 */
static void coroutine_aco_init(processor_t *p, coroutine_t *co) {
    assert(p);
    assert(co);
    if (co->aco.inited) {
        return;
    }

    assert(p->main_aco.inited);
    assert(p->share_stack.sz > 0);

    aco_create_init(&co->aco, &p->main_aco, &p->share_stack, 0, coroutine_wrapper, co);
}

void processor_all_stop_the_world() {
    uint64_t stw_time = uv_hrtime();
    PROCESSOR_FOR(share_processor_list) {
        p->need_stw = stw_time;
    }

    mutex_lock(&solo_processor_locker);
    PROCESSOR_FOR(solo_processor_list) {
        p->need_stw = stw_time;
    }
    mutex_unlock(&solo_processor_locker);
}

void processor_all_start_the_world() {
    PROCESSOR_FOR(share_processor_list) {
        p->need_stw = 0;
        p->safe_point = 0;
        RDEBUGF("[runtime_gc.processor_all_start_the_world] p_index_%d=%d, thread_id=%lu set safe_point=false", p->share, p->index,
                (uint64_t)p->thread_id);
    }

    mutex_lock(&solo_processor_locker);
    PROCESSOR_FOR(solo_processor_list) {
        p->need_stw = 0;
        p->safe_point = 0;
        mutex_unlock(&p->gc_stw_locker);

        TRACEF("[runtime_gc.processor_all_start_the_world] p_index_%d=%d, thread_id=%lu set safe_point=false and unlock gc_stw_locker",
               p->share, p->index, (uint64_t)p->thread_id);
    }
    mutex_unlock(&solo_processor_locker);

    DEBUGF("[runtime_gc.processor_all_start_the_world] all processor stw completed");
}

void uv_stop_callback(uv_timer_t *timer) {
    processor_t *p = timer->data;
    // DEBUGF("[runtime.io_run.uv_stop_callback] loop=%p, p_index=%d", timer->loop, p->index);

    uv_timer_stop(timer);
    // uv_close((uv_handle_t *)timer, NULL);

    uv_stop(timer->loop);
}

/**
 * timeout_ms 推荐 5ms ~ 10ms
 */
int io_run(processor_t *p, uint64_t timeout_ms) {
    // 初始化计时器 (这里不会发生栈切换，所以可以在栈上直接分配)
    p->timer.data = p;

    // 设置计时器超时回调，这将在超时后停止事件循环
    uv_timer_start(&p->timer, uv_stop_callback, timeout_ms, 0); // 只触发一次

    // DEBUGF("[runtime.io_run] uv_run start, p_index=%d, loop=%p", p->index, p->uv_loop);
    return uv_run(&p->uv_loop, UV_RUN_DEFAULT);
}

/**
 * 检测 coroutine 当前是否需要单独线程调度，如果不需要单独线程则直接在当前线程进行 aco_resume
 */
void coroutine_resume(processor_t *p, coroutine_t *co) {
    assert(co->status == CO_STATUS_RUNNABLE && "coroutine status must be runnable");

    // 首次 resume 需要进行初始化
    if (!co->aco.inited) {
        coroutine_aco_init(p, co);
    }

    // 将 RIP 指针移动用户代码片段中
    RDEBUGF("[coroutine_resume] aco_resume will start,co=%p,main=%d,aco=%p, sstack=%p(%zu), p_index_%d=%d(%d)", co, co->main, &co->aco,
            co->aco.save_stack.ptr, co->aco.save_stack.sz, p->share, p->index, p->status);

    // 获取锁才能切换协程并更新状态
    mutex_lock(&p->thread_locker);
    p->coroutine = co;
    // - 再 tls 中记录正在运行的协程
    uv_key_set(&tls_coroutine_key, co);
    co->p = p; // 运行前进行绑定，让 coroutine 在运行中可以准确的找到 processor
    p->status = P_STATUS_RUNNABLE;
    mutex_unlock(&p->thread_locker);

    aco_resume(&co->aco);

    // rtcall/tplcall 都可以无锁进入到 dispatch 状态，dispatch 状态是一个可以安全 stw 的状态
    TRACEF("[coroutine_resume] resume backend, p_index_%d=%d(%d), co=%p, status=%d, gc_work=%d, scan_ret_addr=%p, scan_offset=%lu",
           p->share, p->index, p->status, co, co->status, co->gc_work, (void *)co->scan_ret_addr, co->scan_offset);

    // running -> dispatch
    assert(co->status != CO_STATUS_RUNNING);
    p->co_started_at = 0;
    processor_set_status(p, P_STATUS_DISPATCH);

    uint64_t time = (uv_hrtime() - p->co_started_at) / 1000 / 1000;
    RDEBUGF("[coroutine_resume] resume backend, co=%p, aco=%p, run_time=%lu ms, gc_work=%d", co, &co->aco, time, co->gc_work);
}

// handle by thread
static void processor_run(void *raw) {
    processor_t *p = raw;
    RDEBUGF("[runtime.processor_run] start, p_index=%d, addr=%p, share=%d, loop=%p", p->index, p, p->share, &p->uv_loop);

    processor_set_status(p, P_STATUS_DISPATCH);

    // 初始化 aco 和 main_co
    aco_thread_init(NULL);
    aco_create_init(&p->main_aco, NULL, NULL, 0, NULL, NULL);
    aco_share_stack_init(&p->share_stack, 0);

    // - 初始化 libuv
    uv_loop_init(&p->uv_loop);
    uv_timer_init(&p->uv_loop, &p->timer);

    // 注册线程信号监听, 用于抢占式调度
    // 分配备用信号栈
    //    stack_t *ss = NEW(stack_t);
    //    ss->ss_sp = mallocz(SIGSTKSZ);
    //    ss->ss_size = SIGSTKSZ;
    //    ss->ss_flags = 0;
    //    sigaltstack(ss, NULL); // 配置为信号处理函数使用栈

    p->sig.sa_flags = SA_SIGINFO | SA_RESTART;
    p->sig.sa_sigaction = thread_handle_sig;

    if (sigaction(SIGURG, &p->sig, NULL) == -1) {
        assert(false && "sigaction failed");
    }

    // 将 p 存储在线程维度全局遍历中，方便直接在 coroutine 运行中读取相关的 processor
    uv_key_set(&tls_processor_key, p);

    // 对 p 进行调度处理(p 上面可能还没有 coroutine)
    while (true) {
        TRACEF("[runtime.processor_run] handle, p_index_%d=%d", p->share, p->index);
        // - stw
        if (p->need_stw > 0) {
        STW_WAIT:
            RDEBUGF("[runtime.processor_run] need stw, set safe_point=need_stw(%lu), p_index_%d=%d", p->need_stw, p->share, p->index);
            p->safe_point = p->need_stw;

            // runtime_gc 线程会解除 safe 状态，所以这里一直等待即可
            while (processor_safe(p)) {
                TRACEF("[runtime.processor_run] p_index_%d=%d, need_stw=%lu, safe_point=%lu stw loop....", p->share, p->index, p->need_stw,
                       p->safe_point);
                usleep(WAIT_BRIEF_TIME * 1000); // 1ms
            }

            RDEBUGF("[runtime.processor_run] p_index_%d=%d, stw completed, need_stw=%lu, safe_point=%lu", p->share, p->index, p->need_stw,
                    p->safe_point);
        }

        // - exit
        if (processor_get_exit()) {
            RDEBUGF("[runtime.processor_run] p_index_%d=%d, need stop, goto exit", p->share, p->index);
            goto EXIT;
        }

        // - 处理 coroutine (找到 io 可用的 goroutine)
        while (true) {
            mutex_lock(&p->co_locker);
            coroutine_t *co = rt_linked_pop(&p->runnable_list);
            mutex_unlock(&p->co_locker);
            if (!co) {
                // runnable list 已经处理完成
                TRACEF("[runtime.processor_run] runnable is empty, p_index_%d=%d", p->share, p->index);
                break;
            }

            RDEBUGF("[runtime.processor_run] will handle coroutine, p_index_%d=%d, co=%p, status=%d", p->share, p->index, co, co->status);

            coroutine_resume(p, co);

            if (p->need_stw > 0) {
                RDEBUGF("[runtime.processor_run] coroutine resume and p need stw, will goto stw, p_index_%d=%d, co=%p, status=%d", p->share,
                        p->index, co, co->status);
                goto STW_WAIT;
            }

            RDEBUGF("[runtime.processor_run] coroutine resume backend, p_index_%d=%d, co=%p, status=%d", p->share, p->index, co,
                    co->status);
        }

        // solo processor exit check
        if (!p->share) {
            assert(p->co_list.count == 1);
            coroutine_t *solo_co = rt_linked_first(&p->co_list)->value;
            assertf(solo_co, "solo_co is null, p_index_%d=%d, co_list linked_node=%p", p->share, p->index, rt_linked_first(&p->co_list));

            if (solo_co->status == CO_STATUS_DEAD) {
                RDEBUGF("[runtime.processor_run] solo processor co exit, will exit processor run, p_index=%d, co=%p, status=%d", p->index,
                        solo_co, solo_co->status);
                goto EXIT;
            }
        }

        // - 处理 io 就绪事件(也就是 run 指定时间的 libuv)
        io_run(p, WAIT_SHORT_TIME);
    }

EXIT:
    processor_uv_close(p);
    p->thread_id = 0;
    processor_set_status(p, P_STATUS_EXIT);

    RDEBUGF("[runtime.processor_run] exited, p_index_%d=%d", p->share, p->index);
}

void coroutine_dispatch(coroutine_t *co) {
    DEBUGF("[runtime.coroutine_dispatch] co=%p, solo=%d, share_processor_count=%d", co, co->solo, cpu_count);

    // 分配 coroutine 之前需要给 coroutine 确认初始颜色, 如果是新增的 coroutine，默认都是黑色
    if (gc_stage == GC_STAGE_MARK) {
        co->gc_black = memory->gc_count;
    }

    // - 协程独享线程
    if (co->solo) {
        processor_t *p = processor_new(solo_processor_count++);
        mutex_lock(&p->co_locker);
        rt_linked_push(&p->co_list, co);
        rt_linked_push(&p->runnable_list, co);
        mutex_unlock(&p->co_locker);

        mutex_lock(&solo_processor_locker);
        RT_LIST_PUSH_HEAD(solo_processor_list, p);
        mutex_unlock(&solo_processor_locker);

        if (uv_thread_create(&p->thread_id, processor_run, p) != 0) {
            assert(false && "pthread_create failed");
        }

        DEBUGF("[runtime.coroutine_dispatch] solo processor create, thread_id=%ld", (uint64_t)p->thread_id);
        return;
    }

    // goroutine 默认状态是 runnable
    assert(co->status == CO_STATUS_RUNNABLE);

    // - 遍历 shared_processor_list 找到 co_list->count 最小的 processor 进行调度
    processor_t *select_p = NULL;

    PROCESSOR_FOR(share_processor_list) {
        if (!select_p || p->co_list.count < select_p->co_list.count) {
            select_p = p;
        }
    }

    assert(select_p);
    DEBUGF("[runtime.coroutine_dispatch] select_p_index_%d=%d will push co=%p", select_p->share, select_p->index, co);

    mutex_lock(&select_p->co_locker);
    rt_linked_push(&select_p->co_list, co);
    rt_linked_push(&select_p->runnable_list, co);
    mutex_unlock(&select_p->co_locker);

    DEBUGF("[runtime.coroutine_dispatch] co=%p to p_index_%d=%d, end", co, select_p->share, select_p->index);
}

/**
 * 各种全局变量初始化都通过该方法
 */
void processor_init() {
    // - 初始化 aco 需要的 tls 变量(不能再线程中 create)
    aco_init();

    // - tls 全局变量初始化
    uv_key_create(&tls_processor_key);
    uv_key_create(&tls_coroutine_key);

    // - 读取当前 cpu 线程数初始化相应数量的 p
    uv_cpu_info_t *info;
    uv_cpu_info(&info, &cpu_count);
    uv_free_cpu_info(info, cpu_count);

    // - 初始化全局标识
    processor_need_exit = false;
    gc_barrier = false;
    mutex_init(&gc_stage_locker, false);
    mutex_init(&solo_processor_locker, false);
    gc_stage = GC_STAGE_OFF;
    solo_processor_count = 0;

    rt_linked_init(&global_gc_worklist, NULL, NULL);
    mutex_init(&global_gc_locker, false);

    // - 初始化 processor 和 coroutine 分配器
    fixalloc_init(&coroutine_alloc, sizeof(coroutine_t));
    fixalloc_init(&processor_alloc, sizeof(processor_t));
    mutex_init(&cp_alloc_locker, false);

    DEBUGF("[runtime.processor_init] cpu_count=%d", cpu_count);

    // - 为每一个 processor 创建对应的 thread 进行处理对应的 p
    // 初始化 share_processor_index 数组为 0
    share_processor_list = NULL;
    solo_processor_list = NULL;

    for (int i = 0; i < cpu_count; ++i) {
        processor_t *p = processor_new(i);
        // 仅 share processor 需要 gc worklist
        p->share = true;
        rt_linked_init(&p->gc_worklist, NULL, NULL);
        p->gc_work_finished = memory->gc_count;
        share_processor_index[p->index] = p;

        RT_LIST_PUSH_HEAD(share_processor_list, p);

        // 创建一个新的线程用来处理
        if (uv_thread_create(&p->thread_id, processor_run, p) != 0) {
            assert(false && "pthread_create failed %s");
        }

        DEBUGF("[runtime.processor_init] processor create, index=%d, thread_id=%ld", i, (uint64_t)p->thread_id);
    }
}

processor_t *processor_get() {
    processor_t *p = uv_key_get(&tls_processor_key);
    return p;
}

coroutine_t *coroutine_get() {
    return uv_key_get(&tls_coroutine_key);
}

void rt_coroutine_set_error(char *msg) {
    DEBUGF("[runtime.rt_coroutine_set_error] msg=%s", msg);
    coroutine_t *co = coroutine_get();
    n_errort *error = n_error_new(string_new(msg, strlen(msg)), 1);
    co->error = error;
}

void coroutine_dump_error(n_errort *error) {
    DEBUGF("[runtime.coroutine_dump_error] errort base=%p", error);
    n_string_t *msg = error->msg;
    DEBUGF("[runtime.coroutine_dump_error] memory_string len: %lu, base: %p", msg->length, msg->data);
    assert(error->traces->length > 0);

    n_trace_t first_trace = {};
    vec_access(error->traces, 0, &first_trace);
    char *dump_msg = dsprintf("catch error: '%s' at %s:%d:%d\n", (char *)error->msg->data, (char *)first_trace.path->data, first_trace.line,
                              first_trace.column);

    VOID write(STDOUT_FILENO, dump_msg, strlen(dump_msg));

    if (error->traces->length > 1) {
        char *temp = "stack backtrace:\n";
        VOID write(STDOUT_FILENO, temp, strlen(temp));
        for (int i = 0; i < error->traces->length; ++i) {
            n_trace_t trace = {};
            vec_access(error->traces, i, &trace);
            temp = dsprintf("%d:\t%s\n\t\tat %s:%d:%d\n", i, (char *)trace.ident->data, (char *)trace.path->data, trace.line, trace.column);
            VOID write(STDOUT_FILENO, temp, strlen(temp));
        }
    }
}

void mark_ptr_black(void *value) {
    addr_t addr = (addr_t)value;
    // get mspan by ptr
    mspan_t *span = span_of(addr);
    assert(span);
    mutex_lock(&span->gcmark_locker);
    // get span index
    uint64_t obj_index = (addr - span->base) / span->obj_size;
    bitmap_set(span->gcmark_bits, obj_index);
    DEBUGF("[runtime.mark_ptr_black] addr=%p, span=%p, spc=%d, span_base=%p, obj_index=%lu marked", value, span, span->spanclass,
           (void *)span->base, obj_index);
    mutex_unlock(&span->gcmark_locker);
}

/**
 * TODO target use debug, can delete
 * yield 的入口也是这里
 * @param target
 */
__attribute__((optimize(0))) void pre_tplcall_hook(char *target) {
    coroutine_t *co = coroutine_get();
    processor_t *p = processor_get();

    // 这里需要抢占到锁再进行更新，否则和 wait_sysmon 存在冲突。
    // 如果 wait_sysmon 已经获取了锁，则会阻塞在此处等待 wait_symon 进行抢占, 避免再次进入 tpl
    processor_set_status(p, P_STATUS_TPLCALL);

    uint64_t rbp_value;
#ifdef __x86_64__
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp_value));
#elif
    assert(false && "not support");
#endif

    co->scan_ret_addr = fetch_addr_value(rbp_value + POINTER_SIZE);
    co->scan_offset = (uint64_t)p->share_stack.align_retptr - (rbp_value + POINTER_SIZE + POINTER_SIZE);

    TRACEF("[pre_tplcall_hook] co=%p, status=%d ,target=%s, scan_offset=%lu, ret_addr=%p", co, co->status, target, co->scan_offset,
           (void *)co->scan_ret_addr);

    // #ifdef DEBUG
    //     addr_t ret_addr = fetch_addr_value(rbp_value + POINTER_SIZE);
    //     fndef_t *fn = find_fn(ret_addr);
    //     if (fn) {
    //         TRACEF("[runtime.pre_tpl_hook] ret_addr=%p, fn=%s -> %s, path=%s:%lu", (void *)ret_addr, fn->name, target, fn->rel_path,
    //         fn->line);
    //     }
    //     // 基于 share stack 计算 offset
    //     TRACEF("[runtime.pre_tpl_hook] aco->align_retptr=%p, rbp=%p, bp_offset=%lu", aco->share_stack->align_retptr, (void *)rbp_value,
    //            aco->bp_offset);
    // #endif
}

__attribute__((optimize(0))) void post_tplcall_hook(char *target) {
    processor_t *p = processor_get();
    TRACEF("[runtime.post_tplcall_hook] p_index_%d=%d will set processor_status", p->share, p->index);
    processor_set_status(p, P_STATUS_RUNNING);
}

__attribute__((optimize(0))) void post_rtcall_hook(char *target) {
    processor_t *p = processor_get();
    DEBUGF("[runtime.post_rtcall_hook] target=%s, p_index_%d=%d will set processor_status", target, p->share, p->index);
    processor_set_status(p, P_STATUS_RUNNING);
}

coroutine_t *coroutine_new(void *fn, n_vec_t *args, bool solo, bool main) {
    mutex_lock(&cp_alloc_locker);
    coroutine_t *co = fixalloc_alloc(&coroutine_alloc);
    mutex_unlock(&cp_alloc_locker);

    co->fn = fn;
    co->solo = solo;
    co->gc_black = 0;
    co->status = CO_STATUS_RUNNABLE;
    co->args = args;
    co->p = NULL;
    co->result = NULL;
    co->main = main;
    co->next = NULL;
    co->aco.inited = 0; // 标记为为初始化
    co->scan_ret_addr = 0;
    co->scan_offset = 0;

    DEBUGF("[coroutine_new] co=%p new success", co);
    return co;
}

// 如果被抢占会导致全局队列卡死，所以 linked 和 processor 绑定好了, 这就关系到 fixalloc_t 的释放问题
// 除非在这期间不进行抢占，
processor_t *processor_new(int index) {
    mutex_lock(&cp_alloc_locker);
    processor_t *p = fixalloc_alloc(&processor_alloc);
    mutex_unlock(&cp_alloc_locker);

    // uv_loop_init(&p->uv_loop);
    mutex_init(&p->gc_stw_locker, false);
    p->need_stw = 0;
    p->safe_point = 0;

    mutex_init(&p->thread_locker, false);
    mutex_init(&p->co_locker, false);
    p->status = P_STATUS_INIT;
    p->sig.sa_flags = 0;
    p->thread_id = 0;
    p->coroutine = NULL;
    p->co_started_at = 0;
    p->mcache.flush_gen = 0; // 线程维度缓存，避免内存分配锁
    rt_linked_init(&p->co_list, NULL, NULL);
    rt_linked_init(&p->runnable_list, NULL, NULL);
    p->index = index;
    p->next = NULL;

    return p;
}

/**
 * 当 main processor 退出时，会收到该值
 * @return
 */
bool processor_get_exit() {
    return processor_need_exit;
}

void processor_set_exit() {
    processor_need_exit = true;
}

/**
 * processor_free 不受 stw 影响，所以获取一下 memory locker 避免 uncache 冲突
 * @param p
 */
void processor_free(processor_t *p) {
    DEBUGF("[wait_sysmon.processor_free] start p=%p, p_index_%d=%d, loop=%p, share_stack=%p|%p", p, p->share, p->index, &p->uv_loop,
           &p->share_stack, p->share_stack.ptr);

    aco_share_stack_destroy(&p->share_stack);
    rt_linked_destroy(&p->co_list);
    aco_destroy(&p->main_aco);

    // 归还 mcache span
    for (int j = 0; j < SPANCLASS_COUNT; ++j) {
        mspan_t *span = p->mcache.alloc[j];
        if (!span) {
            continue;
        }

        p->mcache.alloc[j] = NULL; // uncache
        mcentral_t *mcentral = &memory->mheap->centrals[span->spanclass];
        uncache_span(mcentral, span);
        DEBUGF("[wait_sysmon.processor_free] uncache span=%p, span_base=%p, spc=%d, alloc_count=%lu", span, (void *)span->base,
               span->spanclass, span->alloc_count);
    }

    RDEBUGF("[wait_sysmon.processor_free] will free uv_loop p_index_%d=%d, loop=%p", p->share, p->index, &p->uv_loop);
    int share = p->share;
    int index = p->index;

    RDEBUGF("[wait_sysmon.processor_free] will free processor p_index_%d=%d", share, index);
    mutex_lock(&cp_alloc_locker);
    fixalloc_free(&processor_alloc, p);
    mutex_unlock(&cp_alloc_locker);
    DEBUGF("[wait_sysmon.processor_free] succ free p_index_%d=%d", share, index);
}

/**
 * 是否所有的 processor 都到达了安全点
 * @return
 */
bool processor_all_safe() {
    PROCESSOR_FOR(share_processor_list) {
        if (p->status == P_STATUS_EXIT) {
            continue;
        }

        if (processor_safe(p)) {
            continue;
        }

        RDEBUGF("[runtime_gc.processor_all_safe] share processor p_index_%d=%d, thread_id=%lu not safe, need_stw=%lu, safe_point=%lu",
                p->share, p->index, (uint64_t)p->thread_id, p->need_stw, p->safe_point);
        return false;
    }

    // safe_point 或者获取到 gc_stw_locker
    mutex_lock(&solo_processor_locker);
    PROCESSOR_FOR(solo_processor_list) {
        if (p->status == P_STATUS_EXIT) {
            continue;
        }

        if (processor_safe(p)) {
            continue;
        }

        // 获取了锁不代表进入了安全状态,此时可能还在 user code 中, 必须要以 safe_point 为准
        // if (mutex_trylock(&p->gc_stw_locker) != 0) {
        //     continue;
        // }
        RDEBUGF("[runtime_gc.processor_all_safe] solo processor p_index_%d=%d, thread_id=%lu not safe", p->share, p->index,
                (uint64_t)p->thread_id);
        mutex_unlock(&solo_processor_locker);
        return false;
    }
    mutex_unlock(&solo_processor_locker);

    return true;
}

void processor_all_wait_safe() {
    RDEBUGF("[processor_all_wait_safe] start");
    while (!processor_all_safe()) {
        usleep(WAIT_BRIEF_TIME * 1000);
    }

    RDEBUGF("[processor_all_wait_safe] end");
}

void processor_stw_unlock() {
    mutex_lock(&solo_processor_locker);
    PROCESSOR_FOR(solo_processor_list) {
        mutex_unlock(&p->gc_stw_locker);
        RDEBUGF("[runtime.processor_all_safe_or_lock] solo processor p_index_%d=%d gc_stw_locker unlock", p->share, p->index);
    }
    mutex_unlock(&solo_processor_locker);
}

/**
 * 遍历所有的 share processor 和 solo processor 判断 gc 是否全部完成
 * @return
 */
static bool all_gc_work_finished() {
    PROCESSOR_FOR(share_processor_list) {
        if (p->gc_work_finished < memory->gc_count) {
            return false;
        }
    }

    return true;
}

/**
 * 需要等待独享和共享协程的 gc_work 全部完成 mark,
 */
void wait_all_gc_work_finished() {
    RDEBUGF("[runtime_gc.wait_all_gc_work_finished] start");

    while (all_gc_work_finished() == false) {
        usleep(WAIT_SHORT_TIME * 1000);
    }

    DEBUGF("[runtime_gc.wait_all_gc_work_finished] all processor gc work finish");
}

void *global_gc_worklist_pop() {
    mutex_lock(&global_gc_locker);
    void *value = rt_linked_pop(&global_gc_worklist);
    mutex_unlock(&global_gc_locker);
    return value;
}

/**
 * 1. 只要没有进行新的 resume, 那及时 yield 了，当前 aco 信息就还是存储在 share stack 中
 * 2. 可以从 rsp 寄存器中读取栈顶
 * @param aco
 * @param stack
 */
void co_migrate(aco_t *aco, aco_share_stack_t *new_st) {
    // 起始迁移点(最小值)
    void *sp = aco->reg[ACO_REG_IDX_SP];
    aco_share_stack_t *old_st = aco->share_stack;

    // 需要迁移的 size
    addr_t size = (addr_t)old_st->align_retptr - (addr_t)sp;

    // 原则上应该从 old_share_stack->align_retptr(最大值)
    memmove(new_st->align_retptr - size, sp, size);

    // 更新栈里面 prev bp 值
    addr_t bp_ptr = (addr_t)aco->reg[ACO_REG_IDX_BP];

    while (true) {
        addr_t prev_bp_value = fetch_addr_value((addr_t)bp_ptr); // 可能压根没有报错 bp 的值，所以必须有一个中断条件

        // 边界情况处理
        if (prev_bp_value <= bp_ptr) {
            break;
        }

        if (prev_bp_value < (addr_t)sp) {
            break;
        }

        if (prev_bp_value > (addr_t)old_st->align_retptr) {
            break;
        }

        addr_t prev_bp_offset = (addr_t)old_st->align_retptr - (addr_t)bp_ptr;

        addr_t new_prev_bp_value = (addr_t)new_st->align_retptr - ((addr_t)old_st->align_retptr - prev_bp_value);

        // 更新相关位置的值
        memmove((void *)((addr_t)new_st->align_retptr - prev_bp_offset), &new_prev_bp_value, POINTER_SIZE);

        // 更新 rbp_ptr 的指向
        bp_ptr = prev_bp_value;
    }

    // 更新 bp/sp 寄存器
    aco->reg[ACO_REG_IDX_BP] = (void *)((addr_t)new_st->align_retptr - ((addr_t)old_st->align_retptr - (addr_t)aco->reg[ACO_REG_IDX_BP]));
    aco->reg[ACO_REG_IDX_SP] = (void *)((addr_t)new_st->align_retptr - ((addr_t)old_st->align_retptr - (addr_t)aco->reg[ACO_REG_IDX_SP]));

    // 更新 co share_stack 指向
    aco->share_stack = new_st;
    new_st->owner = aco;
}

void processor_set_status(processor_t *p, p_status_t status) {
    assert(p);
    assert(p->status != status);

    // rtcall 是不稳定状态，可以随时切换到任意状态
    if (p->status == P_STATUS_RTCALL) {
        p->status = status;
        return;
    }

    // 对于 share processor 来说， tplcall 和 rtcall 是不稳定的，需要切换走的，所以不需要被 thread_locker 锁住
    if (p->share && p->status == P_STATUS_TPLCALL) {
        p->status = status;
        return;
    }

    // solo 状态下 tplcall 可以切换到 dispatch, 但是不能进入到 running
    if (!p->share && p->status == P_STATUS_TPLCALL && status == P_STATUS_DISPATCH) {
        p->status = status;
        return;
    }

    // 必须先获取 thread_locker 再获取 gc_stw_locker
    mutex_lock(&p->thread_locker);

    // 特殊处理 2， solo 切换成 running 时需要获取 gc_stw_locker, 如果 gc_stw_locker 阻塞说明当前正在 stw
    // 必须获取到 stw locker 才能切换到 running 状态(runnable -> running/ tpl_call -> running)
    if (!p->share && status == P_STATUS_RUNNING) {
        mutex_lock(&p->gc_stw_locker);
    }

    p->status = status;

    if (!p->share && status == P_STATUS_RUNNING) {
        mutex_unlock(&p->gc_stw_locker);
    }

    mutex_unlock(&p->thread_locker);

    if (status == P_STATUS_RUNNING) {
        p->co_started_at = uv_hrtime();
    }
}
