#include "allocator.h"
#include "processor.h"
#include "memory.h"
#include "utils/type.h"


static uint8_t calc_sizeclass(uint8_t size) {
    for (int i = 0; i < SIZECLASS_COUNT; ++i) {
        uint obj_size = class_obj_size[i];
        if (size > obj_size) {
            continue;
        }
        // obj 能够刚好装下 size
        return i;
    }
    assertf(false, "size=%d notfound fit class", size);
    return 0;
}

// 7位sizeclass + 1位是否包含指针
static uint8_t make_spanclass(uint8_t sizeclass, uint8_t no_ptr) {
    return (sizeclass << 1) | no_ptr;
}

static uint8_t take_sizeclass(uint8_t spanclass) {
    return spanclass >> 1;
}

static uint span_pages_count(uint8_t spanclass) {
    uint8_t sizeclass = take_sizeclass(spanclass);
    return clas_pages_count[sizeclass];
}

static bool summary_find_continuous(uint8_t level, page_summary_t *summaries, uint64_t *start, uint64_t *end,
                                    uint64_t pages_count) {
    uint64_t find_start = 0;
    uint64_t find_end = 0;
    bool find = false;
    // manager max pages
    uint64_t max_pages_count = summary_page_count[level];
    uint64_t find_max = 0;
    for (uint64_t i = *start; i < *end; ++i) {
        page_summary_t s = summaries[i];
        find_max += s.start;
        if (find_max >= pages_count) {
            // 找到了
            find = true;
            find_end = i;
            break;
        }

        if (s.max > find_max) {
            find_max = s.max;
            find_start = i;
        }
        if (find_max >= pages_count) {
            // 找到了
            find = true;
            find_end = i;
            break;
        }

        // 目前的空间都不满足条件，判断当前空间是否是完整空闲，非完整的空闲，必须要中断了
        if (s.end != max_pages_count) {
            find_start = i;
            find_max = s.end;
        }
    }
    *start = find_start;
    *end = find_end;
    return find;
}


static addr_t chunk_base(addr_t index) {
    return index * (CHUNK_BITS_COUNT * PAGE_SIZE) + ARENA_BASE_OFFSET;
}

/**
 * 一个 chunk 管理的内存大小是 CHUNK_BITS_COUNT * PAGE_SIZE
 * @param base
 * @return
 */
static uint64_t chunk_index(addr_t base) {
    return (base - ARENA_BASE_OFFSET) / (CHUNK_BITS_COUNT * PAGE_SIZE);
}

static uint64_t chunk_index_l1(uint64_t index) {
    return index / PAGE_ALLOC_CHUNK_SPLIT;
}

/**
 * 取余逻辑
 * @param index
 * @return
 */
static uint64_t chunk_index_l2(uint64_t index) {
    return index % PAGE_ALLOC_CHUNK_SPLIT;
}

/**
 * 根据内存地址找到响应的 arena
 * @param base
 * @return
 */
static page_chunk_t *take_chunk(addr_t base) {
    return &memory->mheap.page_alloc.chunks[chunk_index_l1(base)][chunk_index_l2(base)];
}


/**
 * start,max,end
 * @param chunk
 * @return
 */
static page_summary_t chunk_summarize(page_chunk_t chunk) {
    uint32_t max = 0;
    uint bit_start = 0;
    uint bit_end = 0;
    for (int i = 0; i < CHUNK_BITS_COUNT; ++i) {
        bool used = bitmap_test((byte *) chunk.blocks, i);
        if (used) {
            // 重新开始计算
            bit_start = 0;
            bit_end = 0;
        } else {
            // 空闲
            if (bit_start == 0) {
                bit_start = i;
            }
            bit_end = i;
        }

        if ((bit_end + 1 - bit_start) > max) {
            max = (bit_end + 1 - bit_start);
        }
    }
    uint16_t start = 0;
    for (int i = 0; i < CHUNK_BITS_COUNT; ++i) {
        bool used = bitmap_test((byte *) chunk.blocks, i);
        if (used) {
            break;
        }
        start += 1;
    }

    uint16_t end = 0;
    for (int i = CHUNK_BITS_COUNT - 1; i >= 0; i--) {
        bool used = bitmap_test((byte *) chunk.blocks, i);
        if (used) {
            break;
        }
        end += 1;
    }
    page_summary_t summary = {
            .start = start,
            .max = max,
            .end = end,
    };

    return summary;
}

/**
 * 寻找这一组 summaries 中的最大组合值
 * @param summaries
 * @return
 */
static page_summary_t merge_summarize(uint8_t level, page_summary_t summaries[PAGE_SUMMARY_MERGE_COUNT]) {
    uint64_t max_pages_count = summary_page_count[level];

    // max 算法参考 find 算法
    uint32_t max = 0;
    uint64_t continue_max = 0;
    for (int i = 0; i < PAGE_SUMMARY_MERGE_COUNT; ++i) {
        page_summary_t s = summaries[i];
        continue_max += s.start;

        if (s.max > continue_max) {
            continue_max = s.max;
        }
        if (continue_max > max) {
            max = continue_max;
        }

        // 目前的空间都不满足条件，判断当前空间是否是完整空闲，非完整的空闲，必须要中断了
        if (s.end != max_pages_count) {
            continue_max = s.end;
        }
    }

    // 找 start
    uint16_t start = 0;
    for (int i = 0; i < PAGE_SUMMARY_MERGE_COUNT; ++i) {
        page_summary_t s = summaries[i];
        start += s.start;
        if (s.start == max_pages_count) {
            continue;
        }
        break;
    }

    // 找 end
    uint16_t end = 0;
    for (int i = PAGE_SUMMARY_MERGE_COUNT - 1; i >= 0; --i) {
        page_summary_t s = summaries[i];
        end += s.end;
        if (s.end == max_pages_count) {
            continue;
        }
        break;
    }

    page_summary_t summary = {
            .start = start,
            .max = max,
            .end = end
    };

    return summary;
}

/**
 * 计算 base ~ end 所涉及的索引地址范围,level 越高，通常涉及的索引范围也就越小
 * @param level
 * @param base
 * @param end
 * @param base_index
 * @param end_index
 */
static void
calc_page_summary_index(uint8_t level, addr_t start, addr_t end, uint64_t *start_index, uint64_t *end_index) {
    uint scale = summary_index_scale[level];
    // 计算 chunk index
    *start_index = chunk_index(start) / scale;
    *end_index = chunk_index(end - 1) / scale;
}

/**
 * 更新 l5 ~ l1 的 summary
 * @param base
 * @param end
 */
static void page_summary_update(addr_t base, uint64_t size) {
    page_alloc_t *page_alloc = &memory->mheap.page_alloc;
    page_summary_t *last_summaries = page_alloc->summary[PAGE_SUMMARY_LEVEL - 1];

    // 维护 chunks 数据
    for (uint64_t index = chunk_index(base); index < chunk_index(base + size); index++) {
        // 计算 l1 可能为 null
        page_chunk_t *l1_chunks = page_alloc->chunks[chunk_index_l1(index)];
        assertf(l1_chunks, "chunks is null");

        page_chunk_t chunk = l1_chunks[chunk_index_l2(index)];
        // 计算每个 chunk 的  summary
        last_summaries[index] = chunk_summarize(chunk);
    }

    // l5 一个 summary 对应一个 chunk 对应 4MB 的数据
    // l4 则可以管理 4 * 8 = 32M 的数据

    addr_t end = base + size;
    // update l4 ~ l1 summary
    for (uint8_t level = PAGE_SUMMARY_LEVEL - 2; level >= 0; level--) {
        // - 计算 addr 在当前层的 index,例如在 l4 中，
        uint64_t base_index = 0;
        uint64_t end_index = 0;
        calc_page_summary_index(level, base, end, &base_index, &end_index);
        page_summary_t *current_summaries = page_alloc->summary[level];
        page_summary_t *next_level_summaries = page_alloc->summary[level + 1];
        for (uint64_t i = base_index; i < end_index; ++i) {
            // 基于下一级别的 8 个 block, merge 出新的 summary 并赋值
            uint64_t next_summary_start = i * 8;
            uint64_t next_summary_end = next_summary_start + 8;
            page_summary_t temp_summaries[PAGE_SUMMARY_MERGE_COUNT] = {0};
            uint8_t temp_index = 0;
            for (uint64_t j = next_summary_start; j < next_summary_end; j++) {
                page_summary_t temp = next_level_summaries[j];
                temp_summaries[temp_index] = temp;
                temp_index += 1;
            }

            // calc summary
            current_summaries[i] = merge_summarize(level, temp_summaries);
        }
    }
}

/**
 * TODO 可能有越界问题,debug 时需要特别关注
 * @param base
 * @param size
 * @param v
 */
static void chunks_set(addr_t base, uint64_t size, bool v) {
    page_alloc_t *page_alloc = &memory->mheap.page_alloc;
    uint64_t end = base + size;
    for (uint64_t index = chunk_index(base); index < chunk_index(end); index++) {
        // 计算 chunk
        page_chunk_t chunk = page_alloc->chunks[chunk_index_l1(index)][chunk_index_l2(index)];
        uint64_t temp_base = chunk_base(index);
        uint64_t bit_start = 0;
        if (temp_base < base) {
            bit_start = (base - temp_base) / PAGE_SIZE;
        }
        uint64_t temp_end = temp_base + (CHUNK_BITS_COUNT * PAGE_SIZE);
        uint64_t bit_end = CHUNK_BITS_COUNT - 1;
        if (temp_end > end) {
            bit_end = ((end - temp_base) / PAGE_SIZE) - 1;
        }

        for (uint64_t i = bit_start; i <= bit_end; ++i) {
            if (v) {
                bitmap_set((byte *) chunk.blocks, i);
            } else {
                bitmap_clear((byte *) chunk.blocks, i);
            };
        }
    }

    // 更新 summary
    page_summary_update(base, size);
}

/**
 * 从 mheap page_alloc_t 中查找连续空闲的 pages
 * - level 1 查找,找不到就返回 0 (查找时需要综合考虑多个 block 联合的情况,尤其是一组空闲的 summary)
 * - 最终会找到一组连续空闲的 chunks
 * - 更新这一组 chunks 为 1， 并更新相关索引
 * - 返回 chunks 的起始地址就行了
 *
 * summaries 可能会跨越多个 arena, 此时并不遵守连续性的事实？尤其是在计算更高级别的摘要时？
 * chunk 和 summary 覆盖了从 0.75T ~ 128T之间的所有的内存，所以并不存在连续性的问题，在非 arena 申请的区域
 * 其 summary [star=0,max=0,end=0] 并不会影响搜索 pages
 * @param pages_count
 * @return
 */
static addr_t page_alloc_find(uint pages_count) {
    // 第一个 level 需要查找所有的元素
    uint64_t start = 0;
    uint64_t end = PAGE_SUMMARY_COUNT_L1;
    page_alloc_t *page_alloc = &memory->mheap.page_alloc;

    for (int level = 0; level < PAGE_SUMMARY_LEVEL; ++level) {
        page_summary_t *summaries = page_alloc->summary[level];
        bool found = summary_find_continuous(level, summaries, &start, &end, pages_count);
        if (level == 0 && !found) {
            return 0;
        }
        assertf(found, "level zero find, next level must found");
    }

    // start ~ end 指向的一组 chunks 中包含连续的内存空间，现在需要确认起起点位置(假设 start == end, 其 可能是 start or mid or end 中的任意一个位置)
    addr_t find_addr = 0;
    if (start == end) {
        // 从头到尾遍历找到可能的起点位置
        uint64_t bit_start = 0;
        uint64_t bit_end = 0;
        // 一个 chunk 有 512 bit
        page_chunk_t *chunk = take_chunk(start);
        for (int i = 0; i < CHUNK_BITS_COUNT; i++) {
            bool used = bitmap_test((uint8_t *) chunk->blocks, i);
            if (used) {
                bit_start = 0;
                bit_end = 0;
            } else {
                // 空闲
                if (bit_start == 0) {
                    bit_start = i;
                }
                bit_end = i;
            }
            // 1, 1
            if ((bit_end + 1 - bit_start) >= pages_count) {
                break;
            }
        }

        // 计算 find_addr
        find_addr = chunk_base(start) + bit_start * PAGE_SIZE;

        // 更新从 find_addr 对应的 bit ~ page_count 位置的所有 chunk 的 bit 为 1
    } else {
        // 跨越多个块，其起点一定是 summary end
        page_summary_t *l5_summaries = page_alloc->summary[PAGE_SUMMARY_LEVEL - 1];
        page_summary_t start_summary = l5_summaries[start];
        uint64_t bit_start = CHUNK_BITS_COUNT + 1 - start_summary.end;
        find_addr = chunk_base(start) + bit_start * PAGE_SIZE;
    }
    assertf(find_addr % PAGE_SIZE == 0, "find addr=%p not align", find_addr);

    // 更新相关的 chunks 为使用状态
    chunks_set(find_addr, pages_count * PAGE_SIZE, 1);

    return find_addr;
}


/**
 * 建立 mheap.page_alloc_find.chunks
 * 记录一下最大的 index, 待会查找的时候能用到
 * @param base
 * @param size
 * @return
 */
static void page_alloc_grow(addr_t base, uint64_t size) {
    page_alloc_t *page_alloc = &memory->mheap.page_alloc;

    // 维护 chunks 数据
    for (uint64_t index = chunk_index(base); index < chunk_index(base + size); index++) {
        // 计算 l1 可能为 null
        uint64_t l1 = chunk_index_l1(index);
        if (page_alloc->chunks[l1] == NULL) {
            // 这里实际上分配了 8192 个 chunk, 且这些 chunk 都有默认值 0， 我们知道 0 表示对应的 arena page 空闲
            // 但是实际上可能并不是这样，但是不用担心，我们只会更新从 base ~ base+size 对应的 summary
            // 其他 summary 的值默认也为 0，而 summary 为 0 表示没有空闲的内存使用
            // grow 本质是增加一些空闲的页面，而不需要修改 chunk 的 bit = 1
            // 这个工作交给 page_alloc_find 去做
            page_alloc->chunks[l1] = mallocz(sizeof(page_chunk_t) * PAGE_ALLOC_CHUNK_SPLIT);
        }
    }

    // grow 的关键还是 summary 更新让 page_alloc_t 中可以找到连续可用的 pages
    // 由于从 arena 中申请了新的 pages,现在需要更新相关的索引
    page_summary_update(base, base + size);
}

/**
 * - 分配的大小是固定的，再 linux 64 中就是 64MB
 * - 基于 hint 调用 mmap 申请一块内存(如果申请成功则更新 hint)
 * - 更新 mheap.current_arena
 * - 由于这是一个新的 arena,所以需要像 mheap.arenas 中添加数据，而不再是 null 了
 * 但是不需要更新 mheap.arenas 相关的值,因为没有真正的开始触发分配逻辑
 * 同样mheap.pages 同样也不需要更新，因为没有相关的值被更新
 * @return
 */
void *mheap_sys_alloc(mheap_t mheap, uint64_t *size) {
    arena_hint_t *hint = mheap.arena_hints;

    // size 对齐
    uint64_t alloc_size = align((int64_t) *size, ARENA_SIZE);

    void *v = NULL;
    while (true) {
        v = sys_memory_map((void *) hint->addr, alloc_size);
        if (v == (void *) hint->addr) {
            // 分配成功, 定义下一个分配点,基于此可以获得 64MB 对齐的内存地址
            hint->addr += alloc_size;
            break;
        }
        // 分配失败
        // 释放刚刚申请的内存区域
        sys_memory_unmap(v, alloc_size);

        // 进行申请重试
        assertf(!hint->last, "out of memory: arena hint use up");
        hint = hint->next;
    }

    // 申请成功，申请的范围是 v ~ (v+alloc_size), 可能包含多个 arena, 需要创建相关 arena meta
    for (uint64_t i = arena_index((uint64_t) v); i <= arena_index((uint64_t) v + alloc_size - 1); ++i) {
        arena_t *arena = NEW(arena_t);
        arena->base = arena_base(i);
        mheap.arenas[i] = arena;
        slice_push(mheap.arena_indexes, (void *) i);
    }

    *size = alloc_size;
    return v;
}

/**
 * 根据内存地址找到响应的 arena
 * @param base
 * @return
 */
static arena_t *take_arena(addr_t base) {
    return memory->mheap.arenas[arena_index(base)];
}


/**
 * 计算 mheap 的 pages ->spans
 * @param span
 */
static void mheap_set_spans(mspan_t *span) {
    // - 根据 span.base 定位 arena
    arena_t *arena = take_arena(span->base);

    uint page_index = (span->base - arena->base) / PAGE_SIZE;
    for (int i = 0; i < span->pages_count; i++) {
        arena->spans[page_index] = span;
        page_index += 1;
    }
}

static void mheap_clear_spans(mspan_t *span) {
    // - 根据 span.base 定位 arena
    arena_t *arena = take_arena(span->base);

    uint page_index = (span->base - arena->base) / PAGE_SIZE;
    for (int i = 0; i < span->pages_count; i++) {
        arena->spans[page_index] = NULL;
        page_index += 1;
    }
}

/**
 * arena heap 增长 pages_count 长度,如果 current arena 没有足够的空间，将会申请 1 个或者 n个崭新的 arena
 * @param pages_count
 */
static void mheap_grow(uint pages_count) {
    // pages_alloc 按 chunk 管理内存，所以需要按 chunk 包含的 pages_count 对齐
    uint64_t size = align(pages_count, CHUNK_BITS_COUNT) * PAGE_SIZE;

    addr_t cursor = memory->mheap.current_arena.cursor;
    addr_t end = memory->mheap.current_arena.end;
    assertf(end > cursor, "mheap not hold arena failed");

    if (end - cursor < size) {
        // cursor 没有足够的空间，需要重新申请一个新的空间
        // alloc_size 在方法内部会按 arena size 对齐
        uint64_t alloc_size = size;
        addr_t v = (addr_t) mheap_sys_alloc(memory->mheap, &alloc_size);
        if (v == end) {
            // arena 空间连续,此时只需要更新 end
            end += alloc_size;
        } else {
            cursor = v;
            end = v + alloc_size;
        }
    }

    // 基于 current_arena 进行 pages 的分配,分配的 pages 需要被 page_alloc 索引
    page_alloc_grow(cursor, size);

    // 更新 current arena
    memory->mheap.current_arena.cursor = cursor + size;
    memory->mheap.current_arena.end = cursor + end;
}

/**
 * @param pages_count
 * @param spanclass
 * @return
 */
static mspan_t *mheap_alloc_span(uint pages_count, uint8_t spanclass) {
    // - 从 page_alloc 中查看有没有连续 pages_count 空闲的页，如果有就直接分配
    // 因为有垃圾回收的存在，所以 page_alloc 中的某些部分存在空闲且连续的 pages
    addr_t base = page_alloc_find(pages_count);

    if (base == 0) {
        // -  page_alloc_t 中没有找到可用的 pages,基于 current arena 对当前 page alloc 进行扩容(以 chunk 为单位)
        mheap_grow(pages_count);

        // - 经过上面的 grow 再次从 page_alloc 中拉取合适大小的 pages_count 并组成 span
        base = page_alloc_find(pages_count);
    }
    assertf(base > 0, "out of memory: page alloc failed");

    // - 新增的 span 需要在 arena 中建立 page -> span 的关联关系
    mspan_t *span = mspan_new(base, pages_count, spanclass);
    mheap_set_spans(span); // 大内存申请时 span 同样放到了此处管理

    return span;
}

static mspan_t *mcentral_grow(mcentral_t mcentral) {
    // 从 mheap 中按 page 申请一段内存, mspan 对 mheap 是已知的， mheap 同样需要管理 mspan 列表
    uint pages_count = span_pages_count(mcentral.spanclass);

    mspan_t *span = mheap_alloc_span(pages_count, mcentral.spanclass);
    return span;
}

/**
 * 从 mcache 中找到一个空闲的 mspan 并返回
 * @param mcentral
 * @return
 */
static mspan_t *cache_span(mcentral_t mcentral) {
    mspan_t *span = NULL;
    if (!list_is_empty(mcentral.partial_swept)) {
        // partial 是空的，表示当前 mcentral 中已经没有任何可以使用的 mspan 了，需要去 mheap 中申请咯
        span = list_pop(mcentral.partial_swept);
        goto HAVE_SPAN;
    }

    // 当前 mcentral 中已经没有可以使用的 mspan 需要走 grow 逻辑
    span = mcentral_grow(mcentral);

    HAVE_SPAN:
    assertf(span && span->obj_count - span->alloc_count > 0, "span unavailable");
    return span;
}

/**
 * 将 mspan 归还到 mcentral 队列中
 * @param mcentral
 * @param span
 */
void uncache_span(mcentral_t mcentral, mspan_t *span) {
    assertf(span->alloc_count == 0, "mspan alloc_count == 0");

    if (span->obj_count - span->alloc_count > 0) {
        list_push(mcentral.partial_swept, span);
    } else {
        list_push(mcentral.full_swept, span);
    }
}


/**
 * 从 mcentral 中找到一个可以使用的 span 给 mcache 使用
 * mcache.alloc[spanclass] 对应的 mspan 可能 null
 * @param mcache
 * @param spanclass
 * @return
 */
static mspan_t *mcache_refill(mcache_t mcache, uint64_t spanclass) {
    mspan_t *mspan = mcache.alloc[spanclass];
    mcentral_t mcentral = memory->mheap.centrals[spanclass];

    if (mspan) {
        // mspan to mcentral
        uncache_span(mcentral, mspan);
    }

    // cache
    mspan = cache_span(mcentral);
    mcache.alloc[spanclass] = mspan;
    return mspan;
}

/**
 * 从 spanclass 对应的 span 中找到一个 free 的 obj 并返回
 * @param spanclass
 * @return
 */
static addr_t mcache_alloc(uint8_t spanclass, mspan_t **span) {
    processor_t p = processor_get();
    mcache_t mcache = p.mcache;
    mspan_t *mspan = mcache.alloc[spanclass];

    // 如果 mspan 中有空闲的 obj 则优先选择空闲的 obj 进行分配
    // 判断当前 mspan 是否已经满载了，如果满载需要从 mcentral 中填充 mspan
    if (mspan == NULL || mspan->alloc_count == mspan->obj_count) {
        mspan = mcache_refill(mcache, spanclass);
    }

    *span = mspan;

    for (int i = 0; i <= mspan->obj_count; i++) {
        bool used = bitmap_test(mspan->alloc_bits->bits, i);
        if (used) {
            continue;
        }
        // 找到了一个空闲的 obj,计算其
        addr_t addr = mspan->base + i * mspan->obj_size;
        // 标记该节点已经被使用
        bitmap_set(mspan->alloc_bits->bits, i);
        mspan->alloc_count += 1;
        return addr;
    }
    // 没有找到
    assertf(false, "out of memory: mcache_alloc");
    return 0;
}


/**
 * 设置 arena_t 的 bits 具体怎么设置可以参考 arenat_t 中的注释
 * @param addr
 * @param size
 * @param obj_size
 * @param type
 */
static void heap_arena_bits_set(addr_t addr, uint size, uint obj_size, reflect_type_t *type) {
    // 确定 arena bits base
    arena_t *arena = take_arena(addr);
    uint8_t *bits_base = &arena->bits[(addr - arena->base) / (4 * POINTER_SIZE)];

    int index = 0;
    for (addr_t temp_addr = addr; temp_addr < addr + obj_size; temp_addr += POINTER_SIZE) {
        // 标记的是 ptr bit，(scan bit 暂时不做支持)
        int bit_index = (index / 4) * 8 + (index % 4);

        if (bitmap_test(type->gc_bits, index)) {
            bitmap_set(bits_base, bit_index); // 1 表示为指针
        } else {
            bitmap_clear(bits_base, bit_index);
        }
    }
}

// 单位
static addr_t std_malloc(uint size, reflect_type_t *type) {
    bool has_ptr = type != NULL && type->last_ptr;

    uint8_t sizeclass = calc_sizeclass(size);
    uint8_t spanclass = make_spanclass(sizeclass, !has_ptr);
    mspan_t *span;
    addr_t addr = mcache_alloc(spanclass, &span);
    assertf(span, "std_malloc notfound span");

    if (has_ptr) {
        // 对 arena.bits 做标记,标记是指针还是标量
        heap_arena_bits_set(addr, size, span->obj_size, type);
    }

    return addr;
}

static addr_t large_malloc(uint size, reflect_type_t *type) {
    bool no_ptr = type == NULL || type->last_ptr == 0;
    uint8_t spanclass = make_spanclass(0, no_ptr);

    // 计算需要分配的 page count(向上取整)
    uint pages_count = size / PAGE_SIZE;
    if ((size & PAGE_MASK) != 0) {
        pages_count += 1;
    }

    // 直接从堆中分配 span
    mspan_t *s = mheap_alloc_span(pages_count, spanclass);
    assertf(s != NULL, "out of memory");

    // 将 span 推送到 full swept 中，这样才能被 sweept
    mcentral_t *central = &memory->mheap.centrals[spanclass];
    list_push(central->full_swept, s);
    return s->base;
}

/**
 * @return
 */
arena_hint_t *arena_hints_init() {
    arena_hint_t *first = NEW(arena_hint_t);
    first->addr = (uint64_t) ARENA_HINT_BASE;
    first->last = false;
    first->next = NULL;

    arena_hint_t *prev = first;
    for (int i = 1; i < ARENA_HINT_COUNT; i++) {
        arena_hint_t *item = NEW(arena_hint_t);
        item->addr = prev->addr + ARENA_HINT_SIZE;
        prev->next = item;
        prev = item;
    }
    prev->last = true;
    return first;
}

/**
 * - 将 span 从 mcentral 中移除
 * - mspan.base ~ mspan.end 所在的内存区域的 page 需要进行释放
 * - 更新 arena_t 的 span
 * - 能否在不 unmap 的情况下回收物理内存？
 *   使用 madvise(addr, 50 * 1024 * 1024, MADV_REMOVE);
 * @param mheap
 * @param span
 */
void mheap_free_span(mheap_t *mheap, mspan_t *span) {
    // 从 page_alloc 的视角清理 span 对应的内存页
    // chunks bit = 0 表示空闲
    chunks_set(span->base, span->pages_count * PAGE_SIZE, 0);

    // 从 arena 视角清理 span
    mheap_clear_spans(span);

    // arena.bits 保存了当前 span 中的指针 bit, 当下一次当前内存被分配时会覆盖写入
    // 垃圾回收期间不会有任何指针指向该空间，因为当前 span 就是因为没有被任何 ptr 指向才被回收的

    // 将物理内存归还给操作系统
    sys_memory_remove((void *) span->base, span->pages_count * PAGE_SIZE);
}


reflect_type_t *find_rtype(uint index) {
    return &link_rtype_data[index];
}

void fndefs_deserialize() {
    byte *gc_bits_offset = ((byte *) link_fndef_data) + link_fndef_count * sizeof(fndef_t);
    for (int i = 0; i < link_fndef_count; ++i) {
        fndef_t *f = &link_fndef_data[i];
        uint64_t gc_bits_size = calc_gc_bits_size(f->stack_size);

        f->gc_bits = gc_bits_offset;

        gc_bits_offset += gc_bits_size;
    }
}

void rtypes_deserialize() {
    byte *gc_bits_offset = ((byte *) link_rtype_data) + link_rtype_count * sizeof(reflect_type_t);
    for (int i = 0; i < link_rtype_count; ++i) {
        reflect_type_t *r = &link_rtype_data[i];
        uint64_t gc_bits_size = calc_gc_bits_size(r->size);

        r->gc_bits = gc_bits_offset;
        gc_bits_offset += gc_bits_size;
    }
}

void memory_init() {
    // - 初始化 mheap
    mheap_t mheap = {0}; // 所有的结构体，数组初始化为 0, 指针初始化为 null
    mheap.page_alloc.summary[4] = mallocz_big(PAGE_SUMMARY_COUNT_L5 * sizeof(page_summary_t));
    mheap.page_alloc.summary[3] = mallocz_big(PAGE_SUMMARY_COUNT_L4 * sizeof(page_summary_t));
    mheap.page_alloc.summary[2] = mallocz_big(PAGE_SUMMARY_COUNT_L3 * sizeof(page_summary_t));
    mheap.page_alloc.summary[1] = mallocz_big(PAGE_SUMMARY_COUNT_L2 * sizeof(page_summary_t));
    mheap.page_alloc.summary[0] = mallocz_big(PAGE_SUMMARY_COUNT_L1 * sizeof(page_summary_t)); // 8192

    // - arena hint init
    mheap.arena_hints = arena_hints_init();

    // - arena index init
    mheap.arena_indexes = slice_new();

    // new current arena
    mheap.current_arena.cursor = 0;
    mheap.current_arena.end = 0;

    // - 初始化 mcentral
    for (int i = 0; i < SPANCLASS_COUNT; i++) {
        mcentral_t item = mheap.centrals[i];
        item.spanclass = i;
        item.partial_swept = list_new();
//        item.partial_unswept = list_new();
        item.full_swept = list_new();
//        item.full_unswept = list_new();
    }

    // links 数据反序列化，此时 link_fndef_data link_rtype_data 等数据可以正常使用
    fndefs_deserialize();
    rtypes_deserialize();
}

mspan_t *span_of(uint64_t addr) {
    // 根据 ptr 定位 arena, 找到具体的 page_index,
    arena_t *arena = take_arena(addr);
    assertf(arena, "not found arena by addr: %p", addr);
    // 一个 arena 有 ARENA_PAGES_COUNT(8192 个 page), 感觉 addr 定位 page_index
    uint64_t page_index = (addr - arena->base) / PAGE_SIZE;
    mspan_t *span = arena->spans[page_index];
    assertf(span, "not found span by page_index: %d", page_index);
    return span;
}

addr_t mstack_new(uint64_t size) {
    void *base = mallocz(size);
    return (addr_t) base;
}

/**
 * 最后一位如果为 1 表示 no ptr, 0 表示 has ptr
 * @param spanclass
 * @return
 */
bool spanclass_has_ptr(uint8_t spanclass) {
    return (spanclass & 1) == 0;
}


addr_t arena_base(uint64_t arena_index) {
    return arena_index * ARENA_SIZE + ARENA_BASE_OFFSET;
}

uint64_t arena_index(uint64_t base) {
    return (base - ARENA_BASE_OFFSET) / ARENA_SIZE;
}

/**
 * 调用 malloc 时已经将类型数据传递到了 runtime 中，obj 存储时就可以已经计算了详细的 gc_bits 能够方便的扫描出指针
 * @param size
 * @param type
 * @return
 */
addr_t runtime_malloc(uint size, reflect_type_t *type) {
    if (size <= STD_MALLOC_LIMIT) {
        // 1. 标准内存分配(0~32KB)
        return std_malloc(size, type);
    }

    // 2. 大型内存分配(大于>32KB)
    return large_malloc(size, type);
}

mspan_t *mspan_new(uint64_t base, uint pages_count, uint8_t spanclass) {
    mspan_t *span = NEW(mspan_t);
    span->spanclass = spanclass;
    uint8_t sizeclass = take_sizeclass(spanclass);
    if (spanclass == 0) {
        span->obj_size = pages_count * PAGE_SIZE;
    } else {
        span->obj_size = class_obj_size[sizeclass];
    }
    span->base = base;
    span->pages_count = pages_count;
    span->obj_count = span->pages_count * PAGE_SIZE / span->obj_size;
    span->end = span->base + (span->pages_count * PAGE_SIZE);

    span->alloc_bits = bitmap_new((int) span->obj_count);
    span->gcmark_bits = bitmap_new((int) span->obj_count);
    return span;
}
