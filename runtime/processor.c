#include "processor.h"

#include <uv.h>

#include "builtin.h"
#include "runtime.h"
#include "utils/assertf.h"

int cpu_count;
bool processor_need_stw;
bool processor_need_exit;
slice_t* share_processor_list; // 共享协程列表的数量一般就等于线程数量
linked_t* solo_processor_list; // 独享协程列表其实就是多线程
int solo_processor_count;      // 累计数量
linked_t* global_gc_worklist;  // 独享协程列表其实就是多线程
uv_mutex_t global_gc_locker;   // 全局 gc locker

uv_key_t tls_processor_key;
uv_key_t tls_coroutine_key;

static void coroutine_wrapper() {
    coroutine_t* co = aco_get_arg();
    assert(co);

    DEBUGF("[runtime.coroutine_wrapper] co=%p, main=%d", co, co->main);

    // 调用并处理请求参数 TODO 改成内联汇编实现，需要 #ifdef 判定不通架构
    ((void_fn_t)co->fn)();

    if (co->main) {
        // 通知所有协程退出
        processor_set_exit();
        DEBUGF("[runtime.coroutine_wrapper] co=%p, main coroutine exit, set processor_need_exit=true", co);
    }

    // 退出协程，设置协程退出标识
    co->status = CO_STATUS_DEAD;

    DEBUGF("[runtime.coroutine_wrapper] will aco_exit")
    aco_exit1(co->aco);
}

bool processor_get_stw() {
    return processor_need_stw;
}

void processor_stop_the_world() {
    processor_need_stw = true;
}

void processor_start_the_world() {
    processor_need_stw = false;
}

void uv_stop_callback(uv_timer_t* timer) {
    processor_t* p = timer->data;
    //    DEBUGF("[runtime.io_run.uv_stop_callback] loop=%p, p_index=%d", timer->loop, p->index);
    uv_stop(timer->loop);
}

/**
 * timeout_ms 推荐 5ms ~ 10ms
 */
int io_run(processor_t* p, uint64_t timeout_ms) {
    // 初始化计时器
    uv_timer_t timeout_timer;
    uv_timer_init(p->uv_loop, &timeout_timer);
    timeout_timer.data = p;

    // 设置计时器超时回调，这将在超时后停止事件循环
    uv_timer_start(&timeout_timer, uv_stop_callback, timeout_ms, 0); // 只触发一次

    //    DEBUGF("[runtime.io_run] uv_run start, p_index=%d, loop=%p", p->index, p->uv_loop);
    return uv_run(p->uv_loop, UV_RUN_DEFAULT);
}

/**
 * 检测 coroutine 当前是否需要单独线程调度，如果不需要单独线程则直接在当前线程进行 aco_resume
 */
void coroutine_resume(processor_t* p, coroutine_t* co) {
    assertf(co->status == CO_STATUS_RUNNABLE, "coroutine status must be runnable");

    if (co->aco == NULL) {
        DEBUGF("[runtime.coroutine_resume] co=%p first resume, p_index_%d=%d", co, p->share, p->index);
        assert(p->share_stack);
        co->aco = aco_create(p->main_aco, p->share_stack, 0, coroutine_wrapper, co);
    }

    // - 再 tls 中记录正在运行的协程
    uv_key_set(&tls_coroutine_key, co);

    co->status = CO_STATUS_RUNNING;
    p->coroutine = co;
    co->p = p; // 运行前进行绑定，让 coroutine 在运行中可以准确的找到 processor

    // 将 RIP 指针移动用户代码片段中
    DEBUGF("[runtime.coroutine_resume] aco_resume will start, co=%p, aco=%p, p->co_started_at=%lu", co, co->aco, p->co_started_at);

    p->co_started_at = uv_hrtime();
    aco_resume(co->aco);
    p->co_started_at = 0; // 调度完成后及时清空 coroutine，避免被 sysmon thread 抢占

    uint64_t time = (uv_hrtime() - p->co_started_at) / 1000 / 1000;
    DEBUGF("[runtime.coroutine_resume] aco_yield completed, co=%p, aco=%p, status=%d, time=%lu ms", co, co->aco, co->status, time);
}

// handle by thread
void processor_run(void* raw) {
    processor_t* p = raw;
    DEBUGF("[runtime.processor_run] start, p_index=%d, addr=%p, share=%d, loop=%p", p->index, p, p->share, p->uv_loop);

    // 初始化 aco 和 main_co
    aco_thread_init(NULL);
    p->main_aco = aco_create(NULL, NULL, 0, NULL, NULL);
    assert(p->main_aco);
    p->share_stack = aco_share_stack_new(0);

    // 注册线程信号监听, 用于抢占式调度
    p->sig.sa_handler = thread_handle_sigurg;
    if (sigaction(SIGURG, &p->sig, NULL) == -1) {
        assertf(false, "sigaction failed");
    }

    // 将 p 存储在线程维度全局遍历中，方便直接在 coroutine 运行中读取相关的 processor
    uv_key_set(&tls_processor_key, p);

    // 对 p 进行调度处理(p 上面可能还没有 coroutine)
    while (true) {
        //        DEBUGF("[runtime.processor_run] handle, p_index=%d", p->index);
        // - stw
        if (processor_get_stw()) {
            DEBUGF("[runtime.processor_run] need stw, set safe_point=true,  p_index=%d, share=%d", p->index, p->share);
            p->safe_point = true;
            p->no_preempt = true; // 禁止抢占
            while (processor_get_stw()) {
                usleep(WAIT_MID_TIME); // 每 50ms 检测一次 STW 是否解除
            }

            p->safe_point = false;
            p->no_preempt = false;
            DEBUGF("[runtime.processor_run] p_index=%d, stw completed, set safe_point=false, share=%d", p->index, p->share);
        }

        // exit
        if (processor_get_exit()) {
            DEBUGF("[runtime.processor_run] p_index=%d, share=%d need stop, goto exit", p->index, p->share);
            goto EXIT;
        }

        // - 处理 coroutine (找到 io 可用的 goroutine)
        while (true) {
            coroutine_t* co = linked_pop_free(p->runnable_list);
            if (!co) {
                // runnable list 已经处理完成
                break;
            }
            DEBUGF("[runtime.processor_run] will handle coroutine, p_index=%d, co=%p, status=%d, share=%d", p->index, co, co->status,
                   p->share);

            assertf(co->status == CO_STATUS_RUNNABLE, "coroutine status must be runnable");
            coroutine_resume(p, co);

            DEBUGF("[runtime.processor_run] coroutine resume completed, p_index=%d, co=%p, status=%d, share=%d", p->index, co, co->status,
                   p->share);
        }

        // solo processor 退出逻辑
        if (!p->share) {
            // 经过上面的 resume, coroutine 必定已经绑定
            assert(p->coroutine);

            if (p->coroutine->status == CO_STATUS_DEAD) {
                DEBUGF("[runtime.processor_run] solo processor exit, p_index=%d, co=%p, status=%d, share=%d", p->index, p->coroutine,
                       p->coroutine->status, p->share);
                goto EXIT;
            }
        }

        // - 处理 io 就绪事件(也就是 run 指定时间的 libuv)
        io_run(p, WAIT_SHORT_TIME);
    }

EXIT:
    p->exit = true;
    p->no_preempt = true;
    p->thread_id = 0;
    DEBUGF("[runtime.processor_run] exited, p_index=%d", p->index);
}

void coroutine_dispatch(coroutine_t* co) {
    DEBUGF("[runtime.coroutine_dispatch] co=%p, solo=%d, share_processor_list=%d", co, co->solo, share_processor_list->count);

    // 分配 coroutine 之前需要给 coroutine 确认初始颜色, 如果是新增的 coroutine，默认都是黑色
    if (gc_stage == GC_STAGE_MARK) {
        co->gc_black = true;
    }

    // - 协程独享线程
    if (co->solo) {
        processor_t* p = processor_new(solo_processor_count++);
        linked_push(p->co_list, co);
        linked_push(p->runnable_list, co);
        linked_push(solo_processor_list, p);

        if (uv_thread_create(&p->thread_id, processor_run, p) != 0) {
            assertf(false, "pthread_create failed %s", strerror(errno));
        }

        DEBUGF("[runtime.coroutine_dispatch] solo processor create, thread_id=%ld", (uint64_t)p->thread_id);
        return;
    }

    // goroutine 默认状态是 runnable
    assert(co->status == CO_STATUS_RUNNABLE);

    // - 遍历 shared_processor_list 找到 co_list->count 最小的 processor 进行调度
    processor_t* min_p = NULL;
    SLICE_FOR(share_processor_list) {
        processor_t* p = SLICE_VALUE(share_processor_list);

        if (!min_p || p->co_list->count < min_p->co_list->count) {
            min_p = p;
        }
    }

    assert(min_p);
    DEBUGF("[runtime.coroutine_dispatch] min_p=%p, co_list=%p, runnable_list=%p", min_p, min_p->co_list, min_p->runnable_list);

    linked_push(min_p->co_list, co);
    linked_push(min_p->runnable_list, co);
    DEBUGF("[runtime.coroutine_dispatch] co=%p to processor=%p", co, min_p);
}

/**
 * 各种全局变量初始化都通过该方法
 */
void processor_init() {
    // - 读取当前 cpu 线程数初始化相应数量的 p
    uv_cpu_info_t* info;
    uv_cpu_info(&info, &cpu_count);
    uv_free_cpu_info(info, cpu_count);

    // - 初始化全局标识
    processor_need_exit = false;
    processor_need_stw = false;
    gc_barrier = false;
    uv_mutex_init(&gc_stage_locker);
    gc_stage = GC_STAGE_OFF;

    // - 初始化线程维度遍历
    uv_key_create(&tls_processor_key);
    uv_key_create(&tls_coroutine_key);

    // 初始化 aco 需要的 tls 变量(不能再线程中 create)
    uv_key_create(&aco_gtls_co);
    uv_key_create(&aco_gtls_last_word_fp);
    uv_key_create(&aco_gtls_fpucw_mxcsr);

    DEBUGF("[runtime.processor_init] cpu_count=%d", cpu_count);

    // - 为每一个 processor 创建对应的 thread 进行处理对应的 p
    share_processor_list = slice_capacity_new(cpu_count);
    solo_processor_list = linked_new();

    global_gc_worklist = linked_new();
    uv_mutex_init(&global_gc_locker);

    for (int i = 0; i < cpu_count; ++i) {
        processor_t* p = processor_new(i);
        p->share = true;
        slice_push(share_processor_list, p);

        // 创建一个新的线程用来处理
        if (uv_thread_create(&p->thread_id, processor_run, p) != 0) {
            assertf(false, "pthread_create failed %s", strerror(errno));
        }

        DEBUGF("[runtime.processor_init] processor create, index=%d, thread_id=%ld", i, (uint64_t)p->thread_id);
    }
}

processor_t* processor_get() {
    processor_t* p = uv_key_get(&tls_processor_key);
    return p;
}

coroutine_t* coroutine_get() {
    return uv_key_get(&tls_coroutine_key);
}

void rt_processor_attach_errort(char* msg) {
    DEBUGF("[runtime.rt_processor_attach_errort] msg=%s", msg);
    //    processor_t* p = processor_get();
    //    n_errort* errort = n_errort_new(string_new(msg, strlen(msg)), 1);
    //    p->errort = errort;
}

void processor_dump_errort(n_errort* errort) {
    DEBUGF("[runtime.processor_dump_errort] errort base=%p", errort)
    n_string_t* msg = errort->msg;
    DEBUGF("[runtime.processor_dump_errort] memory_string len: %lu, base: %p", msg->length, msg->data);
    assert(errort->traces->length > 0);

    n_trace_t first_trace = {};
    vec_access(errort->traces, 0, &first_trace);
    char* dump_msg = dsprintf("catch error: '%s' at %s:%d:%d\n", (char*)errort->msg->data, (char*)first_trace.path->data, first_trace.line,
                              first_trace.column);

    VOID write(STDOUT_FILENO, dump_msg, strlen(dump_msg));

    if (errort->traces->length > 1) {
        char* temp = "stack backtrace:\n";
        VOID write(STDOUT_FILENO, temp, strlen(temp));
        for (int i = 0; i < errort->traces->length; ++i) {
            n_trace_t trace = {};
            vec_access(errort->traces, i, &trace);
            temp = dsprintf("%d:\t%s\n\t\tat %s:%d:%d\n", i, (char*)trace.ident->data, (char*)trace.path->data, trace.line, trace.column);
            VOID write(STDOUT_FILENO, temp, strlen(temp));
        }
    }
}

void mark_ptr_black(void* ptr) {
    addr_t addr = (addr_t)ptr;
    // get mspan by ptr
    mspan_t* span = span_of(addr);

    //  get span index
    uint64_t obj_index = (addr - span->base) / span->obj_size;
    mark_obj_black(span, obj_index);
}

/**
 * TODO target use debug, can delete
 * @param target
 */
__attribute__((optimize(0))) void pre_tpl_hook(char* target) {
    coroutine_t* c = coroutine_get();
    aco_t* aco = c->aco;

    uint64_t rbp_value;
#ifdef __x86_64__
    asm("mov %%rbp, %0" : "=r"(rbp_value));
#elif
    assertf(false, "not support");
#endif

    aco->bp_offset = (uint64_t)aco->share_stack->align_retptr - rbp_value;

#ifdef DEBUG
    addr_t ret_addr = fetch_addr_value(rbp_value + POINTER_SIZE);
    fndef_t* fn = find_fn(ret_addr);
    if (fn) {
        DEBUGF("[runtime.pre_tpl_hook] ret_addr=%p, fn=%s -> %s, path=%s:%lu", (void*)ret_addr, fn->name, target, fn->rel_path, fn->line);
    }
    // 基于 share stack 计算 offset
    DEBUGF("[runtime.pre_tpl_hook] aco->align_retptr=%p, rbp=%p, bp_offset=%lu", aco->share_stack->align_retptr, (void*)rbp_value,
           aco->bp_offset)
#endif
}

/**
 * ~~所有的 temp_call 都会被认为 block syscall 进行处理~~
 * TODO 只有打了 block tag 的 fn 才会被当成 block syscall 进行处理
 */
void pre_blocking_syscall() {
    processor_t* p = processor_get();
    coroutine_t* c = coroutine_get();
    c->status = CO_STATUS_SYSCALL;

    // solo coroutine 占用整个线程，所以即使是阻塞的 syscall 也不需要进行什么操作
    if (c->solo) {
        // TODO 进入 block call 阻塞之前, 将当前的栈信息进行 save, 从而能够正常的进行 stack 扫描
        return;
    }

    // 接下来 coroutine 将会独占当前线程, 将 thread 绑定在 coroutine 上
    c->thread_id = p->thread_id;

    // 创建一个新的线程用于其他 processor 调度
    if (uv_thread_create(&p->thread_id, processor_run, p) != 0) {
        assertf(false, "pthread_create failed %s", strerror(errno));
    }
}

void post_blocking_syscall() {
    coroutine_t* co = coroutine_get();
    processor_t* p = processor_get();

    if (co->solo) {
        // 只有独享线程在遇到阻塞调用时需要延迟处理 stw, 共享线程遇到阻塞 io 时会启动一个新的线程进行处理
        // 处理完成后直接就退出了，不需要担心后续的操作
        if (processor_get_stw()) {
            DEBUGF("[runtime.post_blocking_syscall] solo processor=%p, need stw, set safe_point=true", p);
            p->safe_point = true;
            while (processor_get_stw()) {
                usleep(WAIT_MID_TIME); // 每 50ms 检测一次 STW 是否解除
            }
            p->safe_point = false;
            DEBUGF("[runtime.post_blocking_syscall] solo processor=%p, stw completed, set safe_point=false", p);
        }
        co->status = CO_STATUS_RUNNING; // coroutine 继续运行
        return;
    }

    // block syscall thread 必定已经解绑。
    assert(co->thread_id != p->thread_id);

    co->thread_id = 0;
    co->status = CO_STATUS_RUNNABLE;
    linked_push(p->runnable_list, co);

    // 让出当前线程控制权即可, processor_run 会做后续的处理
    aco_yield();
}

bool processor_own(processor_t* p) {
    return uv_thread_self() == p->thread_id;
}

coroutine_t* coroutine_new(void* fn, n_vec_t* args, bool solo, bool main) {
    coroutine_t* co = NEW(coroutine_t);
    co->fn = fn;
    co->solo = solo;
    co->gc_black = false;
    co->no_preempt = false;
    co->status = CO_STATUS_RUNNABLE;
    co->args = args;
    co->aco = NULL;
    co->p = NULL;
    co->result = NULL;
    co->thread_id = 0;
    co->main = main;

    return co;
}

processor_t* processor_new(int index) {
    processor_t* p = NEW(processor_t);

    p->uv_loop = NEW(uv_loop_t);
    uv_loop_init(p->uv_loop);
    uv_mutex_init(&p->gc_locker);
    uv_mutex_init(&p->thread_locker);
    p->sig.sa_flags = 0;
    p->thread_id = 0;
    p->coroutine = NULL;
    p->co_started_at = 0;
    p->mcache.flush_gen = 0; // 线程维度缓存，避免内存分配锁
    p->runnable_list = linked_new();
    p->co_list = linked_new();
    p->gc_work_finished = false;
    p->gc_worklist = linked_new();
    p->index = index;

    return p;
}

void thread_handle_sigurg(int sig) {
    // void thread_handle_sigurg(uv_signal_t* handle, int sig) {
    processor_t* p = processor_get();
    DEBUGF("[runtime.thread_handle_sigurg] sig=%d, p_index_%d=%d, p_base=%p, p->main_aco=%p", sig, p->share, p->index, p, p->main_aco);

    coroutine_t* co = p->coroutine;
    co->is_preempt = true;

    aco_t* aco = get_aco_gtls_co();

    DEBUGF("[runtime.thread_handle_sigurg] p_index_%d=%d, co=%p, co->aco=%p, co->aco->main_aco=%p, tls_aco=%p yield to runnable", p->share,
           p->index, co, co->aco, co->aco->main_co, aco);

    assert(co->aco == aco);
    assert(co->aco->main_co);

    // 由于是被抢占，所以当前 coroutine 可以直接继续运行，而不需要等待什么 io 就绪
    co->status = CO_STATUS_RUNNABLE;
    linked_push(p->runnable_list, co);
    aco_yield1(aco);

    co->is_preempt = false;
    DEBUGF("[runtime.thread_handle_sigurg] aco_resume,  p_index_%d=%d, co=%p", p->share, p->index, co);
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

void processor_free(processor_t* p) {
    DEBUGF("[runtime.processor_free] p_index_%d=%d, loop=%p", p->share, p->index, p->uv_loop);
    linked_free(p->runnable_list);
    linked_free(p->co_list);
    aco_destroy(p->main_aco);

    assert(p->uv_loop);
    assert(p->uv_loop->active_reqs.count == 0);
    uv_loop_close(p->uv_loop);
    free(p->uv_loop);

    free(p);
}

/**
 * 是否所有的 processor 都到达了安全点
 * @return
 */
bool processor_all_safe() {
    SLICE_FOR(share_processor_list) {
        processor_t* p = SLICE_VALUE(share_processor_list);
        assert(p);
        if (p->exit) {
            continue;
        }

        if (p->safe_point) {
            continue;
        }

        return false;
    }

    LINKED_FOR(solo_processor_list) {
        processor_t* p = LINKED_VALUE();
        assert(p);
        if (p->exit) {
            continue;
        }

        if (p->safe_point) {
            continue;
        }

        // 如果 coroutine 已经开始调度，并且处于 syscall 状态，则认为当前 solo processor 已经进入了安全点
        if (p->coroutine && p->coroutine->status == CO_STATUS_SYSCALL) {
            continue;
        }

        return false;
    }

    return true;
}

void processor_wait_all_safe() {
    while (!processor_all_safe()) {
        usleep(WAIT_MID_TIME);
    }
}

/**
 * 遍历所有的 share processor 和 solo processor 判断 gc 是否全部完成
 * @return
 */
static bool all_gc_work_finished() {
    SLICE_FOR(share_processor_list) {
        processor_t* p = SLICE_VALUE(share_processor_list);
        if (!p->gc_work_finished) {
            return false;
        }
    }

    LINKED_FOR(solo_processor_list) {
        processor_t* p = LINKED_VALUE();
        if (!p->gc_work_finished) {
            return false;
        }
    }

    return true;
}

/**
 * 需要等待独享和共享协程全部完成 gc 工作
 */
void wait_all_gc_work_finished() {
    DEBUGF("[runtime_gc.wait_all_gc_work_finished] start");

    while (!all_gc_work_finished()) {
        usleep(WAIT_MID_TIME);
    }

    DEBUGF("[runtime_gc.wait_all_gc_work_finished] all processor gc work finish");

    // 重置 gc_work_finish 为 false，避免影响下一次 GC
    SLICE_FOR(share_processor_list) {
        processor_t* p = SLICE_VALUE(share_processor_list);
        if (p->exit) {
            continue;
        }

        p->gc_work_finished = false;
    }

    LINKED_FOR(solo_processor_list) {
        processor_t* p = LINKED_VALUE();
        if (p->exit) {
            continue;
        }

        p->gc_work_finished = false;
    }

    DEBUGF("[runtime_gc.wait_all_gc_work_finished] share/solo processor to gc_work_finished=false, end");
}

void* global_gc_worklist_pop() {
    return linked_pop(global_gc_worklist);
}
