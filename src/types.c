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
    TY_STRUCT, /* monomorphic or instantiated generic; as.st_def holds field names/types */
    TY_GENERIC_STRUCT,
    TY_GENERIC_FN
} TyKind;

typedef struct Type {
    TyKind kind;
    union {
        struct {
            int id;
            struct Type *bound;
            AstList *trait_bounds; /* NODE_IDENT trait names; NULL for plain inference vars */
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
        struct {
            AstNode *decl; /* NODE_STRUCT_DECL or NODE_FN_DECL with generic_params */
        } g_schema;
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
    int catch_try_depth; /* >0 inside try { … } that has ≥1 catch; governs `throw` */
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
    t->as.var.trait_bounds = NULL;
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
        case TY_GENERIC_STRUCT:
        case TY_GENERIC_FN:
            return false;
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

static Type *ast_type_to_type_ex(Checker *c, AstNode *n, Env *tparam_env);

static AstNode *lookup_struct_decl_node_types(AstNode *program, const char *name) {
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM || !name) {
        return NULL;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_STRUCT_DECL && strcmp(AS_STRUCT_DECL(d->item).name, name) == 0) {
            return d->item;
        }
    }
    return NULL;
}

static bool impl_exists_for_struct_trait(AstNode *program, const char *struct_name, const char *trait_name) {
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM || !struct_name || !trait_name) {
        return false;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_IMPL_DECL && AS_IMPL_DECL(d->item).trait_name &&
            strcmp(AS_IMPL_DECL(d->item).trait_name, trait_name) == 0 &&
            strcmp(AS_IMPL_DECL(d->item).struct_name, struct_name) == 0) {
            return true;
        }
    }
    return false;
}

static bool check_subst_satisfies_bounds(Checker *c, Type *concrete, AstList *bounds, SrcLoc loc) {
    AstList *b;
    concrete = prune(concrete);
    if (!bounds) {
        return true;
    }
    if (concrete->kind != TY_STRUCT) {
        checker_fail(c, loc, "trait bounds apply only when the type argument is a struct");
        return false;
    }
    for (b = bounds; b; b = b->next) {
        AstNode *bid = b->item;
        const char *trname;
        if (!bid || bid->kind != NODE_IDENT) {
            checker_fail(c, loc, "internal: trait bound");
            return false;
        }
        trname = AS_IDENT(bid).name;
        if (!impl_exists_for_struct_trait(c->program, concrete->as.st_def.name, trname)) {
            checker_fail(c, loc, "type argument does not implement required trait");
            return false;
        }
    }
    return true;
}

static bool append_mangled_type(Checker *c, char *buf, size_t cap, size_t *len, Type *t) {
    const char *tag;
    int n;
    t = prune(t);
    if (!t) {
        return false;
    }
    switch (t->kind) {
        case TY_INT:
            tag = "i";
            break;
        case TY_FLOAT:
            tag = "f";
            break;
        case TY_DOUBLE:
            tag = "d";
            break;
        case TY_BOOL:
            tag = "b";
            break;
        case TY_STRING:
            tag = "s";
            break;
        case TY_STRUCT:
            n = snprintf(buf + *len, cap > *len ? cap - *len : 0, "S%s", t->as.st_def.name);
            if (n < 0 || (size_t)n >= cap - *len) {
                checker_fail(c, (SrcLoc){0}, "type name too long for generic mangling");
                return false;
            }
            *len += (size_t)n;
            return true;
        case TY_ARRAY:
            if (*len + 2u >= cap) {
                checker_fail(c, (SrcLoc){0}, "type name too long for generic mangling");
                return false;
            }
            buf[(*len)++] = 'A';
            return append_mangled_type(c, buf, cap, len, t->as.inner);
        case TY_OPTION:
            if (*len + 2u >= cap) {
                checker_fail(c, (SrcLoc){0}, "type name too long for generic mangling");
                return false;
            }
            buf[(*len)++] = 'O';
            return append_mangled_type(c, buf, cap, len, t->as.inner);
        case TY_TUPLE:
            {
                size_t i;
                if (*len + 2u >= cap) {
                    checker_fail(c, (SrcLoc){0}, "type name too long for generic mangling");
                    return false;
                }
                buf[(*len)++] = 'T';
                buf[(*len)++] = '(';
                for (i = 0; i < t->as.tuple.nelems; i++) {
                    if (i > 0) {
                        if (*len + 1u >= cap) {
                            return false;
                        }
                        buf[(*len)++] = ',';
                    }
                    if (!append_mangled_type(c, buf, cap, len, t->as.tuple.elems[i])) {
                        return false;
                    }
                }
                if (*len + 1u >= cap) {
                    return false;
                }
                buf[(*len)++] = ')';
                return true;
            }
        default:
            checker_fail(c, (SrcLoc){0}, "type not supported in generic mangling yet");
            return false;
    }
    if (*len + 1u >= cap) {
        return false;
    }
    buf[(*len)++] = tag[0];
    return true;
}

static char *mangle_generic_struct_name(Checker *c, const char *base, Type **args, size_t narg) {
    char buf[512];
    size_t len = 0;
    size_t i;
    int w;
    w = snprintf(buf, sizeof(buf), "%s@", base);
    if (w < 0 || (size_t)w >= sizeof(buf)) {
        checker_fail(c, (SrcLoc){0}, "type name too long for generic mangling");
        return NULL;
    }
    len = (size_t)w;
    for (i = 0; i < narg; i++) {
        if (i > 0) {
            if (len + 1u >= sizeof(buf)) {
                checker_fail(c, (SrcLoc){0}, "type name too long for generic mangling");
                return NULL;
            }
            buf[len++] = ',';
        }
        if (!append_mangled_type(c, buf, sizeof(buf), &len, args[i])) {
            return NULL;
        }
    }
    buf[len] = '\0';
    {
        char *cpy = (char *)malloc(len + 1u);
        if (!cpy) {
            checker_fail(c, (SrcLoc){0}, "out of memory");
            return NULL;
        }
        memcpy(cpy, buf, len + 1u);
        return cpy;
    }
}

static Type *instantiate_generic_struct_type(Checker *c, AstNode *st_decl, AstList *type_arg_nodes, SrcLoc loc) {
    AstList *gp;
    AstList *fl;
    Type **args;
    Type **fts;
    const char **fnames;
    char *mangled;
    Type *t;
    size_t npar;
    size_t narg;
    size_t nf;
    size_t i;
    Env subst = {0};

    if (!st_decl || st_decl->kind != NODE_STRUCT_DECL || !AS_STRUCT_DECL(st_decl).generic_params) {
        checker_fail(c, loc, "internal: not a generic struct");
        return NULL;
    }
    npar = ast_list_len(AS_STRUCT_DECL(st_decl).generic_params);
    narg = ast_list_len(type_arg_nodes);
    if (npar != narg) {
        checker_fail(c, loc, "wrong number of type arguments for generic struct");
        return NULL;
    }
    args = (Type **)malloc(npar * sizeof(Type *));
    if (npar > 0 && !args) {
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    i = 0;
    for (gp = AS_STRUCT_DECL(st_decl).generic_params; gp; gp = gp->next, i++) {
        AstNode *gpn = gp->item;
        AstList *ta;
        Type *concrete;
        if (!gpn || gpn->kind != NODE_GENERIC_PARAM) {
            env_free_head(&subst);
            free(args);
            checker_fail(c, loc, "internal: generic param");
            return NULL;
        }
        ta = type_arg_nodes;
        {
            size_t j;
            for (j = 0; j < i && ta; j++) {
                ta = ta->next;
            }
        }
        if (!ta || !ta->item) {
            env_free_head(&subst);
            free(args);
            checker_fail(c, loc, "internal: type arg list");
            return NULL;
        }
        concrete = ast_type_to_type_ex(c, ta->item, NULL);
        if (c->error) {
            env_free_head(&subst);
            free(args);
            return NULL;
        }
        if (!check_subst_satisfies_bounds(c, concrete, AS_GENERIC_PARAM(gpn).bounds, loc)) {
            env_free_head(&subst);
            free(args);
            return NULL;
        }
        args[i] = concrete;
        env_insert(&subst, AS_GENERIC_PARAM(gpn).name, concrete);
    }

    nf = ast_list_len(AS_STRUCT_DECL(st_decl).fields);
    fts = (Type **)malloc(nf * sizeof(Type *));
    fnames = (const char **)malloc(nf * sizeof(char *));
    if (nf > 0 && (!fts || !fnames)) {
        env_free_head(&subst);
        free(args);
        free(fts);
        free(fnames);
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    i = 0;
    for (fl = AS_STRUCT_DECL(st_decl).fields; fl; fl = fl->next, i++) {
        AstNode *sf = fl->item;
        fnames[i] = AS_STRUCT_FIELD(sf).name;
        fts[i] = ast_type_to_type_ex(c, AS_STRUCT_FIELD(sf).type, &subst);
        if (c->error) {
            env_free_head(&subst);
            free(args);
            free(fts);
            free(fnames);
            return NULL;
        }
    }

    mangled = mangle_generic_struct_name(c, AS_STRUCT_DECL(st_decl).name, args, npar);
    free(args);
    if (!mangled || c->error) {
        env_free_head(&subst);
        free(fts);
        free(fnames);
        return NULL;
    }

    t = alloc_type(c);
    if (!t) {
        env_free_head(&subst);
        free(mangled);
        free(fts);
        free(fnames);
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    t->kind = TY_STRUCT;
    t->as.st_def.name = mangled;
    t->as.st_def.field_types = fts;
    t->as.st_def.field_names = fnames;
    t->as.st_def.nfields = nf;
    env_free_head(&subst);
    return t;
}

static Type *instantiate_generic_fn_sig(Checker *c, AstNode *fn_decl, AstList *type_arg_nodes, SrcLoc loc) {
    AstList *gp;
    AstList *pl;
    Type **params;
    Type *ret;
    Type *ft;
    Env subst = {0};
    size_t npar;
    size_t narg;
    size_t i;

    if (!fn_decl || fn_decl->kind != NODE_FN_DECL || !AS_FN_DECL(fn_decl).generic_params) {
        checker_fail(c, loc, "internal: not a generic function");
        return NULL;
    }
    npar = ast_list_len(AS_FN_DECL(fn_decl).generic_params);
    narg = ast_list_len(type_arg_nodes);
    if (npar != narg) {
        checker_fail(c, loc, "wrong number of type arguments for generic function");
        return NULL;
    }
    i = 0;
    for (gp = AS_FN_DECL(fn_decl).generic_params; gp; gp = gp->next, i++) {
        AstNode *gpn = gp->item;
        AstList *ta;
        Type *concrete;
        if (!gpn || gpn->kind != NODE_GENERIC_PARAM) {
            env_free_head(&subst);
            checker_fail(c, loc, "internal: generic param");
            return NULL;
        }
        ta = type_arg_nodes;
        {
            size_t j;
            for (j = 0; j < i && ta; j++) {
                ta = ta->next;
            }
        }
        if (!ta || !ta->item) {
            env_free_head(&subst);
            checker_fail(c, loc, "internal: type arg list");
            return NULL;
        }
        concrete = ast_type_to_type_ex(c, ta->item, NULL);
        if (c->error) {
            env_free_head(&subst);
            return NULL;
        }
        if (!check_subst_satisfies_bounds(c, concrete, AS_GENERIC_PARAM(gpn).bounds, loc)) {
            env_free_head(&subst);
            return NULL;
        }
        env_insert(&subst, AS_GENERIC_PARAM(gpn).name, concrete);
    }

    narg = ast_list_len(AS_FN_DECL(fn_decl).params);
    params = (Type **)malloc(narg * sizeof(Type *));
    if (narg > 0 && !params) {
        env_free_head(&subst);
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    i = 0;
    for (pl = AS_FN_DECL(fn_decl).params; pl; pl = pl->next, i++) {
        AstNode *param = pl->item;
        if (!AS_PARAM(param).type) {
            env_free_head(&subst);
            free(params);
            checker_fail(c, param->loc, "parameter type required");
            return NULL;
        }
        params[i] = ast_type_to_type_ex(c, AS_PARAM(param).type, &subst);
        if (c->error) {
            env_free_head(&subst);
            free(params);
            return NULL;
        }
    }
    if (AS_FN_DECL(fn_decl).ret_type) {
        ret = ast_type_to_type_ex(c, AS_FN_DECL(fn_decl).ret_type, &subst);
    } else {
        ret = new_var(c);
    }
    if (c->error) {
        env_free_head(&subst);
        free(params);
        return NULL;
    }
    ft = new_fn_type(c, params, narg, ret);
    if (!ft) {
        env_free_head(&subst);
        free(params);
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    env_free_head(&subst);
    return ft;
}

static Type *ast_type_to_type(Checker *c, AstNode *n) {
    return ast_type_to_type_ex(c, n, NULL);
}

static Type *ast_type_to_type_ex(Checker *c, AstNode *n, Env *tparam_env) {
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
                Type *el = ast_type_to_type_ex(c, AS_TYPE_ARRAY(n).elem_type, tparam_env);
                if (c->error) {
                    return NULL;
                }
                return new_array_type(c, el);
            }
        case NODE_TYPE_OPTION:
            {
                Type *in = ast_type_to_type_ex(c, AS_TYPE_OPTION(n).inner, tparam_env);
                if (c->error) {
                    return NULL;
                }
                return new_option(c, in);
            }
        case NODE_TYPE_RESULT:
            {
                Type *ok = ast_type_to_type_ex(c, AS_TYPE_RESULT(n).ok_type, tparam_env);
                Type *err = ast_type_to_type_ex(c, AS_TYPE_RESULT(n).err_type, tparam_env);
                if (c->error) {
                    return NULL;
                }
                return new_result(c, ok, err);
            }
        case NODE_TYPE_NAMED:
            {
                const char *nm = AS_TYPE_NAMED(n).name;
                Type *got;

                if (tparam_env) {
                    got = env_lookup(tparam_env, nm);
                    if (got) {
                        return got;
                    }
                }

                if (strcmp(nm, "Option") == 0) {
                    checker_fail(c, n->loc, "Option requires type arguments");
                    return NULL;
                }
                if (strcmp(nm, "Result") == 0) {
                    checker_fail(c, n->loc, "Result requires type arguments");
                    return NULL;
                }

                got = env_lookup(c->global_env, nm);
                if (!got) {
                    checker_fail(c, n->loc, "unknown type");
                    return NULL;
                }
                got = prune(got);

                if (AS_TYPE_NAMED(n).type_args) {
                    if (got->kind == TY_GENERIC_STRUCT) {
                        return instantiate_generic_struct_type(c, got->as.g_schema.decl, AS_TYPE_NAMED(n).type_args,
                                                               n->loc);
                    }
                    checker_fail(c, n->loc, "type arguments not allowed for this type");
                    return NULL;
                }
                if (got->kind == TY_GENERIC_STRUCT) {
                    checker_fail(c, n->loc, "generic struct requires type arguments");
                    return NULL;
                }
                if (got->kind == TY_GENERIC_FN) {
                    checker_fail(c, n->loc, "function type cannot be used as a struct");
                    return NULL;
                }
                if (got->kind != TY_STRUCT) {
                    checker_fail(c, n->loc, "unknown struct type");
                    return NULL;
                }
                return got;
            }
        case NODE_TYPE_SELF:
            {
                Type *got;

                if (!tparam_env) {
                    checker_fail(c, n->loc, "`Self` is only valid in trait method signatures");
                    return NULL;
                }
                got = env_lookup(tparam_env, "Self");
                if (!got) {
                    checker_fail(c, n->loc, "`Self` not bound (internal)");
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
                    buf[i++] = ast_type_to_type_ex(c, l->item, tparam_env);
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
                    params[i++] = ast_type_to_type_ex(c, pl->item, tparam_env);
                    if (c->error) {
                        free(params);
                        return NULL;
                    }
                }
                Type *ret = ast_type_to_type_ex(c, AS_TYPE_FN(n).ret_type, tparam_env);
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

/* Materialize an inferred Type* as AST type syntax (arena). Used so bytecode / tools see param types. */
static AstNode *type_to_ast_type(Checker *c, Type *t, SrcLoc loc) {
    Type *p = prune(t);
    if (!p) {
        checker_fail(c, loc, "internal: null type");
        return NULL;
    }
    switch (p->kind) {
        case TY_INT: {
            AstNode *n = ast_alloc(NODE_TYPE_PRIMITIVE, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_PRIM(n).prim = PRIM_INT;
            return n;
        }
        case TY_FLOAT: {
            AstNode *n = ast_alloc(NODE_TYPE_PRIMITIVE, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_PRIM(n).prim = PRIM_FLOAT;
            return n;
        }
        case TY_DOUBLE: {
            AstNode *n = ast_alloc(NODE_TYPE_PRIMITIVE, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_PRIM(n).prim = PRIM_DOUBLE;
            return n;
        }
        case TY_BOOL: {
            AstNode *n = ast_alloc(NODE_TYPE_PRIMITIVE, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_PRIM(n).prim = PRIM_BOOL;
            return n;
        }
        case TY_STRING: {
            AstNode *n = ast_alloc(NODE_TYPE_PRIMITIVE, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_PRIM(n).prim = PRIM_STRING;
            return n;
        }
        case TY_VOID:
            checker_fail(c, loc, "internal: void in type materialization");
            return NULL;
        case TY_VAR:
            checker_fail(c, loc, "could not fully infer type (add annotations)");
            return NULL;
        case TY_GENERIC_STRUCT:
        case TY_GENERIC_FN:
            checker_fail(c, loc, "internal: generic schema in type materialization");
            return NULL;
        case TY_STRUCT: {
            AstNode *n = ast_alloc(NODE_TYPE_NAMED, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_NAMED(n).name = p->as.st_def.name;
            AS_TYPE_NAMED(n).type_args = NULL;
            return n;
        }
        case TY_ARRAY: {
            AstNode *el = type_to_ast_type(c, p->as.inner, loc);
            if (c->error || !el) {
                return NULL;
            }
            {
                AstNode *n = ast_alloc(NODE_TYPE_ARRAY, loc);
                if (!n) {
                    checker_fail(c, loc, "out of memory");
                    return NULL;
                }
                AS_TYPE_ARRAY(n).elem_type = el;
                return n;
            }
        }
        case TY_OPTION: {
            AstNode *in = type_to_ast_type(c, p->as.inner, loc);
            if (c->error || !in) {
                return NULL;
            }
            {
                AstNode *n = ast_alloc(NODE_TYPE_OPTION, loc);
                if (!n) {
                    checker_fail(c, loc, "out of memory");
                    return NULL;
                }
                AS_TYPE_OPTION(n).inner = in;
                return n;
            }
        }
        case TY_RESULT: {
            AstNode *ok = type_to_ast_type(c, p->as.result.ok, loc);
            if (c->error || !ok) {
                return NULL;
            }
            {
                AstNode *err = type_to_ast_type(c, p->as.result.err, loc);
                AstNode *n;
                if (c->error || !err) {
                    return NULL;
                }
                n = ast_alloc(NODE_TYPE_RESULT, loc);
                if (!n) {
                    checker_fail(c, loc, "out of memory");
                    return NULL;
                }
                AS_TYPE_RESULT(n).ok_type = ok;
                AS_TYPE_RESULT(n).err_type = err;
                return n;
            }
        }
        case TY_TUPLE: {
            AstList *lst = NULL;
            size_t j;
            for (j = 0; j < p->as.tuple.nelems; j++) {
                AstNode *el = type_to_ast_type(c, p->as.tuple.elems[j], loc);
                if (c->error || !el) {
                    return NULL;
                }
                lst = ast_list_append(lst, el);
                if (!lst) {
                    checker_fail(c, loc, "out of memory");
                    return NULL;
                }
            }
            {
                AstNode *n = ast_alloc(NODE_TYPE_TUPLE, loc);
                if (!n) {
                    checker_fail(c, loc, "out of memory");
                    return NULL;
                }
                AS_TYPE_TUPLE(n).elem_types = lst;
                return n;
            }
        }
        case TY_FN: {
            AstList *ptypes = NULL;
            size_t j;
            AstNode *rt;
            AstNode *n;
            for (j = 0; j < p->as.fn.nparams; j++) {
                AstNode *pt = type_to_ast_type(c, p->as.fn.params[j], loc);
                if (c->error || !pt) {
                    return NULL;
                }
                ptypes = ast_list_append(ptypes, pt);
                if (!ptypes) {
                    checker_fail(c, loc, "out of memory");
                    return NULL;
                }
            }
            {
                Type *rpr = prune(p->as.fn.ret);
                if (rpr->kind == TY_VOID) {
                    checker_fail(c, loc, "cannot materialize function type with void return");
                    return NULL;
                }
                rt = type_to_ast_type(c, rpr, loc);
            }
            if (c->error || !rt) {
                return NULL;
            }
            n = ast_alloc(NODE_TYPE_FN, loc);
            if (!n) {
                checker_fail(c, loc, "out of memory");
                return NULL;
            }
            AS_TYPE_FN(n).param_types = ptypes;
            AS_TYPE_FN(n).ret_type = rt;
            return n;
        }
        default:
            checker_fail(c, loc, "internal: unsupported type for materialization");
            return NULL;
    }
}

static Type *infer_expr_impl(Checker *c, AstNode *n);
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

static Type *struct_field_type(Type *st, const char *fname, size_t *out_index);

static bool pat_has_wildcard(AstNode *pat) {
    if (!pat) {
        return false;
    }
    if (pat->kind == NODE_PAT_WILDCARD) {
        return true;
    }
    if (pat->kind == NODE_PAT_OR) {
        return pat_has_wildcard(AS_PAT_OR(pat).left) || pat_has_wildcard(AS_PAT_OR(pat).right);
    }
    return false;
}

static bool pat_covers_bool_true(AstNode *pat) {
    if (!pat) {
        return false;
    }
    if (pat->kind == NODE_PAT_WILDCARD || pat->kind == NODE_PAT_BIND) {
        return true;
    }
    if (pat->kind == NODE_PAT_LITERAL && AS_PAT_LIT(pat).lit &&
        AS_PAT_LIT(pat).lit->kind == NODE_LIT_BOOL && AS_LIT_BOOL(AS_PAT_LIT(pat).lit).value) {
        return true;
    }
    if (pat->kind == NODE_PAT_OR) {
        return pat_covers_bool_true(AS_PAT_OR(pat).left) || pat_covers_bool_true(AS_PAT_OR(pat).right);
    }
    return false;
}

static bool pat_covers_bool_false(AstNode *pat) {
    if (!pat) {
        return false;
    }
    if (pat->kind == NODE_PAT_WILDCARD || pat->kind == NODE_PAT_BIND) {
        return true;
    }
    if (pat->kind == NODE_PAT_LITERAL && AS_PAT_LIT(pat).lit &&
        AS_PAT_LIT(pat).lit->kind == NODE_LIT_BOOL && !AS_LIT_BOOL(AS_PAT_LIT(pat).lit).value) {
        return true;
    }
    if (pat->kind == NODE_PAT_OR) {
        return pat_covers_bool_false(AS_PAT_OR(pat).left) || pat_covers_bool_false(AS_PAT_OR(pat).right);
    }
    return false;
}

static bool pat_covers_none(AstNode *pat) {
    if (!pat) {
        return false;
    }
    if (pat->kind == NODE_PAT_WILDCARD || pat->kind == NODE_PAT_BIND) {
        return true;
    }
    if (pat->kind == NODE_PAT_LITERAL && AS_PAT_LIT(pat).lit && AS_PAT_LIT(pat).lit->kind == NODE_LIT_NONE) {
        return true;
    }
    if (pat->kind == NODE_PAT_ENUM && strcmp(AS_PAT_ENUM(pat).variant, "None") == 0) {
        return true;
    }
    if (pat->kind == NODE_PAT_OR) {
        return pat_covers_none(AS_PAT_OR(pat).left) || pat_covers_none(AS_PAT_OR(pat).right);
    }
    return false;
}

static bool pat_covers_some(AstNode *pat) {
    if (!pat) {
        return false;
    }
    if (pat->kind == NODE_PAT_WILDCARD || pat->kind == NODE_PAT_BIND) {
        return true;
    }
    if (pat->kind == NODE_PAT_ENUM && strcmp(AS_PAT_ENUM(pat).variant, "Some") == 0) {
        return true;
    }
    if (pat->kind == NODE_PAT_OR) {
        return pat_covers_some(AS_PAT_OR(pat).left) || pat_covers_some(AS_PAT_OR(pat).right);
    }
    return false;
}

static bool match_exhaustive_for_type(Checker *c, Type *sty, AstList *arms, SrcLoc loc) {
    AstList *a;
    bool wild = false;
    bool ctrue = false;
    bool cfalse = false;
    bool cnone = false;
    bool csome = false;

    sty = prune(sty);
    if (!sty) {
        checker_fail(c, loc, "internal: null match subject type");
        return false;
    }

    for (a = arms; a; a = a->next) {
        AstNode *arm = a->item;
        AstNode *pat;
        if (!arm || arm->kind != NODE_MATCH_ARM) {
            continue;
        }
        pat = AS_MATCH_ARM(arm).pattern;
        if (pat_has_wildcard(pat)) {
            wild = true;
        }
        ctrue |= pat_covers_bool_true(pat);
        cfalse |= pat_covers_bool_false(pat);
        cnone |= pat_covers_none(pat);
        csome |= pat_covers_some(pat);
    }

    if (wild) {
        return true;
    }
    if (sty->kind == TY_BOOL) {
        if (ctrue && cfalse) {
            return true;
        }
        checker_fail(c, loc, "non-exhaustive match on bool (need `_` or both true and false)");
        return false;
    }
    if (sty->kind == TY_OPTION) {
        if (cnone && csome) {
            return true;
        }
        checker_fail(c, loc, "non-exhaustive match on Option (need `_` or both None and Some arms)");
        return false;
    }
    if (sty->kind == TY_INT || sty->kind == TY_FLOAT || sty->kind == TY_DOUBLE || sty->kind == TY_STRING) {
        for (a = arms; a; a = a->next) {
            AstNode *arm = a->item;
            if (arm && arm->kind == NODE_MATCH_ARM &&
                AS_MATCH_ARM(arm).pattern && AS_MATCH_ARM(arm).pattern->kind == NODE_PAT_BIND) {
                return true;
            }
        }
        checker_fail(c, loc, "non-exhaustive match on scalar (use a binding pattern like `x` or `_`)");
        return false;
    }
    if (sty->kind == TY_STRUCT) {
        checker_fail(c, loc, "match on struct requires a wildcard `_` arm for now");
        return false;
    }
    checker_fail(c, loc, "match on this type requires a wildcard `_` arm");
    return false;
}

static bool check_pat_shape(Checker *c, Type *subj, AstNode *pat, SrcLoc loc) {
    Type *s;

    if (!pat) {
        checker_fail(c, loc, "internal: null pattern");
        return false;
    }
    s = prune(subj);
    switch (pat->kind) {
        case NODE_PAT_WILDCARD:
            return true;
        case NODE_PAT_BIND:
            return true;
        case NODE_PAT_OR:
            return check_pat_shape(c, subj, AS_PAT_OR(pat).left, loc) &&
                   check_pat_shape(c, subj, AS_PAT_OR(pat).right, loc);
        case NODE_PAT_LITERAL:
            {
                Type *lt = infer_expr(c, AS_PAT_LIT(pat).lit);
                if (c->error) {
                    return false;
                }
                return unify(c, lt, s, loc);
            }
        case NODE_PAT_ENUM:
            {
                const char *tn = AS_PAT_ENUM(pat).type_name;
                const char *vn = AS_PAT_ENUM(pat).variant;
                AstList *fl = AS_PAT_ENUM(pat).fields;
                if (!s) {
                    return false;
                }
                if (strcmp(tn, "Option") == 0) {
                    if (strcmp(vn, "None") == 0) {
                        if (fl != NULL) {
                            checker_fail(c, loc, "Option::None takes no sub-patterns");
                            return false;
                        }
                        return s->kind == TY_OPTION;
                    }
                    if (strcmp(vn, "Some") == 0) {
                        if (!fl || !fl->item || fl->next) {
                            checker_fail(c, loc, "Option::Some expects exactly one sub-pattern");
                            return false;
                        }
                        if (s->kind != TY_OPTION) {
                            checker_fail(c, loc, "Option::Some pattern needs Option subject");
                            return false;
                        }
                        return check_pat_shape(c, s->as.inner, fl->item, loc);
                    }
                    checker_fail(c, loc, "unknown Option variant in pattern");
                    return false;
                }
                checker_fail(c, loc, "match enum patterns besides Option are not type-checked yet");
                return false;
            }
        case NODE_PAT_STRUCT:
            {
                AstList *fl;
                if (!s || s->kind != TY_STRUCT) {
                    checker_fail(c, loc, "struct pattern needs struct subject");
                    return false;
                }
                if (strcmp(s->as.st_def.name, AS_PAT_STRUCT(pat).name) != 0) {
                    checker_fail(c, loc, "struct pattern name does not match subject type");
                    return false;
                }
                for (fl = AS_PAT_STRUCT(pat).field_pats; fl; fl = fl->next) {
                    AstNode *pf = fl->item;
                    Type *ft;
                    size_t ix;
                    if (!pf || pf->kind != NODE_PAT_FIELD) {
                        checker_fail(c, loc, "internal: struct pattern field");
                        return false;
                    }
                    ft = struct_field_type(s, AS_PAT_FIELD(pf).field, &ix);
                    if (!ft) {
                        checker_fail(c, loc, "unknown field in struct pattern");
                        return false;
                    }
                    if (AS_PAT_FIELD(pf).pattern) {
                        if (!check_pat_shape(c, ft, AS_PAT_FIELD(pf).pattern, loc)) {
                            return false;
                        }
                    }
                }
                return true;
            }
        case NODE_PAT_TUPLE:
        case NODE_PAT_ARRAY:
            checker_fail(c, loc, "tuple/array patterns in match not type-checked yet");
            return false;
        default:
            checker_fail(c, loc, "unsupported pattern form");
            return false;
    }
}

static void pat_bind_env(Checker *c, Env *e, Type *subj, AstNode *pat);

static bool pat_has_bind(AstNode *pat) {
    AstList *fl;

    if (!pat) {
        return false;
    }
    switch (pat->kind) {
        case NODE_PAT_BIND:
            return true;
        case NODE_PAT_OR:
            return pat_has_bind(AS_PAT_OR(pat).left) || pat_has_bind(AS_PAT_OR(pat).right);
        case NODE_PAT_ENUM:
            for (fl = AS_PAT_ENUM(pat).fields; fl; fl = fl->next) {
                if (pat_has_bind(fl->item)) {
                    return true;
                }
            }
            return false;
        case NODE_PAT_STRUCT:
            for (fl = AS_PAT_STRUCT(pat).field_pats; fl; fl = fl->next) {
                AstNode *pf = fl->item;
                if (!pf || pf->kind != NODE_PAT_FIELD) {
                    continue;
                }
                if (AS_PAT_FIELD(pf).pattern) {
                    if (pat_has_bind(AS_PAT_FIELD(pf).pattern)) {
                        return true;
                    }
                } else {
                    return true;
                }
            }
            return false;
        default:
            return false;
    }
}

static void pat_bind_env(Checker *c, Env *e, Type *subj, AstNode *pat) {
    Type *s = prune(subj);

    if (!pat || c->error) {
        return;
    }
    switch (pat->kind) {
        case NODE_PAT_WILDCARD:
            return;
        case NODE_PAT_BIND:
            env_insert(e, AS_PAT_BIND(pat).name, s);
            return;
        case NODE_PAT_OR:
            if (pat_has_bind(pat)) {
                checker_fail(c, pat->loc, "or-patterns cannot introduce bindings yet");
            }
            return;
        case NODE_PAT_LITERAL:
            return;
        case NODE_PAT_ENUM:
            if (strcmp(AS_PAT_ENUM(pat).type_name, "Option") == 0 && strcmp(AS_PAT_ENUM(pat).variant, "Some") == 0) {
                AstList *fl = AS_PAT_ENUM(pat).fields;
                if (s && s->kind == TY_OPTION && fl && fl->item && !fl->next) {
                    pat_bind_env(c, e, s->as.inner, fl->item);
                }
            }
            return;
        case NODE_PAT_STRUCT:
            {
                AstList *fl;
                for (fl = AS_PAT_STRUCT(pat).field_pats; fl && !c->error; fl = fl->next) {
                    AstNode *pf = fl->item;
                    Type *ft;
                    size_t ix;
                    if (!pf || pf->kind != NODE_PAT_FIELD) {
                        continue;
                    }
                    ft = struct_field_type(s, AS_PAT_FIELD(pf).field, &ix);
                    if (!ft) {
                        checker_fail(c, pf->loc, "unknown struct field in pattern");
                        return;
                    }
                    if (AS_PAT_FIELD(pf).pattern) {
                        pat_bind_env(c, e, ft, AS_PAT_FIELD(pf).pattern);
                    } else {
                        env_insert(e, AS_PAT_FIELD(pf).field, ft);
                    }
                }
            }
            return;
        default:
            return;
    }
}

static Type *infer_match_expr(Checker *c, AstNode *n) {
    Type *sty;
    Type *acc = NULL;
    AstList *a;
    AstNode *subj = AS_MATCH(n).subject;
    AstList *arms = AS_MATCH(n).arms;

    sty = infer_expr(c, subj);
    if (c->error) {
        return NULL;
    }
    if (!match_exhaustive_for_type(c, sty, arms, n->loc)) {
        return NULL;
    }

    for (a = arms; a && !c->error; a = a->next) {
        AstNode *arm = a->item;
        AstNode *pat;
        AstNode *guard;
        AstNode *body;
        Env arm_env = { NULL, c->current_env };
        Type *bt;

        if (!arm || arm->kind != NODE_MATCH_ARM) {
            checker_fail(c, n->loc, "internal: match arm");
            return NULL;
        }
        pat = AS_MATCH_ARM(arm).pattern;
        guard = AS_MATCH_ARM(arm).guard;
        body = AS_MATCH_ARM(arm).body;

        if (!check_pat_shape(c, sty, pat, pat->loc)) {
            env_free_head(&arm_env);
            return NULL;
        }
        pat_bind_env(c, &arm_env, sty, pat);
        if (c->error) {
            env_free_head(&arm_env);
            return NULL;
        }

        c->current_env = &arm_env;
        if (guard) {
            Type *gt = infer_expr(c, guard);
            if (c->error) {
                c->current_env = arm_env.parent;
                env_free_head(&arm_env);
                return NULL;
            }
            if (!unify(c, gt, ty_bool, guard->loc)) {
                c->current_env = arm_env.parent;
                env_free_head(&arm_env);
                return NULL;
            }
        }
        bt = infer_expr_or_block(c, body);
        c->current_env = arm_env.parent;
        env_free_head(&arm_env);
        if (c->error) {
            return NULL;
        }
        if (!acc) {
            acc = bt;
        } else if (!unify(c, acc, bt, body->loc)) {
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

/* `t.field` when `t` is `T` with trait bounds: every struct that implements *all* bounds must
 * define `field`, and field types must unify. */
static Type *bounded_typevar_field_type(Checker *c, Type *obj_t, const char *fname, SrcLoc loc) {
    AstList *d;
    AstList *b;
    Type *acc = NULL;

    obj_t = prune(obj_t);
    if (!obj_t || obj_t->kind != TY_VAR || !obj_t->as.var.trait_bounds || !fname) {
        return NULL;
    }
    for (d = AS_PROGRAM(c->program).decls; d; d = d->next) {
        const char *sname;
        Type *st;
        Type *ft;
        bool ok;

        if (d->item->kind != NODE_STRUCT_DECL) {
            continue;
        }
        sname = AS_STRUCT_DECL(d->item).name;
        ok = true;
        for (b = obj_t->as.var.trait_bounds; b; b = b->next) {
            AstNode *tid = b->item;
            if (!tid || tid->kind != NODE_IDENT) {
                ok = false;
                break;
            }
            if (!impl_exists_for_struct_trait(c->program, sname, AS_IDENT(tid).name)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        st = env_lookup(c->global_env, sname);
        if (!st) {
            continue;
        }
        st = prune(st);
        if (st->kind != TY_STRUCT) {
            continue;
        }
        ft = struct_field_type(st, fname, NULL);
        if (!ft) {
            checker_fail(c, loc,
                         "field access on bounded type parameter: field not present on every struct "
                         "that implements the bounds");
            return NULL;
        }
        if (!acc) {
            acc = ft;
        } else if (!unify(c, acc, ft, loc)) {
            return NULL;
        }
    }
    if (!acc) {
        checker_fail(c, loc,
                     "field access on bounded type parameter: no struct implements all required traits");
        return NULL;
    }
    return acc;
}

static AstNode *lookup_trait_decl(AstNode *program, const char *trait_name) {
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM || !trait_name) {
        return NULL;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_TRAIT_DECL && strcmp(AS_TRAIT_DECL(d->item).name, trait_name) == 0) {
            return d->item;
        }
    }
    return NULL;
}

static AstNode *find_trait_fn_sig(AstNode *trait_decl, const char *method_name) {
    AstList *it;
    if (!trait_decl || trait_decl->kind != NODE_TRAIT_DECL || !method_name) {
        return NULL;
    }
    for (it = AS_TRAIT_DECL(trait_decl).items; it; it = it->next) {
        if (it->item && it->item->kind == NODE_TRAIT_FN_SIG &&
            strcmp(AS_TRAIT_FN_SIG(it->item).name, method_name) == 0) {
            return it->item;
        }
    }
    return NULL;
}

/* Compare trait type syntax to impl type syntax; `Self` in the trait side matches `impl_struct` on the impl side. */
static bool type_ast_equal_sub_self(const AstNode *trait_ty, const AstNode *impl_ty, const char *impl_struct) {
    if (!trait_ty && !impl_ty) {
        return true;
    }
    if (!trait_ty || !impl_ty) {
        return false;
    }
    if (trait_ty->kind == NODE_TYPE_SELF && impl_ty->kind == NODE_TYPE_SELF) {
        return true;
    }
    if (trait_ty->kind == NODE_TYPE_SELF) {
        if (!impl_struct || impl_ty->kind != NODE_TYPE_NAMED) {
            return false;
        }
        return strcmp(AS_TYPE_NAMED(impl_ty).name, impl_struct) == 0;
    }
    if (trait_ty->kind != impl_ty->kind) {
        return false;
    }
    switch (trait_ty->kind) {
        case NODE_TYPE_PRIMITIVE:
            return AS_TYPE_PRIM(trait_ty).prim == AS_TYPE_PRIM(impl_ty).prim;
        case NODE_TYPE_NAMED:
            {
                AstList *la;
                AstList *lb;
                if (strcmp(AS_TYPE_NAMED(trait_ty).name, AS_TYPE_NAMED(impl_ty).name) != 0) {
                    return false;
                }
                la = AS_TYPE_NAMED(trait_ty).type_args;
                lb = AS_TYPE_NAMED(impl_ty).type_args;
                for (; la && lb; la = la->next, lb = lb->next) {
                    if (!type_ast_equal_sub_self(la->item, lb->item, impl_struct)) {
                        return false;
                    }
                }
                return la == NULL && lb == NULL;
            }
        case NODE_TYPE_ARRAY:
            return type_ast_equal_sub_self(AS_TYPE_ARRAY(trait_ty).elem_type, AS_TYPE_ARRAY(impl_ty).elem_type,
                                           impl_struct);
        case NODE_TYPE_OPTION:
            return type_ast_equal_sub_self(AS_TYPE_OPTION(trait_ty).inner, AS_TYPE_OPTION(impl_ty).inner,
                                           impl_struct);
        case NODE_TYPE_RESULT:
            return type_ast_equal_sub_self(AS_TYPE_RESULT(trait_ty).ok_type, AS_TYPE_RESULT(impl_ty).ok_type,
                                           impl_struct) &&
                   type_ast_equal_sub_self(AS_TYPE_RESULT(trait_ty).err_type, AS_TYPE_RESULT(impl_ty).err_type,
                                           impl_struct);
        case NODE_TYPE_REF:
            return AS_TYPE_REF(trait_ty).is_mut == AS_TYPE_REF(impl_ty).is_mut &&
                   type_ast_equal_sub_self(AS_TYPE_REF(trait_ty).inner, AS_TYPE_REF(impl_ty).inner, impl_struct);
        case NODE_TYPE_TUPLE:
            {
                AstList *la = AS_TYPE_TUPLE(trait_ty).elem_types;
                AstList *lb = AS_TYPE_TUPLE(impl_ty).elem_types;
                for (; la && lb; la = la->next, lb = lb->next) {
                    if (!type_ast_equal_sub_self(la->item, lb->item, impl_struct)) {
                        return false;
                    }
                }
                return la == NULL && lb == NULL;
            }
        case NODE_TYPE_FN:
            {
                AstList *la = AS_TYPE_FN(trait_ty).param_types;
                AstList *lb = AS_TYPE_FN(impl_ty).param_types;
                for (; la && lb; la = la->next, lb = lb->next) {
                    if (!type_ast_equal_sub_self(la->item, lb->item, impl_struct)) {
                        return false;
                    }
                }
                if (la != NULL || lb != NULL) {
                    return false;
                }
                return type_ast_equal_sub_self(AS_TYPE_FN(trait_ty).ret_type, AS_TYPE_FN(impl_ty).ret_type,
                                               impl_struct);
            }
        default:
            return false;
    }
}

static const char *impl_fn_self_struct_type_name(AstNode *fn) {
    AstNode *p0;
    if (!fn || fn->kind != NODE_FN_DECL || !AS_FN_DECL(fn).params || !AS_FN_DECL(fn).params->item) {
        return NULL;
    }
    p0 = AS_FN_DECL(fn).params->item;
    if (strcmp(AS_PARAM(p0).name, "self") != 0 || !AS_PARAM(p0).type) {
        return NULL;
    }
    if (AS_PARAM(p0).type->kind != NODE_TYPE_NAMED) {
        return NULL;
    }
    return AS_TYPE_NAMED(AS_PARAM(p0).type).name;
}

static bool type_ast_equal(const AstNode *a, const AstNode *b) {
    if (!a && !b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case NODE_TYPE_SELF:
            return true;
        case NODE_TYPE_PRIMITIVE:
            return AS_TYPE_PRIM(a).prim == AS_TYPE_PRIM(b).prim;
        case NODE_TYPE_NAMED:
            {
                AstList *la;
                AstList *lb;
                if (strcmp(AS_TYPE_NAMED(a).name, AS_TYPE_NAMED(b).name) != 0) {
                    return false;
                }
                la = AS_TYPE_NAMED(a).type_args;
                lb = AS_TYPE_NAMED(b).type_args;
                for (; la && lb; la = la->next, lb = lb->next) {
                    if (!type_ast_equal(la->item, lb->item)) {
                        return false;
                    }
                }
                return la == NULL && lb == NULL;
            }
        case NODE_TYPE_ARRAY:
            return type_ast_equal(AS_TYPE_ARRAY(a).elem_type, AS_TYPE_ARRAY(b).elem_type);
        case NODE_TYPE_OPTION:
            return type_ast_equal(AS_TYPE_OPTION(a).inner, AS_TYPE_OPTION(b).inner);
        case NODE_TYPE_REF:
            return AS_TYPE_REF(a).is_mut == AS_TYPE_REF(b).is_mut &&
                   type_ast_equal(AS_TYPE_REF(a).inner, AS_TYPE_REF(b).inner);
        case NODE_TYPE_TUPLE:
            {
                AstList *la = AS_TYPE_TUPLE(a).elem_types;
                AstList *lb = AS_TYPE_TUPLE(b).elem_types;
                for (; la && lb; la = la->next, lb = lb->next) {
                    if (!type_ast_equal(la->item, lb->item)) {
                        return false;
                    }
                }
                return la == NULL && lb == NULL;
            }
        default:
            return false;
    }
}

static bool trait_sig_matches_impl_fn(AstNode *sig, AstNode *fn) {
    AstList *ps;
    AstList *pf;
    const char *impl_st;
    if (!sig || sig->kind != NODE_TRAIT_FN_SIG || !fn || fn->kind != NODE_FN_DECL) {
        return false;
    }
    impl_st = impl_fn_self_struct_type_name(fn);
    if (!impl_st) {
        return false;
    }
    if (ast_list_len(AS_TRAIT_FN_SIG(sig).params) != ast_list_len(AS_FN_DECL(fn).params)) {
        return false;
    }
    for (ps = AS_TRAIT_FN_SIG(sig).params, pf = AS_FN_DECL(fn).params; ps && pf;
         ps = ps->next, pf = pf->next) {
        AstNode *sp = ps->item;
        AstNode *fp = pf->item;
        if (!sp || sp->kind != NODE_PARAM || !fp || fp->kind != NODE_PARAM) {
            return false;
        }
        if (strcmp(AS_PARAM(sp).name, AS_PARAM(fp).name) != 0) {
            return false;
        }
        if (!type_ast_equal_sub_self(AS_PARAM(sp).type, AS_PARAM(fp).type, impl_st)) {
            return false;
        }
    }
    return type_ast_equal_sub_self(AS_TRAIT_FN_SIG(sig).ret_type, AS_FN_DECL(fn).ret_type, impl_st);
}

static bool method_receiver_struct_matches(Type *recv_st, AstNode *fn) {
    AstNode *p0;
    recv_st = prune(recv_st);
    if (!fn || fn->kind != NODE_FN_DECL || !AS_FN_DECL(fn).params || !AS_FN_DECL(fn).params->item) {
        return false;
    }
    p0 = AS_FN_DECL(fn).params->item;
    if (strcmp(AS_PARAM(p0).name, "self") != 0 || !AS_PARAM(p0).type) {
        return false;
    }
    if (recv_st->kind != TY_STRUCT) {
        return false;
    }
    if (AS_PARAM(p0).type->kind == NODE_TYPE_NAMED) {
        return strcmp(AS_TYPE_NAMED(AS_PARAM(p0).type).name, recv_st->as.st_def.name) == 0;
    }
    return false;
}

static AstNode *lookup_fn_decl_for_method(Checker *c, const char *method_name, Type *recv_t) {
    AstList *d;
    AstList *m;
    recv_t = prune(recv_t);
    if (!c->program || c->program->kind != NODE_PROGRAM || !method_name) {
        return NULL;
    }
    for (d = AS_PROGRAM(c->program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL && strcmp(AS_FN_DECL(d->item).name, method_name) == 0) {
            if (method_receiver_struct_matches(recv_t, d->item)) {
                return d->item;
            }
        }
    }
    for (d = AS_PROGRAM(c->program).decls; d; d = d->next) {
        if (d->item->kind != NODE_IMPL_DECL) {
            continue;
        }
        for (m = AS_IMPL_DECL(d->item).methods; m; m = m->next) {
            AstNode *fn = m->item;
            if (fn && fn->kind == NODE_FN_DECL && strcmp(AS_FN_DECL(fn).name, method_name) == 0) {
                if (method_receiver_struct_matches(recv_t, fn)) {
                    return fn;
                }
            }
        }
    }
    return NULL;
}

/* Bounded type parameter: find a trait item that declares `method_name` (same trait may repeat in bounds). */
static AstNode *find_trait_method_sig_for_typevar(Checker *c, AstList *trait_bounds, const char *method_name,
                                                  SrcLoc loc) {
    AstList *b;
    AstNode *first_sig = NULL;
    AstNode *first_trait = NULL;

    if (!trait_bounds || !method_name) {
        return NULL;
    }
    for (b = trait_bounds; b; b = b->next) {
        AstNode *tid = b->item;
        AstNode *tdecl;
        AstNode *sig;
        if (!tid || tid->kind != NODE_IDENT) {
            continue;
        }
        tdecl = lookup_trait_decl(c->program, AS_IDENT(tid).name);
        if (!tdecl) {
            continue;
        }
        sig = find_trait_fn_sig(tdecl, method_name);
        if (!sig) {
            continue;
        }
        if (first_sig == NULL) {
            first_sig = sig;
            first_trait = tdecl;
        } else if (first_trait != tdecl) {
            checker_fail(c, loc,
                         "ambiguous method on type parameter: multiple traits declare this method");
            return NULL;
        }
    }
    (void)first_trait;
    return first_sig;
}

/* Instantiate trait method types by replacing the trait self struct name with recv_subst (the type var T). */
static Type *fn_type_from_trait_method_sig(Checker *c, AstNode *sig, Type *recv_subst, SrcLoc loc) {
    AstList *pl;
    Env subst = {0};
    Type **params;
    size_t n;
    size_t j;
    Type *ret;
    Type *ft;
    AstNode *p0;

    if (!sig || sig->kind != NODE_TRAIT_FN_SIG || !recv_subst) {
        checker_fail(c, loc, "internal: trait method type");
        return NULL;
    }
    if (!AS_TRAIT_FN_SIG(sig).params || !AS_TRAIT_FN_SIG(sig).params->item) {
        checker_fail(c, loc, "internal: trait method params");
        return NULL;
    }
    p0 = AS_TRAIT_FN_SIG(sig).params->item;
    if (!p0 || p0->kind != NODE_PARAM || !AS_PARAM(p0).type) {
        checker_fail(c, loc, "internal: trait method self parameter");
        return NULL;
    }
    if (AS_PARAM(p0).type->kind == NODE_TYPE_SELF) {
        env_insert(&subst, "Self", recv_subst);
    } else if (AS_PARAM(p0).type->kind == NODE_TYPE_NAMED) {
        const char *selfnm = AS_TYPE_NAMED(AS_PARAM(p0).type).name;
        env_insert(&subst, selfnm, recv_subst);
    } else {
        checker_fail(c, loc, "trait method self type must be `Self` or a named struct type");
        return NULL;
    }

    n = ast_list_len(AS_TRAIT_FN_SIG(sig).params);
    params = (Type **)malloc(n * sizeof(Type *));
    if (n > 0 && !params) {
        env_free_head(&subst);
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    j = 0;
    for (pl = AS_TRAIT_FN_SIG(sig).params; pl; pl = pl->next, j++) {
        AstNode *param = pl->item;
        if (!AS_PARAM(param).type) {
            env_free_head(&subst);
            free(params);
            checker_fail(c, loc, "trait method parameter needs a type");
            return NULL;
        }
        params[j] = ast_type_to_type_ex(c, AS_PARAM(param).type, &subst);
        if (c->error) {
            env_free_head(&subst);
            free(params);
            return NULL;
        }
    }
    ret = ast_type_to_type_ex(c, AS_TRAIT_FN_SIG(sig).ret_type, &subst);
    env_free_head(&subst);
    if (c->error) {
        free(params);
        return NULL;
    }
    ft = new_fn_type(c, params, n, ret);
    if (!ft) {
        free(params);
        checker_fail(c, loc, "out of memory");
        return NULL;
    }
    return ft;
}

static bool verify_resolved_generic_params(Checker *c, AstNode *fn, Type **rigid_vars, size_t ngp) {
    AstList *gp;
    size_t i;

    if (!fn || fn->kind != NODE_FN_DECL) {
        return false;
    }
    for (gp = AS_FN_DECL(fn).generic_params, i = 0; gp && i < ngp; gp = gp->next, i++) {
        AstNode *gpn = gp->item;
        Type *pt;

        if (!gpn || gpn->kind != NODE_GENERIC_PARAM) {
            checker_fail(c, fn->loc, "internal: generic parameter");
            return false;
        }
        pt = prune(rigid_vars[i]);
        if (!pt) {
            checker_fail(c, fn->loc, "internal: generic type parameter");
            return false;
        }
        if (pt->kind == TY_VAR) {
            /* Still abstract after body: ok for `T: Trait` (witnesses checked at `f::<U>(...)`). */
            if (pt->as.var.trait_bounds) {
                continue;
            }
            /* Unconstrained `T` may stay a var if the parameter is unused in the body. */
            if (!AS_GENERIC_PARAM(gpn).bounds) {
                continue;
            }
            checker_fail(c, fn->loc, "type parameter could not be fully determined (add annotations or uses)");
            return false;
        }
        if (AS_GENERIC_PARAM(gpn).bounds &&
            !check_subst_satisfies_bounds(c, pt, AS_GENERIC_PARAM(gpn).bounds, gpn->loc)) {
            return false;
        }
    }
    return true;
}

static void assign_bc_tag(AstNode *n, Type *t) {
    if (!n) {
        return;
    }
    n->bc_ty = AST_BC_TY_NONE;
    if (!t) {
        return;
    }
    switch (t->kind) {
        case TY_INT:
            n->bc_ty = AST_BC_TY_INT;
            break;
        case TY_FLOAT:
            n->bc_ty = AST_BC_TY_FLOAT;
            break;
        case TY_DOUBLE:
            n->bc_ty = AST_BC_TY_DOUBLE;
            break;
        case TY_BOOL:
            n->bc_ty = AST_BC_TY_BOOL;
            break;
        default:
            break;
    }
}

static Type *infer_expr(Checker *c, AstNode *n) {
    Type *t = infer_expr_impl(c, n);
    if (!c->error && n && t) {
        assign_bc_tag(n, prune(t));
    }
    return t;
}

static Type *infer_expr_impl(Checker *c, AstNode *n) {
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
                AstNode *cal = AS_CALL(n).callee;
                if (cal && cal->kind == NODE_IDENT && AS_CALL(n).type_args) {
                    Type *gt = env_lookup(c->global_env, AS_IDENT(cal).name);
                    gt = gt ? prune(gt) : NULL;
                    if (gt && gt->kind == TY_GENERIC_FN) {
                        Type *cft = instantiate_generic_fn_sig(c, gt->as.g_schema.decl, AS_CALL(n).type_args, n->loc);
                        AstList *arg;
                        size_t i = 0;
                        if (c->error || !cft) {
                            return NULL;
                        }
                        for (arg = AS_CALL(n).args; arg; arg = arg->next, i++) {
                            if (i >= cft->as.fn.nparams) {
                                checker_fail(c, n->loc, "too many arguments");
                                return NULL;
                            }
                            {
                                Type *at = infer_expr(c, arg->item);
                                if (c->error) {
                                    return NULL;
                                }
                                if (!unify(c, at, cft->as.fn.params[i], arg->item->loc)) {
                                    return NULL;
                                }
                            }
                        }
                        if (i != cft->as.fn.nparams) {
                            checker_fail(c, n->loc, "too few arguments");
                            return NULL;
                        }
                        return cft->as.fn.ret;
                    }
                    if (gt && gt->kind != TY_GENERIC_FN) {
                        checker_fail(c, n->loc, "type arguments only apply to generic functions");
                        return NULL;
                    }
                }

                {
                    Type *callee_t = infer_expr(c, cal);
                    if (c->error) {
                        return NULL;
                    }
                    callee_t = prune(callee_t);
                    if (callee_t->kind == TY_GENERIC_FN) {
                        checker_fail(c, n->loc, "generic function requires explicit type arguments (name::<T>(...))");
                        return NULL;
                    }
                    if (callee_t->kind != TY_FN) {
                        checker_fail(c, n->loc, "call target is not a function");
                        return NULL;
                    }
                    {
                        AstList *arg;
                        size_t i = 0;
                        for (arg = AS_CALL(n).args; arg; arg = arg->next, i++) {
                            if (i >= callee_t->as.fn.nparams) {
                                checker_fail(c, n->loc, "too many arguments");
                                return NULL;
                            }
                            {
                                Type *at = infer_expr(c, arg->item);
                                if (c->error) {
                                    return NULL;
                                }
                                if (!unify(c, at, callee_t->as.fn.params[i], arg->item->loc)) {
                                    return NULL;
                                }
                            }
                        }
                        if (i != callee_t->as.fn.nparams) {
                            checker_fail(c, n->loc, "too few arguments");
                            return NULL;
                        }
                        return callee_t->as.fn.ret;
                    }
                }
            }
        /* Same as eval: top-level fn named `method`, first param must be `self` = receiver type. */
        case NODE_METHOD_CALL:
            {
                Type *recv_t = infer_expr(c, AS_METHOD_CALL(n).receiver);
                Type *recv_p;
                AstNode *fn_ast;
                Type *ft;
                AstList *arg;
                size_t j;
                size_t nargs;
                AstNode *trait_sig;

                if (c->error) {
                    return NULL;
                }
                recv_p = prune(recv_t);
                if (recv_p->kind == TY_VAR && recv_p->as.var.trait_bounds) {
                    trait_sig = find_trait_method_sig_for_typevar(c, recv_p->as.var.trait_bounds, AS_METHOD_CALL(n).method,
                                                                  n->loc);
                    if (c->error) {
                        return NULL;
                    }
                    if (!trait_sig) {
                        checker_fail(c, n->loc, "unknown method for bounded type parameter");
                        return NULL;
                    }
                    ft = fn_type_from_trait_method_sig(c, trait_sig, recv_p, n->loc);
                    if (c->error || !ft) {
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
                    AS_METHOD_CALL(n).resolved_fn = NULL;
                    return ft->as.fn.ret;
                }

                fn_ast = lookup_fn_decl_for_method(c, AS_METHOD_CALL(n).method, recv_t);
                if (!fn_ast) {
                    checker_fail(c, n->loc, "unknown method");
                    return NULL;
                }
                AS_METHOD_CALL(n).resolved_fn = fn_ast;
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
                if (!ft && obj_t->kind == TY_VAR && obj_t->as.var.trait_bounds) {
                    ft = bounded_typevar_field_type(c, obj_t, AS_FIELD_ACCESS(n).field, n->loc);
                }
                if (!ft) {
                    if (!c->error) {
                        checker_fail(c, n->loc, "unknown field or not a struct value");
                    }
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
        case NODE_MATCH:
            return infer_match_expr(c, n);
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
                AstNode *decl;
                AstList *fi;
                bool *seen;
                size_t i;
                size_t nf;
                st_t = st_t ? prune(st_t) : NULL;
                decl = lookup_struct_decl_node_types(c->program, sname);
                if (!decl) {
                    checker_fail(c, n->loc, "unknown struct type in initializer");
                    return NULL;
                }
                if (AS_STRUCT_INIT(n).type_args) {
                    if (!AS_STRUCT_DECL(decl).generic_params) {
                        checker_fail(c, n->loc, "type arguments not allowed for non-generic struct");
                        return NULL;
                    }
                    st_t = instantiate_generic_struct_type(c, decl, AS_STRUCT_INIT(n).type_args, n->loc);
                } else {
                    if (AS_STRUCT_DECL(decl).generic_params) {
                        checker_fail(c, n->loc, "generic struct initializer requires type arguments (Type::<T> { ... })");
                        return NULL;
                    }
                    if (!st_t || st_t->kind != TY_STRUCT) {
                        checker_fail(c, n->loc, "unknown struct type in initializer");
                        return NULL;
                    }
                }
                if (c->error || !st_t || st_t->kind != TY_STRUCT) {
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
            {
                AstNode *lam = n;
                Env lam_env = { .head = NULL, .parent = c->current_env };
                AstList *pl;
                size_t nparam = ast_list_len(AS_LAMBDA(lam).params);
                Type **pts;
                size_t i;
                Type *ret_goal;
                Type *body_ty;
                Type *ft;
                Type *saved_exp = c->expected_return;

                pts = nparam ? (Type **)malloc(nparam * sizeof(Type *)) : NULL;
                if (nparam > 0 && !pts) {
                    checker_fail(c, lam->loc, "out of memory");
                    return NULL;
                }
                i = 0;
                for (pl = AS_LAMBDA(lam).params; pl; pl = pl->next, i++) {
                    AstNode *param = pl->item;
                    Type *pt;
                    if (AS_PARAM(param).type) {
                        pt = ast_type_to_type(c, AS_PARAM(param).type);
                        if (c->error) {
                            free(pts);
                            return NULL;
                        }
                    } else {
                        pt = new_var(c);
                        if (c->error) {
                            free(pts);
                            return NULL;
                        }
                    }
                    env_insert(&lam_env, AS_PARAM(param).name, pt);
                    pts[i] = pt;
                }

                if (AS_LAMBDA(lam).ret_type) {
                    ret_goal = ast_type_to_type(c, AS_LAMBDA(lam).ret_type);
                    if (c->error) {
                        env_free_head(&lam_env);
                        free(pts);
                        return NULL;
                    }
                } else {
                    ret_goal = new_var(c);
                    if (c->error) {
                        env_free_head(&lam_env);
                        free(pts);
                        return NULL;
                    }
                }

                c->current_env = &lam_env;
                c->expected_return = ret_goal;

                if (AS_LAMBDA(lam).body->kind == NODE_BLOCK) {
                    body_ty = infer_block(c, AS_LAMBDA(lam).body);
                } else {
                    body_ty = infer_expr(c, AS_LAMBDA(lam).body);
                }

                c->expected_return = saved_exp;
                c->current_env = lam_env.parent;

                if (c->error) {
                    env_free_head(&lam_env);
                    free(pts);
                    return NULL;
                }
                if (!body_ty) {
                    env_free_head(&lam_env);
                    free(pts);
                    checker_fail(c, AS_LAMBDA(lam).body->loc, "could not infer lambda body type");
                    return NULL;
                }
                if (!unify(c, body_ty, ret_goal, AS_LAMBDA(lam).body->loc)) {
                    env_free_head(&lam_env);
                    free(pts);
                    return NULL;
                }

                for (pl = AS_LAMBDA(lam).params, i = 0; pl; pl = pl->next, i++) {
                    AstNode *param = pl->item;
                    if (AS_PARAM(param).type == NULL) {
                        AstNode *ann = type_to_ast_type(c, pts[i], param->loc);
                        if (c->error || !ann) {
                            env_free_head(&lam_env);
                            free(pts);
                            return NULL;
                        }
                        AS_PARAM(param).type = ann;
                    }
                }
                if (!AS_LAMBDA(lam).ret_type) {
                    Type *prt = prune(ret_goal);
                    if (prt && prt->kind != TY_VOID) {
                        AstNode *rt = type_to_ast_type(c, prt, lam->loc);
                        if (c->error || !rt) {
                            env_free_head(&lam_env);
                            free(pts);
                            return NULL;
                        }
                        AS_LAMBDA(lam).ret_type = rt;
                    }
                }

                env_free_head(&lam_env);

                ft = new_fn_type(c, pts, nparam, prune(ret_goal));
                if (!ft) {
                    free(pts);
                    checker_fail(c, lam->loc, "out of memory");
                    return NULL;
                }
                return ft;
            }
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
                if (it->kind == TY_ARRAY) {
                    Type *elem = it->as.inner;
                    Env inner = { .head = NULL, .parent = c->current_env };
                    env_insert(&inner, AS_FOR(stmt).var, elem);
                    c->current_env = &inner;
                    (void)infer_block(c, AS_FOR(stmt).body);
                    env_free_head(&inner);
                    c->current_env = inner.parent;
                    break;
                }
                if (it->kind == TY_TUPLE) {
                    size_t i;
                    Type *elem;
                    if (it->as.tuple.nelems == 0) {
                        checker_fail(c, stmt->loc, "for-in over empty tuple");
                        return;
                    }
                    elem = prune(it->as.tuple.elems[0]);
                    for (i = 1; i < it->as.tuple.nelems; i++) {
                        if (!unify(c, elem, it->as.tuple.elems[i], stmt->loc)) {
                            return;
                        }
                        elem = prune(elem);
                    }
                    {
                        Env inner = { .head = NULL, .parent = c->current_env };
                        env_insert(&inner, AS_FOR(stmt).var, elem);
                        c->current_env = &inner;
                        (void)infer_block(c, AS_FOR(stmt).body);
                        env_free_head(&inner);
                        c->current_env = inner.parent;
                    }
                    break;
                }
                checker_fail(c, stmt->loc, "for-in expects an array or homogeneous tuple");
                return;
            }
        case NODE_TRY:
            {
                AstList *cl;
                size_t ncatch = ast_list_len(AS_TRY(stmt).catch_clauses);
                int bump = ncatch > 0 ? 1 : 0;

                if (bump) {
                    c->catch_try_depth++;
                }
                (void)infer_block(c, AS_TRY(stmt).body);
                if (c->error) {
                    if (bump) {
                        c->catch_try_depth--;
                    }
                    return;
                }
                for (cl = AS_TRY(stmt).catch_clauses; cl; cl = cl->next) {
                    AstNode *cn = cl->item;
                    Type *ct;

                    if (!cn || cn->kind != NODE_CATCH_CLAUSE) {
                        continue;
                    }
                    if (!AS_CATCH(cn).type || AS_CATCH(cn).type->kind != NODE_TYPE_PRIMITIVE) {
                        checker_fail(c, cn->loc, "catch type must be a primitive type for now");
                        if (bump) {
                            c->catch_try_depth--;
                        }
                        return;
                    }
                    ct = ast_type_to_type(c, AS_CATCH(cn).type);
                    if (c->error) {
                        if (bump) {
                            c->catch_try_depth--;
                        }
                        return;
                    }
                    {
                        Env inner = { NULL, c->current_env };
                        env_insert(&inner, AS_CATCH(cn).var, ct);
                        c->current_env = &inner;
                        (void)infer_block(c, AS_CATCH(cn).body);
                        env_free_head(&inner);
                        c->current_env = inner.parent;
                    }
                    if (c->error) {
                        if (bump) {
                            c->catch_try_depth--;
                        }
                        return;
                    }
                }
                if (AS_TRY(stmt).finally_body) {
                    (void)infer_block(c, AS_TRY(stmt).finally_body);
                }
                if (bump) {
                    c->catch_try_depth--;
                }
                break;
            }
        case NODE_THROW:
            if (c->catch_try_depth == 0) {
                checker_fail(c, stmt->loc, "`throw` requires an enclosing `try` with at least one `catch` clause");
                return;
            }
            (void)infer_expr(c, AS_THROW(stmt).expr);
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
    Type *from_returns = NULL;

    for (s = AS_BLOCK(block_node).stmts; s; s = s->next) {
        check_stmt(c, s->item);
        if (c->error) {
            break;
        }
        if (s->item->kind == NODE_RETURN && AS_RETURN(s->item).value) {
            Type *rt = infer_expr(c, AS_RETURN(s->item).value);
            if (c->error) {
                break;
            }
            if (!from_returns) {
                from_returns = rt;
            } else if (!unify(c, from_returns, rt, AS_RETURN(s->item).value->loc)) {
                break;
            }
        }
    }

    if (!c->error && AS_BLOCK(block_node).tail_expr) {
        Type *tail_ty = infer_expr(c, AS_BLOCK(block_node).tail_expr);
        if (c->error) {
            env_free_head(&inner);
            c->current_env = inner.parent;
            return NULL;
        }
        if (from_returns) {
            if (!unify(c, from_returns, tail_ty, AS_BLOCK(block_node).tail_expr->loc)) {
                env_free_head(&inner);
                c->current_env = inner.parent;
                return NULL;
            }
        } else {
            from_returns = tail_ty;
        }
    }

    env_free_head(&inner);
    c->current_env = inner.parent;
    if (c->error) {
        return NULL;
    }
    return from_returns ? from_returns : ty_void;
}

static bool register_fn_sig(Checker *c, AstNode *fn) {
    AstList *pl;
    AstList *gp;
    size_t n;
    Type *gty;
    if (env_lookup(c->global_env, AS_FN_DECL(fn).name)) {
        checker_fail(c, fn->loc, "duplicate function or type name");
        return false;
    }

    if (AS_FN_DECL(fn).generic_params) {
        Env tenv = {0};
        for (gp = AS_FN_DECL(fn).generic_params; gp; gp = gp->next) {
            AstNode *gpn = gp->item;
            Type *tv;
            if (!gpn || gpn->kind != NODE_GENERIC_PARAM) {
                env_free_head(&tenv);
                checker_fail(c, fn->loc, "internal: generic parameter");
                return false;
            }
            tv = new_var(c);
            if (!tv) {
                env_free_head(&tenv);
                checker_fail(c, fn->loc, "out of memory");
                return false;
            }
            env_insert(&tenv, AS_GENERIC_PARAM(gpn).name, tv);
        }
        for (pl = AS_FN_DECL(fn).params; pl; pl = pl->next) {
            AstNode *param = pl->item;
            if (!AS_PARAM(param).type) {
                env_free_head(&tenv);
                checker_fail(c, param->loc, "parameter type required");
                return false;
            }
            (void)ast_type_to_type_ex(c, AS_PARAM(param).type, &tenv);
            if (c->error) {
                env_free_head(&tenv);
                return false;
            }
        }
        if (AS_FN_DECL(fn).ret_type) {
            (void)ast_type_to_type_ex(c, AS_FN_DECL(fn).ret_type, &tenv);
            if (c->error) {
                env_free_head(&tenv);
                return false;
            }
        }
        env_free_head(&tenv);

        gty = alloc_type(c);
        if (!gty) {
            checker_fail(c, fn->loc, "out of memory");
            return false;
        }
        gty->kind = TY_GENERIC_FN;
        gty->as.g_schema.decl = fn;
        env_insert(c->global_env, AS_FN_DECL(fn).name, gty);
        return true;
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

static bool register_trait_decl(Checker *c, AstNode *tr) {
    const char *nm = AS_TRAIT_DECL(tr).name;
    AstList *d;
    AstList *it;
    int n_dup = 0;

    if (!tr || tr->kind != NODE_TRAIT_DECL) {
        checker_fail(c, (SrcLoc){0}, "internal: trait decl");
        return false;
    }
    for (d = AS_PROGRAM(c->program).decls; d; d = d->next) {
        if (d->item->kind == NODE_TRAIT_DECL && strcmp(AS_TRAIT_DECL(d->item).name, nm) == 0) {
            n_dup++;
        }
    }
    if (n_dup > 1) {
        checker_fail(c, tr->loc, "duplicate trait declaration");
        return false;
    }
    if (env_lookup(c->global_env, nm)) {
        checker_fail(c, tr->loc, "trait name conflicts with struct, function, or const");
        return false;
    }
    if (AS_TRAIT_DECL(tr).generic_params) {
        checker_fail(c, tr->loc, "generic traits are not supported yet");
        return false;
    }
    if (AS_TRAIT_DECL(tr).super_traits) {
        checker_fail(c, tr->loc, "trait super-bounds are not supported yet");
        return false;
    }
    for (it = AS_TRAIT_DECL(tr).items; it; it = it->next) {
        AstNode *sig = it->item;
        AstNode *p0;
        if (!sig || sig->kind != NODE_TRAIT_FN_SIG) {
            checker_fail(c, tr->loc, "trait body must contain fn signatures only");
            return false;
        }
        if (!AS_TRAIT_FN_SIG(sig).params || !AS_TRAIT_FN_SIG(sig).params->item) {
            checker_fail(c, sig->loc, "trait method needs at least a self parameter");
            return false;
        }
        p0 = AS_TRAIT_FN_SIG(sig).params->item;
        if (strcmp(AS_PARAM(p0).name, "self") != 0) {
            checker_fail(c, p0->loc, "trait method first parameter must be named self");
            return false;
        }
        if (!AS_PARAM(p0).type) {
            checker_fail(c, p0->loc, "trait method self parameter must have a type");
            return false;
        }
    }
    return true;
}

static bool register_impl_decl(Checker *c, AstNode *impl) {
    const char *sn = AS_IMPL_DECL(impl).struct_name;
    Type *st;
    AstList *m;
    AstNode *trait_decl = NULL;

    if (AS_IMPL_DECL(impl).generic_params || AS_IMPL_DECL(impl).type_generic_params) {
        checker_fail(c, impl->loc, "generic impl not supported yet");
        return false;
    }
    st = env_lookup(c->global_env, sn);
    if (!st || prune(st)->kind != TY_STRUCT) {
        checker_fail(c, impl->loc, "impl target must be a struct type");
        return false;
    }
    st = prune(st);

    if (AS_IMPL_DECL(impl).trait_name) {
        trait_decl = lookup_trait_decl(c->program, AS_IMPL_DECL(impl).trait_name);
        if (!trait_decl) {
            checker_fail(c, impl->loc, "unknown trait");
            return false;
        }
    }

    for (m = AS_IMPL_DECL(impl).methods; m; m = m->next) {
        AstNode *fn = m->item;
        AstList *pl;
        AstNode *p0;
        Type *pty;
        AstNode *sig;

        if (!fn || fn->kind != NODE_FN_DECL) {
            checker_fail(c, impl->loc, "impl body must contain functions");
            return false;
        }
        pl = AS_FN_DECL(fn).params;
        if (!pl || !pl->item) {
            checker_fail(c, fn->loc, "impl method needs at least self parameter");
            return false;
        }
        p0 = pl->item;
        if (strcmp(AS_PARAM(p0).name, "self") != 0) {
            checker_fail(c, p0->loc, "impl method first parameter must be named self");
            return false;
        }
        if (!AS_PARAM(p0).type) {
            checker_fail(c, p0->loc, "self parameter must have a type");
            return false;
        }
        pty = ast_type_to_type(c, AS_PARAM(p0).type);
        if (c->error) {
            return false;
        }
        if (!unify(c, pty, st, p0->loc)) {
            checker_fail(c, p0->loc, "self type must match impl struct");
            return false;
        }
        if (trait_decl) {
            sig = find_trait_fn_sig(trait_decl, AS_FN_DECL(fn).name);
            if (!sig) {
                checker_fail(c, fn->loc, "impl method not declared in trait");
                return false;
            }
            if (!trait_sig_matches_impl_fn(sig, fn)) {
                checker_fail(c, fn->loc, "impl method signature does not match trait");
                return false;
            }
        }
        if (!register_fn_sig(c, fn)) {
            return false;
        }
    }
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

/* Type-check generic function body with rigid type variables per type parameter. */
static bool check_generic_fn_body(Checker *c, AstNode *fn) {
    Env tparam = {0};
    AstList *gp;
    AstList *pl;
    Type **params;
    Type *ret;
    Type *ft;
    Type **rigid_vars;
    size_t ngp;
    size_t n;
    size_t i;

    ngp = ast_list_len(AS_FN_DECL(fn).generic_params);
    rigid_vars = (Type **)calloc(ngp, sizeof(Type *));
    if (ngp > 0 && !rigid_vars) {
        checker_fail(c, fn->loc, "out of memory");
        return false;
    }

    for (gp = AS_FN_DECL(fn).generic_params, i = 0; gp; gp = gp->next, i++) {
        AstNode *gpn = gp->item;
        Type *v;
        if (!gpn || gpn->kind != NODE_GENERIC_PARAM) {
            env_free_head(&tparam);
            free(rigid_vars);
            checker_fail(c, fn->loc, "internal: generic parameter");
            return false;
        }
        v = new_var(c);
        if (!v) {
            env_free_head(&tparam);
            free(rigid_vars);
            checker_fail(c, fn->loc, "out of memory");
            return false;
        }
        v->as.var.trait_bounds = AS_GENERIC_PARAM(gpn).bounds;
        rigid_vars[i] = v;
        env_insert(&tparam, AS_GENERIC_PARAM(gpn).name, v);
    }

    n = ast_list_len(AS_FN_DECL(fn).params);
    params = (Type **)malloc(n * sizeof(Type *));
    if (n > 0 && !params) {
        env_free_head(&tparam);
        free(rigid_vars);
        checker_fail(c, fn->loc, "out of memory");
        return false;
    }
    i = 0;
    for (pl = AS_FN_DECL(fn).params; pl; pl = pl->next, i++) {
        AstNode *param = pl->item;
        if (!AS_PARAM(param).type) {
            env_free_head(&tparam);
            free(params);
            free(rigid_vars);
            checker_fail(c, param->loc, "parameter type required");
            return false;
        }
        params[i] = ast_type_to_type_ex(c, AS_PARAM(param).type, &tparam);
        if (c->error) {
            env_free_head(&tparam);
            free(params);
            free(rigid_vars);
            return false;
        }
    }
    if (AS_FN_DECL(fn).ret_type) {
        ret = ast_type_to_type_ex(c, AS_FN_DECL(fn).ret_type, &tparam);
    } else {
        ret = new_var(c);
    }
    env_free_head(&tparam);
    if (c->error) {
        free(params);
        free(rigid_vars);
        return false;
    }

    ft = new_fn_type(c, params, n, ret);
    if (!ft) {
        free(params);
        free(rigid_vars);
        checker_fail(c, fn->loc, "out of memory");
        return false;
    }

    {
        Env fn_env = { .head = NULL, .parent = c->global_env };
        for (pl = AS_FN_DECL(fn).params, i = 0; pl; pl = pl->next, i++) {
            env_insert(&fn_env, AS_PARAM(pl->item).name, ft->as.fn.params[i]);
        }

        c->current_env = &fn_env;
        c->expected_return = ft->as.fn.ret;

        {
            Type *body_ty = infer_block(c, AS_FN_DECL(fn).body);
            if (c->error) {
                env_free_head(&fn_env);
                c->current_env = c->global_env;
                c->expected_return = NULL;
                free(rigid_vars);
                return false;
            }
            if (!body_ty) {
                env_free_head(&fn_env);
                c->current_env = c->global_env;
                c->expected_return = NULL;
                free(rigid_vars);
                checker_fail(c, AS_FN_DECL(fn).body->loc, "could not infer block type");
                return false;
            }
            if (!unify(c, body_ty, ft->as.fn.ret, AS_FN_DECL(fn).body->loc)) {
                env_free_head(&fn_env);
                c->current_env = c->global_env;
                c->expected_return = NULL;
                free(rigid_vars);
                return false;
            }
        }

        env_free_head(&fn_env);
        c->current_env = c->global_env;
        c->expected_return = NULL;
    }

    if (!verify_resolved_generic_params(c, fn, rigid_vars, ngp)) {
        free(rigid_vars);
        return false;
    }
    free(rigid_vars);
    return true;
}

/* Params get types from the already-registered fn signature; block type must match return. */
static bool check_fn_body(Checker *c, AstNode *fn) {
    Type *sig;

    if (AS_FN_DECL(fn).generic_params) {
        return check_generic_fn_body(c, fn);
    }

    sig = env_lookup(c->global_env, AS_FN_DECL(fn).name);
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

    if (env_lookup(c->global_env, nm)) {
        checker_fail(c, st->loc, "duplicate struct or function name");
        return false;
    }

    if (AS_STRUCT_DECL(st).generic_params) {
        Env tenv = {0};
        AstList *gp;
        for (gp = AS_STRUCT_DECL(st).generic_params; gp; gp = gp->next) {
            AstNode *gpn = gp->item;
            Type *tv;
            if (!gpn || gpn->kind != NODE_GENERIC_PARAM) {
                env_free_head(&tenv);
                checker_fail(c, st->loc, "internal: generic parameter");
                return false;
            }
            tv = new_var(c);
            if (!tv) {
                env_free_head(&tenv);
                checker_fail(c, st->loc, "out of memory");
                return false;
            }
            env_insert(&tenv, AS_GENERIC_PARAM(gpn).name, tv);
        }
        for (fl = AS_STRUCT_DECL(st).fields; fl; fl = fl->next) {
            AstNode *sf = fl->item;
            if (AS_STRUCT_FIELD(sf).type == NULL) {
                env_free_head(&tenv);
                checker_fail(c, sf->loc, "struct field type required");
                return false;
            }
            (void)ast_type_to_type_ex(c, AS_STRUCT_FIELD(sf).type, &tenv);
            if (c->error) {
                env_free_head(&tenv);
                return false;
            }
        }
        env_free_head(&tenv);

        t = alloc_type(c);
        if (!t) {
            checker_fail(c, st->loc, "out of memory");
            return false;
        }
        t->kind = TY_GENERIC_STRUCT;
        t->as.g_schema.decl = st;
        env_insert(c->global_env, nm, t);
        return true;
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
    /* Pass 2a: traits (names must not collide with struct/fn/const). */
    if (!chk.error) {
        for (d = AS_PROGRAM(program).decls; d; d = d->next) {
            if (d->item->kind == NODE_TRAIT_DECL) {
                if (!register_trait_decl(&chk, d->item)) {
                    break;
                }
            }
        }
    }
    /* Pass 2b: consts (typed), fn signatures, impl method signatures. */
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
            } else if (d->item->kind == NODE_IMPL_DECL) {
                if (!register_impl_decl(&chk, d->item)) {
                    break;
                }
            } else if (d->item->kind == NODE_STRUCT_DECL ||
                       d->item->kind == NODE_TRAIT_DECL ||
                       d->item->kind == NODE_USE_DECL) {
                continue;
            } else {
                checker_fail(&chk, d->item->loc, "unsupported top-level declaration");
                break;
            }
        }
    }

    /* Pass 3: fn bodies (params + global parent), including impl methods. */
    if (!chk.error) {
        for (d = AS_PROGRAM(program).decls; d; d = d->next) {
            if (d->item->kind == NODE_FN_DECL) {
                if (!check_fn_body(&chk, d->item)) {
                    break;
                }
            } else if (d->item->kind == NODE_IMPL_DECL) {
                AstList *m;
                bool impl_ok = true;
                for (m = AS_IMPL_DECL(d->item).methods; m && impl_ok; m = m->next) {
                    impl_ok = check_fn_body(&chk, m->item);
                }
                if (!impl_ok) {
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
