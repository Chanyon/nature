#include "basic.h"
#include "runtime/allocator.h"

/**
 * TODO value 可能是各个角度传递过来的实际的值, 比如 int 就是 int
 * 但是当传递当类型为 float 时，由于 float 走 xmm0 寄存器，所以会有读取当问题
 * @param input_rtype_index
 * @param value
 * @return
 */
memory_any_t *convert_any(uint64_t input_rtype_index, void *value_ref) {
    // - 根据 input_rtype_index 找到对应的
    rtype_t *rtype = rt_find_rtype(input_rtype_index);

    assertf(rtype, "cannot find rtype, index = %lu", input_rtype_index);

    DEBUGF("[convert_any] input_rtype_index=%lu, kind=%d, actual_index=%lu, in_heap=%d",
           input_rtype_index, rtype->kind, rtype->index, rtype->in_heap);

    rtype_t any_rtype = gc_rtype(2, TYPE_GC_NOSCAN, to_gc_kind(rtype->kind));

    // any_t 在 element_rtype list 中是可以预注册的，因为其 gc_bits 不会变来变去的，都是恒定不变的！
    memory_any_t *any = runtime_malloc(sizeof(memory_any_t), &any_rtype);

    DEBUGF("[convert_any] any_base: %p, memmove value_ref(%p) -> any->value(%p), size=%lu, fetch_value_8byte=%p",
           any,
           value_ref,
           &any->value,
           rtype_heap_out_size(rtype, POINTER_SIZE),
           (void *) fetch_addr_value((addr_t) value_ref))

    any->rtype = rtype;
    memmove(&any->value, value_ref, rtype_heap_out_size(rtype, POINTER_SIZE));
    DEBUGF("[convert_any] success")

    return any;
}

memory_int_t convert_int(uint64_t input_rtype_index, int64_t int_value, double float_value) {
    DEBUGF("[convert_int] input_index=%lu, int_v=%lu, float_v=%f",
           input_rtype_index,
           int_value,
           float_value);

    rtype_t *input_rtype = rt_find_rtype(input_rtype_index);

    if (is_integer(input_rtype->kind)) {
        return (memory_int_t) int_value; // 避免空间不足造成的非 0 值
    }

    if (is_float(input_rtype->kind)) {
        return (memory_int_t) float_value;
    }

    assertf(false, "cannot convert type=%s to int", type_kind_string[input_rtype->kind]);
    exit(0);
}

memory_f64_t convert_f64(uint64_t input_rtype_index, int64_t int_value, double float_value) {
    DEBUGF("[convert_f64] input_index=%lu, int_v=%lu, float_v=%f",
           input_rtype_index,
           int_value,
           float_value);

    rtype_t *input_rtype = rt_find_rtype(input_rtype_index);
    if (is_float(input_rtype->kind)) {
        return (memory_f64_t) float_value; // 不特别写转换直接返回
    }

    if (is_integer(input_rtype->kind)) {
        return (memory_f64_t) int_value;
    }

    assertf(false, "cannot convert type=%s to float", type_kind_string[input_rtype->kind]);
    exit(1);
}


memory_f32_t convert_f32(uint64_t input_rtype_index, int64_t int_value, double float_value) {
    DEBUGF("[convert_f32] input_index=%lu, int_v=%lu, float_v=%f",
           input_rtype_index,
           int_value,
           float_value);

    rtype_t *input_rtype = rt_find_rtype(input_rtype_index);
    if (is_float(input_rtype->kind)) {
        return (memory_f32_t) float_value; // 不特别写转换直接返回
    }

    if (is_integer(input_rtype->kind)) {
        return (memory_f32_t) int_value;
    }

    assertf(false, "cannot convert type=%s to float", type_kind_string[input_rtype->kind]);
    exit(1);
}

/**
 * null/false/0 会转换成 false, 其他都是 true
 * @param input_rtype_index
 * @param int_value
 * @return
 */
memory_bool_t convert_bool(uint64_t input_rtype_index, int64_t int_value, double float_value) {
    DEBUGF("[runtime.convert_bool] input_rtype_index=%lu, int_value=%lu, f64_value=%f",
           input_rtype_index,
           int_value,
           float_value);
    rtype_t *input_rtype = rt_find_rtype(input_rtype_index);
    if (is_float(input_rtype->kind)) {
        return float_value != 0;
    }

    return int_value != 0;
}

/**
 * @param iterator
 * @param rtype_index
 * @param cursor
 * @return
 */
int64_t iterator_next_key(void *iterator, uint64_t rtype_index, int64_t cursor, void *key_ref) {
    DEBUGF("[runtime.iterator_next_key] iterator base=%p,rtype_index=%lu, cursor=%lu",
           iterator, rtype_index, cursor);

    rtype_t *iterator_rtype = rt_find_rtype(rtype_index);

    cursor += 1;
    if (iterator_rtype->kind == TYPE_LIST) {
        memory_list_t *list = iterator;
        DEBUGF("[runtime.iterator_next_key] kind is list, len=%lu, cap=%lu, data_base=%p", list->length, list->capacity,
               list->array_data);

        if (cursor >= list->length) {
            return -1;
        }

        memmove(key_ref, &cursor, INT_SIZE);
        return cursor;
    }

    if (iterator_rtype->kind == TYPE_MAP) {
        memory_map_t *map = iterator;
        uint64_t key_size = rt_rtype_heap_out_size(map->key_index);
        DEBUGF("[runtime.iterator_next_key] kind is map, len=%lu, key_base=%p, key_index=%lu, key_size=%lu",
               map->length, map->key_data, map->key_index, key_size);

        if (cursor >= map->length) {
            return -1;
        }

        memmove(key_ref, map->key_data + key_size * cursor, key_size);
        return cursor;
    }

    assertf(false, "cannot support iterator type=%d", iterator_rtype->kind);
    exit(0);
}


void iterator_value(void *iterator, uint64_t rtype_index, int64_t cursor, void *value_ref) {
    DEBUGF("[runtime.iterator_next_value] iterator base=%p,rtype_index=%lu, cursor=%lu",
           iterator, rtype_index, cursor);
    assertf(cursor != -1, "cannot iterator value");

    rtype_t *iterator_rtype = rt_find_rtype(rtype_index);

    if (iterator_rtype->kind == TYPE_LIST) {
        memory_list_t *list = iterator;
        DEBUGF("[runtime.iterator_key] kind is list, len=%lu, cap=%lu, data_base=%p", list->length, list->capacity,
               list->array_data);

        assertf(cursor < list->length, "[runtime.iterator_value] cursor=%d >= list->length=%d", cursor, list->length);

        uint64_t element_size = rt_rtype_heap_out_size(list->element_rtype_index);
        memmove(value_ref, list->array_data + element_size * cursor, element_size);
        return;
    }

    if (iterator_rtype->kind == TYPE_MAP) {
        memory_map_t *map = iterator;
        uint64_t value_size = rt_rtype_heap_out_size(map->value_index);
        DEBUGF("[runtime.iterator_key] kind is map, len=%lu, value_base=%p, value_index=%lu, value_size=%lu",
               map->length, map->value_data, map->value_index, value_size);

        assertf(cursor < map->length, "[runtime.iterator_value] cursor=%d >= map->length=%d", cursor, map->length);

        memmove(value_ref, map->value_data + value_size * cursor, value_size);
        return;
    }

    assertf(false, "cannot support iterator type=%d", iterator_rtype->kind);
    exit(0);
}


/**
 * dst offset 按字节记
 * @param dst
 * @param dst_offset
 * @param src
 * @param size
 */
void memory_move(byte *dst, uint64_t dst_offset, void *src, uint64_t src_offset, uint64_t size) {
    DEBUGF("[runtime.memory_move] dst=%p, dst_offset=%lu, src=%p, src_offset=%lu, size=%lu",
           dst, dst_offset, src, src_offset, size);
    memmove(dst + dst_offset, src + src_offset, size);
}