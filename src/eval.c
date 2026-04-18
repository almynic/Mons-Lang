/*
 * eval.c — tree-walk interpreter.
 *
 * Environments are singly-linked frames (EvalEnv); lookup walks parent chain.
 * halt_return + return_value implement `return` out of nested blocks.
 *
 * Arrays, tuples, structs are heap values with refcount (value_retain / value_release).
 * Many eval_expr paths value_release operands after use to avoid leaks.
 *
 * eval_call_by_name seeds top-level `const` bindings into a global frame, then runs
 * the requested function with that frame as the outer parent so `ident` finds consts.
 */
#include "eval.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ValBind {
    const char *name;
    Value val;
    struct ValBind *next;
} ValBind;

typedef struct EvalEnv {
    ValBind *head;
    struct EvalEnv *parent;
} EvalEnv;

typedef struct EvalCtx {
    AstNode *program; /* for lookup_fn / lookup_struct_decl */
    EvalEnv *env;
    int halt_return; /* set by NODE_RETURN; cleared when entering a fresh block for control */
    Value return_value;
    char errbuf[256];
    const char *error;
} EvalCtx;

static void eval_fail(EvalCtx *ctx, const char *msg) {
    if (ctx->error) {
        return;
    }
    ctx->error = msg;
}

static void eval_fail_fmt(EvalCtx *ctx, const char *fmt, long x) {
    if (ctx->error) {
        return;
    }
    (void)snprintf(ctx->errbuf, sizeof(ctx->errbuf), fmt, x);
    ctx->error = ctx->errbuf;
}

static Value val_void(void) {
    Value v;
    v.kind = VAL_VOID;
    return v;
}

static Value val_bool(bool b) {
    Value v;
    v.kind = VAL_BOOL;
    v.as.b = b;
    return v;
}

/* Bump refcount for shared composite payloads; primitives unchanged. */
Value value_retain(Value v) {
    if ((v.kind == VAL_ARRAY || v.kind == VAL_TUPLE) && v.as.seq) {
        v.as.seq->refc++;
    } else if (v.kind == VAL_STRUCT && v.as.st) {
        v.as.st->refc++;
    } else if (v.kind == VAL_CLOSURE && v.as.closure) {
        v.as.closure->refc++;
    }
    return v;
}

void value_release(Value *v) {
    size_t i;
    if (!v) {
        return;
    }
    switch (v->kind) {
        case VAL_ARRAY:
        case VAL_TUPLE:
            if (v->as.seq && --v->as.seq->refc == 0) {
                for (i = 0; i < v->as.seq->len; i++) {
                    value_release(&v->as.seq->items[i]);
                }
                free(v->as.seq->items);
                free(v->as.seq);
            }
            break;
        case VAL_STRUCT:
            if (v->as.st && --v->as.st->refc == 0) {
                for (i = 0; i < v->as.st->n; i++) {
                    value_release(&v->as.st->values[i]);
                }
                free(v->as.st->field_names);
                free(v->as.st->values);
                free(v->as.st);
            }
            break;
        case VAL_CLOSURE:
            if (v->as.closure && --v->as.closure->refc == 0) {
                ValClosure *cl = v->as.closure;
                for (i = 0; i < cl->ncap; i++) {
                    value_release(&cl->cap_vals[i]);
                }
                free(cl->cap_vals);
                if (cl->cap_names) {
                    free(cl->cap_names);
                }
                free(cl);
            }
            break;
        default:
            break;
    }
    v->kind = VAL_VOID;
}

static bool value_is_true(const Value *v) {
    switch (v->kind) {
        case VAL_BOOL:
            return v->as.b;
        case VAL_INT:
            return v->as.i != 0;
        case VAL_FLOAT:
            return v->as.f != 0.0f;
        case VAL_DOUBLE:
            return v->as.d != 0.0;
        default:
            return false;
    }
}

static Value *env_lookup_slot(EvalEnv *e, const char *name) {
    while (e) {
        ValBind *b;
        for (b = e->head; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                return &b->val;
            }
        }
        e = e->parent;
    }
    return NULL;
}

static void env_insert(EvalCtx *ctx, EvalEnv *frame, const char *name, Value val) {
    ValBind *b = (ValBind *)malloc(sizeof(ValBind));
    if (!b) {
        eval_fail(ctx, "out of memory");
        return;
    }
    b->name = name;
    b->val = val;
    b->next = frame->head;
    frame->head = b;
}

static void env_free_head(EvalEnv *e) {
    ValBind *b = e->head;
    while (b) {
        ValBind *n = b->next;
        value_release(&b->val);
        free(b);
        b = n;
    }
    e->head = NULL;
}

static bool env_assign(EvalCtx *ctx, const char *name, Value val) {
    Value *slot = env_lookup_slot(ctx->env, name);
    if (!slot) {
        eval_fail(ctx, "assignment to unknown binding");
        return false;
    }
    value_release(slot);
    *slot = value_retain(val);
    (void)ctx;
    return true;
}

void value_fprint(FILE *fp, const Value *v) {
    size_t i;
    switch (v->kind) {
        case VAL_INT:
            fprintf(fp, "%lld", (long long)v->as.i);
            break;
        case VAL_FLOAT:
            fprintf(fp, "%f", (double)v->as.f);
            break;
        case VAL_DOUBLE:
            fprintf(fp, "%f", v->as.d);
            break;
        case VAL_BOOL:
            fprintf(fp, "%s", v->as.b ? "true" : "false");
            break;
        case VAL_STRING:
            fprintf(fp, "\"%s\"", v->as.s);
            break;
        case VAL_NONE:
            fprintf(fp, "None");
            break;
        case VAL_VOID:
            fprintf(fp, "void");
            break;
        case VAL_ARRAY:
            fprintf(fp, "[");
            if (v->as.seq) {
                for (i = 0; i < v->as.seq->len; i++) {
                    if (i) {
                        fprintf(fp, ", ");
                    }
                    value_fprint(fp, &v->as.seq->items[i]);
                }
            }
            fprintf(fp, "]");
            break;
        case VAL_TUPLE:
            fprintf(fp, "(");
            if (v->as.seq) {
                for (i = 0; i < v->as.seq->len; i++) {
                    if (i) {
                        fprintf(fp, ", ");
                    }
                    value_fprint(fp, &v->as.seq->items[i]);
                }
            }
            fprintf(fp, ")");
            break;
        case VAL_STRUCT:
            fprintf(fp, "%s{", v->as.st && v->as.st->type_name ? v->as.st->type_name : "?");
            if (v->as.st) {
                for (i = 0; i < v->as.st->n; i++) {
                    if (i) {
                        fprintf(fp, ", ");
                    }
                    fprintf(fp, "%s: ", v->as.st->field_names[i] ? v->as.st->field_names[i] : "?");
                    value_fprint(fp, &v->as.st->values[i]);
                }
            }
            fprintf(fp, "}");
            break;
        case VAL_CLOSURE:
            if (v->as.closure) {
                if (v->as.closure->is_bytecode) {
                    fprintf(fp, "<bc closure chunk=%u ncap=%zu>",
                            (unsigned)v->as.closure->bc_chunk_idx,
                            v->as.closure->ncap);
                } else {
                    fprintf(fp, "<closure>");
                }
            } else {
                fprintf(fp, "<closure>");
            }
            break;
    }
}

#define LAMBDA_BOUND_MAX 96u

static bool fv_bound_has(const char *stack[], size_t nb, const char *name) {
    size_t i;
    for (i = 0; i < nb; i++) {
        if (strcmp(stack[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void fv_cap_add(const char ***caps, size_t *nc, size_t *capa, const char *name) {
    size_t i;
    for (i = 0; i < *nc; i++) {
        if (strcmp((*caps)[i], name) == 0) {
            return;
        }
    }
    if (*nc >= *capa) {
        size_t ncap = *capa == 0 ? 8u : *capa * 2u;
        const char **na = (const char **)realloc(*caps, ncap * sizeof(const char *));
        if (!na) {
            return;
        }
        *caps = na;
        *capa = ncap;
    }
    (*caps)[*nc] = name;
    (*nc)++;
}

static void fv_walk(AstNode *n, const char *bound[], size_t nb, const char ***caps, size_t *nc, size_t *capa);

static void fv_block(AstNode *block, const char *bound[], size_t nb, const char ***caps, size_t *nc, size_t *capa) {
    const char *stack[LAMBDA_BOUND_MAX];
    size_t nbs = nb;
    AstList *sl;

    if (block->kind != NODE_BLOCK || nbs > LAMBDA_BOUND_MAX) {
        return;
    }
    memcpy(stack, bound, nb * sizeof(bound[0]));

    for (sl = AS_BLOCK(block).stmts; sl; sl = sl->next) {
        AstNode *st = sl->item;
        if (st->kind == NODE_LET) {
            fv_walk(AS_LET(st).init, stack, nbs, caps, nc, capa);
            if (nbs < LAMBDA_BOUND_MAX) {
                stack[nbs++] = AS_LET(st).name;
            }
        } else if (st->kind == NODE_EXPR_STMT) {
            fv_walk(AS_EXPR_STMT(st).expr, stack, nbs, caps, nc, capa);
        } else if (st->kind == NODE_RETURN && AS_RETURN(st).value) {
            fv_walk(AS_RETURN(st).value, stack, nbs, caps, nc, capa);
        } else if (st->kind == NODE_FOR) {
            fv_walk(AS_FOR(st).iter, stack, nbs, caps, nc, capa);
            if (nbs < LAMBDA_BOUND_MAX) {
                const char *st2[LAMBDA_BOUND_MAX];
                memcpy(st2, stack, nbs * sizeof(stack[0]));
                st2[nbs] = AS_FOR(st).var;
                fv_block(AS_FOR(st).body, st2, nbs + 1u, caps, nc, capa);
            }
        }
    }
    if (AS_BLOCK(block).tail_expr) {
        fv_walk(AS_BLOCK(block).tail_expr, stack, nbs, caps, nc, capa);
    }
}

static void fv_walk(AstNode *n, const char *bound[], size_t nb, const char ***caps, size_t *nc, size_t *capa) {
    AstList *al;
    if (!n) {
        return;
    }
    switch (n->kind) {
        case NODE_IDENT:
            if (!fv_bound_has(bound, nb, AS_IDENT(n).name)) {
                fv_cap_add(caps, nc, capa, AS_IDENT(n).name);
            }
            return;
        case NODE_BINARY:
            fv_walk(AS_BINARY(n).left, bound, nb, caps, nc, capa);
            fv_walk(AS_BINARY(n).right, bound, nb, caps, nc, capa);
            return;
        case NODE_UNARY:
            fv_walk(AS_UNARY(n).operand, bound, nb, caps, nc, capa);
            return;
        case NODE_CALL:
            fv_walk(AS_CALL(n).callee, bound, nb, caps, nc, capa);
            for (al = AS_CALL(n).args; al; al = al->next) {
                fv_walk(al->item, bound, nb, caps, nc, capa);
            }
            return;
        case NODE_METHOD_CALL:
            fv_walk(AS_METHOD_CALL(n).receiver, bound, nb, caps, nc, capa);
            for (al = AS_METHOD_CALL(n).args; al; al = al->next) {
                fv_walk(al->item, bound, nb, caps, nc, capa);
            }
            return;
        case NODE_FIELD_ACCESS:
            fv_walk(AS_FIELD_ACCESS(n).object, bound, nb, caps, nc, capa);
            return;
        case NODE_INDEX:
            fv_walk(AS_INDEX(n).object, bound, nb, caps, nc, capa);
            fv_walk(AS_INDEX(n).index, bound, nb, caps, nc, capa);
            return;
        case NODE_ASSIGN:
            fv_walk(AS_ASSIGN(n).target, bound, nb, caps, nc, capa);
            fv_walk(AS_ASSIGN(n).value, bound, nb, caps, nc, capa);
            return;
        case NODE_BLOCK:
            fv_block(n, bound, nb, caps, nc, capa);
            return;
        case NODE_IF:
            {
                AstList *br = AS_IF(n).branches;
                while (br) {
                    fv_walk(br->item, bound, nb, caps, nc, capa);
                    br = br->next;
                    if (!br) {
                        break;
                    }
                    fv_walk(br->item, bound, nb, caps, nc, capa);
                    br = br->next;
                }
                if (AS_IF(n).else_body) {
                    fv_walk(AS_IF(n).else_body, bound, nb, caps, nc, capa);
                }
            }
            return;
        case NODE_LAMBDA:
            {
                const char *stack[LAMBDA_BOUND_MAX];
                size_t n2 = nb;
                AstList *pl;
                if (n2 > LAMBDA_BOUND_MAX) {
                    return;
                }
                memcpy(stack, bound, nb * sizeof(bound[0]));
                for (pl = AS_LAMBDA(n).params; pl; pl = pl->next) {
                    if (n2 >= LAMBDA_BOUND_MAX) {
                        return;
                    }
                    stack[n2++] = AS_PARAM(pl->item).name;
                }
                if (AS_LAMBDA(n).body->kind == NODE_BLOCK) {
                    fv_block(AS_LAMBDA(n).body, stack, n2, caps, nc, capa);
                } else {
                    fv_walk(AS_LAMBDA(n).body, stack, n2, caps, nc, capa);
                }
            }
            return;
        case NODE_ARRAY:
            for (al = AS_ARRAY(n).elements; al; al = al->next) {
                fv_walk(al->item, bound, nb, caps, nc, capa);
            }
            return;
        case NODE_TUPLE:
            for (al = AS_TUPLE(n).elements; al; al = al->next) {
                fv_walk(al->item, bound, nb, caps, nc, capa);
            }
            return;
        case NODE_STRUCT_INIT:
            for (al = AS_STRUCT_INIT(n).fields; al; al = al->next) {
                fv_walk(AS_FIELD_INIT(al->item).value, bound, nb, caps, nc, capa);
            }
            if (AS_STRUCT_INIT(n).base) {
                fv_walk(AS_STRUCT_INIT(n).base, bound, nb, caps, nc, capa);
            }
            return;
        default:
            return;
    }
}

static AstNode *lookup_fn(AstNode *program, const char *name) {
    AstList *d;
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL && strcmp(AS_FN_DECL(d->item).name, name) == 0) {
            return d->item;
        }
    }
    return NULL;
}

static AstNode *lookup_struct_decl(AstNode *program, const char *name) {
    AstList *d;
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_STRUCT_DECL && strcmp(AS_STRUCT_DECL(d->item).name, name) == 0) {
            return d->item;
        }
    }
    return NULL;
}

static size_t struct_field_index(AstNode *struct_decl, const char *field_name) {
    AstList *fl;
    size_t j = 0;
    for (fl = AS_STRUCT_DECL(struct_decl).fields; fl; fl = fl->next, j++) {
        if (strcmp(AS_STRUCT_FIELD(fl->item).name, field_name) == 0) {
            return j;
        }
    }
    return (size_t)-1;
}

static Value eval_expr(EvalCtx *ctx, AstNode *n);
static void eval_stmt(EvalCtx *ctx, AstNode *stmt);
static Value eval_block(EvalCtx *ctx, AstNode *block);

static Value eval_expr_or_block(EvalCtx *ctx, AstNode *n) {
    if (n->kind == NODE_BLOCK) {
        return eval_block(ctx, n);
    }
    return eval_expr(ctx, n);
}

static Value eval_if_expr(EvalCtx *ctx, AstNode *n) {
    AstList *br = AS_IF(n).branches;
    while (br) {
        AstNode *cond = br->item;
        br = br->next;
        if (!br) {
            break;
        }
        AstNode *body = br->item;
        br = br->next;
        Value c = eval_expr(ctx, cond);
        if (ctx->error) {
            return val_void();
        }
        if (value_is_true(&c)) {
            value_release(&c);
            return eval_expr_or_block(ctx, body);
        }
        value_release(&c);
    }
    if (AS_IF(n).else_body) {
        return eval_expr_or_block(ctx, AS_IF(n).else_body);
    }
    return val_void();
}

static int value_cmp_order(const Value *a, const Value *b) {
    if (a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
        case VAL_INT:
            if (a->as.i < b->as.i) {
                return -1;
            }
            if (a->as.i > b->as.i) {
                return 1;
            }
            return 0;
        case VAL_FLOAT:
            if (a->as.f < b->as.f) {
                return -1;
            }
            if (a->as.f > b->as.f) {
                return 1;
            }
            return 0;
        case VAL_DOUBLE:
            if (a->as.d < b->as.d) {
                return -1;
            }
            if (a->as.d > b->as.d) {
                return 1;
            }
            return 0;
        case VAL_STRING:
            return strcmp(a->as.s, b->as.s);
        default:
            return 0;
    }
}

static bool value_equal(const Value *a, const Value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case VAL_INT:
            return a->as.i == b->as.i;
        case VAL_FLOAT:
            return a->as.f == b->as.f;
        case VAL_DOUBLE:
            return a->as.d == b->as.d;
        case VAL_BOOL:
            return a->as.b == b->as.b;
        case VAL_STRING:
            return strcmp(a->as.s, b->as.s) == 0;
        case VAL_NONE:
            return true;
        case VAL_ARRAY:
        case VAL_TUPLE:
            return a->as.seq == b->as.seq;
        case VAL_STRUCT:
            return a->as.st == b->as.st;
        case VAL_CLOSURE:
            return a->as.closure == b->as.closure;
        default:
            return false;
    }
}

static Value eval_invoke_closure(EvalCtx *ctx, ValClosure *cl, const Value *args, size_t nargs);
static Value eval_invoke_fn(EvalCtx *ctx, AstNode *fn, const Value *args, size_t nargs);

/* Dispatch by NODE_*; see NODE_METHOD_CALL (self desugar), NODE_STRUCT_INIT (..base). */
static Value eval_expr(EvalCtx *ctx, AstNode *n) {
    if (!n) {
        eval_fail(ctx, "internal: null expression");
        return val_void();
    }
    if (ctx->error) {
        return val_void();
    }

    switch (n->kind) {
        case NODE_LIT_INT:
            {
                Value v;
                v.kind = VAL_INT;
                v.as.i = AS_LIT_INT(n).value;
                return v;
            }
        case NODE_LIT_FLOAT:
            {
                Value v;
                v.kind = VAL_FLOAT;
                v.as.f = AS_LIT_FLOAT(n).value;
                return v;
            }
        case NODE_LIT_DOUBLE:
            {
                Value v;
                v.kind = VAL_DOUBLE;
                v.as.d = AS_LIT_DOUBLE(n).value;
                return v;
            }
        case NODE_LIT_BOOL:
            return val_bool(AS_LIT_BOOL(n).value);
        case NODE_LIT_STRING:
            {
                Value v;
                v.kind = VAL_STRING;
                v.as.s = AS_LIT_STRING(n).value;
                return v;
            }
        case NODE_LIT_NONE:
            {
                Value v;
                v.kind = VAL_NONE;
                return v;
            }
        case NODE_IDENT:
            {
                Value *sl = env_lookup_slot(ctx->env, AS_IDENT(n).name);
                if (!sl) {
                    eval_fail(ctx, "unknown identifier at runtime");
                    return val_void();
                }
                return value_retain(*sl);
            }
        case NODE_UNARY:
            {
                Value x = eval_expr(ctx, AS_UNARY(n).operand);
                if (ctx->error) {
                    return val_void();
                }
                if (AS_UNARY(n).op == UNOP_NOT) {
                    Value out = val_bool(!value_is_true(&x));
                    value_release(&x);
                    return out;
                }
                /* UNOP_NEG */
                switch (x.kind) {
                    case VAL_INT:
                        x.as.i = -x.as.i;
                        return x;
                    case VAL_FLOAT:
                        x.as.f = -x.as.f;
                        return x;
                    case VAL_DOUBLE:
                        x.as.d = -x.as.d;
                        return x;
                    default:
                        eval_fail(ctx, "unary '-' type error");
                        value_release(&x);
                        return val_void();
                }
            }
        case NODE_BINARY:
            {
                BinOp op = AS_BINARY(n).op;
                if (op == BINOP_AND) {
                    Value l = eval_expr(ctx, AS_BINARY(n).left);
                    if (ctx->error) {
                        return val_void();
                    }
                    if (!value_is_true(&l)) {
                        value_release(&l);
                        return val_bool(false);
                    }
                    Value r = eval_expr(ctx, AS_BINARY(n).right);
                    value_release(&l);
                    if (ctx->error) {
                        return val_void();
                    }
                    {
                        Value out = val_bool(value_is_true(&r));
                        value_release(&r);
                        return out;
                    }
                }
                if (op == BINOP_OR) {
                    Value l = eval_expr(ctx, AS_BINARY(n).left);
                    if (ctx->error) {
                        return val_void();
                    }
                    if (value_is_true(&l)) {
                        value_release(&l);
                        return val_bool(true);
                    }
                    Value r = eval_expr(ctx, AS_BINARY(n).right);
                    value_release(&l);
                    if (ctx->error) {
                        return val_void();
                    }
                    {
                        Value out = val_bool(value_is_true(&r));
                        value_release(&r);
                        return out;
                    }
                }

                Value l = eval_expr(ctx, AS_BINARY(n).left);
                Value r = eval_expr(ctx, AS_BINARY(n).right);
                if (ctx->error) {
                    value_release(&l);
                    value_release(&r);
                    return val_void();
                }

                if (op == BINOP_EQ || op == BINOP_NEQ) {
                    bool eq = value_equal(&l, &r);
                    Value out = val_bool(op == BINOP_EQ ? eq : !eq);
                    value_release(&l);
                    value_release(&r);
                    return out;
                }
                if (op == BINOP_LT || op == BINOP_GT || op == BINOP_LTE || op == BINOP_GTE) {
                    int o = value_cmp_order(&l, &r);
                    bool outb;
                    if (l.kind != r.kind || l.kind == VAL_BOOL || l.kind == VAL_NONE || l.kind == VAL_VOID) {
                        eval_fail(ctx, "invalid comparison");
                        value_release(&l);
                        value_release(&r);
                        return val_void();
                    }
                    if (op == BINOP_LT) {
                        outb = o < 0;
                    } else if (op == BINOP_GT) {
                        outb = o > 0;
                    } else if (op == BINOP_LTE) {
                        outb = o <= 0;
                    } else {
                        outb = o >= 0;
                    }
                    value_release(&l);
                    value_release(&r);
                    return val_bool(outb);
                }

                if (op == BINOP_MOD) {
                    if (l.kind != VAL_INT || r.kind != VAL_INT) {
                        eval_fail(ctx, "'%' expects int operands");
                        value_release(&l);
                        value_release(&r);
                        return val_void();
                    }
                    if (r.as.i == 0) {
                        eval_fail(ctx, "division by zero");
                        value_release(&l);
                        value_release(&r);
                        return val_void();
                    }
                    {
                        Value v;
                        v.kind = VAL_INT;
                        v.as.i = l.as.i % r.as.i;
                        value_release(&l);
                        value_release(&r);
                        return v;
                    }
                }

                /* + - * / */
                if (l.kind != r.kind) {
                    eval_fail(ctx, "type mismatch in arithmetic");
                    value_release(&l);
                    value_release(&r);
                    return val_void();
                }
                {
                    Value outv;
                    outv.kind = l.kind;
                    switch (l.kind) {
                        case VAL_INT:
                            if (op == BINOP_DIV && r.as.i == 0) {
                                eval_fail(ctx, "division by zero");
                                value_release(&l);
                                value_release(&r);
                                return val_void();
                            }
                            if (op == BINOP_ADD) {
                                outv.as.i = l.as.i + r.as.i;
                            } else if (op == BINOP_SUB) {
                                outv.as.i = l.as.i - r.as.i;
                            } else if (op == BINOP_MUL) {
                                outv.as.i = l.as.i * r.as.i;
                            } else if (op == BINOP_DIV) {
                                outv.as.i = l.as.i / r.as.i;
                            } else {
                                eval_fail(ctx, "invalid operator");
                                value_release(&l);
                                value_release(&r);
                                return val_void();
                            }
                            value_release(&l);
                            value_release(&r);
                            return outv;
                        case VAL_FLOAT:
                            if (op == BINOP_DIV && r.as.f == 0.0f) {
                                eval_fail(ctx, "division by zero");
                                value_release(&l);
                                value_release(&r);
                                return val_void();
                            }
                            if (op == BINOP_ADD) {
                                outv.as.f = l.as.f + r.as.f;
                            } else if (op == BINOP_SUB) {
                                outv.as.f = l.as.f - r.as.f;
                            } else if (op == BINOP_MUL) {
                                outv.as.f = l.as.f * r.as.f;
                            } else if (op == BINOP_DIV) {
                                outv.as.f = l.as.f / r.as.f;
                            } else {
                                eval_fail(ctx, "invalid operator");
                                value_release(&l);
                                value_release(&r);
                                return val_void();
                            }
                            value_release(&l);
                            value_release(&r);
                            return outv;
                        case VAL_DOUBLE:
                            if (op == BINOP_DIV && r.as.d == 0.0) {
                                eval_fail(ctx, "division by zero");
                                value_release(&l);
                                value_release(&r);
                                return val_void();
                            }
                            if (op == BINOP_ADD) {
                                outv.as.d = l.as.d + r.as.d;
                            } else if (op == BINOP_SUB) {
                                outv.as.d = l.as.d - r.as.d;
                            } else if (op == BINOP_MUL) {
                                outv.as.d = l.as.d * r.as.d;
                            } else if (op == BINOP_DIV) {
                                outv.as.d = l.as.d / r.as.d;
                            } else {
                                eval_fail(ctx, "invalid operator");
                                value_release(&l);
                                value_release(&r);
                                return val_void();
                            }
                            value_release(&l);
                            value_release(&r);
                            return outv;
                        default:
                            eval_fail(ctx, "arithmetic on non-numeric value");
                            value_release(&l);
                            value_release(&r);
                            return val_void();
                    }
                }
            }
        case NODE_ASSIGN:
            {
                AstNode *tgt = AS_ASSIGN(n).target;
                Value rhs = eval_expr(ctx, AS_ASSIGN(n).value);
                if (ctx->error) {
                    return val_void();
                }
                if (tgt->kind != NODE_IDENT) {
                    eval_fail(ctx, "assignment target not supported");
                    value_release(&rhs);
                    return val_void();
                }
                if (!env_assign(ctx, AS_IDENT(tgt).name, rhs)) {
                    value_release(&rhs);
                    return val_void();
                }
                {
                    Value out = value_retain(rhs);
                    value_release(&rhs);
                    return out;
                }
            }
        case NODE_CALL:
            {
                AstNode *ce = AS_CALL(n).callee;
                size_t argc = ast_list_len(AS_CALL(n).args);
                Value *argv = (Value *)malloc(argc * sizeof(Value));
                AstList *al;
                size_t i;

                if (!argv && argc > 0) {
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }
                for (al = AS_CALL(n).args, i = 0; al; al = al->next, i++) {
                    argv[i] = eval_expr(ctx, al->item);
                    if (ctx->error) {
                        size_t j;
                        for (j = 0; j < i; j++) {
                            value_release(&argv[j]);
                        }
                        free(argv);
                        return val_void();
                    }
                }

                if (ce->kind == NODE_IDENT) {
                    Value *slot = env_lookup_slot(ctx->env, AS_IDENT(ce).name);
                    if (slot && slot->kind == VAL_CLOSURE && slot->as.closure) {
                        Value out = eval_invoke_closure(ctx, slot->as.closure, argv, argc);
                        for (i = 0; i < argc; i++) {
                            value_release(&argv[i]);
                        }
                        free(argv);
                        return out;
                    }
                    {
                        AstNode *fn = lookup_fn(ctx->program, AS_IDENT(ce).name);
                        if (fn) {
                            Value out = eval_invoke_fn(ctx, fn, argv, argc);
                            for (i = 0; i < argc; i++) {
                                value_release(&argv[i]);
                            }
                            free(argv);
                            return out;
                        }
                    }
                    eval_fail(ctx, "unknown call target");
                    for (i = 0; i < argc; i++) {
                        value_release(&argv[i]);
                    }
                    free(argv);
                    return val_void();
                }

                {
                    Value callee_v = eval_expr(ctx, ce);
                    if (ctx->error) {
                        for (i = 0; i < argc; i++) {
                            value_release(&argv[i]);
                        }
                        free(argv);
                        return val_void();
                    }
                    if (callee_v.kind == VAL_CLOSURE && callee_v.as.closure) {
                        Value out = eval_invoke_closure(ctx, callee_v.as.closure, argv, argc);
                        value_release(&callee_v);
                        for (i = 0; i < argc; i++) {
                            value_release(&argv[i]);
                        }
                        free(argv);
                        return out;
                    }
                    value_release(&callee_v);
                    eval_fail(ctx, "call target is not a function");
                    for (i = 0; i < argc; i++) {
                        value_release(&argv[i]);
                    }
                    free(argv);
                    return val_void();
                }
            }
        /* r.m(a,b) → call top-level `m` with argv = [r, a, b] (types.c requires first param name `self`). */
        case NODE_METHOD_CALL:
            {
                const char *mname = AS_METHOD_CALL(n).method;
                AstNode *fn = lookup_fn(ctx->program, mname);
                size_t nparam;
                size_t argc;
                Value *argv;
                AstList *al;
                size_t i;
                Value recv;
                if (!fn) {
                    eval_fail(ctx, "call to unknown function");
                    return val_void();
                }
                nparam = ast_list_len(AS_FN_DECL(fn).params);
                argc = 1 + ast_list_len(AS_METHOD_CALL(n).args);
                if (argc != nparam) {
                    eval_fail_fmt(ctx, "wrong argument count (expected %ld)", (long)nparam);
                    return val_void();
                }
                argv = (Value *)malloc(argc * sizeof(Value));
                if (!argv && argc > 0) {
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }
                recv = eval_expr(ctx, AS_METHOD_CALL(n).receiver);
                if (ctx->error) {
                    free(argv);
                    return val_void();
                }
                argv[0] = recv;
                i = 1;
                for (al = AS_METHOD_CALL(n).args; al; al = al->next, i++) {
                    argv[i] = eval_expr(ctx, al->item);
                    if (ctx->error) {
                        size_t j;
                        for (j = 0; j < i; j++) {
                            value_release(&argv[j]);
                        }
                        free(argv);
                        return val_void();
                    }
                }
                {
                    Value out = eval_invoke_fn(ctx, fn, argv, argc);
                    for (i = 0; i < argc; i++) {
                        value_release(&argv[i]);
                    }
                    free(argv);
                    return out;
                }
            }
        case NODE_IF:
            return eval_if_expr(ctx, n);
        case NODE_BLOCK:
            return eval_block(ctx, n);
        case NODE_ARRAY:
        case NODE_TUPLE:
            {
                AstList *el;
                size_t nmem = n->kind == NODE_ARRAY
                    ? ast_list_len(AS_ARRAY(n).elements)
                    : ast_list_len(AS_TUPLE(n).elements);
                Value *tmp = NULL;
                ValSeq *seq;
                Value v;
                size_t i;
                if (nmem > 0) {
                    tmp = (Value *)malloc(nmem * sizeof(Value));
                    if (!tmp) {
                        eval_fail(ctx, "out of memory");
                        return val_void();
                    }
                }
                i = 0;
                el = n->kind == NODE_ARRAY ? AS_ARRAY(n).elements : AS_TUPLE(n).elements;
                for (; el; el = el->next) {
                    tmp[i] = eval_expr(ctx, el->item);
                    if (ctx->error) {
                        size_t k;
                        for (k = 0; k < i; k++) {
                            value_release(&tmp[k]);
                        }
                        free(tmp);
                        return val_void();
                    }
                    i++;
                }
                seq = (ValSeq *)malloc(sizeof(ValSeq));
                if (!seq) {
                    for (i = 0; i < nmem; i++) {
                        value_release(&tmp[i]);
                    }
                    free(tmp);
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }
                seq->len = nmem;
                seq->refc = 1;
                seq->items = tmp;
                v.kind = n->kind == NODE_ARRAY ? VAL_ARRAY : VAL_TUPLE;
                v.as.seq = seq;
                return v;
            }
        case NODE_INDEX:
            {
                Value obj = eval_expr(ctx, AS_INDEX(n).object);
                Value ixv = eval_expr(ctx, AS_INDEX(n).index);
                if (ctx->error) {
                    value_release(&obj);
                    return val_void();
                }
                if (ixv.kind != VAL_INT) {
                    eval_fail(ctx, "index must be int");
                    value_release(&obj);
                    value_release(&ixv);
                    return val_void();
                }
                if (ixv.as.i < 0) {
                    eval_fail(ctx, "negative index");
                    value_release(&obj);
                    value_release(&ixv);
                    return val_void();
                }
                {
                    size_t k = (size_t)ixv.as.i;
                    value_release(&ixv);
                    if ((obj.kind != VAL_ARRAY && obj.kind != VAL_TUPLE) || !obj.as.seq) {
                        eval_fail(ctx, "index target must be array or tuple");
                        value_release(&obj);
                        return val_void();
                    }
                    if (k >= obj.as.seq->len) {
                        eval_fail(ctx, "index out of bounds");
                        value_release(&obj);
                        return val_void();
                    }
                    {
                        Value out = value_retain(obj.as.seq->items[k]);
                        value_release(&obj);
                        return out;
                    }
                }
            }
        /*
         * No base: each field from FIELD_INIT list (declaration order).
         * With base: copy fields from base ValStruct (retain), then overwrite listed fields.
         */
        case NODE_STRUCT_INIT:
            {
                const char *sname = AS_STRUCT_INIT(n).struct_name;
                AstNode *decl = lookup_struct_decl(ctx->program, sname);
                ValStruct *vs;
                const char **fnames;
                Value *vals;
                Value basev;
                AstList *fl;
                AstList *inits;
                size_t i;
                size_t nf;

                if (!decl) {
                    eval_fail(ctx, "unknown struct type");
                    return val_void();
                }
                nf = ast_list_len(AS_STRUCT_DECL(decl).fields);
                vals = (Value *)malloc(nf * sizeof(Value));
                fnames = (const char **)malloc(nf * sizeof(char *));
                if (nf > 0 && (!vals || !fnames)) {
                    free(vals);
                    free(fnames);
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }

                if (AS_STRUCT_INIT(n).base) {
                    basev = eval_expr(ctx, AS_STRUCT_INIT(n).base);
                    if (ctx->error) {
                        free(vals);
                        free(fnames);
                        return val_void();
                    }
                    if (basev.kind != VAL_STRUCT || !basev.as.st) {
                        eval_fail(ctx, "struct update base must be a struct value");
                        value_release(&basev);
                        free(vals);
                        free(fnames);
                        return val_void();
                    }
                    if (strcmp(basev.as.st->type_name, sname) != 0) {
                        eval_fail(ctx, "struct update base has wrong type");
                        value_release(&basev);
                        free(vals);
                        free(fnames);
                        return val_void();
                    }
                    if (basev.as.st->n != nf) {
                        eval_fail(ctx, "internal: struct layout mismatch");
                        value_release(&basev);
                        free(vals);
                        free(fnames);
                        return val_void();
                    }
                    for (i = 0; i < nf; i++) {
                        vals[i] = value_retain(basev.as.st->values[i]);
                    }
                    value_release(&basev);
                    for (inits = AS_STRUCT_INIT(n).fields; inits; inits = inits->next) {
                        AstNode *fi = inits->item;
                        size_t idx = struct_field_index(decl, AS_FIELD_INIT(fi).name);
                        if (idx == (size_t)-1) {
                            for (i = 0; i < nf; i++) {
                                value_release(&vals[i]);
                            }
                            free(vals);
                            free(fnames);
                            eval_fail(ctx, "unknown field in struct initializer");
                            return val_void();
                        }
                        value_release(&vals[idx]);
                        vals[idx] = eval_expr(ctx, AS_FIELD_INIT(fi).value);
                        if (ctx->error) {
                            for (i = 0; i < nf; i++) {
                                value_release(&vals[i]);
                            }
                            free(vals);
                            free(fnames);
                            return val_void();
                        }
                    }
                } else {
                    i = 0;
                    for (fl = AS_STRUCT_DECL(decl).fields; fl; fl = fl->next, i++) {
                        const char *fname = AS_STRUCT_FIELD(fl->item).name;
                        bool got = false;
                        fnames[i] = fname;
                        for (inits = AS_STRUCT_INIT(n).fields; inits; inits = inits->next) {
                            AstNode *fi = inits->item;
                            if (strcmp(AS_FIELD_INIT(fi).name, fname) == 0) {
                                vals[i] = eval_expr(ctx, AS_FIELD_INIT(fi).value);
                                got = true;
                                break;
                            }
                        }
                        if (!got) {
                            size_t k;
                            for (k = 0; k < i; k++) {
                                value_release(&vals[k]);
                            }
                            free(vals);
                            free(fnames);
                            eval_fail(ctx, "internal: missing struct field at eval");
                            return val_void();
                        }
                        if (ctx->error) {
                            size_t k;
                            for (k = 0; k <= i; k++) {
                                value_release(&vals[k]);
                            }
                            free(vals);
                            free(fnames);
                            return val_void();
                        }
                    }
                }

                if (AS_STRUCT_INIT(n).base) {
                    i = 0;
                    for (fl = AS_STRUCT_DECL(decl).fields; fl; fl = fl->next, i++) {
                        fnames[i] = AS_STRUCT_FIELD(fl->item).name;
                    }
                }

                vs = (ValStruct *)malloc(sizeof(ValStruct));
                if (!vs) {
                    for (i = 0; i < nf; i++) {
                        value_release(&vals[i]);
                    }
                    free(vals);
                    free(fnames);
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }
                vs->refc = 1;
                vs->type_name = sname;
                vs->n = nf;
                vs->field_names = fnames;
                vs->values = vals;
                {
                    Value out;
                    out.kind = VAL_STRUCT;
                    out.as.st = vs;
                    return out;
                }
            }
        case NODE_FIELD_ACCESS:
            {
                Value obj = eval_expr(ctx, AS_FIELD_ACCESS(n).object);
                size_t i;
                if (ctx->error) {
                    return val_void();
                }
                if (obj.kind != VAL_STRUCT || !obj.as.st) {
                    eval_fail(ctx, "field access on non-struct value");
                    value_release(&obj);
                    return val_void();
                }
                for (i = 0; i < obj.as.st->n; i++) {
                    if (strcmp(obj.as.st->field_names[i], AS_FIELD_ACCESS(n).field) == 0) {
                        Value out = value_retain(obj.as.st->values[i]);
                        value_release(&obj);
                        return out;
                    }
                }
                eval_fail(ctx, "unknown struct field");
                value_release(&obj);
                return val_void();
            }
        case NODE_LAMBDA:
            {
                const char *stack[LAMBDA_BOUND_MAX];
                const char **caps = NULL;
                size_t nc = 0;
                size_t capa = 0;
                size_t nb = 0;
                AstList *pl;
                ValClosure *cl;
                Value out;
                size_t j;

                for (pl = AS_LAMBDA(n).params; pl; pl = pl->next) {
                    if (nb >= LAMBDA_BOUND_MAX) {
                        eval_fail(ctx, "too many lambda parameters for closure capture");
                        return val_void();
                    }
                    stack[nb++] = AS_PARAM(pl->item).name;
                }
                if (AS_LAMBDA(n).body->kind == NODE_BLOCK) {
                    fv_block(AS_LAMBDA(n).body, stack, nb, &caps, &nc, &capa);
                } else {
                    fv_walk(AS_LAMBDA(n).body, stack, nb, &caps, &nc, &capa);
                }

                cl = (ValClosure *)malloc(sizeof(ValClosure));
                if (!cl) {
                    free(caps);
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }
                cl->refc = 1;
                cl->lambda = n;
                cl->is_bytecode = false;
                cl->bc_chunk_idx = 0;
                cl->ncap = nc;
                cl->cap_names = (const char **)malloc(nc * sizeof(const char *));
                cl->cap_vals = (Value *)malloc(nc * sizeof(Value));
                if (nc > 0 && (!cl->cap_names || !cl->cap_vals)) {
                    free(cl->cap_names);
                    free(cl->cap_vals);
                    free(cl);
                    free(caps);
                    eval_fail(ctx, "out of memory");
                    return val_void();
                }
                if (nc > 0) {
                    memcpy(cl->cap_names, caps, nc * sizeof(const char *));
                }
                free(caps);

                for (j = 0; j < nc; j++) {
                    Value *sl = env_lookup_slot(ctx->env, cl->cap_names[j]);
                    if (!sl) {
                        eval_fail(ctx, "unknown identifier in closure environment");
                        while (j > 0) {
                            j--;
                            value_release(&cl->cap_vals[j]);
                        }
                        free(cl->cap_names);
                        free(cl->cap_vals);
                        free(cl);
                        return val_void();
                    }
                    cl->cap_vals[j] = value_retain(*sl);
                }

                out.kind = VAL_CLOSURE;
                out.as.closure = cl;
                return out;
            }
        case NODE_MACRO_CALL:
            eval_fail(ctx, "expression form not supported by interpreter yet");
            return val_void();
        default:
            eval_fail(ctx, "unsupported expression in interpreter");
            return val_void();
    }
}

static void eval_stmt(EvalCtx *ctx, AstNode *stmt) {
    if (ctx->error || ctx->halt_return) {
        return;
    }
    switch (stmt->kind) {
        case NODE_LET:
            {
                Value v = eval_expr(ctx, AS_LET(stmt).init);
                if (ctx->error) {
                    return;
                }
                env_insert(ctx, ctx->env, AS_LET(stmt).name, v);
                break;
            }
        case NODE_EXPR_STMT:
            {
                Value v = eval_expr(ctx, AS_EXPR_STMT(stmt).expr);
                value_release(&v);
                break;
            }
        case NODE_RETURN:
            if (AS_RETURN(stmt).value) {
                ctx->return_value = eval_expr(ctx, AS_RETURN(stmt).value);
            } else {
                ctx->return_value = val_void();
            }
            ctx->halt_return = 1;
            break;
        case NODE_FOR:
            {
                Value iter_v = eval_expr(ctx, AS_FOR(stmt).iter);
                if (ctx->error) {
                    return;
                }
                if (iter_v.kind != VAL_ARRAY && iter_v.kind != VAL_TUPLE) {
                    eval_fail(ctx, "for-in expects an array or tuple value");
                    value_release(&iter_v);
                    return;
                }
                if (iter_v.as.seq) {
                    size_t idx;
                    for (idx = 0; idx < iter_v.as.seq->len && !ctx->error && !ctx->halt_return; idx++) {
                        EvalEnv loop = { NULL, ctx->env };
                        Value elem = value_retain(iter_v.as.seq->items[idx]);
                        env_insert(ctx, &loop, AS_FOR(stmt).var, elem);
                        if (ctx->error) {
                            value_release(&elem);
                            env_free_head(&loop);
                            break;
                        }
                        ctx->env = &loop;
                        (void)eval_block(ctx, AS_FOR(stmt).body);
                        ctx->env = loop.parent;
                        env_free_head(&loop);
                        if (ctx->halt_return) {
                            break;
                        }
                    }
                }
                value_release(&iter_v);
                break;
            }
        case NODE_TRY:
            eval_fail(ctx, "statement not supported by interpreter yet");
            break;
        default:
            eval_fail(ctx, "unsupported statement");
            break;
    }
}

/* Inner env for stmts; tail_expr is the block’s value unless return already fired. */
static Value eval_block(EvalCtx *ctx, AstNode *block_node) {
    int prev_halt = ctx->halt_return;
    ctx->halt_return = 0;

    EvalEnv inner = { NULL, ctx->env };
    ctx->env = &inner;

    AstList *s;
    for (s = AS_BLOCK(block_node).stmts; s; s = s->next) {
        eval_stmt(ctx, s->item);
        if (ctx->error || ctx->halt_return) {
            break;
        }
    }

    Value out = val_void();
    if (!ctx->error && ctx->halt_return) {
        out = ctx->return_value;
    } else if (!ctx->error && AS_BLOCK(block_node).tail_expr) {
        out = eval_expr(ctx, AS_BLOCK(block_node).tail_expr);
        if (ctx->halt_return) {
            out = ctx->return_value;
        }
    }

    env_free_head(&inner);
    ctx->env = inner.parent;

    if (!ctx->halt_return) {
        ctx->halt_return = prev_halt;
    }
    return out;
}

static Value eval_lambda_body(EvalCtx *ctx, AstNode *body) {
    if (body->kind == NODE_BLOCK) {
        return eval_block(ctx, body);
    }
    return eval_expr(ctx, body);
}

/* Captures + parameters, then lambda body (block or single expr). */
static Value eval_invoke_closure(EvalCtx *ctx, ValClosure *cl, const Value *args, size_t nargs) {
    AstNode *lam;
    size_t nparam;

    if (!cl->lambda || cl->is_bytecode) {
        eval_fail(ctx, "internal: not an interpreter closure");
        return val_void();
    }
    lam = cl->lambda;
    nparam = ast_list_len(AS_LAMBDA(lam).params);
    EvalEnv capf = { NULL, ctx->env };
    EvalEnv argf = { NULL, &capf };
    AstList *pl;
    size_t i;
    EvalEnv *saved;
    Value out;

    if (nargs != nparam) {
        eval_fail_fmt(ctx, "wrong argument count (expected %ld)", (long)nparam);
        return val_void();
    }

    for (i = 0; i < cl->ncap; i++) {
        env_insert(ctx, &capf, cl->cap_names[i], value_retain(cl->cap_vals[i]));
        if (ctx->error) {
            env_free_head(&capf);
            return val_void();
        }
    }

    i = 0;
    for (pl = AS_LAMBDA(lam).params; pl; pl = pl->next, i++) {
        env_insert(ctx, &argf, AS_PARAM(pl->item).name, value_retain(args[i]));
        if (ctx->error) {
            env_free_head(&argf);
            env_free_head(&capf);
            return val_void();
        }
    }

    saved = ctx->env;
    ctx->env = &argf;
    ctx->halt_return = 0;

    out = eval_lambda_body(ctx, AS_LAMBDA(lam).body);
    if (ctx->halt_return) {
        out = ctx->return_value;
    }
    ctx->halt_return = 0;
    ctx->env = saved;
    env_free_head(&argf);
    env_free_head(&capf);
    return out;
}

/* New frame: params bound with value_retain(args[i]); parent is ctx->env (globals + consts). */
static Value eval_invoke_fn(EvalCtx *ctx, AstNode *fn, const Value *args, size_t nargs) {
    size_t nparam = ast_list_len(AS_FN_DECL(fn).params);
    if (nargs != nparam) {
        eval_fail_fmt(ctx, "wrong argument count (expected %ld)", (long)nparam);
        return val_void();
    }

    EvalEnv frame = { NULL, ctx->env };
    AstList *pl;
    size_t i = 0;
    for (pl = AS_FN_DECL(fn).params; pl; pl = pl->next, i++) {
        env_insert(ctx, &frame, AS_PARAM(pl->item).name, value_retain(args[i]));
        if (ctx->error) {
            env_free_head(&frame);
            return val_void();
        }
    }

    EvalEnv *saved = ctx->env;
    ctx->env = &frame;
    ctx->halt_return = 0;

    Value out = eval_block(ctx, AS_FN_DECL(fn).body);
    if (ctx->halt_return) {
        out = ctx->return_value;
    }
    ctx->halt_return = 0;
    ctx->env = saved;
    env_free_head(&frame);
    return out;
}

/* Source order: each const initializer sees earlier consts already in `global`. */
static void eval_seed_consts(EvalCtx *ctx, EvalEnv *global) {
    AstList *d;
    for (d = AS_PROGRAM(ctx->program).decls; d; d = d->next) {
        if (d->item->kind == NODE_CONST_DECL) {
            Value v = eval_expr(ctx, AS_CONST_DECL(d->item).value);
            if (ctx->error) {
                return;
            }
            env_insert(ctx, global, AS_CONST_DECL(d->item).name, v);
        }
    }
}

EvalResult eval_call_by_name(AstNode *program, const char *fn_name, const Value *args, size_t nargs) {
    EvalResult er;
    EvalEnv global = { NULL, NULL };
    EvalCtx ctx;

    er.ok = false;
    er.result = val_void();
    er.error_message = NULL;

    if (!program || program->kind != NODE_PROGRAM) {
        er.error_message = "internal: not a program";
        return er;
    }

    AstNode *fn = lookup_fn(program, fn_name);
    if (!fn) {
        er.error_message = "function not found";
        return er;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.program = program;
    ctx.env = &global;

    eval_seed_consts(&ctx, &global);
    if (ctx.error) {
        env_free_head(&global);
        er.error_message = ctx.error;
        return er;
    }

    {
        Value out = eval_invoke_fn(&ctx, fn, args, nargs);
        if (ctx.error) {
            env_free_head(&global);
            er.error_message = ctx.error;
            return er;
        }
        env_free_head(&global);
        er.ok = true;
        er.result = out;
        return er;
    }
}
