#ifndef NATURE_STRUCTS_H
#define NATURE_STRUCTS_H

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "utils/linked.h"
#include "utils/slice.h"
#include "src/symbol/symbol.h"

typedef uint64_t flag_t;

typedef enum {
    VR_FLAG_FIRST = 1,
    VR_FLAG_SECOND,
    VR_FLAG_OUTPUT,
    VR_FLAG_ALLOC_INT,
    VR_FLAG_ALLOC_FLOAT,
    VR_FLAG_USE,
    VR_FLAG_DEF,
    VR_FLAG_INDIRECT_ADDR_BASE,
} vr_flag_t;


typedef enum {
    MODULE_TYPE_MAIN = 1,
    MODULE_TYPE_IMPORTED = 2,
    MODULE_TYPE_BUILTIN = 3
} module_type_t;

typedef struct {
    char *source;
    char *current;
    char *guard;
    int length;
    int line; // 当前所在代码行，用于代码报错提示

    bool has_newline;
    char space_prev;
    char space_next;
} scanner_cursor_t;

typedef struct {
    bool has;
    char *message;
} scanner_error_t;


typedef struct {
    linked_node *current;
} parser_cursor_t;

typedef struct {
    symbol_type type;
    void *decl; // ast_var_decl,ast_type_decl_stmt,ast_new_fn
    string ident; // 原始名称
    string unique_ident; // 唯一名称
    int scope_depth;
    bool is_capture; // 是否被捕获(是否被下级引用)
} local_ident_t;

/**
 * free_var 是在 parent function 作用域中被使用,但是被捕获存放在了 current function free_vars 中,
 * 所以这里的 is_local 指的是在 parent 中的位置
 * 如果 is_local 为 true 则 index 为 parent.locals[index]
 * 如果 is_local 为 false 则 index 为参数 env[index]
 */
typedef struct {
    bool is_local;
    int env_index; // env_index
    string ident;
    int index; // free in frees index
} free_ident_t;

typedef struct local_scope_t {
    struct local_scope_t *parent;
    slice_t *idents; // local_ident_t*

    uint8_t scope_depth;
} local_scope_t;

// 函数定义在当前作用域仅加载 fn decl
// 函数体的解析则延迟到当前作用域内的所有标识符都定义好后再做
// 从而能够支持，fn def 中引用 fn def 之后定义的符号(golang 不支持，python 支持)
typedef struct {
    // 由于需要延迟处理，所以缓存函数定义时的 scope，在处理时进行还原。
    local_scope_t *scope;
    ast_fndef_t *fndef;
    bool is_stmt;
} delay_fndef_t;

/**
 * 词法作用域
 */
typedef struct analysis_fndef_t {
    struct analysis_fndef_t *parent;

    local_scope_t *current_scope;

    // 使用了当前函数作用域之外的变量
    slice_t *frees; // analysis_free_ident_t*
    table_t *free_table; // analysis_free_ident_t*

    // 当前函数内的块作用域深度(基于当前函数,所以初始值为 0, 用于块作用域判定)
    uint8_t scope_depth;

    char *fn_name;
    // 函数定义在当前作用域仅加载 function as
    // 函数体的解析则延迟到当前作用域内的所有标识符都定义明确好
    list_t *delay_fndefs;
} analysis_fndef_t;


/**
 * 可以理解为文件维度数据
 * path 基于 import 编译， import 能提供完整的 full_path 以及 module_name
 * Target district
 */
typedef struct {
    char *source; // 文件内容
    char *source_path; // 文件完整路径(外面丢进来的)
    char *source_dir; // 文件所在目录,去掉 xxx.n
//    string namespace; // is dir, 从 base_ns 算起的 source_dir
    string ident; // 符号表中都使用这个前缀 /code/nature/foo/bar.n => unique_name: nature/foo/bar

//    bool entry; // 入口
    module_type_t type;

    scanner_cursor_t s_cursor;
    scanner_error_t s_error;
    linked_t *token_list; // scanner 结果

    int var_unique_count; // 同一个 module 下到所有变量都会通过该 ident 附加唯一标识

    parser_cursor_t p_cursor;
    slice_t *stmt_list;

    // analysis
    analysis_fndef_t *analysis_current;
    int analysis_line;

    // infer
    ast_fndef_t *infer_current; // 当前正在 infer 都 fn, return 时需要基于改值判断 return type
    int infer_line;
    // infer 第一步就会将所有的 typedef ident 的右值进行 reduction, 完成之后将会在这里打上标记
    bool reduction_typedef;

    // compiler
    struct closure_t *compiler_current;
    int compiler_line;

    // call init stmt
    ast_stmt *call_init_stmt;  // analysis 阶段写入

    // TODO asm_global_symbols to init closures? 这样就只需要 compiler closures 就行了
    // 分析阶段(包括 closure_t 构建,全局符号表构建), 根据是否为 main 生成 import/symbol/asm_global_symbols(symbol)/closure_decls
    slice_t *imports; // import_t, 图遍历 imports
    table_t *import_table; // 使用处做符号改写使用

    // 对外全局符号 -> 三种类型 var/fn/type_decl
    slice_t *global_symbols; // symbol_t, 这里只存储全局符号

    // ast_fndef
    slice_t *ast_fndefs;

    // closure_t
    slice_t *closures; // 包含 lir, 无论是 local 还是 global 都会在这里进行注册

    // native -> opcodes
    int asm_temp_var_decl_count;
    slice_t *asm_operations; // 和架构相关
    slice_t *asm_global_symbols; // 和架构无关

    // elf target.o
    uint64_t elf_count;
    uint8_t *elf_binary;
    string object_file;
} module_t;

/**
 * 遍历期间，block 第一次被访问时打上 visited 标识
 * 当 block 的所有 succs 被遍历后，该块同时被标记为 active
 * visited 在 iteration 期间不会被清除,而 active 标志则会在处理完所有后继后清除？
 */
typedef struct {
    bool visited;
    bool active;
    bool header;
    bool end;
    bool index_map[INT8_MAX]; // 默认都是 false
    int8_t index; // 默认值为 -1， 标识不在循环中 block maybe in multi loops，index is unique number in innermost(最深的) loop
    uint8_t depth; // block 的嵌套级别,数字越高嵌套的越深
} loop_t;

typedef struct basic_block_t {
    uint8_t id; // label 标号, 基本块编号(可以方便用于数组索引)， 和 op_label 还是要稍微区分一下,
    string name;

    // op pointer
//    linked_node *phi; // fist_node 即可
    linked_node *first_op; // 真正的指令开始,在插入 phi 和 label 之前的指令开始位置
    linked_node *last_op; // last_node 即可
    linked_t *operations;

    slice_t *preds;
    slice_t *succs;
    slice_t *forward_succs; // 当前块正向的 succ 列表
    struct basic_block_t *backward_succ; // loop end
    uint8_t incoming_forward_count; // 正向进入到该节点的节点数量
    slice_t *loop_ends; // 仅 loop header 有这个值

    slice_t *use;
    slice_t *def;
    slice_t *live_out;
    slice_t *live_in; // ssa 阶段计算的精确 live in 一个变量如果在当前块被使用，或者再当前块的后继块中被使用，则其属于入口活跃
    slice_t *live; // reg alloc 阶段计算
    slice_t *dom; // 当前块被哪些基本块支配
    slice_t *df;
    slice_t *be_idom; // 哪些块已当前块作为最近支配块,其组成了支配者树
    struct basic_block_t *idom; // 当前块的最近支配者

    // loop detection
    loop_t loop;
} basic_block_t;

/**
 * 1. cfg 需要专门构造一个结尾 basic block 么，用来处理函数返回值等？其一定位于 blocks[count - 1]
 * 形参有一条专门的指令 lir_formal_param 编译这条指令的时候处理形参即可
 * lir_formal_param 在寄存器分配阶段已经分配了合适的 stack or reg, 所以依次遍历处理即可
 * 如果函数的返回值大于 8 个字节，则需要引用传递返回, ABI 规定形参 1 为引用返回地址
 * 假如形参和所有局部变量占用的总长为 N Byte, 那么 -n(rbp) 处存储的就是最后一个形参的位置(向上存储)
 * 所以还是需要 closure_t 增加一个字段记录大值地址响应值, 从而可以正常返回
 *
 * 2. closure_t 可以能定义在文件中的全局函数，也可能是定义在结构体中的局部函数，在类型推导阶段是有能力识别到函数的左值
 * 是一个变量，还是结构体的元素
 */
typedef struct closure_t {
    slice_t *globals; // closure_t 中定义的变量列表, 用于 ssa 构建, 以及寄存器分配时的 interval 也是基于次
    slice_t *blocks; // 根据解析顺序得到

    basic_block_t *entry; // 基本块入口, 指向 blocks[0]
    table_t *interval_table; // key 包括 fixed register as 和 variable.ident
    int interval_count; // 虚拟寄存器的偏移量 从 40 开始算，在这之前都是物理寄存器

    // 定义环境
    string name;
    string end_label; // 结束地址
    linked_t *operations; // 指令列表

    int64_t stack_size; // 初始值为 0，像下增长，用于寄存器分配时的栈区 slot 分配
    slice_t *stack_vars; // 与栈增长顺序一致,随着栈的增长而填入, 其存储的值为 *lir_var_t

    // loop collect
    int8_t loop_count;
    slice_t *loop_headers;
    slice_t *loop_ends;

    // refer module
    uint64_t text_count; // asm_operations 编译完成后占用的 count
    slice_t *asm_operations; // 和架构相关, 首个 opcode 一定是 label
    slice_t *asm_symbols;
    module_t *module;
} closure_t;

#endif //NATURE_STRUCTS_H
