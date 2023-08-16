#include "interval.h"
#include "src/ssa.h"
#include "utils/stack.h"
#include "assert.h"
#include "src/debug/debug.h"


static bool interval_need_move(interval_t *from, interval_t *to) {
    if (from->assigned && to->assigned && from->assigned == to->assigned) {
        return false;
    }

    if (from->spilled && to->spilled && from->stack_slot == to->stack_slot) {
        return false;
    }

    return true;
}

/**
 * 深度优先(右侧优先) 遍历，并标记 loop index/header
 * 深度优先有前序和后续遍历，此时采取后续遍历的方式来为 loop header 标号，
 * 这样可以保证所有的子循环都标号完毕后才标号当前 block
 */
static void loop_header_detect(closure_t *c, basic_block_t *current, basic_block_t *parent) {
    // 探测到循环，current is loop header,parent is loop end
    // current 可能被多个 loop ends 进入,所以这里不能收集 loop headers
    if (current->loop.active) {
        assert(current->loop.visited);
        assert(parent);

        current->loop.header = true;
        parent->loop.end = true;

        parent->backward_succ = current;

        assert(parent->succs->count == 1 && parent->succs->take[0] == current && "critical edge must broken");

        slice_push(current->loop_ends, parent);
        slice_push(c->loop_ends, parent); // 一个 header 可能对应多个 end
        return;
    }

    // increment count of incoming forward branches parent -> current is forward
    current->incoming_forward_count += 1;
    if (parent) {
        slice_push(parent->forward_succs, current);
    }

    if (current->loop.visited) {
        return;
    }

    // num block++
    current->loop.visited = true;
    current->loop.active = true;

    for (int i = current->succs->count - 1; i >= 0; --i) {
        basic_block_t *succ = current->succs->take[i];
        loop_header_detect(c, succ, current);
    }

    current->loop.active = false;

    // 后序操作(此时 current 可能在某次 backward succ 中作为 loop header 打上了标记)
    // 深度优先遍历加上 visited 机制，保证一个节点只会被 iteration
    if (current->loop.header) {
        assert(current->loop.index == -1);
        // 所有的内循环已经处理完毕了，所以外循环的编号总是大于内循环
        current->loop.index = c->loop_count++;
        slice_push(c->loop_headers, current);
    }
}

/**
 * 遍历所有 loop ends，找到这个 loop 下的所有 block 即可。
 * 如果一个 block 被多个 loop 经过，则 block index_list 的 key 就是 loop_index, value 就是是否被改 loop 穿过
 */
static void loop_mark(closure_t *c) {
    linked_t *work_list = linked_new();

    for (int i = 0; i < c->loop_ends->count; ++i) {
        basic_block_t *end = c->loop_ends->take[i];

        assert(end->succs->count == 1 && "critical edge must broken");

        basic_block_t *header = end->succs->take[0];
        assert(header->loop.header);
        assert(header->loop.index >= 0);
        int8_t loop_index = header->loop.index;

        linked_push(work_list, end);
        end->loop.index_map[loop_index] = true;

        do {
            basic_block_t *current = linked_pop(work_list);

            assert(current->loop.index_map[loop_index]);

            if (current == header) {
                continue;
            }

            // end -> preds -> preds -> header 之间的所有 block 都属于当前 index
            for (int j = 0; j < current->preds->count; ++j) {
                basic_block_t *pred = current->preds->take[j];
                if (pred->loop.index_map[loop_index]) {
                    // 已经配置过了，直接跳过
                    continue;
                }

                linked_push(work_list, pred);
                pred->loop.index_map[loop_index] = true;
            }

        } while (!linked_empty(work_list));
    }
}

/**
 * 遍历所有 block 分配 index 和 depth
 * @param c
 */
static void loop_assign_depth(closure_t *c) {
    for (int i = 0; i < c->blocks->count; ++i) {
        basic_block_t *block = c->blocks->take[i];
        assert(block->loop.depth == 0);
        int max_depth = 0;
        int8_t min_index = -1;
        for (int j = c->loop_count - 1; j >= 0; --j) {
            if (!block->loop.index_map[j]) {
                continue;
            }

            // block 在 loop j 中
            max_depth++;
            min_index = j;
        }

        block->loop.index = min_index;
        block->loop.depth = max_depth;
    }
}

static void loop_detect(closure_t *c) {
    loop_header_detect(c, c->entry, NULL);
    loop_mark(c);
    loop_assign_depth(c);
}

static interval_t *interval_new_child(closure_t *c, interval_t *i) {
    interval_t *child = interval_new(c);
    interval_t *parent = i;
    if (parent->parent) {
        parent = parent->parent;
    }

    if (i->var) {
        lir_var_t *var = unique_var_operand(c->module, i->var->type, "child")->value;
        // 加入到 global 中
        slice_push(c->var_defs, var);

        child->var = var;
        table_set(c->interval_table, var->ident, child);
    } else {
        assert(parent->fixed);
        child->fixed = parent->fixed;
        child->assigned = parent->assigned;
        table_set(c->interval_table, alloc_regs[child->assigned]->name, child);
    }

    child->parent = parent;
    child->reg_hint = parent;
    child->alloc_type = parent->alloc_type;
    child->stack_slot = parent->stack_slot;

    return child;
}

static bool resolve_blocked(int8_t *block_regs, interval_t *from, interval_t *to) {
    if (to->spilled) {
        return false;
    }

    if (block_regs[to->assigned] == 0) {
        return false;
    }

    if (block_regs[to->assigned] == 1 && to->assigned == from->assigned) {
        return false;
    }

    return true;
}

/**
 * 同一个 id 可能需要插入多个值，此时按先后顺序插入
 * @param block
 * @param id
 * @param src_i
 * @param dst_i
 */
static void block_insert_mov(basic_block_t *block, int id, interval_t *src_i, interval_t *dst_i, bool imm_replace) {
    LINKED_FOR(block->operations) {
        lir_op_t *op = LINKED_VALUE();
        if (op->id <= id) {
            continue;
        }

        // last->id < id < op->id
        lir_operand_t *dst = operand_new(LIR_OPERAND_VAR, dst_i->var);
        lir_operand_t *src = operand_new(LIR_OPERAND_VAR, src_i->var);
        if (imm_replace) {
            var_replace(dst, dst_i);
            var_replace(src, src_i);
        }

        lir_op_t *mov_op = lir_op_move(dst, src);
        mov_op->id = id;
        linked_insert_before(block->operations, LINKED_NODE(), mov_op);

        if (block->first_op == LINKED_NODE()) {
            block->first_op = LINKED_NODE()->prev;
        }
        return;
    }
    assertf(false, "id=%v notfound in block=%s", id, block->name);
}

static void closure_insert_mov(closure_t *c, int insert_id, interval_t *src_i, interval_t *dst_i) {
    SLICE_FOR(c->blocks) {
        basic_block_t *block = SLICE_VALUE(c->blocks);
        lir_op_t *first = linked_first(block->operations)->value;
        lir_op_t *last = OP(block->last_op);
        if (first->id < insert_id && insert_id < last->id) {
            block_insert_mov(block, insert_id, src_i, dst_i, false);
            return;
        }
    }
    assertf(false, "closure=%s cannot find insert id=%d position", c->symbol_name, insert_id);
}

static interval_t *operand_interval(closure_t *c, lir_operand_t *operand) {
    if (operand->assert_type == LIR_OPERAND_VAR) {
        lir_var_t *var = operand->value;
        interval_t *interval = table_get(c->interval_table, var->ident);
        assert(interval);
        return interval;
    }
    if (operand->assert_type == LIR_OPERAND_REG) {
        reg_t *reg = covert_alloc_reg(operand->value);
        if (!reg->alloc_id) {
            return NULL;
        }

        interval_t *interval = table_get(c->interval_table, reg->name);
        assert(interval);
        return interval;
    }

    return NULL;
}

static bool in_range(interval_range_t *range, int position) {
    return range->from <= position && position < range->to;
}

// 大值在栈顶被优先处理 block_to_stack
static void block_to_depth_stack(ct_stack_t *work_list, basic_block_t *block) {
    // next->next->next
    stack_node *p = work_list->top; // top 指向栈中的下一个可用元素，总是为 NULL
    while (p->next != NULL && ((basic_block_t *) p->next->value)->loop.depth > block->loop.depth) {
        p = p->next;
    }

    // p->next == NULL 或者 p->next 小于等于 当前 block
    // block 插入到 p = 3 -> new = 2 ->  p->next = 2
    stack_node *next_node = p->next;
    // 初始化一个 node
    stack_node *new_node = stack_new_node(block);
    p->next = new_node;
    work_list->count++;

    if (next_node != NULL) {
        new_node->next = next_node;
    }
}

// 优秀的排序从而构造更短更好的 lifetime interval
// 权重越大排序越靠前
// 权重的本质是？或者说权重越大一个基本块？
void interval_block_order(closure_t *c) {
    loop_detect(c);

    slice_t *order_blocks = slice_new();
    ct_stack_t *work_list = stack_new();
    stack_push(work_list, c->entry);

    while (!stack_empty(work_list)) {
        basic_block_t *block = stack_pop(work_list);
        assertf(block && block->first_op, "block or block->first_op is null");

        slice_push(order_blocks, block);

        // 需要计算每一个块的正向前驱的数量
        for (int i = 0; i < block->forward_succs->count; ++i) {
            basic_block_t *succ = block->forward_succs->take[i];
            succ->incoming_forward_count--;
            // 如果一个块的正向进入的节点已经处理完毕了，则将当前块压入到栈中
            if (succ->incoming_forward_count == 0) {
                // sort into work_list by loop.depth, 权重越大越靠前，越先出栈
                block_to_depth_stack(work_list, succ);
            }
        }
    }

    c->blocks = order_blocks;
}

void interval_build(closure_t *c) {
    // new_interval for all physical registers
    for (int reg_id = 1; reg_id < cross_alloc_reg_count(); ++reg_id) {
        reg_t *reg = alloc_regs[reg_id];
        interval_t *interval = interval_new(c);
        interval->index = reg_id;
        interval->fixed = true;
        interval->assigned = reg_id;
        assertf(reg->flag & (FLAG(LIR_FLAG_ALLOC_FLOAT) | FLAG(LIR_FLAG_ALLOC_INT)), "reg must be alloc float or int");
        interval->alloc_type = reg->flag & FLAG(LIR_FLAG_ALLOC_FLOAT) ? LIR_FLAG_ALLOC_FLOAT : LIR_FLAG_ALLOC_INT;
        table_set(c->interval_table, reg->name, interval);
    }

    // new interval for all virtual registers in closure
    for (int i = 0; i < c->var_defs->count; ++i) {
        lir_var_t *var = c->var_defs->take[i];
        interval_t *interval = interval_new(c);
        interval->var = var;
        interval->alloc_type = type_kind_trans_alloc(var->type.kind);
        table_set(c->interval_table, var->ident, interval);
    }

    // 倒序遍历顺序基本块基本块
    for (int i = c->blocks->count - 1; i >= 0; --i) {
        basic_block_t *block = c->blocks->take[i];
        // 计算当前 block 中到 lives
        slice_t *lives = slice_new();
        table_t *live_table = table_new();

        // 1. calc lives in = union of successor.liveIn for each successor of b
        for (int j = 0; j < block->succs->count; ++j) {
            basic_block_t *succ = block->succs->take[j];
            for (int k = 0; k < succ->live->count; ++k) {
                lir_var_t *var = succ->live->take[k];
                // 同时添加到 table 和 lives 中
                live_add(live_table, lives, var);
            }
        }

        // 2. phi function phi of successors of b do
        for (int j = 0; j < block->succs->count; ++j) {
            basic_block_t *succ_block = block->succs->take[j];
            // first is label
            linked_node *current = linked_first(succ_block->operations)->succ;
            while (current->value != NULL && OP(current)->code == LIR_OPCODE_PHI) {
                lir_op_t *phi_op = OP(current);
                lir_var_t *var = ssa_phi_body_of(phi_op->first->value, succ_block->preds, block);

                live_add(live_table, lives, var);

                current = current->succ;
            }
        }


        int block_from = OP(linked_first(block->operations))->id;
        int block_to = OP(block->last_op)->id + 2; // +2 是为了让 interval lifetime 具有连续性，从而在 add range 时能够进行 merge

        // lives in add full range 遍历所有的 lives(union all succ, so it similar live_out),直接添加跨越整个块到间隔
        // 后续遇到 def 时会缩减长度， add_range 会对 range 进行合并,上面的 +2 是合并的基础
        for (int k = 0; k < lives->count; ++k) {
            lir_var_t *var = lives->take[k];
            interval_t *interval = table_get(c->interval_table, var->ident);
            interval_add_range(c, interval, block_from, block_to);
        }

        // 倒序遍历所有块指令添加 use and pos
        linked_node *current = linked_last(block->operations);
        while (current != NULL && current->value != NULL) {
            // 判断是否是 call op,是的话就截断所有物理寄存器
            lir_op_t *op = current->value;

            // phi hint
            if (op->code == LIR_OPCODE_PHI) {
                interval_t *def_interval = operand_interval(c, op->output);
                slice_t *phi_body = op->first->value;
                for (int j = 0; j < phi_body->count; ++j) {
                    lir_var_t *var = phi_body->take[j];
                    interval_t *hint_interval = operand_interval(c, operand_new(LIR_OPERAND_VAR, var));
                    assertf(hint_interval, "phi body var=%s not build interval", var->ident);
                    slice_push(def_interval->phi_hints, hint_interval);
                }
            }

            // TODO 内联优化将会提高寄存器分配的使用率
            // fixed all phy reg in call
            if (lir_op_call(op) || op->code == LIR_OPCODE_ENV_CLOSURE) {
                // traverse all register
                for (int j = 1; j < cross_alloc_reg_count(); ++j) {
                    reg_t *reg = alloc_regs[j];
                    interval_t *interval = table_get(c->interval_table, reg->name);
                    if (interval != NULL) {
                        interval_add_range(c, interval, op->id, op->id + 1);
                    }
                }
            }
            // add reg hint for move
            if (op->code == LIR_OPCODE_MOVE) {
                interval_t *hint_interval = operand_interval(c, op->first);
                interval_t *interval = operand_interval(c, op->output);
                if (hint_interval != NULL && interval != NULL) {
                    interval->reg_hint = hint_interval;
                }
            }

            // interval by output params, so it contain opcode phi
            // 可能存在变量定义却未使用的情况, 此时直接加 op->id, op->id_1 即可
            // ssa 完成后会拿一个 pass 进行不活跃的变量进行清除
            slice_t *def_operands = extract_op_operands(op,
                                                        FLAG(LIR_OPERAND_VAR) | FLAG(LIR_OPERAND_REG),
                                                        FLAG(LIR_FLAG_DEF), false);
            for (int j = 0; j < def_operands->count; ++j) {
                lir_operand_t *operand = def_operands->take[j];
                interval_t *interval = operand_interval(c, operand);
                if (!interval) {
                    continue;
                }

                // first range 为 null,表示仅定义，后续并未使用(倒叙遍历，所以应该先遇到使用)
                if (interval->first_range == NULL) {
                    interval_add_range(c, interval, op->id, op->id + 1);
                } else {
                    interval->first_range->from = op->id; // 范围缩小，同样适用于固定寄存器
                }

                if (!interval->fixed) {
                    assertf(operand->assert_type == LIR_OPERAND_VAR, "only var can be lives");
                    live_remove(live_table, lives, interval->var);
                    interval_add_use_pos(c, interval, op->id, cross_alloc_kind_of_def(c, op, operand->value));
                }
            }


            // phi body 中到 var 已经在上面通过 live 到形式补充了 range, 这里不需要重复操作了
            if (op->code != LIR_OPCODE_PHI) {
                slice_t *use_operands = extract_op_operands(op, FLAG(LIR_OPERAND_VAR) | FLAG(LIR_OPERAND_REG),
                                                            FLAG(LIR_FLAG_USE), false);
                for (int j = 0; j < use_operands->count; ++j) {
                    lir_operand_t *operand = use_operands->take[j];
                    interval_t *interval = operand_interval(c, operand);
                    if (!interval) {
                        continue;
                    }
                    interval_add_range(c, interval, block_from, op->id);

                    if (!interval->fixed) {
                        assertf(operand->assert_type == LIR_OPERAND_VAR, "only var can be lives");
                        // phi body 一定是在当前 block 的起始位置,上面已经进行了特殊处理 merge live，所以这里不需要添加 live
                        live_add(live_table, lives, interval->var);
                        interval_add_use_pos(c, interval, op->id, cross_alloc_kind_of_use(c, op, operand->value));
                    }
                }
            }

            current = current->prev;
        }

        // lives in 中不能包含 phi output
        current = linked_first(block->operations)->succ;
        while (current->value != NULL && OP(current)->code == LIR_OPCODE_PHI) {
            lir_op_t *op = OP(current);

            lir_var_t *var = op->output->value;
            live_remove(live_table, lives, var);
            current = current->succ;
        }

        /**
         * 由于采取了倒序遍历的方式，loop end 已经被遍历过了，但是 header 却还没有被处理。
         * 为 for 循环中的变量添加跨越整个声明周期的环境变量
         */
        if (block->loop.header) {
            for (int j = 0; j < block->loop_ends->count; ++j) {
                basic_block_t *end = block->loop_ends->take[j];
                for (int k = 0; k < lives->count; ++k) {
                    lir_var_t *var = lives->take[k];
                    interval_t *interval = table_get(c->interval_table, var->ident);
                    interval_add_range(c, interval, block_from, OP(end->last_op)->id + 2);
                }
            }
        }
        block->live = lives;
    }
}

interval_t *interval_new(closure_t *c) {
    interval_t *i = malloc(sizeof(interval_t));
    memset(i, 0, sizeof(interval_t));
    i->ranges = linked_new();
    i->use_pos_list = linked_new();
    i->children = linked_new();
    i->phi_hints = slice_new();
    i->stack_slot = NEW(int64_t);
    *i->stack_slot = 0;
    i->spilled = false;
    i->fixed = false;
    i->parent = NULL;
    i->index = c->interval_count++; // 基于 closure_t 做自增 id 即可
    return i;
}

bool range_covered(interval_range_t *range, int position, bool is_input) {
    if (is_input) {
        position -= 1;
    }

    // range to 不包含在 range 里面
    if (range->from <= position && position < range->to) {
        return true;
    }
    return false;
}

bool interval_expired(interval_t *i, int position, bool is_input) {
    if (is_input) {
        position -= 1;
    }

    int last_to = i->last_range->to; // interval < last_to
    // 由于 interval < last_to, 所以 position == last_to 时，interval 已经开始 expired 了
    return position >= last_to;
}


bool interval_covered(interval_t *i, int position, bool is_input) {
    if (i->ranges->count == 0) {
        return false;
    }
    if (position > i->last_range->to || position < i->first_range->from) {
        return false;
    }

    linked_node *current = linked_first(i->ranges);
    while (current->value != NULL) {
        interval_range_t *range = current->value;
        if (range_covered(range, position, is_input)) {
            return true;
        }

        current = current->succ;
    }
    return false;
}


bool interval_is_intersect(interval_t *current, interval_t *select) {
    assertf(select->ranges->count > 0, "select interval=%d not ranges, cannot calc intersection", select->index);
    assertf(select->last_range->to > current->first_range->from, "select interval=%d is expired", select->index);

    int position = current->first_range->from;
    while (position < current->last_range->to) {
        if (interval_covered(select, position, false)) {
            return true;
        }
        position++;
    }

    return false;
}

/**
 * 1. 如果重合则返回第一个重合的点
 * 2. 如果遍历到 current->last_to 都不重合则继续像后遍历，直到遇到第一被 select covert 位置
 * 3. 仅处于 active 或者 inactive 中到 interval 才会调用该函数
 * 两个 interval 的重合点
 * @param current
 * @param select
 * @return
 */
int interval_next_intersect(interval_t *current, interval_t *select) {
    assertf(select->ranges->count > 0, "select interval=%d not ranges, cannot calc intersection", select->index);
    assertf(select->last_range->to > current->first_range->from, "select interval=%d is expired", select->index);

    int position = current->first_range->from; // first_from 指向 range 的开头

    int64_t end = max(current->last_range->to, select->last_range->to);

    int select_first_cover = -1;
    // first covert select position
    while (position < end) {
        bool cover_current = interval_covered(current, position, false);
        bool cover_select = interval_covered(select, position, false);

        if (select_first_cover == -1 && cover_select) {
            select_first_cover = position;
        }

        if (cover_current && cover_select) {
            return position;
        }

        // 已经超过了 current 表示不可能存在交集，此时找到 select 在 current->last_to 之后都最后一个使用位置即可
        if (position > current->last_range->to && cover_select) {
            return position;
        }
        position++;
    }

    assertf(select_first_cover >= 0, "select range not found any intersection from=%d to=%d",
            current->first_range->from,
            end);
    // 此时应该返回 select 大于 current->first_range->from 的首个 cover select 的节点
    // 因为即使没有交集，该寄存器的最大空闲时间也是到这个节点
    // current ---              ----
    // select         ---
    return select_first_cover;
}

// 在 before 前挑选一个最佳的位置进行 split
int interval_find_optimal_split_pos(closure_t *c, interval_t *current, int before) {
    return before - 1;
}

/**
 * add range 总是基于 block_from ~ input.id,
 * 且从后往前遍历，所以只需要根据 to 和 first_from 比较，就能判断出是否重叠
 * @param c
 * @param i
 * @param from
 * @param to
 */
void interval_add_range(closure_t *c, interval_t *i, int from, int to) {
    assert(from <= to);

    if (linked_empty(i->ranges)) {
        interval_range_t *range = NEW(interval_range_t);
        range->from = from;
        range->to = to;
        linked_push(i->ranges, range);
        i->first_range = range;
        i->last_range = range;
        return;
    }

    if (i->first_range->from <= to) {
        // form 选小的， to 选大的
        if (from < i->first_range->from) {
            i->first_range->from = from;
        }
        if (to > i->first_range->to) {
            i->first_range->to = to;

            // to 可能跨越了多个 range
            linked_node *current = linked_first(i->ranges)->succ;
            while (current->value && ((interval_range_t *) current->value)->from <= to) {
                interval_range_t *current_interval = current->value;
                // current_interval->to 可能也小于 to, 所以这里不能随意覆盖
                if (current_interval->to > i->first_range->to) {
                    i->first_range->to = current_interval->to;
                }

                linked_remove(i->ranges, current);

                current = current->succ;
            }
            // 重新计算 last range
            i->last_range = linked_last(i->ranges)->value;
        }


    } else {
        // 新的 range 和 旧的 range 无交集
        // 不重叠,则 range 插入到 ranges 的最前面
        interval_range_t *range = NEW(interval_range_t);
        range->from = from;
        range->to = to;

        linked_insert_before(i->ranges, NULL, range);
        i->first_range = range;
        if (i->ranges->count == 1) {
            i->last_range = range;
        }
    }
}

/**
 * 按从小到大排序
 * @param c
 * @param i
 * @param position
 * @param kind
 */
void interval_add_use_pos(closure_t *c, interval_t *i, int position, alloc_kind_e kind) {
    linked_t *pos_list = i->use_pos_list;

    use_pos_t *new_pos = NEW(use_pos_t);
    new_pos->kind = kind;
    new_pos->value = position;
    if (linked_empty(pos_list)) {
        linked_push(pos_list, new_pos);
        return;
    }

    linked_node *current = linked_first(pos_list);
    while (current->value != NULL && ((use_pos_t *) current->value)->value < position) {
        current = current->succ;
    }

    linked_insert_before(pos_list, current, new_pos);
}


int interval_next_use_position(interval_t *i, int after_position) {
    linked_t *pos_list = i->use_pos_list;

    LINKED_FOR(pos_list) {
        use_pos_t *current_pos = LINKED_VALUE();
        if (current_pos->value > after_position) {
            return current_pos->value;
        }
    }
    return 0;
}

/**
 * 从 position 将 interval 分成两端，多个 child interval 在一个 list 中，而不是多级 list
 * 如果 position 被 range cover, 则对 range 进行切分
 * child 的 register_hint 指向 parent
 *
 * 需要判断 position 是否在 hole 位置，例如下面这种情况，产生了 hole (传统非 ssa 形式)
 * 10 x -> a
 * 12 a -> x
 * 14 a -> x
 * ...
 * 60 x -> a
 * 62 a -> x
 *
 * 所以如果 position 在 interval 的 hole(包含 to) 中, 那就不需要进行任何额外的 mov 操作。
 * ssa 中面临的则是 if 条件产生的 hole, 同样也不需要处理 mov, resolve_data_flow 会处理这种情况
 * @param c
 * @param i
 * @param position
 */
interval_t *interval_split_at(closure_t *c, interval_t *i, int position) {
    assert(position < i->last_range->to);
    assert(position >= i->first_range->from);

    interval_t *child = interval_new_child(c, i);

    // 将 child 加入 parent 的 children 中,
    interval_t *parent = i;
    if (parent->parent) {
        parent = parent->parent;
        // i 此时存在 parent,因为是从 i 中分割出来的，所以需要插入到 i 对应到 node 的后方
        if (linked_empty(parent->children)) {
            linked_push(parent->children, child);
        } else {
            LINKED_FOR(parent->children) {
                interval_t *current = LINKED_VALUE();
                if (current->index == i->index) {
                    linked_insert_after(parent->children, LINKED_NODE(), child);
                    break;
                }
            }
        }
    } else {
        // 如果 i 就是 parent?
        // child 从 i 中刚刚分割出来都，那么其 last->rang->to 一定会大于 exists children 中都任意值
        // 所以这里只需要将 child 插入到 parent->children 都最前面即可 都最前面
        linked_insert_before(parent->children, NULL, child);
    }


    bool need_mov = false;
    linked_t *left_ranges = linked_new();
    linked_t *right_ranges = linked_new();
    LINKED_FOR(i->ranges) {
        interval_range_t *range = LINKED_VALUE();

        // covered
        if (range_covered(range, position, false)) {
            need_mov = true;
        }

        if (range->to <= position) {
            linked_push(left_ranges, range);
        } else if (range->from >= position) {
            // 必定在右边
            linked_push(right_ranges, range);
        } else {
            // 新建的丢到右边
            interval_range_t *new_range = NEW(interval_range_t);
            new_range->from = position;
            new_range->to = range->to;
            linked_push(right_ranges, new_range);

            // from < position < to
            range->to = position; // 截短丢到左边
            linked_push(left_ranges, range);

        }
    }
    i->ranges = left_ranges;
    i->first_range = linked_first(i->ranges)->value;
    i->last_range = linked_last(i->ranges)->value;

    child->ranges = right_ranges;
    child->first_range = linked_first(child->ranges)->value;
    child->last_range = linked_last(child->ranges)->value;

    // 划分 position
    LINKED_FOR(i->use_pos_list) {
        use_pos_t *pos = LINKED_VALUE();
        if (pos->value < position) {
            continue;
        }

        // pos->value >= position, pos 和其之后的 pos 都需要加入到 new child 中
        child->use_pos_list = linked_split(i->use_pos_list, LINKED_NODE());
        break;
    }

    if (need_mov) {
        // mov id = position - 1
        // 此时 child 还没有确定是分配寄存器还是溢出
        closure_insert_mov(c, position, i, child);

    }

    return child;
}

/**
 * spill slot，清空 assign
 * 所有 slit_child 本质上是一个 var, 所以都溢出到同一个 stack_slot（存储在_canonical_spill_slot中）
 * @param i
 */
void interval_spill_slot(closure_t *c, interval_t *i) {
    assert(i->stack_slot);

    i->assigned = 0;
    i->spilled = true;
    if (*i->stack_slot != 0) { // 已经分配了 slot 了
        return;
    }

    // 由于栈是从高向低增长的，所以需要先预留 size
    int64_t size = type_kind_sizeof(i->var->type.kind);
    c->stack_offset += size;
    c->stack_offset = align_up(c->stack_offset, size);
    slice_push(c->stack_vars, i->var);

    *i->stack_slot = -c->stack_offset; // 取负数，一般栈都是高往低向下增长
}

/**
 * use_positions 是否包含 kind > 0 的position, 有则返回 use_position，否则返回 NULL
 * @param i
 * @return
 */
use_pos_t *interval_must_reg_pos(interval_t *i) {
    LINKED_FOR(i->use_pos_list) {
        use_pos_t *pos = LINKED_VALUE();
        if (pos->kind == ALLOC_KIND_MUST) {
            return pos;
        }
    }
    return NULL;
}

/**
 * @param i
 * @return
 */
use_pos_t *interval_must_stack_pos(interval_t *i) {
    LINKED_FOR(i->use_pos_list) {
        use_pos_t *pos = LINKED_VALUE();
        if (pos->kind == ALLOC_KIND_NOT) {
            return pos;
        }
    }
    return NULL;
}


/**
 * linear scan 中将 block 当成是线性块处理，但是实际上 block 是个图，resolve_data_flow 的作用就是以图的角度将缺失的 edge 进行关联
 * @param c
 */
void resolve_data_flow(closure_t *c) {
    SLICE_FOR(c->blocks) {
        basic_block_t *from = SLICE_VALUE(c->blocks);
        for (int i = 0; i < from->succs->count; ++i) {
            basic_block_t *to = from->succs->take[i];

            resolver_t r = {
                    .from_list = slice_new(),
                    .to_list = slice_new(),
                    .insert_block = NULL,
                    .insert_id = 0,
            };

            // to 入口活跃则可能存在对同一个变量在进入到当前块之前就已经存在了，所以可能会进行 spill/reload
            // for each interval it live at begin of successor do ? 怎么拿这样的 interval? 最简单办法是通过 live
            // live not contain phi def interval
            for (int j = 0; j < to->live->count; ++j) {
                lir_var_t *var = to->live->take[j];
                interval_t *parent_interval = table_get(c->interval_table, var->ident);
                assert(parent_interval);

                // 判断是否在 form->to edge 最终的 interval TODO last_op + 1?
                interval_t *from_interval = interval_child_at(parent_interval, OP(from->last_op)->id + 1, false);

                //  如果 live var interval 刚好是在 to->first_op 中到 use 作为声明周期到则需要特殊处理
                // from->last_op 不需要担心这个问题，其总是 branch op
                bool is_use = false;
                slice_t *vars = extract_var_operands(to->first_op->value, LIR_FLAG_USE | LIR_FLAG_DEF);
                for (int k = 0; k < vars->count; ++k) {
                    lir_var_t *temp_var = vars->take[k];
                    if (str_equal(temp_var->ident, var->ident) && temp_var->flag & FLAG(LIR_FLAG_USE)) {
                        is_use = true;
                    }
                }

                interval_t *to_interval = interval_child_at(parent_interval, OP(to->first_op)->id, is_use);
                // 因为 from 和 interval 是相连接的 edge,
                // 如果from_interval != to_interval(指针对比即可)
                // 则说明在其他 edge 上对 interval 进行了 spilt/reload
                // 因此需要 link from and to interval
                if (interval_need_move(from_interval, to_interval)) {
                    slice_push(r.from_list, from_interval);
                    slice_push(r.to_list, to_interval);
                }
            }

            // phi def interval(label op -> phi op -> ... -> phi op -> other op)
            linked_node *current = linked_first(to->operations)->succ;
            while (current->value != NULL && OP(current)->code == LIR_OPCODE_PHI) {
                lir_op_t *to_op = OP(current);
                //  to phi.inputOf(pred def) will is from interval
                // TODO ssa body constant handle
                lir_var_t *var = ssa_phi_body_of(to_op->first->value, to->preds, from);
                interval_t *form_parent_interval = table_get(c->interval_table, var->ident);
                assert(form_parent_interval);
                interval_t *from_interval = interval_child_at(form_parent_interval, OP(from->last_op)->id, false);

                lir_var_t *def = to_op->output->value; // result must assign reg
                interval_t *to_parent_interval = table_get(c->interval_table, def->ident);
                assert(to_parent_interval);
                interval_t *to_interval = interval_child_at(to_parent_interval, to_op->id, false);

                if (interval_need_move(from_interval, to_interval)) {
                    slice_push(r.from_list, from_interval);
                    slice_push(r.to_list, to_interval);
                }

                current = current->succ;
            }

            // 对一条边的处理完毕，可能涉及多个寄存器，stack, 也有可能一个寄存器被多次操作,所以需要处理覆盖问题
            // 同一条边上的所有 resolve 操作只插入在同一块中，要么是在 from、要么是在 to。 tips: 所有的 critical edge 都已经被处理了
            resolve_find_insert_pos(&r, from, to);

            // 按顺序插入 move
            resolve_mappings(c, &r);
        }
    }
}

/**
 * 如果是 input 则 使用 op_id < last_range->to+1
 * output 则不用
 * @param i
 * @param op_id
 * @return
 */
interval_t *interval_child_at(interval_t *i, int op_id, bool is_use) {
    assert(op_id >= 0 && "invalid op_id (method can not be called for spill moves)");

    if (linked_empty(i->children)) {
        return i;
    }

    int last_to_offset = is_use ? 1 : 0;

    if (i->first_range->from <= op_id && op_id < (i->last_range->to + last_to_offset)) {
        return i;
    }

    assertf(i->children->count > 0, "interval=%s not contains op_id=%d", i->var->ident, op_id);

    // i->var 在不同的指令处可能作为 input 也可能作为 output
    // 甚至在同一条指令处即作为 input，又作为 output， 比如 20: v1 + 1 -> v2
    LINKED_FOR(i->children) {
        interval_t *child = LINKED_VALUE();
        if (child->first_range->from <= op_id && op_id < (child->last_range->to + last_to_offset)) {
            return child;
        }
    }

    assert(false && "op_id not in interval");
}

/**
 * 由于 ssa resolve 的存在，所以存在从 interval A(stack A) 移动到 interval B(stackB)
 * 但是大多数寄存器不支持从 stack 移动到 stack，所以必须给 phi def 添加一个寄存器，也就是 use_pos kind = MUST
 *
 * 可能存在类似下面这样的冲突，此时无论先操作哪个移动指令都会造成 overwrite
 * mov rax -> rcx
 * mov rcx -> rax
 * 修改为
 * mov rax -> stack
 * mov stack -> rcx
 * mov rcx -> rax
 * @param c
 * @param r
 */
void resolve_mappings(closure_t *c, resolver_t *r) {
    if (r->from_list->count == 0) {
        return;
    }

    // block all from interval, value 表示被 input 引用的次数, 0 表示为被引用
    // 避免出现同一个寄存器的 output 覆盖 input
    int8_t block_regs[UINT8_MAX] = {0};
    // from 如果使用了寄存器，则需要 block 这些寄存器，避免被其他人覆盖
    SLICE_FOR(r->from_list) {
        interval_t *i = SLICE_VALUE(r->from_list);
        if (i->assigned) {
            block_regs[i->assigned] += 1;
        }
    }

    int spill_candidate = -1;
    while (r->from_list->count > 0) {
        bool processed = false;
        for (int i = 0; i < r->from_list->count; ++i) {
            interval_t *from = r->from_list->take[i];
            interval_t *to = r->to_list->take[i];
            assert(!(from->spilled && to->spilled) && "cannot move stack to stack");

            if (resolve_blocked(block_regs, from, to)) {
                // this interval cannot be processed now because target is not free
                // it starts in a register, so it is a possible candidate for spilling
                spill_candidate = i;
                continue;
            }

            block_insert_mov(r->insert_block, r->insert_id, from, to, true);

            if (from->assigned) {
                block_regs[from->assigned] -= 1;
            }

            slice_remove(r->from_list, i);
            slice_remove(r->to_list, i);

            processed = true;
        }

        // 已经卡死了，那进行再多次尝试也是没有意义的
        if (!processed) {
            assert(spill_candidate != -1 && "cannot resolve mappings");
            interval_t *from = r->from_list->take[spill_candidate];
            interval_t *spill_child = interval_new_child(c, from);
            interval_add_range(c, spill_child, 1, 2);
            interval_spill_slot(c, spill_child);

            // insert mov
            block_insert_mov(r->insert_block, r->insert_id, from, spill_child, true);

            // from update
            r->from_list->take[spill_candidate] = spill_child;

            // unlock interval reg
            block_regs[from->assigned] -= 1;
        }
    }

}

void resolve_find_insert_pos(resolver_t *r, basic_block_t *from, basic_block_t *to) {
    if (from->succs->count <= 1) {
        // from only one succ
        // insert before branch
        r->insert_block = from;

        lir_op_t *last_op = from->last_op->value;
        if (lir_op_branch(last_op)) {
            // insert before last op
            r->insert_id = last_op->id - 1;
        } else {
            r->insert_id = last_op->id + 1;
        }

    } else {
        r->insert_block = to;
        r->insert_id = OP(to->first_op)->id - 1;
    }
}

use_pos_t *first_use_pos(interval_t *i, alloc_kind_e kind) {
    if (i->use_pos_list->count == 0) {
        return NULL;
    }
//    assert(i->use_pos_list->count > 0);

    if (!kind) {
        return linked_first(i->use_pos_list)->value;
    }

    LINKED_FOR(i->use_pos_list) {
        use_pos_t *pos = LINKED_VALUE();
        if (pos->kind == kind) {
            return pos;
        }
    }

//    assert(false && "no use pos found");
    return NULL;
}