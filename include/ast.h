/*
 * ast.h — Abstract Syntax Tree node definitions for Mons Lang
 *
 * Design notes:
 *   - All nodes share a common AstNode header (type tag + source location).
 *   - The node "body" is accessed via a tagged union; use the AS_* macros.
 *   - Strings are interned: const char* fields point into a string table,
 *     never heap-allocated separately.
 *   - Lists (params, arms, fields, …) use a simple AstList singly-linked
 *     chain; the parser always appends in order.
 */

#ifndef MONS_AST_H
#define MONS_AST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Source location ────────────────────────────────────────── */

typedef struct {
    const char *file;
    uint32_t    line;
    uint32_t    col;
} SrcLoc;

/* ── Generic singly-linked list node ────────────────────────── */

typedef struct AstList {
    struct AstNode  *item;
    struct AstList  *next;
} AstList;

/* helper: count items in an AstList */
static inline size_t ast_list_len(const AstList *l) {
    size_t n = 0;
    for (; l; l = l->next) n++;
    return n;
}

/* ── Node type tags ─────────────────────────────────────────── */

typedef enum {
    /* ── Declarations ── */
    NODE_PROGRAM,
    NODE_FN_DECL,
    NODE_PARAM,
    NODE_STRUCT_DECL,
    NODE_STRUCT_FIELD,
    NODE_TRAIT_DECL,
    NODE_TRAIT_FN_SIG,       /* required (abstract) method */
    NODE_IMPL_DECL,
    NODE_MACRO_DECL,
    NODE_USE_DECL,
    NODE_CONST_DECL,
    NODE_GENERIC_PARAM,

    /* ── Statements ── */
    NODE_BLOCK,
    NODE_LET,
    NODE_RETURN,
    NODE_EXPR_STMT,
    NODE_FOR,
    NODE_TRY,
    NODE_CATCH_CLAUSE,

    /* ── Expressions ── */
    NODE_BINARY,
    NODE_UNARY,
    NODE_ASSIGN,
    NODE_IF,
    NODE_MATCH,
    NODE_MATCH_ARM,
    NODE_CALL,
    NODE_METHOD_CALL,
    NODE_FIELD_ACCESS,
    NODE_INDEX,
    NODE_LAMBDA,
    NODE_STRUCT_INIT,
    NODE_FIELD_INIT,
    NODE_ARRAY,
    NODE_TUPLE,
    NODE_MACRO_CALL,
    NODE_IDENT,

    /* ── Literals ── */
    NODE_LIT_INT,
    NODE_LIT_FLOAT,
    NODE_LIT_DOUBLE,
    NODE_LIT_BOOL,
    NODE_LIT_STRING,
    NODE_LIT_NONE,

    /* ── Types ── */
    NODE_TYPE_PRIMITIVE,
    NODE_TYPE_NAMED,
    NODE_TYPE_ARRAY,
    NODE_TYPE_FN,
    NODE_TYPE_OPTION,
    NODE_TYPE_RESULT,
    NODE_TYPE_TUPLE,
    NODE_TYPE_REF,

    /* ── Patterns ── */
    NODE_PAT_WILDCARD,
    NODE_PAT_LITERAL,
    NODE_PAT_BIND,
    NODE_PAT_ENUM,
    NODE_PAT_STRUCT,
    NODE_PAT_TUPLE,
    NODE_PAT_ARRAY,
    NODE_PAT_OR,

    NODE_KIND_COUNT   /* sentinel — always last */
} NodeKind;

/* ── Operator enums ─────────────────────────────────────────── */

typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_EQ,  BINOP_NEQ, BINOP_LT,  BINOP_GT,  BINOP_LTE, BINOP_GTE,
    BINOP_AND, BINOP_OR
} BinOp;

typedef enum {
    UNOP_NEG,   /* - */
    UNOP_NOT    /* ! */
} UnOp;

/* ── Primitive type tag ─────────────────────────────────────── */

typedef enum {
    PRIM_INT, PRIM_FLOAT, PRIM_DOUBLE, PRIM_BOOL, PRIM_STRING
} PrimKind;

/* After typecheck: guides bytecode for numeric/bool primitives (unset otherwise). */
typedef enum {
    AST_BC_TY_NONE = 0,
    AST_BC_TY_INT,
    AST_BC_TY_FLOAT,
    AST_BC_TY_DOUBLE,
    AST_BC_TY_BOOL
} AstBcTypeTag;

/* ════════════════════════════════════════════════════════════════
 *  Central AstNode
 * ════════════════════════════════════════════════════════════════ */

typedef struct AstNode {
    NodeKind kind;
    SrcLoc   loc;
    AstBcTypeTag bc_ty;

    union {

        /* ── NODE_PROGRAM ─────────────────────────────────────── */
        struct {
            AstList *decls;          /* list of top-level decls */
        } program;

        /* ── NODE_FN_DECL ─────────────────────────────────────── */
        struct {
            const char *name;
            AstList    *generic_params; /* AstNode NODE_GENERIC_PARAM */
            AstList    *params;         /* AstNode NODE_PARAM         */
            struct AstNode *ret_type;   /* NULL → inferred / void     */
            struct AstNode *body;       /* NODE_BLOCK                 */
            bool        is_pub;
        } fn_decl;

        /* ── NODE_PARAM ───────────────────────────────────────── */
        struct {
            const char     *name;
            struct AstNode *type;   /* may be NULL → inferred */
            bool            is_mut;
            bool            is_self; /* true for the 'self' receiver */
        } param;

        /* ── NODE_GENERIC_PARAM ───────────────────────────────── */
        struct {
            const char *name;
            AstList    *bounds;   /* list of NODE_IDENT (trait names) */
        } generic_param;

        /* ── NODE_STRUCT_DECL ─────────────────────────────────── */
        struct {
            const char *name;
            AstList    *generic_params;
            AstList    *fields;   /* list of NODE_STRUCT_FIELD */
            bool        is_pub;
        } struct_decl;

        /* ── NODE_STRUCT_FIELD ────────────────────────────────── */
        struct {
            const char     *name;
            struct AstNode *type;
            bool            is_pub;
            bool            is_mut;
        } struct_field;

        /* ── NODE_TRAIT_DECL ──────────────────────────────────── */
        struct {
            const char *name;
            AstList    *generic_params;
            AstList    *super_traits;  /* list of NODE_IDENT */
            AstList    *items;         /* NODE_TRAIT_FN_SIG or NODE_FN_DECL */
            bool        is_pub;
        } trait_decl;

        /* ── NODE_TRAIT_FN_SIG ────────────────────────────────── */
        struct {
            const char     *name;
            AstList        *generic_params;
            AstList        *params;
            struct AstNode *ret_type;
        } trait_fn_sig;

        /* ── NODE_IMPL_DECL ───────────────────────────────────── */
        struct {
            const char *struct_name;
            const char *trait_name;  /* NULL → inherent impl */
            AstList    *generic_params;
            AstList    *type_generic_params; /* after target type name, or NULL */
            AstList    *methods;             /* list of NODE_FN_DECL */
            bool        is_pub;              /* `pub impl` — visibility metadata */
        } impl_decl;

        /* ── NODE_MACRO_DECL ──────────────────────────────────── */
        struct {
            const char *name;
            AstList    *rules;      /* opaque token trees for Phase 1 */
        } macro_decl;

        /* ── NODE_USE_DECL ────────────────────────────────────── */
        struct {
            const char *path;   /* "std::io::File" as interned string */
            AstList    *items;  /* NULL → plain use; else named items */
            bool        glob;   /* use foo::* */
        } use_decl;

        /* ── NODE_CONST_DECL ──────────────────────────────────── */
        struct {
            const char     *name;
            struct AstNode *type;
            struct AstNode *value;
            bool            is_pub;
        } const_decl;

        /* ── NODE_BLOCK ───────────────────────────────────────── */
        struct {
            AstList        *stmts;      /* list of stmt nodes */
            struct AstNode *tail_expr;  /* NULL if block ends with ";" */
        } block;

        /* ── NODE_LET ─────────────────────────────────────────── */
        struct {
            const char     *name;
            struct AstNode *type;       /* NULL → inferred */
            struct AstNode *init;
            bool            is_mut;
        } let;

        /* ── NODE_RETURN ──────────────────────────────────────── */
        struct {
            struct AstNode *value;      /* NULL → return; */
        } ret;

        /* ── NODE_EXPR_STMT ───────────────────────────────────── */
        struct {
            struct AstNode *expr;
        } expr_stmt;

        /* ── NODE_FOR ─────────────────────────────────────────── */
        struct {
            const char     *var;
            struct AstNode *iter;       /* any expr implementing Iterator */
            struct AstNode *body;       /* NODE_BLOCK */
        } for_loop;

        /* ── NODE_TRY ─────────────────────────────────────────── */
        struct {
            struct AstNode *body;         /* NODE_BLOCK */
            AstList        *catch_clauses;/* list of NODE_CATCH_CLAUSE */
            struct AstNode *finally_body; /* NODE_BLOCK or NULL */
        } try_stmt;

        /* ── NODE_CATCH_CLAUSE ────────────────────────────────── */
        struct {
            const char     *var;
            struct AstNode *type;
            struct AstNode *body;   /* NODE_BLOCK */
        } catch_clause;

        /* ── NODE_BINARY ──────────────────────────────────────── */
        struct {
            BinOp           op;
            struct AstNode *left;
            struct AstNode *right;
        } binary;

        /* ── NODE_UNARY ───────────────────────────────────────── */
        struct {
            UnOp            op;
            struct AstNode *operand;
        } unary;

        /* ── NODE_ASSIGN ──────────────────────────────────────── */
        struct {
            struct AstNode *target;  /* must resolve to a mut binding */
            struct AstNode *value;
        } assign;

        /* ── NODE_IF ──────────────────────────────────────────── */
        struct {
            AstList        *branches; /* list of {cond, body} pairs as flat list:
                                         cond0, body0, cond1, body1, …
                                         NULL cond = else branch           */
            struct AstNode *else_body; /* NULL if no else */
        } if_expr;

        /* ── NODE_MATCH ───────────────────────────────────────── */
        struct {
            struct AstNode *subject;
            AstList        *arms;    /* list of NODE_MATCH_ARM */
        } match;

        /* ── NODE_MATCH_ARM ───────────────────────────────────── */
        struct {
            struct AstNode *pattern;
            struct AstNode *guard;   /* "if expr" guard, or NULL */
            struct AstNode *body;    /* expr or block */
        } match_arm;

        /* ── NODE_CALL ────────────────────────────────────────── */
        struct {
            struct AstNode *callee;
            AstList        *args;
            AstList        *type_args; /* explicit generic args, usually NULL */
        } call;

        /* ── NODE_METHOD_CALL ─────────────────────────────────── */
        struct {
            struct AstNode *receiver;
            const char     *method;
            AstList        *args;
            AstList        *type_args;
        } method_call;

        /* ── NODE_FIELD_ACCESS ────────────────────────────────── */
        struct {
            struct AstNode *object;
            const char     *field;
        } field_access;

        /* ── NODE_INDEX ───────────────────────────────────────── */
        struct {
            struct AstNode *object;
            struct AstNode *index;
        } index;

        /* ── NODE_LAMBDA ──────────────────────────────────────── */
        struct {
            AstList        *params;    /* list of NODE_PARAM */
            struct AstNode *ret_type;  /* NULL → inferred */
            struct AstNode *body;      /* expr or NODE_BLOCK */
        } lambda;

        /* ── NODE_STRUCT_INIT ─────────────────────────────────── */
        struct {
            const char     *struct_name;
            AstList        *fields;     /* list of NODE_FIELD_INIT */
            struct AstNode *base;       /* struct update syntax "..base", or NULL */
        } struct_init;

        /* ── NODE_FIELD_INIT ──────────────────────────────────── */
        struct {
            const char     *name;
            struct AstNode *value;
        } field_init;

        /* ── NODE_ARRAY ───────────────────────────────────────── */
        struct {
            AstList *elements;
        } array;

        /* ── NODE_TUPLE ───────────────────────────────────────── */
        struct {
            AstList *elements;
        } tuple;

        /* ── NODE_MACRO_CALL ──────────────────────────────────── */
        struct {
            const char *name;
            AstList    *tokens;   /* opaque token list, expanded later */
        } macro_call;

        /* ── NODE_IDENT ───────────────────────────────────────── */
        struct {
            const char *name;
        } ident;

        /* ── Literals ─────────────────────────────────────────── */
        struct { int64_t  value; } lit_int;
        struct { float    value; } lit_float;
        struct { double   value; } lit_double;
        struct { bool     value; } lit_bool;
        struct { const char *value; } lit_string;
        /* NODE_LIT_NONE has no payload */

        /* ── Types ────────────────────────────────────────────── */

        /* NODE_TYPE_PRIMITIVE */
        struct { PrimKind prim; } type_primitive;

        /* NODE_TYPE_NAMED  — e.g. "MyStruct[T]" */
        struct {
            const char *name;
            AstList    *type_args;
        } type_named;

        /* NODE_TYPE_ARRAY  — "[T]" */
        struct { struct AstNode *elem_type; } type_array;

        /* NODE_TYPE_FN  — "fn(A, B) -> C" */
        struct {
            AstList        *param_types;
            struct AstNode *ret_type;
        } type_fn;

        /* NODE_TYPE_OPTION  — "Option[T]" */
        struct { struct AstNode *inner; } type_option;

        /* NODE_TYPE_RESULT  — "Result[T, E]" */
        struct {
            struct AstNode *ok_type;
            struct AstNode *err_type;
        } type_result;

        /* NODE_TYPE_TUPLE  — "(A, B, C)" */
        struct { AstList *elem_types; } type_tuple;

        /* NODE_TYPE_REF  — "&T" or "&mut T" */
        struct {
            struct AstNode *inner;
            bool            is_mut;
        } type_ref;

        /* ── Patterns ─────────────────────────────────────────── */

        /* NODE_PAT_WILDCARD — "_"        (no payload) */
        /* NODE_PAT_LITERAL  — wraps a literal expr */
        struct { struct AstNode *lit; } pat_literal;

        /* NODE_PAT_BIND  — "x" or "mut x" */
        struct {
            const char *name;
            bool        is_mut;
        } pat_bind;

        /* NODE_PAT_ENUM  — "Option::Some(x)" */
        struct {
            const char *type_name;
            const char *variant;
            AstList    *fields;    /* inner patterns, may be NULL */
        } pat_enum;

        /* NODE_PAT_STRUCT  — "Point { x, y: p }" */
        struct {
            const char *name;
            AstList    *field_pats;  /* list of {field_name, pattern} pairs */
            bool        rest;        /* true when ".." is present */
        } pat_struct;

        /* NODE_PAT_TUPLE  — "(a, b, c)" */
        struct { AstList *elements; } pat_tuple;

        /* NODE_PAT_ARRAY  — "[a, b, c]" */
        struct { AstList *elements; } pat_array;

        /* NODE_PAT_OR  — "A | B" */
        struct {
            struct AstNode *left;
            struct AstNode *right;
        } pat_or;

    } as;   /* end union */

} AstNode;


/* ── Convenience access macros ──────────────────────────────── */

#define AS_PROGRAM(n)       ((n)->as.program)
#define AS_FN_DECL(n)       ((n)->as.fn_decl)
#define AS_PARAM(n)         ((n)->as.param)
#define AS_GENERIC_PARAM(n) ((n)->as.generic_param)
#define AS_STRUCT_DECL(n)   ((n)->as.struct_decl)
#define AS_STRUCT_FIELD(n)  ((n)->as.struct_field)
#define AS_TRAIT_DECL(n)    ((n)->as.trait_decl)
#define AS_IMPL_DECL(n)     ((n)->as.impl_decl)
#define AS_MACRO_DECL(n)    ((n)->as.macro_decl)
#define AS_USE_DECL(n)      ((n)->as.use_decl)
#define AS_CONST_DECL(n)    ((n)->as.const_decl)
#define AS_BLOCK(n)         ((n)->as.block)
#define AS_LET(n)           ((n)->as.let)
#define AS_RETURN(n)        ((n)->as.ret)
#define AS_EXPR_STMT(n)     ((n)->as.expr_stmt)
#define AS_FOR(n)           ((n)->as.for_loop)
#define AS_TRY(n)           ((n)->as.try_stmt)
#define AS_CATCH(n)         ((n)->as.catch_clause)
#define AS_BINARY(n)        ((n)->as.binary)
#define AS_UNARY(n)         ((n)->as.unary)
#define AS_ASSIGN(n)        ((n)->as.assign)
#define AS_IF(n)            ((n)->as.if_expr)
#define AS_MATCH(n)         ((n)->as.match)
#define AS_MATCH_ARM(n)     ((n)->as.match_arm)
#define AS_CALL(n)          ((n)->as.call)
#define AS_METHOD_CALL(n)   ((n)->as.method_call)
#define AS_FIELD_ACCESS(n)  ((n)->as.field_access)
#define AS_INDEX(n)         ((n)->as.index)
#define AS_LAMBDA(n)        ((n)->as.lambda)
#define AS_STRUCT_INIT(n)   ((n)->as.struct_init)
#define AS_FIELD_INIT(n)    ((n)->as.field_init)
#define AS_ARRAY(n)         ((n)->as.array)
#define AS_TUPLE(n)         ((n)->as.tuple)
#define AS_MACRO_CALL(n)    ((n)->as.macro_call)
#define AS_IDENT(n)         ((n)->as.ident)
#define AS_LIT_INT(n)       ((n)->as.lit_int)
#define AS_LIT_FLOAT(n)     ((n)->as.lit_float)
#define AS_LIT_DOUBLE(n)    ((n)->as.lit_double)
#define AS_LIT_BOOL(n)      ((n)->as.lit_bool)
#define AS_LIT_STRING(n)    ((n)->as.lit_string)
#define AS_TYPE_PRIM(n)     ((n)->as.type_primitive)
#define AS_TYPE_NAMED(n)    ((n)->as.type_named)
#define AS_TYPE_ARRAY(n)    ((n)->as.type_array)
#define AS_TYPE_FN(n)       ((n)->as.type_fn)
#define AS_TYPE_OPTION(n)   ((n)->as.type_option)
#define AS_TYPE_RESULT(n)   ((n)->as.type_result)
#define AS_TYPE_TUPLE(n)    ((n)->as.type_tuple)
#define AS_TYPE_REF(n)      ((n)->as.type_ref)
#define AS_PAT_LIT(n)       ((n)->as.pat_literal)
#define AS_PAT_BIND(n)      ((n)->as.pat_bind)
#define AS_PAT_ENUM(n)      ((n)->as.pat_enum)
#define AS_PAT_STRUCT(n)    ((n)->as.pat_struct)
#define AS_PAT_TUPLE(n)     ((n)->as.pat_tuple)
#define AS_PAT_ARRAY(n)     ((n)->as.pat_array)
#define AS_PAT_OR(n)        ((n)->as.pat_or)


/* ── Allocator interface (implement in arena.c) ────────── */

AstNode *ast_alloc(NodeKind kind, SrcLoc loc);
AstList *ast_list_append(AstList *list, AstNode *item);
void     ast_free_all(void);   /* frees the entire arena */

/* Copy `len` bytes from `src` into the arena as a null-terminated string. */
const char *ast_copy_string(const char *src, size_t len);


/* ── Debug printer (implement in ast_print.c) ─────────── */

void ast_print(const AstNode *node, int indent);


#endif /* MONS_AST_H */
