#include "fs.h"

static void on_write_cb(uv_fs_t *req) {
    fs_context_t *ctx = CONTAINER_OF(req, fs_context_t, req);
    coroutine_t *co = req->data;

    if (req->result < 0) {
        // 文件写入异常，设置错误并返回
        rt_co_throw(co, (char *) uv_strerror(req->result), false);
        co_ready(co);
        uv_fs_req_cleanup(&ctx->req);
        return;
    }

    // 写入成功，req->result 包含写入的字节数
    DEBUGF("[on_write_cb] write file success, bytes written: %ld", req->result);
    ctx->data_len = req->result;

    co_ready(co);
    uv_fs_req_cleanup(&ctx->req);
}

static inline void on_open_cb(uv_fs_t *req) {
    fs_context_t *ctx = CONTAINER_OF(req, fs_context_t, req);
    if (req->result < 0) {
        DEBUGF("[on_open_cb] open file failed: %s, co: %p", uv_strerror(req->result), req->data);

        rt_co_throw(req->data, (char *) uv_strerror(req->result), false);

        co_ready(req->data);
        uv_fs_req_cleanup(&ctx->req);
        return;
    }

    // 文件打开成功，设置 fd 并返回
    ctx->fd = req->result;
    co_ready(req->data);

    uv_fs_req_cleanup(&ctx->req);
}

static void on_read_at_cb(uv_fs_t *req) {
    fs_context_t *ctx = CONTAINER_OF(req, fs_context_t, req);
    coroutine_t *co = req->data;

    if (req->result < 0) {
        // 文件读取异常，设置错误并返回
        rt_co_throw(co, (char *) uv_strerror(req->result), false);
        co_ready(co);
        uv_fs_req_cleanup(&ctx->req);
        return;
    }

    // result >= 0, 表示读取的数据长度
    ctx->data_len = req->result;

    DEBUGF("[on_read_at_cb] read file success, data_len: %ld", ctx->data_len);
    co_ready(co);
    uv_fs_req_cleanup(&ctx->req);
}

static void on_read_cb(uv_fs_t *req) {
    fs_context_t *ctx = CONTAINER_OF(req, fs_context_t, req);
    coroutine_t *co = req->data;

    if (req->result < 0) {
        // 文件读取异常， 设置错误并返回，不需要关闭 fd, fd 由外部控制
        rt_co_throw(co, (char *) uv_strerror(req->result), false);
        co_ready(co);
        uv_fs_req_cleanup(&ctx->req);
        return;
    }

    ctx->data_len += req->result;
    assert(ctx->data_len <= ctx->data_cap);

    DEBUGF("[on_read_cb] read file success, data_len: %ld", ctx->data_len);
    co_ready(co);
    uv_fs_req_cleanup(&ctx->req);
}

fs_context_t *rt_uv_fs_open(n_string_t *path, int64_t flags, int64_t mode) {
    // 创建 context, 不需要主动销毁，后续由用户端接手该变量，并由 GC 进行销毁
    fs_context_t *ctx = rti_gc_malloc(sizeof(fs_context_t), NULL);
    n_processor_t *p = processor_get();
    coroutine_t *co = coroutine_get();

    int result = uv_fs_open(&p->uv_loop, &ctx->req, rt_string_ref(path), (int) flags, (int) mode, on_open_cb);
    if (result) {
        rt_co_throw(co, (char *) uv_strerror(result), false);
        return NULL;
    }

    // 可以 co 的恢复点
    ctx->req.data = co;

    // yield wait file open
    co_yield_waiting(co, NULL, NULL);

    if (co->has_error) {
        DEBUGF("[fs_open] open file failed: %s", rti_error_msg(co->error));
        return NULL;
    } else {
        DEBUGF("[fs_open] open file success: %s", rt_string_ref(path));
    }

    return ctx;
}

n_string_t *rt_uv_fs_content(fs_context_t *ctx) {
    coroutine_t *co = coroutine_get();
    n_processor_t *p = processor_get();
    DEBUGF("[rt_uv_fs_content] read file: %d", ctx->fd);

    if (ctx->closed) {
        rt_co_throw(co, "fd already closed", false);
        return 0;
    }

    // 通过 seek end 判断文件的大小
    off_t file_size = lseek(ctx->fd, 0, SEEK_END);

    ctx->data_cap = file_size;
    ctx->data_len = 0;
    ctx->data = malloc(file_size + 1);
    if (!ctx->data) {
        rt_co_throw(co, "out of memory", false);
        return 0;
    }

    ctx->buf = uv_buf_init(ctx->data, file_size);


    // 基于 fd offset 进行读取, 从 0 开始读取
    ctx->req.data = co;
    uv_fs_read(&p->uv_loop, &ctx->req, ctx->fd, &ctx->buf, 1, 0, on_read_cb);

    co_yield_waiting(co, NULL, NULL);

    if (co->has_error) {
        DEBUGF("[fs_read] read file failed: %s", rti_error_msg(co->error));
        return 0;
    } else {
        DEBUGF("[fs_read] read file success");
    }

    n_string_t *result = string_new(ctx->data, ctx->data_len);

    // 读取完成, 清理 data
    free(ctx->data);
    ctx->data = NULL;
    ctx->data_len = 0;
    ctx->data_cap = 0;

    return result;
}

n_int_t rt_uv_fs_read(fs_context_t *ctx, n_vec_t *buf) {
    return rt_uv_fs_read_at(ctx, buf, -1);
}

n_int_t rt_uv_fs_read_at(fs_context_t *ctx, n_vec_t *buf, int offset) {
    coroutine_t *co = coroutine_get();
    n_processor_t *p = processor_get();
    DEBUGF("[rt_uv_fs_read] read file: %d", ctx->fd);

    if (ctx->closed) {
        rt_co_throw(co, "fd already closed", false);
        return 0;
    }

    // 配置初始缓冲区
    ctx->data_cap = buf->capacity;
    ctx->data_len = 0; // 记录实际读取的数量
    ctx->data = (char *) buf->data;
    ctx->buf = uv_buf_init(ctx->data, buf->capacity);

    // 基于 fd offset 进行读取
    ctx->req.data = co;
    uv_fs_read(&p->uv_loop, &ctx->req, ctx->fd, &ctx->buf, 1, offset, on_read_cb);

    co_yield_waiting(co, NULL, NULL);

    if (co->has_error) {
        DEBUGF("[rt_uv_fs_read] read file failed: %s", rti_error_msg(co->error));
        return 0;
    } else {
        DEBUGF("[rt_uv_fs_read] read file success");
    }

    return ctx->data_len;
}

n_int_t rt_uv_fs_write_at(fs_context_t *ctx, n_vec_t *buf, int offset) {
    coroutine_t *co = coroutine_get();
    n_processor_t *p = processor_get();

    if (ctx->closed) {
        rt_co_throw(co, "fd already closed", false);
        return 0;
    }

    DEBUGF("[fs_write_at] write file: %d, offset: %d, data_len: %d", ctx->fd, offset, buf->length);

    // 配置写入缓冲区
    uv_buf_t uv_buf = uv_buf_init((char *) buf->data, buf->length);

    // 设置协程恢复点
    ctx->req.data = co;

    // 发起异步写入请求，指定偏移量
    uv_fs_write(&p->uv_loop, &ctx->req, ctx->fd, &uv_buf, 1, offset, on_write_cb);

    // 挂起协程等待写入完成
    co_yield_waiting(co, NULL, NULL);

    if (co->has_error) {
        DEBUGF("[fs_write_at] write file failed: %s", rti_error_msg(co->error));
    } else {
        DEBUGF("[fs_write_at] write file success");
    }

    return ctx->data_len;
}

n_int_t rt_uv_fs_write(fs_context_t *ctx, n_vec_t *buf) {
    return rt_uv_fs_write_at(ctx, buf, -1);
}

void rt_uv_fs_close(fs_context_t *ctx) {
    n_processor_t *p = processor_get();
    DEBUGF("[fs_close] close file: %d", ctx->fd);

    // 重复关闭直接返回
    if (ctx->closed) {
        return;
    }

    // 同步方式关闭文件
    int result = uv_fs_close(&p->uv_loop, &ctx->req, ctx->fd, NULL);
    if (result < 0) {
        DEBUGF("[fs_close] close file failed: %s", uv_strerror(result));
    } else {
        DEBUGF("[fs_close] close file success");
    }

    // 清理请求和释放内存
    uv_fs_req_cleanup(&ctx->req);
    if (ctx->data) {
        free(ctx->data);
    }

    ctx->closed = true;

    // ctx 的内存应该由 GC 释放，而不是此处主动释放。
    // 否则 用户端 再次读取 ctx 从而出现奇怪的行为！
//    free(ctx);
}
