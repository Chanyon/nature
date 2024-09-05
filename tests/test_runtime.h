#ifndef NATURE_TEST_RUNTIME_H
#define NATURE_TEST_RUNTIME_H

#include "runtime/memory.h"
#include "runtime/runtime.h"

addr_t rt_fn_main_base;

uint64_t rt_symdef_count;
symdef_t rt_symdef_data;

uint64_t rt_fndef_count;
fndef_t rt_fndef_data;// 仅需要修复一下 gc_bits 数据即可

uint64_t rt_rtype_count;
rtype_t rt_rtype_data;

void user_main() {
    printf("hello world in _main\n");
}

#endif//NATURE_TEST_RUNTIME_H
