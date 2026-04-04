/*
 * types.c — Hindley–Milner–style inference with explicit annotations on consts and params.
 *
 * TY_VAR nodes are unification variables; prune() follows var.bound chains. unify() binds
 * vars or recurses structurally; occurs_in catches infinite types.
 *
 * type_check_program runs in three passes: (1) register struct names + field layouts into
 * global_env, (2) register const types (infer rhs, unify with annotation) and fn signatures,
 * (3) check each fn body with a child env (params + parent = global).
 */
#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TY_INT,
    TY_FLOAT,
    TY_DOUBLE,
    TY_BOOL,
    TY_STRING,
    TY_VOID,
    TY_VAR,
    TY_FN,
    TY_OPTION,
    TY_RESULT,
    TY_ARRAY,
    TY_TUPLE,
    TY_STRUCT /* named struct; as.st_def holds field names/types for init/checking */
} TyKind;

typedef struct Type {
    TyKind kind;
    union {
        struct {
            int id;
            struct Type *bound;
        } var;
        struct {
            struct Type **params;
            size_t nparams;
            struct Type *ret;
        } fn;
        struct Type *inner; /* OPTION, ARRAY */
        struct {
            struct Type *ok;
            struct Type *err;
        } result;
        struct {
            struct Type **elems;
            size_t nelems;
        } tuple;
        struct {
            const char *name;
            struct Type **field_types;
            const char **field_names;
            size_t nfields;
        } st_def;
    } as;
} Type;

typedef struct Bind {
    const char *name;
    Type *type;
    struct Bind *next;
} Bind;

typedef struct Env {
    Bind *head;
    struct Env *parent;
} Env;

/* Tracks allocated Type* for cleanup; global_env holds structs, consts, fn sigs. */
typedef struct Checker {
    Type **allocs;
    size_t nallocs;
    size_t cap_allocs;
    int next_var_id;
    AstNode *program;
    Env *global_env;
    Env *current_env;
    Type *expected_return;
    const char *error;
    uint32_t err_line;
    uint32_t err_col;
} Checker;

static Type ty_int_storage = { .kind = TY_INT };
static Type ty_float_storage = { .kind = TY_FLOAT };
static Type ty_double_storage = { .kind = TY_DOUBLE };
static Type ty_bool_storage = { .kind = TY_BOOL };
static Type ty_string_storage = { .kind = TY_STRING };
static Type ty_void_storage = { .kind = TY_VOID };

static Type *ty_int = &ty_int_storage;
static Type *ty_float = &ty_float_storage;
static Type *ty_double = &ty_double_storage;
static Type *ty_bool = &ty_bool_storage;
static Type *ty_string = &ty_string_storage;
static Type *ty_void = &ty_void_storage;

static void checker_fail(Checker *c, SrcLoc loc, const char *msg) {
    if (c->error) {
        return;
    }
    c->error = msg;
    c->err_line = loc.line;
    c->err_col = loc.col;
}

static Type *alloc_type(Checker *c) {
    if (c->nallocs >= c->cap_allocs) {
        size_t ncap = c->cap_allocs == 0 ? 32 : c->cap_allocs * 2;
        Type **na = (Type **)realloc(c->allocs, ncap * sizeof(Type *));
        if (!na) {
            return NULL;
        }
        c->allocs = na;
        c->cap_allocs = ncap;
    }
    Type *t = (Type *)calloc(1, sizeof(Type));
    if (!t) {
        return NULL;
    }
    c->allocs[c->nallocs++] = t;
    return t;
}

static void checker_free_allocs(Checker *c) {
    size_t i;
    for (i = 0; i < c->nallocs; i++) {
        Type *t = c->allocs[i];
        if (t->kind == TY_FN) {
            free(t->as.fn.params);
        } else if (t->kind == TY_TUPLE) {
            free(t->as.tuple.elems);
        } else if (t->kind == TY_STRUCT) {
            free(t->as.st_def.field_types);
            free(t->as.st_def.field_names);
        }
        free(t);
    }
    free(c->allocs);
    c->allocs = NULL;
    c->nallocs = 0;
    c->cap_allocs = 0;
}

static Type *new_var(Checker *c) {
    Type *t = alloc_type(c);
    if (!t) {
        return NULL;
    }
    t->kind = TY_VAR;
    t->as.var.id = c->next_var_id++;
    t->as.var.bound = NULL;
    return t;
}

static Type *new_fn_type(Checker *c, Type **params, size_t n, Type *ret) {
    Type *t = alloc_type(c);
    if (!t) {
        return NULL;
    }
    t->kind = TY_FN;
    t->as.fn.params = params;
    t->as.fn.nparams = n;
    t->as.fn.ret = ret;
    return t;
}

static Type *new_option(Checker *c, Type *inner) {
    Type *t = alloc_type(c);
    if (!t) {
        return NULL;
    }
    t->kind = TY_OPTION;
    t->as.inner = inner;
    return t;
}

static Type *new_result(Checker *c, Type *ok, Type *err) {
    Type *t = alloc_type(c);
    if (!t) {
        return NULL;
    }
    t->kind = TY_RESULT;
    t->as.result.ok = ok;
    t->as.result.err = err;
    return t;
}

static Type *new_array_type(Checker *c, Type *elem) {
    Type *t = alloc_type(c);
    if (!t) {
        return NULL;
    }
    t->kind = TY_ARRAY;
    t->as.inner = elem;
    return t;
}

static Type *new_tuple_type(Checker *c, Type **elems, size_t n) {
    Type *t = alloc_type(c);
    if (!t) {
        return NULL;
    }
    t->kind = TY_TUPLE;
    t->as.tuple.elems = elems;
    t->as.tuple.nelems = n;
    return t;
}

static Type *prune(Type *t) {
    if (!t || t->kind != TY_VAR) {
        return t;
    }
    if (!t->as.var.bound) {
        return t;
    }
    Type *root = prune(t->as.var.bound);
    t->as.var.bound = root;
    return root;
}

static bool type_same_head(Type *a, Type *b) {
    a = prune(a);
    b = prune(b);
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == TY_VAR) {
        return a->as.var.id == b->as.var.id;
    }
    return true;
}

static bool occurs_in(Type *var, Type *t) {
    t = prune(t);
    if (t->kind == TY_VAR) {
        return type_same_head(var, t);
    }
    switch (t->kind) {
        case TY_FN:
            {
                size_t i;
                for (i = 0; i < t->as.fn.nparams; i++) {
                    if (occurs_in(var, t->as.fn.params[i])) {
                        return true;
                    }
                }
                return occurs_in(var, t->as.fn.ret);
            }
        case TY_OPTION:
        case TY_ARRAY:
            return occurs_in(var, t->as.inner);
        case TY_RESULT:
            return occurs_in(var, t->as.result.ok) || occurs_in(var, t->as.result.err);
        case TY_TUPLE:
            {
                size_t i;
                for (i = 0; i < t->as.tuple.nelems; i++) {
                    if (occurs_in(var, t->as.tuple.elems[i])) {
                        return true;
                    }
                }
                return false;
            }
        case TY_STRUCT:
            {
                size_t i;
                for (i = 0; i < t->as.st_def.nfields; i++) {
                    if (occurs_in(var, t->as.st_def.field_types[i])) {
                        return true;
                    }
                }
                return false;
            }
        default:
            return false;
    }
}

static bool unify(Checker *c, Type *a, Type *b, SrcLoc loc);

static bool unify_structural(Checker *c, Type *a, Type *b, SrcLoc loc) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case TY_FN:
            if (a->as.fn.nparams != b->as.fn.nparams) {
                return false;
            }
            {
                size_t i;
                for (i = 0; i < a->as.fn.nparams; i++) {
                    if (!unify(c, a->as.fn.params[i], b->as.fn.params[i], loc)) {
                        return false;
                    }
                }
                return unify(c, a->as.fn.ret, b->as.fn.ret, loc);
            }
        case TY_OPTION:
            return unify(c, a->as.inner, b->as.inner, loc);
        case TY_ARRAY:
            return unify(c, a->as.inner, b->as.inner, loc);
        case TY_RESULT:
            return unify(c, a->as.result.ok, b->as.result.ok, loc) &&
                   unify(c, a->as.result.err, b->as.result.err, loc);
        case TY_TUPLE:
            if (a->as.tuple.nelems != b->as.tuple.nelems) {
                return false;
            }
            {
                size_t i;
                for (i = 0; i < a->as.tuple.nelems; i++) {
                    if (!unify(c, a->as.tuple.elems[i], b->as.tuple.elems[i], loc)) {
                        return false;
                    }
                }
                return true;
            }
        case TY_STRUCT:
            return strcmp(a->as.st_def.name, b->as.st_def.name) == 0;
        default:
            return false;
    }
}

/* Make a and b equal by binding TY_VARs or comparing concrete shapes; prune first. */
static bool unify(Checker *c, Type *a, Type *b, SrcLoc loc) {
    if (!a || !b) {
        checker_fail(c, loc, "internal type error");
        return false;
    }
    a = prune(a);
    b = prune(b);
    if (a->kind == TY_VAR) {
        if (type_same_head(a, b)) {
            return true;
        }
        if (occurs_in(a, b)) {
            checker_fail(c, loc, "infinite type");
            return false;
        }
        a->as.var.bound = b;
        return true;
    }
    if (b->kind == TY_VAR) {
        return unify(c, b, a, loc);
    }
    if (a->kind != b->kind) {
        checker_fail(c, loc, "type mismatch");
        return false;
    }
    switch (a->kind) {
        case TY_INT:
        case TY_FLOAT:
        case TY_DOUBLE:
        case TY_BOOL:
        case TY_STRING:
        case TY_VOID:
            return true;
        case TY_FN:
        case TY_OPTION:
        case TY_RESULT:
        case TY_ARRAY:
        case TY_TUPLE:
        case TY_STRUCT:
            return unify_structural(c, a, b, loc);
        default:
            return false;
    }
}

static bool is_numeric(Type *t) {
    t = prune(t);
    return t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_DOUBLE;
}

static void env_insert(Env *e, const char *name, Type *ty) {
    Bind *b = (Bind *)malloc(sizeof(Bind));
    if (!b) {
        return;
    }
    b->name = name;
    b->type = ty;
    b->next = e->head;
    e->head = b;
}

static Type *env_lookup(Env *e, const char *name) {
    for (; e; e = e->parent) {
        Bind *b;
        for (b = e->head; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                return b->type;
            }
        }
    }
    return NULL;
}

static void env_free_head(Env *e) {
    Bind *b = e->head;
    while (b) {
        Bind *n = b->next;
        free(b);
        b = n;
    }
    e->head = NULL;
}

static Type *ast_type_to_type(Checker *c, AstNode *n);

static Type *ast_type_to_type(Checker *c, AstNode *n) {
    if (!n) {
        return ty_void;
    }
    switch (n->kind) {
        case NODE_TYPE_PRIMITIVE:
            switch (AS_TYPE_PRIM(n).prim) {
                case PRIM_INT: return ty_int;
                case PRIM_FLOAT: return ty_float;
                case PRIM_DOUBLE: return ty_double;
                case PRIM_BOOL: return ty_bool;
                case PRIM_STRING: return ty_string;
                default:
                    checker_fail(c, n->loc, "unknown primitive type");
                    return NULL;
            }
        case NODE_TYPE_ARRAY:
            {
                Type *el = ast_type_to_type(c, AS_TYPE_ARRAY(n).elem_type);
                if (c->error) {
                    return NULL;
                }
                return new_array_type(c, el);
            }
        case NODE_TYPE_OPTION:
            {
                Type *in = ast_type_to_type(c, AS_TYPE_OPTION(n).inner);
                if (c->error) {
                    return NULL;
                }
                return new_option(c, in);
            }
        case NODE_TYPE_RESULT:
            {
                Type *ok = ast_type_to_type(c, AS_TYPE_RESULT(n).ok_type);
                Type *err = ast_type_to_type(c, AS_TYPE_RESULT(n).err_type);
                if (c->error) {
                    return NULL;
                }
                return new_result(c, ok, err);
            }
        case NODE_TYPE_NAMED:
            {
                const char *nm = AS_TYPE_NAMED(n).name;
                Type *got;
                if (strcmp(nm, "Option") == 0) {
                    checker_fail(c, n->loc, "Option requires type arguments");
                    return NULL;
                }
                if (strcmp(nm, "Result") == 0) {
                    checker_fail(c, n->loc, "Result requires type arguments");
                    return NULL;
                }
                got = env_lookup(c->global_env, nm);
                if (!got || prune(got)->kind != TY_STRUCT) {
                    checker_fail(c, n->loc, "unknown struct type");
                    return NULL;
                }
                return got;
            }
        case NODE_TYPE_TUPLE:
            {
                AstList *l;
                size_t n_elem = ast_list_len(AS_TYPE_TUPLE(n).elem_types);
                Type **buf = (Type **)malloc(n_elem * sizeof(Type *));
                if (!buf && n_elem > 0) {
                    return NULL;
                }
                size_t i = 0;
                for (l = AS_TYPE_TUPLE(n).elem_types; l; l = l->next) {
                    buf[i++] = ast_type_to_type(c, l->item);
                    if (c->error) {
                        free(buf);
                        return NULL;
                    }
                }
                return new_tuple_type(c, buf, n_elem);
            }
        case NODE_TYPE_FN:
            {
                AstList *pl;
                size_t n_params = ast_list_len(AS_TYPE_FN(n).param_types);
                Type **params = (Type **)malloc(n_params * sizeof(Type *));
                if (!params && n_params > 0) {
                    return NULL;
                }
                size_t i = 0;
                for (pl = AS_TYPE_FN(n).param_types; pl; pl = pl->next) {
                    params[i++] = ast_type_to_type(c, pl->item);
                    if (c->error) {
                        free(params);
                        return NULL;
                    }
                }
                Type *ret = ast_type_to_type(c, AS_TYPE_FN(n).ret_type);
                if (c->error) {
                    free(params);
                    return NULL;
                }
                return new_fn_type(c, params, n_params, ret);
            }
        case NODE_TYPE_REF:
            checker_fail(c, n->loc, "reference types not supported in Phase 1 checker");
            return NULL;
        default:
            checker_fail(c, n->loc, "unsupported type syntax");
            return NULL;
    }
}

static Type *infer_expr(Checker *c, AstNode *n);

static Type *infer_block(Checker *c, AstNode *block);

static void check_stmt(Checker *c, AstNode *stmt);

static Type *infer_expr_or_block(Checker *c, AstNode *n) {
    if (n->kind == NODE_BLOCK) {
        return infer_block(c, n);
    }
    return infer_expr(c, n);
}

static Type *infer_if_expr(Checker *c, AstNode *n) {
    AstList *br = AS_IF(n).branches;
    Type *acc = NULL;
    while (br) {
        AstNode *cond = br->item;
        br = br->next;
        if (!br) {
            break;
        }
        AstNode *body = br->item;
        br = br->next;
        Type *ct = infer_expr(c, cond);
        if (c->error) {
            return NULL;
        }
        if (!unify(c, ct, ty_bool, cond->loc)) {
            return NULL;
        }
        Type *bt = infer_expr_or_block(c, body);
        if (c->error) {
            return NULL;
        }
        if (!acc) {
            acc = bt;
        } else if (!unify(c, acc, bt, body->loc)) {
            return NULL;
        }
    }
    if (AS_IF(n).else_body) {
        Type *et = infer_expr_or_block(c, AS_IF(n).else_body);
        if (c->error) {
            return NULL;
        }
        if (!acc) {
            acc = et;
        } else if (!unify(c, acc, et, AS_IF(n).else_body->loc)) {
            return NULL;
        }
    }
    return acc ? acc : ty_void;
}

static Type *struct_field_type(Type *st, const char *fname, size_t *out_index) {
    size_t i;
    st = prune(st);
    if (!st || st->kind != TY_STRUCT) {
        return NULL;
    }
    for (i = 0; i < st->as.st_def.nfields; i++) {
        if (strcmp(st->as.st_def.field_names[i], fname) == 0) {
            if (out_index) {
                *out_index = i;
            }
            return st->as.st_def.field_types[i];
        }
    }
    return NULL;
}

static AstNode *lookup_fn_decl(Checker *c, const char *name) {
    AstList *d;
    if (!c->program || c->program->kind != NODE_PROGRAM) {
        return NULL;
    }
    for (d = AS_PROGRAM(c->program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL && strcmp(AS_FN_DECL(d->item).name, name) == 0) {
            return d->item;
        }
    }
    return NULL;
}

static Type *infer_expr(Checker *c, AstNode *n) {
    if (!n) {
        checker_fail(c, (SrcLoc){0}, "internal: null expr");
        return NULL;
    }
    switch (n->kind) {
        case NODE_LIT_INT:
            return ty_int;
        case NODE_LIT_FLOAT:
            return ty_float;
        case NODE_LIT_DOUBLE:
            return ty_double;
        case NODE_LIT_BOOL:
            return ty_bool;
        case NODE_LIT_STRING:
            return ty_string;
        case NODE_LIT_NONE:
            {
                Type *v = new_var(c);
                if (!v) {
                    checker_fail(c, n->loc, "out of memory");
                    return NULL;
                }
                return new_option(c, v);
            }
        case NODE_IDENT:
            {
                Type *t = env_lookup(c->current_env, AS_IDENT(n).name);
                if (!t) {
                    checker_fail(c, n->loc, "unknown identifier");
                    return NULL;
                }
                return t;
            }
        case NODE_UNARY:
            {
                Type *opd = infer_expr(c, AS_UNARY(n).operand);
                if (c->error) {
                    return NULL;
                }
                if (AS_UNARY(n).op == UNOP_NOT) {
                    if (!unify(c, opd, ty_bool, n->loc)) {
                        return NULL;
                    }
                    return ty_bool;
                }
                /* UNOP_NEG */
                {
                    Type *p = prune(opd);
                    if (!is_numeric(p)) {
                        checker_fail(c, n->loc, "unary '-' expects numeric operand");
                        return NULL;
                    }
                    return p;
                }
            }
        case NODE_BINARY:
            {
                Type *l = infer_expr(c, AS_BINARY(n).left);
                Type *r = infer_expr(c, AS_BINARY(n).right);
                if (c->error) {
                    return NULL;
                }
                BinOp op = AS_BINARY(n).op;
                if (op == BINOP_AND || op == BINOP_OR) {
                    if (!unify(c, l, ty_bool, n->loc) || !unify(c, r, ty_bool, n->loc)) {
                        return NULL;
                    }
                    return ty_bool;
                }
                if (op == BINOP_EQ || op == BINOP_NEQ || op == BINOP_LT || op == BINOP_GT ||
                    op == BINOP_LTE || op == BINOP_GTE) {
                    if (!unify(c, l, r, n->loc)) {
                        return NULL;
                    }
                    return ty_bool;
                }
                if (op == BINOP_MOD) {
                    if (!unify(c, l, ty_int, n->loc) || !unify(c, r, ty_int, n->loc)) {
                        return NULL;
                    }
                    return ty_int;
                }
                /* arithmetic */
                if (!unify(c, l, r, n->loc)) {
                    return NULL;
                }
                if (!is_numeric(l)) {
                    checker_fail(c, n->loc, "arithmetic expects numeric operands");
                    return NULL;
                }
                return prune(l);
            }
        case NODE_ASSIGN:
            {
                AstNode *tgt = AS_ASSIGN(n).target;
                Type *rhs = infer_expr(c, AS_ASSIGN(n).value);
                if (c->error) {
                    return NULL;
                }
                if (tgt->kind != NODE_IDENT) {
                    checker_fail(c, n->loc, "assignment target not supported yet");
                    return NULL;
                }
                Type *lt = env_lookup(c->current_env, AS_IDENT(tgt).name);
                if (!lt) {
                    checker_fail(c, tgt->loc, "unknown identifier");
                    return NULL;
                }
                if (!unify(c, lt, rhs, n->loc)) {
                    return NULL;
                }
                return rhs;
            }
        case NODE_CALL:
            {
                Type *callee_t = infer_expr(c, AS_CALL(n).callee);
                if (c->error) {
                    return NULL;
                }
                callee_t = prune(callee_t);
                if (callee_t->kind != TY_FN) {
                    checker_fail(c, n->loc, "call target is not a function");
                    return NULL;
                }
                AstList *arg;
                size_t i = 0;
                for (arg = AS_CALL(n).args; arg; arg = arg->next, i++) {
                    if (i >= callee_t->as.fn.nparams) {
                        checker_fail(c, n->loc, "too many arguments");
                        return NULL;
                    }
                    Type *at = infer_expr(c, arg->item);
                    if (c->error) {
                        return NULL;
                    }
                    if (!unify(c, at, callee_t->as.fn.params[i], arg->item->loc)) {
                        return NULL;
                    }
                }
                if (i != callee_t->as.fn.nparams) {
                    checker_fail(c, n->loc, "too few arguments");
                    return NULL;
                }
                return callee_t->as.fn.ret;
            }
        /* Same as eval: top-level fn named `method`, first param must be `self` = receiver type. */
        case NODE_METHOD_CALL:
            {
                Type *recv_t = infer_expr(c, AS_METHOD_CALL(n).receiver);
                AstNode *fn_ast;
                Type *ft;
                AstList *arg;
                size_t j;
                size_t nargs;
                if (c->error) {
                    return NULL;
                }
                fn_ast = lookup_fn_decl(c, AS_METHOD_CALL(n).method);
                if (!fn_ast) {
                    checker_fail(c, n->loc, "unknown method");
                    return NULL;
                }
                ft = env_lookup(c->global_env, AS_METHOD_CALL(n).method);
                if (!ft || prune(ft)->kind != TY_FN) {
                    checker_fail(c, n->loc, "unknown method");
                    return NULL;
                }
                ft = prune(ft);
                if (ft->as.fn.nparams == 0 || !AS_FN_DECL(fn_ast).params) {
                    checker_fail(c, n->loc, "method call requires a function with a self parameter");
                    return NULL;
                }
                if (strcmp(AS_PARAM(AS_FN_DECL(fn_ast).params->item).name, "self") != 0) {
                    checker_fail(c, n->loc, "inherent method requires first parameter named self");
                    return NULL;
                }
                if (!unify(c, recv_t, ft->as.fn.params[0], AS_METHOD_CALL(n).receiver->loc)) {
                    return NULL;
                }
                nargs = ast_list_len(AS_METHOD_CALL(n).args);
                if (nargs + 1 != ft->as.fn.nparams) {
                    checker_fail(c, n->loc, "wrong argument count in method call");
                    return NULL;
                }
                j = 0;
                for (arg = AS_METHOD_CALL(n).args; arg; arg = arg->next, j++) {
                    Type *at = infer_expr(c, arg->item);
                    if (c->error) {
                        return NULL;
                    }
                    if (!unify(c, at, ft->as.fn.params[j + 1], arg->item->loc)) {
                        return NULL;
                    }
                }
                return ft->as.fn.ret;
            }
        case NODE_FIELD_ACCESS:
            {
                Type *obj_t = infer_expr(c, AS_FIELD_ACCESS(n).object);
                Type *ft;
                if (c->error) {
                    return NULL;
                }
                obj_t = prune(obj_t);
                ft = struct_field_type(obj_t, AS_FIELD_ACCESS(n).field, NULL);
                if (!ft) {
                    checker_fail(c, n->loc, "unknown field or not a struct value");
                    return NULL;
                }
                return ft;
            }
        case NODE_INDEX:
            {
                Type *obj_t = infer_expr(c, AS_INDEX(n).object);
                Type *idx_t = infer_expr(c, AS_INDEX(n).index);
                if (c->error) {
                    return NULL;
                }
                if (!unify(c, idx_t, ty_int, AS_INDEX(n).index->loc)) {
                    return NULL;
                }
                obj_t = prune(obj_t);
                if (obj_t->kind == TY_ARRAY) {
                    return obj_t->as.inner;
                }
                if (obj_t->kind == TY_TUPLE) {
                    AstNode *in = AS_INDEX(n).index;
                    if (in->kind != NODE_LIT_INT) {
                        checker_fail(c, n->loc, "tuple index must be an integer literal");
                        return NULL;
                    }
                    int64_t k = AS_LIT_INT(in).value;
                    if (k < 0 || (size_t)k >= obj_t->as.tuple.nelems) {
                        checker_fail(c, n->loc, "tuple index out of range");
                        return NULL;
                    }
                    return obj_t->as.tuple.elems[(size_t)k];
                }
                checker_fail(c, n->loc, "index target must be array or tuple");
                return NULL;
            }
        case NODE_IF:
            return infer_if_expr(c, n);
        case NODE_BLOCK:
            return infer_block(c, n);
        case NODE_ARRAY:
            {
                AstList *el;
                Type *elem0 = NULL;
                for (el = AS_ARRAY(n).elements; el; el = el->next) {
                    Type *et = infer_expr(c, el->item);
                    if (c->error) {
                        return NULL;
                    }
                    if (!elem0) {
                        elem0 = et;
                    } else if (!unify(c, elem0, et, el->item->loc)) {
                        return NULL;
                    }
                }
                if (!elem0) {
                    Type *v = new_var(c);
                    return new_array_type(c, v);
                }
                return new_array_type(c, prune(elem0));
            }
        case NODE_TUPLE:
            {
                size_t nmem = ast_list_len(AS_TUPLE(n).elements);
                Type **buf = (Type **)malloc(nmem * sizeof(Type *));
                if (!buf && nmem > 0) {
                    return NULL;
                }
                size_t i = 0;
                AstList *el;
                for (el = AS_TUPLE(n).elements; el; el = el->next) {
                    buf[i++] = infer_expr(c, el->item);
                    if (c->error) {
                        free(buf);
                        return NULL;
                    }
                }
                return new_tuple_type(c, buf, nmem);
            }
        /* Listed fields checked vs st_def; without base, every field must appear exactly once. */
        case NODE_STRUCT_INIT:
            {
                const char *sname = AS_STRUCT_INIT(n).struct_name;
                Type *st_t = env_lookup(c->global_env, sname);
                AstList *fi;
                bool *seen;
                size_t i;
                size_t nf;
                st_t = st_t ? prune(st_t) : NULL;
                if (!st_t || st_t->kind != TY_STRUCT) {
                    checker_fail(c, n->loc, "unknown struct type in initializer");
                    return NULL;
                }
                nf = st_t->as.st_def.nfields;
                seen = nf ? (bool *)calloc(nf, sizeof(bool)) : NULL;
                if (nf > 0 && !seen) {
                    checker_fail(c, n->loc, "out of memory");
                    return NULL;
                }
                for (fi = AS_STRUCT_INIT(n).fields; fi; fi = fi->next) {
                    AstNode *init = fi->item;
                    const char *fnm = AS_FIELD_INIT(init).name;
                    size_t idx;
                    Type *ft = struct_field_type(st_t, fnm, &idx);
                    Type *vt;
                    if (!ft) {
                        checker_fail(c, init->loc, "unknown struct field in initializer");
                        free(seen);
                        return NULL;
                    }
                    if (seen[idx]) {
                        checker_fail(c, init->loc, "duplicate field in struct initializer");
                        free(seen);
                        return NULL;
                    }
                    seen[idx] = true;
                    vt = infer_expr(c, AS_FIELD_INIT(init).value);
                    if (c->error) {
                        free(seen);
                        return NULL;
                    }
                    if (!unify(c, vt, ft, AS_FIELD_INIT(init).value->loc)) {
                        free(seen);
                        return NULL;
                    }
                }
                if (AS_STRUCT_INIT(n).base) {
                    Type *bt = infer_expr(c, AS_STRUCT_INIT(n).base);
                    if (c->error) {
                        free(seen);
                        return NULL;
                    }
                    if (!unify(c, bt, st_t, AS_STRUCT_INIT(n).base->loc)) {
                        free(seen);
                        return NULL;
                    }
                } else {
                    for (i = 0; i < nf; i++) {
                        if (!seen[i]) {
                            checker_fail(c, n->loc, "missing field in struct initializer");
                            free(seen);
                            return NULL;
                        }
                    }
                }
                free(seen);
                return st_t;
            }
        case NODE_LAMBDA:
            checker_fail(c, n->loc, "lambdas not type-checked yet");
            return NULL;
        default:
            checker_fail(c, n->loc, "expression form not supported in type checker yet");
            return NULL;
    }
}

static void check_stmt(Checker *c, AstNode *stmt) {
    if (c->error) {
        return;
    }
    switch (stmt->kind) {
        case NODE_LET:
            {
                Type *rhs = infer_expr(c, AS_LET(stmt).init);
                if (c->error) {
                    return;
                }
                Type *bound = rhs;
                if (AS_LET(stmt).type) {
                    Type *ann = ast_type_to_type(c, AS_LET(stmt).type);
                    if (c->error) {
                        return;
                    }
                    if (!unify(c, ann, rhs, stmt->loc)) {
                        return;
                    }
                    bound = prune(ann);
                }
                env_insert(c->current_env, AS_LET(stmt).name, bound);
                break;
            }
        case NODE_EXPR_STMT:
            (void)infer_expr(c, AS_EXPR_STMT(stmt).expr);
            break;
        case NODE_RETURN:
            if (AS_RETURN(stmt).value) {
                Type *t = infer_expr(c, AS_RETURN(stmt).value);
                if (c->error) {
                    return;
                }
                if (c->expected_return && !unify(c, t, c->expected_return, stmt->loc)) {
                    return;
                }
            } else {
                if (c->expected_return && !unify(c, ty_void, c->expected_return, stmt->loc)) {
                    return;
                }
            }
            break;
        case NODE_FOR:
            {
                Type *it = infer_expr(c, AS_FOR(stmt).iter);
                if (c->error) {
                    return;
                }
                it = prune(it);
                if (it->kind != TY_ARRAY) {
                    checker_fail(c, stmt->loc, "for-in expects an array value");
                    return;
                }
                Type *elem = it->as.inner;
                Env inner = { .head = NULL, .parent = c->current_env };
                env_insert(&inner, AS_FOR(stmt).var, elem);
                c->current_env = &inner;
                (void)infer_block(c, AS_FOR(stmt).body);
                env_free_head(&inner);
                c->current_env = inner.parent;
                break;
            }
        case NODE_TRY:
            checker_fail(c, stmt->loc, "try/catch not type-checked yet");
            break;
        default:
            checker_fail(c, stmt->loc, "statement not supported in type checker yet");
            break;
    }
}

static Type *infer_block(Checker *c, AstNode *block_node) {
    Env inner = { .head = NULL, .parent = c->current_env };
    c->current_env = &inner;

    AstList *s;
    for (s = AS_BLOCK(block_node).stmts; s; s = s->next) {
        check_stmt(c, s->item);
        if (c->error) {
            break;
        }
    }

    Type *tail_ty = ty_void;
    if (!c->error && AS_BLOCK(block_node).tail_expr) {
        tail_ty = infer_expr(c, AS_BLOCK(block_node).tail_expr);
    }

    env_free_head(&inner);
    c->current_env = inner.parent;
    return tail_ty;
}

static bool register_fn_sig(Checker *c, AstNode *fn) {
    AstList *pl;
    size_t n;
    if (env_lookup(c->global_env, AS_FN_DECL(fn).name)) {
        checker_fail(c, fn->loc, "duplicate function or type name");
        return false;
    }
    n = ast_list_len(AS_FN_DECL(fn).params);
    Type **params = (Type **)malloc(n * sizeof(Type *));
    if (!params && n > 0) {
        checker_fail(c, fn->loc, "out of memory");
        return false;
    }
    size_t i = 0;
    for (pl = AS_FN_DECL(fn).params; pl; pl = pl->next) {
        AstNode *param = pl->item;
        if (AS_PARAM(param).type == NULL) {
            free(params);
            checker_fail(c, param->loc, "parameter type required");
            return false;
        }
        params[i++] = ast_type_to_type(c, AS_PARAM(param).type);
        if (c->error) {
            free(params);
            return false;
        }
    }

    Type *ret;
    if (AS_FN_DECL(fn).ret_type) {
        ret = ast_type_to_type(c, AS_FN_DECL(fn).ret_type);
        if (c->error) {
            free(params);
            return false;
        }
    } else {
        ret = new_var(c);
    }

    Type *ft = new_fn_type(c, params, n, ret);
    if (!ft) {
        free(params);
        return false;
    }
    env_insert(c->global_env, AS_FN_DECL(fn).name, ft);
    return true;
}

/* Type-only pass: infer initializer, must match explicit annotation; no runtime eval. */
static bool register_const(Checker *c, AstNode *cd) {
    const char *nm = AS_CONST_DECL(cd).name;
    Type *ann;
    Type *rhs_ty;

    if (env_lookup(c->global_env, nm)) {
        checker_fail(c, cd->loc, "duplicate const, function, or struct name");
        return false;
    }
    ann = ast_type_to_type(c, AS_CONST_DECL(cd).type);
    if (c->error) {
        return false;
    }
    c->current_env = c->global_env;
    rhs_ty = infer_expr(c, AS_CONST_DECL(cd).value);
    if (c->error) {
        return false;
    }
    if (!unify(c, ann, rhs_ty, AS_CONST_DECL(cd).value->loc)) {
        return false;
    }
    env_insert(c->global_env, nm, prune(ann));
    return true;
}

/* Params get types from the already-registered fn signature; block type must match return. */
static bool check_fn_body(Checker *c, AstNode *fn) {
    Type *sig = env_lookup(c->global_env, AS_FN_DECL(fn).name);
    if (!sig || prune(sig)->kind != TY_FN) {
        checker_fail(c, fn->loc, "internal: missing function signature");
        return false;
    }
    sig = prune(sig);

    Env fn_env = { .head = NULL, .parent = c->global_env };
    AstList *pl;
    size_t i = 0;
    for (pl = AS_FN_DECL(fn).params; pl; pl = pl->next, i++) {
        env_insert(&fn_env, AS_PARAM(pl->item).name, sig->as.fn.params[i]);
    }

    c->current_env = &fn_env;
    c->expected_return = sig->as.fn.ret;

    Type *body_ty = infer_block(c, AS_FN_DECL(fn).body);
    if (c->error) {
        env_free_head(&fn_env);
        c->current_env = c->global_env;
        c->expected_return = NULL;
        return false;
    }
    if (!body_ty) {
        env_free_head(&fn_env);
        c->current_env = c->global_env;
        c->expected_return = NULL;
        checker_fail(c, AS_FN_DECL(fn).body->loc, "could not infer block type");
        return false;
    }

    if (!unify(c, body_ty, sig->as.fn.ret, AS_FN_DECL(fn).body->loc)) {
        env_free_head(&fn_env);
        c->current_env = c->global_env;
        c->expected_return = NULL;
        return false;
    }

    env_free_head(&fn_env);
    c->current_env = c->global_env;
    c->expected_return = NULL;
    return true;
}

/* Builds TY_STRUCT with parallel field_names / field_types; rejects duplicate top-level name. */
static bool register_struct(Checker *c, AstNode *st) {
    const char *nm = AS_STRUCT_DECL(st).name;
    Type *t;
    Type **fts;
    const char **fnames;
    AstList *fl;
    size_t nf;
    size_t i;

    if (AS_STRUCT_DECL(st).generic_params) {
        checker_fail(c, st->loc, "generic structs not supported yet");
        return false;
    }
    if (env_lookup(c->global_env, nm)) {
        checker_fail(c, st->loc, "duplicate struct or function name");
        return false;
    }

    nf = ast_list_len(AS_STRUCT_DECL(st).fields);
    fts = (Type **)malloc(nf * sizeof(Type *));
    fnames = (const char **)malloc(nf * sizeof(char *));
    if (nf > 0 && (!fts || !fnames)) {
        free(fts);
        free(fnames);
        checker_fail(c, st->loc, "out of memory");
        return false;
    }

    i = 0;
    for (fl = AS_STRUCT_DECL(st).fields; fl; fl = fl->next) {
        AstNode *sf = fl->item;
        if (AS_STRUCT_FIELD(sf).type == NULL) {
            free(fts);
            free(fnames);
            checker_fail(c, sf->loc, "struct field type required");
            return false;
        }
        fnames[i] = AS_STRUCT_FIELD(sf).name;
        fts[i] = ast_type_to_type(c, AS_STRUCT_FIELD(sf).type);
        if (c->error) {
            free(fts);
            free(fnames);
            return false;
        }
        i++;
    }

    t = alloc_type(c);
    if (!t) {
        free(fts);
        free(fnames);
        checker_fail(c, st->loc, "out of memory");
        return false;
    }
    t->kind = TY_STRUCT;
    t->as.st_def.name = nm;
    t->as.st_def.field_types = fts;
    t->as.st_def.field_names = fnames;
    t->as.st_def.nfields = nf;
    env_insert(c->global_env, nm, t);
    return true;
}

TypeCheckResult type_check_program(AstNode *program) {
    TypeCheckResult res;
    res.ok = true;
    res.error_message = NULL;
    res.error_line = 0;
    res.error_col = 0;

    if (!program || program->kind != NODE_PROGRAM) {
        res.ok = false;
        res.error_message = "internal: not a program node";
        return res;
    }

    Checker chk = {0};
    chk.global_env = (Env *)calloc(1, sizeof(Env));
    if (!chk.global_env) {
        res.ok = false;
        res.error_message = "out of memory";
        return res;
    }
    chk.current_env = chk.global_env;
    chk.program = program;

    AstList *d;
    /* Pass 1: structs only (fn/const signatures may reference struct types). */
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_STRUCT_DECL) {
            if (!register_struct(&chk, d->item)) {
                break;
            }
        }
    }
    /* Pass 2: consts (typed) and fn signatures (no bodies). */
    if (!chk.error) {
        for (d = AS_PROGRAM(program).decls; d; d = d->next) {
            if (d->item->kind == NODE_CONST_DECL) {
                if (!register_const(&chk, d->item)) {
                    break;
                }
            } else if (d->item->kind == NODE_FN_DECL) {
                if (!register_fn_sig(&chk, d->item)) {
                    break;
                }
            } else if (d->item->kind != NODE_STRUCT_DECL) {
                checker_fail(&chk, d->item->loc, "unsupported top-level declaration");
                break;
            }
        }
    }

    /* Pass 3: fn bodies (params + global parent). */
    if (!chk.error) {
        for (d = AS_PROGRAM(program).decls; d; d = d->next) {
            if (d->item->kind == NODE_FN_DECL) {
                if (!check_fn_body(&chk, d->item)) {
                    break;
                }
            }
        }
    }

    env_free_head(chk.global_env);
    free(chk.global_env);

    if (chk.error) {
        res.ok = false;
        res.error_message = chk.error;
        res.error_line = chk.err_line;
        res.error_col = chk.err_col;
    }

    checker_free_allocs(&chk);
    return res;
}
