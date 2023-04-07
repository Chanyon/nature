#ifndef NATURE_SRC_AST_H_
#define NATURE_SRC_AST_H_

#include <stdlib.h>
#include "utils/value.h"
#include "utils/slice.h"
#include "utils/ct_list.h"
#include "utils/table.h"
#include "utils/type.h"

//#define AST_BASE_TYPE_FALSE "false"
//#define AST_BASE_TYPE_TRUE "true"
//#define AST_BASE_TYPE_NULL "null"
//#define AST_VAR "var"
//
//typedef enum {
//    AST_COMPLEX_TYPE_CLOSURE,
//    AST_COMPLEX_TYPE_ENV,
//} ast_complex_type;

typedef enum {
    AST_EXPR_LITERAL = 1, // 常数值 => 预计将存储在 data 段中
    AST_EXPR_BINARY,
    AST_EXPR_UNARY,
    AST_EXPR_IDENT,

    AST_EXPR_MAP_ACCESS,
    AST_EXPR_LIST_ACCESS,
    AST_EXPR_TUPLE_ACCESS,

    AST_EXPR_STRUCT_SELECT,
    AST_EXPR_LIST_SELECT, // [1, 2, 3].push(3)
    AST_EXPR_MAP_SELECT, // [1, 2, 3].push(3)
    AST_EXPR_SET_SELECT, // [1, 2, 3].push(3)

    AST_EXPR_ENV_ACCESS,

    AST_EXPR_LIST_NEW, // [1, 2, 3]
    AST_EXPR_MAP_NEW, // {"a": 1, "b": 2}
    AST_EXPR_SET_NEW, // {1, 2, 3, 4}
    AST_EXPR_TUPLE_NEW, // (1, 1.1, true)
    AST_EXPR_TUPLE_DESTR, // (var_a, var_b, (var_c, var_d))
    AST_EXPR_STRUCT_NEW, // person {a = 1; b = 2}
    AST_EXPR_CATCH, // catch call()


    AST_EXPR_STRUCT_DECL, // struct {int a = 1; int b = 2}
    // 抽象复合类型
    AST_EXPR_ACCESS,
    AST_EXPR_SELECT,
    AST_VAR_DECL,

    // stmt
    AST_STMT_IMPORT,
    AST_STMT_VAR_DEF, // todo 这里应该是叫 var def
    AST_STMT_VAR_TUPLE_DESTR,
    AST_STMT_ASSIGN,
    AST_STMT_RETURN,
    AST_STMT_IF,
    AST_STMT_THROW,
    AST_STMT_FOR_ITERATOR,
    AST_STMT_FOR_COND,
    AST_STMT_FOR_TRADITION,
    AST_STMT_TYPEDEF,
    AST_CALL,
    AST_FNDEF, // fn def (其包含 body)
    AST_CLOSURE_DEF, // closure def
} ast_type_t;

typedef enum {
    AST_EXPR_OPERATOR_ADD,
    AST_EXPR_OPERATOR_SUB,
    AST_EXPR_OPERATOR_MUL,
    AST_EXPR_OPERATOR_DIV,

    AST_EXPR_OPERATOR_LT,
    AST_EXPR_OPERATOR_LTE,
    AST_EXPR_OPERATOR_GT, // >
    AST_EXPR_OPERATOR_GTE,  // >=
    AST_EXPR_OPERATOR_EQ_EQ, // ==
    AST_EXPR_OPERATOR_NOT_EQ, // !=

    AST_EXPR_OPERATOR_NOT, // unary !right
    AST_EXPR_OPERATOR_NEG, // unary -right
    AST_EXPR_OPERATOR_IA, // *解引用
} ast_expr_operator_t;

static string ast_expr_op_str[] = {
        [AST_EXPR_OPERATOR_ADD] = "+",
        [AST_EXPR_OPERATOR_SUB] = "-",
        [AST_EXPR_OPERATOR_MUL] = "*",
        [AST_EXPR_OPERATOR_DIV] = "/",

        [AST_EXPR_OPERATOR_LT] = "<",
        [AST_EXPR_OPERATOR_LTE] = "<=",
        [AST_EXPR_OPERATOR_GT] = ">", // >
        [AST_EXPR_OPERATOR_GTE] = ">=",  // >=
        [AST_EXPR_OPERATOR_EQ_EQ] = "==", // ==
        [AST_EXPR_OPERATOR_NOT_EQ] = "!=", // !=

        [AST_EXPR_OPERATOR_NOT] = "!", // unary !right
        [AST_EXPR_OPERATOR_NEG] = "-", // unary -right
};

typedef struct {
    int line; // 行号
    ast_type_t assert_type; // 声明语句类型
    void *value;
} ast_stmt;

typedef struct {
    int line;
    ast_type_t assert_type; // 表达式断言
    typeuse_t type; // 表达式自身的类型
    typeuse_t target_type; // 表达式赋值的目标的 type
    void *value;
} ast_expr;

typedef struct {
    size_t count; // 当前数量, unsigned int
    size_t capacity; // 容量
    ast_stmt *list;
} ast_block_stmt;

typedef struct {
    string literal;
} ast_ident;

// 一元表达式
typedef struct {
    ast_expr_operator_t operator; // 取反，取绝对值, 解引用等
    ast_expr operand; // 操作对象
} ast_unary_expr;

// 二元表达式
typedef struct {
    ast_expr_operator_t operator; // +/-/*// 等 二元表达式
    ast_expr right;
    ast_expr left;
} ast_binary_expr;

// 调用函数
typedef struct {
    ast_expr left;
    list_t *actual_params;// ast_expr
    bool spread_param;
} ast_call;

// 值类型
typedef struct {
    type_kind kind;
    char *value;
} ast_literal; // 标量值

// (xx, xx, xx)
typedef struct {
    // var 中, ast_expr 的 type 是  ast_var_decl 和 ast_tuple_destr
    // assign 中 (a, b, (c.e, d[0])) = (1, 2) ast_expr 可能是所有 expr 类型，包括 ast_tuple_destr 自身
    list_t *elements;  // ast_expr
} ast_tuple_destr;

typedef struct {
    ast_expr left; // a  或 foo.bar.car 或者 d[0] 或者 (xx, xx, xx)
    ast_expr right;
} ast_assign_stmt;

// 仅仅包含了声明
// int a;
typedef struct {
    string ident;
    typeuse_t type; // type 已经决定了 size
} ast_var_decl;

// 包含了声明与赋值，所以统称为定义
typedef struct {
    ast_var_decl var_decl; // 左值
    ast_expr right; // 右值
} ast_vardef_stmt;

// 基于 tuple 解构语法的变量快速赋值
// var (a, b, (c, d)) = (1, 2)
// 通过 tuple destruct 快速定义变量
typedef struct {
    ast_tuple_destr *tuple_destr;
    ast_expr right;
} ast_var_tuple_def_stmt;

typedef struct {
    ast_expr condition;
    slice_t *consequent; // ast_stmt
    slice_t *alternate;
} ast_if_stmt;

/**
 * for (true) {}
 */
typedef struct {
    ast_expr condition;
    slice_t *body;
} ast_for_cond_stmt;

/**
 * throw "not found"
 */
typedef struct {
    ast_expr error;
} ast_throw_stmt;


/**
 * var (res, error) = catch call()
 */
typedef struct {
    ast_call *call; // 必须接上 call
} ast_catch;

/**
 * for (int i = 0; i < 100; i++) {}
 */
typedef struct {
    ast_stmt *init;
    ast_expr cond;
    ast_stmt *update;
    slice_t *body;
} ast_for_tradition_stmt;

/**
 * for (key,value in list)
 * for (key in list)
 */
typedef struct {
    ast_expr iterate; // list, foo.list, bar[0]
    ast_var_decl key; // 类型推导, type 可能是 int 或者 string
    ast_var_decl *value; // value 可选，可能为 null
    slice_t *body;
} ast_for_iterator_stmt;

typedef struct {
    ast_expr *expr;
} ast_return_stmt;

// import "module_path" module_name alias
typedef struct {
    string path; // import "xxx" 的 xxx 部分
    string as; // import "foo/bar" as xxx 的 xxx 部分  代码中使用都是基于这个 as 的，没有就使用 bra 作为 as

    // 计算得出
    string full_path; // 绝对完整的文件路径
    string module_ident; // 在符号表中的名称前缀,基于 full_path 计算出来
} ast_import;

/**
 * 虽然共用了 ast_struct_property, 但是使用的字段是不同的，
 * 使用了 key, value
 * people {
 *    a = 1,
 *    b = 2,
 *    c = 3
 * }
 */
typedef struct {
    // parser 阶段是 typedef ident
    // infer 完成后是 typeuse_struct
    typeuse_t type;

    list_t *properties; // struct_property_t
} ast_struct_new_t;

// 1. a.b
// 2. a.c.b
// 3. a[1].b
// 4. a().b
// ...
// a_233().b 左值可以编译成
// call a_233;
// type.b = 12; ??
// module_name.test
typedef struct {
    ast_expr left; // left is struct
    string key;
} ast_select;

typedef struct {
    ast_expr left;
    string key;
    struct_property_t *property; // 冗余方便计算
} ast_instance_select_t;


typedef struct {
    ast_expr left;
    string key;
} ast_list_select_t;

typedef struct {
    ast_expr left;
    string key;
} ast_map_select_t;

typedef struct {
    ast_expr left;
    string key;
} ast_set_select_t;

/**
 * 如何确定 left_type?
 * optimize 表达式阶段生成该值，不行也要行！
 */
typedef struct {
    typeuse_t element_type; // 访问的 value 的类型
    ast_expr left;
    ast_expr index;
} ast_list_access_t;

typedef struct {
    typeuse_t element_type; // index 对应的 value 的 type
    ast_expr left;
    uint64_t index;
} ast_tuple_access_t;

typedef struct {
    typeuse_t key_type;
    typeuse_t value_type;

    ast_expr left;
    ast_expr key;
} ast_map_access_t;

typedef struct {
    ast_expr left;
    ast_expr key;
} ast_access; // foo.bar[key()], bar[]

// [1,a.b, call()]
typedef struct {
    list_t *values; // ast_expr
    typeuse_t type; // list的类型 (类型推导截断冗余)
} ast_list_new;

typedef struct {
    ast_expr key;
    ast_expr value;
} ast_map_element;

// {key: value}
typedef struct {
    list_t *elements; // ast_map_element
} ast_map_new;

// var s = {1, 2, 3, call(), xxx}
typedef struct {
    list_t *keys; // 值为 ast_expr
} ast_set_new;

// var s = (1, 2, 2.14, 1.15, true)
typedef struct {
    list_t *elements; // 值为 ast_expr
} ast_tuple_new;

typedef struct {
    char *fn_name;
    uint8_t index;
    char *unique_ident;
} ast_env_access;

//typedef struct {
//  string as;
//  // struct items
//} ast_struct_stmt;

/**
 * type my_int = int
 * type my_string =  string
 * type my_my_string =  my_string
 * type my = struct {}
 */
typedef struct {
    string ident; // my_int (自定义的类型名称)
    typeuse_t type; // int (类型)
} ast_typedef_stmt;

// 这里包含 body, 所以属于 def
typedef struct ast_fndef_t {
    char *name;
    typeuse_t return_type;
    list_t *formals; // ast_var_decl
    bool rest_param;
    slice_t *body; // ast_stmt* 函数体
    void *closure; // 全局 closure 冗余

    // ast_expr, 由 parent closure 编译当前 fndef 时负责写入
    list_t *parent_view_envs;

    // analysis stage, 当 fn 定义在 struct 中,用于记录 struct type
    typeuse_t *self_struct;

    struct ast_fndef_t *parent;
} ast_fndef_t; // 既可以是 expression,也可以是 stmt

typedef struct {
//    slice_t *env_list; // ast_expr*
    list_t *env_list; // ast_expr

    string env_name; // 唯一标识，可以全局定位
    ast_fndef_t *fn;
} ast_closure_t;

//ast_block_stmt ast_new_block_stmt();

ast_ident *ast_new_ident(string literal);

typeuse_t select_actual_param(ast_call *call, uint8_t index);

typeuse_t select_formal_param(type_fn_t *formal_fn, uint8_t index);

bool type_compare(typeuse_t left, typeuse_t right);

bool can_assign(ast_type_t t);

#endif //NATURE_SRC_AST_H_
