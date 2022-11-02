#include "allocate.h"
#include <assert.h>

/**
 * 根据 interval 列表 初始化 unhandled 列表
 * 采用链表结构是因为跟方便排序插入，有序遍历
 * @return
 */
static list *unhandled_new(closure_t *c) {
    list *unhandled = list_new();
    // 遍历所有变量,根据 interval from 进行排序
    for (int i = 0; i < c->globals->count; ++i) {
        interval_t *item = table_get(c->interval_table, ((lir_var_t *) c->globals->take[i])->ident);

        assert(item);

        sort_to_unhandled(unhandled, item);
    }

    return unhandled;
}

/**
 * 所有的 fixed interval 在初始化时加入到 inactive 中,后续计算相交时都是使用 inactive 计算的
 * @return
 */
static list *inactive_new(closure_t *c) {
    list *inactive = list_new();

    // 遍历所有固定寄存器生成 fixed_interval
    for (int i = 1; i < alloc_reg_count(); ++i) {
        reg_t *reg = alloc_regs[i];
        interval_t *item = table_get(c->interval_table, reg->name);
        assert(item && "physic reg interval not found");

        // 如果一个物理寄存器从未被使用过,就没有 ranges
        // 所以也不需要写入到 inactive 中进行处理
        if (item->first_range == NULL) {
            continue;
        }

        // free_pos = int_max
        list_push(inactive, item);
    }

    return inactive;
}

/**
 * @param operand
 * @param i
 */
static void var_replace(lir_operand_t *operand, interval_t *i) {
    lir_var_t *var = operand->value;
    if (i->spilled) {
        lir_stack_t *stack = NEW(lir_stack_t);
        stack->slot = *i->stack_slot;
        stack->size = type_base_sizeof(var->type_base);
        operand->type = LIR_OPERAND_STACK;
        operand->value = stack;
    } else {
        reg_t *reg = alloc_regs[i->assigned];
        uint8_t index = reg->index;
        reg = reg_select(index, var->type_base);
        assert(reg);

        operand->type = LIR_OPERAND_REG;
        operand->value = reg;
    }
}

/**
 * 在 free 中找到一个尽量空闲的寄存器分配给 current, 优先考虑 register hint 分配的寄存器
 * 如果相应的寄存器不能使用到 current 结束
 * @param current
 * @param free_pos
 * @return
 */
static uint8_t find_free_reg(interval_t *current, int *free_pos) {
    uint8_t min_full_reg_id = 0; // 直接分配不用 split
    uint8_t max_part_reg_id = 0; // 需要 split current to unhandled
    uint8_t hint_reg_id = 0; // register hint 对应的 interval 分配的 reg
    if (current->reg_hint != NULL && current->reg_hint->assigned > 0) {
        hint_reg_id = current->reg_hint->assigned;
    }

    for (int i = 1; i < alloc_reg_count(); ++i) {
        if (free_pos[i] > current->last_range->to) {
            // 如果有多个寄存器比较空闲，则优先考虑 hint
            // 否则优先考虑 free 时间最小的寄存器，从而可以充分利用寄存器
            if (min_full_reg_id == 0 || i == hint_reg_id ||
                (min_full_reg_id != hint_reg_id && free_pos[i] < free_pos[min_full_reg_id])) {
                min_full_reg_id = i;
            }
        } else if (free_pos[i] > current->first_range->from + 1) {
            // 如果有多个寄存器可以借给 current 使用一段时间，则优先考虑能够借用时间最长的寄存器(free[i] 最大的)
            if (max_part_reg_id == 0 || i == hint_reg_id ||
                (max_part_reg_id != hint_reg_id && free_pos[i] > free_pos[max_part_reg_id])) {
                max_part_reg_id = i;
            }
        }
    }

    if (min_full_reg_id > 0) {
        return min_full_reg_id;
    }

    if (max_part_reg_id > 0) {
        return max_part_reg_id;
    }

    return 0;
}

/**
 * 只要找到一个在 current 第一次使用时空闲的寄存器即可
 * @param current
 * @param free_pos
 * @return
 */
static uint8_t find_block_reg(interval_t *current, int *use_pos, int *block_pos) {
    uint8_t max_reg_id = 0; // 直接分配不用 split
    uint8_t hint_reg_id = 0; // register hint 对应的 interval 分配的 reg
    if (current->reg_hint != NULL && current->reg_hint->assigned > 0) {
        hint_reg_id = current->reg_hint->assigned;
    }

    for (int i = 1; i < alloc_reg_count(); ++i) {
        if (use_pos[i] <= current->first_range->from + 1) {
            continue;
        }

        if (max_reg_id == 0 || i == hint_reg_id ||
            (max_reg_id != hint_reg_id && use_pos[i] > use_pos[max_reg_id])) {
            max_reg_id = i;
        }
    }

    return max_reg_id;
}


static void handle_active(allocate_t *a) {
    // output position
    int position = a->current->first_range->from;
    list_node *current = a->active->front;
    while (current->value != NULL) {
        interval_t *select = (interval_t *) current->value;
        bool is_expired = interval_expired(select, position, false);
        bool is_covers = interval_covered(select, position, false);

        if (!is_covers || is_expired) {
            list_remove(a->active, current);

            if (is_expired) {
                list_push(a->handled, select);
            } else {
                list_push(a->inactive, select);
            }
        }

        current = current->succ;
    }
}

static void handle_inactive(allocate_t *a) {
    int position = a->current->first_range->from;
    list_node *current = a->inactive->front;
    while (current->value != NULL) {
        interval_t *select = (interval_t *) current->value;
        bool is_expired = interval_expired(select, position, false);
        bool is_covers = interval_covered(select, position, false);

        if (is_covers || is_expired) {
            list_remove(a->inactive, current);
            if (is_expired) {
                list_push(a->handled, select);
            } else {
                list_push(a->active, select);
            }
        }

        current = current->succ;
    }
}

static void set_pos(int *list, uint8_t index, int position) {
    if (list[index] != -1 && position > list[index]) {
        return;
    }

    list[index] = position;
}

void allocate_walk(closure_t *c) {
    allocate_t *a = malloc(sizeof(allocate_t));
    a->unhandled = unhandled_new(c);
    a->handled = list_new();
    a->active = list_new();
    a->inactive = inactive_new(c);

    while (a->unhandled->count != 0) {
        a->current = (interval_t *) list_pop(a->unhandled);

        // handle active
        handle_active(a);
        // handle inactive
        handle_inactive(a);

        // 尝试为 current 分配寄存器
        bool allocated = allocate_free_reg(c, a);
        if (allocated) {
            list_push(a->active, a->current);
            continue;
        }

        allocated = allocate_block_reg(c, a);
        if (allocated) {
            list_push(a->active, a->current);
            continue;
        }

        // 分不到寄存器，只能 spill 了， spill 的 interval 放到 handled 中，再也不会被 traverse 了
        list_push(a->handled, a->current);
    }
}

/**
 * 将 to 根据 interval 的 from 字段排序，值越小越靠前
 * i1 < i2 < i3 < to < i4 < i5
 * @param unhandled
 * @param to
 */
void sort_to_unhandled(list *unhandled, interval_t *to) {
    if (unhandled->count == 0) {
        list_push(unhandled, to);
        return;
    }

    list_node *current = list_first(unhandled);
    while (current->value != NULL && ((interval_t *) current->value)->first_range->from < to->first_range->from) {
        current = current->succ;
    }
    //  to < current, 将 to 插入到 current 前面
    list_insert_before(unhandled, current, to);
}

static uint8_t max_pos_index(const int list[UINT8_MAX]) {
    uint8_t max_index = 0;
    for (int i = 1; i < UINT8_MAX; ++i) {
        if (list[i] > list[max_index]) {
            max_index = i;
        }
    }

    return max_index;
}

/**
 * 由于不需要 spill 任何寄存器，所以不需要考虑 fixed interval
 * @param c
 * @param a
 * @return
 */
bool allocate_free_reg(closure_t *c, allocate_t *a) {
    int free_pos[UINT8_MAX];
    memset(free_pos, -1, sizeof(free_pos));

    for (int i = 1; i < alloc_reg_count(); ++i) {
        reg_t *reg = alloc_regs[i];
        if (a->current->alloc_type != reg->type) { // already set 0
            continue;
        }
        set_pos(free_pos, i, INT32_MAX);
    }

    // active(已经分配到了 reg) interval 不予分配，所以 pos 设置为 0
    LIST_FOR(a->active) {
        interval_t *select = LIST_VALUE();
        set_pos(free_pos, select->assigned, 0);
    }

    // ssa 表单中不会因为 redefine 产生 lifetime hole，只会由于 if-else block 产生少量的 hole
    LIST_FOR(a->inactive) {
        interval_t *select = LIST_VALUE();
        int pos = interval_next_intersection(a->current, select);
        if (pos == 0) {
            continue;
        }
        // potions 表示两个 interval 重合，重合点之前都是可以自由分配的区域
        set_pos(free_pos, select->assigned, pos);
    }


    // 找到空闲时间最长的寄存器,返回空闲直接最长的寄存器的 id
    uint8_t reg_id = find_free_reg(a->current, free_pos);
    if (!reg_id || free_pos[reg_id] == 0) {
        // 最长空闲的寄存器也不空闲
        return false;
    }

    a->current->assigned = reg_id;

    // 有空闲的寄存器，但是空闲时间不够,需要 split current
    if (free_pos[reg_id] < a->current->last_range->to) {
        int optimal_position = interval_find_optimal_split_pos(c, a->current, free_pos[reg_id]);

        // 从最佳位置切割 interval, 切割后的 interval 并不是一定会溢出，而是可能会再次被分配到寄存器(加入到 unhandled 中)
        interval_t *child = interval_split_at(c, a->current, optimal_position);
        sort_to_unhandled(a->unhandled, child);
    }

    return true;
}

/**

 * @param c
 * @param a
 * @return
 */
bool allocate_block_reg(closure_t *c, allocate_t *a) {
    // 用于判断寄存器的空闲时间
    // key is physical register index, value is position
    int use_pos[UINT8_MAX] = {0}; // use_pos 一定小于等于 block_pos
    int block_pos[UINT8_MAX] = {0}; // 设置 block pos 的同时需要隐式设置 use pos
    memset(use_pos, -1, sizeof(use_pos));
    memset(block_pos, -1, sizeof(block_pos));

    for (int reg_id = 1; reg_id < alloc_reg_count(); ++reg_id) {
        reg_t *reg = alloc_regs[reg_id];
        if (a->current->alloc_type != reg->type) {
            continue;
        }
        SET_BLOCK_POS(reg_id, INT32_MAX);
    }

    int first_from = a->current->first_range->from;

    // 遍历固定寄存器(active) TODO 固定间隔也进不来呀？
    LIST_FOR(a->active) {
        interval_t *select = LIST_VALUE();
        // 固定间隔本身就是 short range 了，但如果还在 current pos is active,so will set that block and use to 0
        if (select->fixed) {
            // 正在使用中的 fixed register,所有使用了该寄存器的 interval 都要让路
            SET_BLOCK_POS(select->assigned, 0);
        } else {
            // 找一个大于 current first use_position 的位置(可以为0，0 表示没找到)
            // 查找 active interval 的下一个使用位置,用来判断其空闲时长
            int pos = interval_next_use_position(select, first_from);
            SET_USE_POS(select->assigned, pos);
        }
    }

    // 遍历非固定寄存器(active intersect current)
    // 如果 lifetime hole 没有和 current intersect 在 allocate free 的时候已经用完了
    int pos;
    LIST_FOR(a->inactive) {
        interval_t *select = LIST_VALUE();
        pos = interval_next_intersection(a->current, select);
        if (pos == 0) {
            continue;
        }

        if (select->fixed) {
            // 该 interval 虽然是固定 interval(short range), 但是当前正处于 hole 中
            // 所以在 pos 之前的位置都是可以使用该 assigned 对应都寄存器
            // 但是一旦到了 pos 位置，current 必须 split at and spill child
            SET_BLOCK_POS(select->assigned, pos);
        } else {
            pos = interval_next_use_position(select, first_from);
            SET_USE_POS(select->assigned, pos);
        }
    }


    // max use pos 表示空闲时间最长的寄存器(必定会有一个大于 0 的值, 因为寄存器从 1 开始算的)
    uint8_t reg_id = find_block_reg(a->current, use_pos, block_pos);

    use_pos_t *first_use = first_use_pos(a->current, 0);
    if (!reg_id || use_pos[reg_id] < first_use->value) {
        // 此处最典型的应用就是 current 被 call fixed interval 阻塞，需要溢出自身

        //  a->current,range 为 4 ~ 18, 且 4 是 mov output first use pos, 所以必须占用一个寄存器, 此时是否会进入到 use_pos[reg_id] < first_use ？
        //  假设所有的寄存器都被 active interval 占用， use_pos 记录的是 first_use + 1(指令之间的间隔是 2) 之后的使用位置(还在 active 就表示至少还有一个使用位置)
        //  则不可能出现，所有的 use_pos min <  first_use + 1, 必定是 use pos min(下一条指令) > first_use + 1, 即使 ip = 6 指令是 call, 同样如此
        //  并不影响 id = 4 的位置拿一个寄存器来用。 call 指令并不会添加 use_pos, 只是添加了 range, 此时所有的物理寄存器被 block，
        if (first_use->kind == USE_KIND_MUST && first_use->value == first_from) {
            assert(false && "cannot spill and spilt current, use_pos collect exception");
        }

        //  active/inactive interval 的下一个 pos 都早于 current first use pos, 所以最好直接 spill 整个 current
        // assign spill slot to current
        spill_interval(c, a, a->current, 0);
    } else if (block_pos[reg_id] > a->current->last_range->to) {
        // 一般都会进入到这一条件中
        // reg_id 对应的寄存器的空闲时间 大于 current.first_use
        // 甚至大于 current->last_range->to，所以优先溢出 reg_id 对应都 interval
        // 所有和 current intersecting 的 interval 都需要在 current start 之前 split 并且 spill 到内存中
        // 当然，如果 child interval 存在 use pos 必须要加载 reg, 则需要二次 spilt into unhandled
        LIST_FOR(a->active) {
            interval_t *i = LIST_VALUE();
            if (i->assigned != reg_id) {
                continue;
            }
            // first_use 表示必须在 first_use 之前 spill, 否则会影响 current 使用 reg_id
            spill_interval(c, a, i, first_from);
        }
        LIST_FOR(a->inactive) {
            interval_t *i = LIST_VALUE();
            if (i->assigned != reg_id) {
                continue;
            }
            spill_interval(c, a, i, first_from);
        }

        // assign register reg to interval current
        a->current->assigned = reg_id;
        return true;
    } else {
        // 1. current.first_use < use_pos < current.last_use
        // 2. block_pos < current.last_use
        // 虽然 first use pos < reg_index 对应的 interval 的使用位置，但是
        // current start ~ end 之间被 block_pos 所截断，所以必须 split  current in block pos 之前, child in to unhandled list
        // split and spill interval active/inactive intervals for reg
        LIST_FOR(a->active) {
            interval_t *i = LIST_VALUE();
            if (i->assigned != reg_id) {
                continue;
            }
            spill_interval(c, a, i, first_from);
        }
        LIST_FOR(a->inactive) {
            interval_t *i = LIST_VALUE();
            if (i->assigned != reg_id) {
                continue;
            }
            spill_interval(c, a, i, first_from);
        }

        // assign register reg to interval current
        a->current->assigned = reg_id;

        // split current at block_pos
        int split_current_pos = interval_find_optimal_split_pos(c, a->current, block_pos[reg_id]);
        interval_t *child = interval_split_at(c, a->current, split_current_pos);
        sort_to_unhandled(a->unhandled, child);
        return true;
    }

    return false;
}

/**
 * 在 before_pos 之前需要切割出一个合适的位置，并 spilt spill 到内存中
 * 由于 spill 的部分后续不会在 handle，所以如果包含 use_position kind 则需要再次 spilt 丢出去
 * @param c
 * @param i
 * @param before_pos
 * @return
 */
void spill_interval(closure_t *c, allocate_t *a, interval_t *i, int before_pos) {
    int split_pos;
    interval_t *child = i;
    if (before_pos > 0) {
        // spill current before current first use position
        split_pos = interval_find_optimal_split_pos(c, i, before_pos);
        child = interval_split_at(c, i, split_pos);
    }

    use_pos_t *must_pos = interval_must_reg_pos(child);
    if (must_pos) {
        split_pos = interval_find_optimal_split_pos(c, child, must_pos->value);
        interval_t *unhandled = interval_split_at(c, child, split_pos);
        sort_to_unhandled(a->unhandled, unhandled);
    }

    // child to slot
    interval_spill_slot(c, child);
}

/**
 * 虚拟寄存器替换成 stack slot 和 physical register
 * @param c
 */
void replace_virtual_register(closure_t *c) {
    for (int i = 0; i < c->blocks->count; ++i) {
        basic_block_t *block = c->blocks->take[i];
        list_node *current = block->first_op;
        while (current->value != NULL) {
            lir_op_t *op = current->value;
            slice_t *vars = lir_op_nest_operands(op, FLAG(LIR_OPERAND_VAR));

            for (int j = 0; j < vars->count; ++j) {
                lir_operand_t *operand = vars->take[j];
                lir_var_t *var = operand->value;
                interval_t *parent = table_get(c->interval_table, var->ident);
                assert(parent);
                bool is_input = !(var->flag & FLAG(VAR_FLAG_OUTPUT));

                interval_t *interval = interval_child_at(parent, op->id, is_input);

                var_replace(operand, interval);
            }

            if (op->code == LIR_OPCODE_MOVE) {
//                if (lir_operand_equal(op->first, op->output)) {
//                    list_remove(block->operations, current);
//                }
            }

            current = current->succ;
        }

        // remove phi op
        current = list_first(block->operations)->succ;
        while (current->value != NULL && OP(current)->code == LIR_OPCODE_PHI) {
            list_remove(block->operations, current);

            current = current->succ;
        }
    }
}

