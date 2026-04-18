#include "compile.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BC_NO_STRUCT 255u

typedef struct Ctx Ctx;

typedef struct {
    uint8_t is_local; /* 1 = enclosing local slot; 0 = enclosing upvalue */
    uint8_t index;
} UpvalDesc;

struct Ctx {
    CompileProgramResult *bc;
    int                   self_idx;
    Chunk                *chunk;
    AstNode              *program;
    Ctx                  *enclosing;
    const char           *local_names[256];
    uint8_t               local_struct[256]; /* BC_NO_STRUCT or BcStructLayout index */
    uint8_t               next_slot;
    UpvalDesc             upvals[64];
    uint8_t               nupvals;
    const char           *error;
};

static void compile_fail(Ctx *c, const char *msg) {
    if (c && !c->error) {
        c->error = msg;
    }
}

static void ctx_refresh_chunk(Ctx *c) {
    if (c && c->bc && c->self_idx >= 0) {
        c->chunk = &c->bc->prog.chunks[(size_t)c->self_idx];
    }
}

static int bc_append_chunk(CompileProgramResult *r) {
    size_t n = r->prog.nchunks;
    Chunk *p = (Chunk *)realloc(r->prog.chunks, (n + 1u) * sizeof(Chunk));
    if (!p) {
        return -1;
    }
    r->prog.chunks = p;
    chunk_init(&r->prog.chunks[n]);
    r->prog.nchunks = n + 1u;
    return (int)n;
}

static int count_fn_decls(AstNode *program) {
    int cn = 0;
    AstList *d;
    AstList *m;
    if (!program || program->kind != NODE_PROGRAM) {
        return 0;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL) {
            cn++;
        } else if (d->item->kind == NODE_IMPL_DECL) {
            for (m = AS_IMPL_DECL(d->item).methods; m; m = m->next) {
                cn++;
            }
        }
    }
    return cn;
}

static AstNode *fn_decl_at_index(AstNode *program, int idx) {
    int n = 0;
    AstList *d;
    AstList *m;
    if (!program || program->kind != NODE_PROGRAM) {
        return NULL;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL) {
            if (n == idx) {
                return d->item;
            }
            n++;
        } else if (d->item->kind == NODE_IMPL_DECL) {
            for (m = AS_IMPL_DECL(d->item).methods; m; m = m->next) {
                if (n == idx) {
                    return m->item;
                }
                n++;
            }
        }
    }
    return NULL;
}

int bc_fn_index(AstNode *program, const char *name) {
    int idx = 0;
    AstList *d;
    AstList *m;
    if (!program || program->kind != NODE_PROGRAM) {
        return -1;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL) {
            if (strcmp(AS_FN_DECL(d->item).name, name) == 0) {
                return idx;
            }
            idx++;
        } else if (d->item->kind == NODE_IMPL_DECL) {
            for (m = AS_IMPL_DECL(d->item).methods; m; m = m->next) {
                if (strcmp(AS_FN_DECL(m->item).name, name) == 0) {
                    return idx;
                }
                idx++;
            }
        }
    }
    return -1;
}

static int fn_name_to_index(AstNode *program, const char *name) {
    return bc_fn_index(program, name);
}

static AstNode *lookup_struct_decl_node(AstNode *program, const char *name) {
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM) {
        return NULL;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_STRUCT_DECL && strcmp(AS_STRUCT_DECL(d->item).name, name) == 0) {
            return d->item;
        }
    }
    return NULL;
}

static int struct_layout_named(const CompileProgramResult *r, const char *name) {
    size_t i;
    if (!r || !name || !r->prog.structs) {
        return -1;
    }
    for (i = 0; i < r->prog.nstructs; i++) {
        if (strcmp(r->prog.structs[i].type_name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int fn_self_struct_layout(AstNode *program, AstNode *fn, const CompileProgramResult *r) {
    AstList *d;
    AstList *m;
    if (!program || program->kind != NODE_PROGRAM || !fn) {
        return -1;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind != NODE_IMPL_DECL) {
            continue;
        }
        for (m = AS_IMPL_DECL(d->item).methods; m; m = m->next) {
            if (m->item == fn) {
                return struct_layout_named(r, AS_IMPL_DECL(d->item).struct_name);
            }
        }
    }
    return -1;
}

static int field_index_for_layout(const BcStructLayout *L, const char *field) {
    size_t i;
    if (!L) {
        return -1;
    }
    for (i = 0; i < L->nfields; i++) {
        if (strcmp(L->field_names[i], field) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Intern field name (or other) string for OP_GET_FIELD_NAMED; pool owned by compile result. */
static bool bc_intern_symbol(CompileProgramResult *r, const char *s, uint16_t *out_idx) {
    size_t i;
    char *copy;
    char **np;

    if (!r || !s || !out_idx) {
        return false;
    }
    for (i = 0; i < r->prog.nsymbols; i++) {
        if (strcmp(r->prog.symbol_pool[i], s) == 0) {
            *out_idx = (uint16_t)i;
            return true;
        }
    }
    if (r->prog.nsymbols >= 65536u) {
        return false;
    }
    copy = strdup(s);
    if (!copy) {
        return false;
    }
    if (r->prog.nsymbols >= r->prog.symbol_cap) {
        size_t nc = r->prog.symbol_cap ? r->prog.symbol_cap * 2u : 8u;
        np = (char **)realloc(r->prog.symbol_pool, nc * sizeof(char *));
        if (!np) {
            free(copy);
            return false;
        }
        r->prog.symbol_pool = np;
        r->prog.symbol_cap = nc;
    }
    r->prog.symbol_pool[r->prog.nsymbols++] = copy;
    *out_idx = (uint16_t)(r->prog.nsymbols - 1u);
    return true;
}

static void compile_expr(Ctx *c, AstNode *n);

static void compile_struct_init(Ctx *c, AstNode *n) {
    const char *sname;
    int li;
    AstNode *decl;
    AstList *fl;
    AstList *in;
    AstNode *base_expr;
    uint8_t base_slot = 0;
    int field_idx;

    if (!n || n->kind != NODE_STRUCT_INIT) {
        compile_fail(c, "internal: struct init");
        return;
    }
    sname = AS_STRUCT_INIT(n).struct_name;
    li = struct_layout_named(c->bc, sname);
    decl = lookup_struct_decl_node(c->program, sname);
    if (li < 0 || !decl) {
        compile_fail(c, "unknown struct type for bytecode");
        return;
    }
    base_expr = AS_STRUCT_INIT(n).base;

    if (base_expr) {
        if (li >= (int)BC_NO_STRUCT) {
            compile_fail(c, "struct layout index too large for bytecode");
            return;
        }
        base_slot = c->next_slot++;
        if (c->next_slot > 255) {
            compile_fail(c, "too many locals");
            return;
        }
        if (c->chunk->nlocals < c->next_slot) {
            c->chunk->nlocals = c->next_slot;
        }
        compile_expr(c, base_expr);
        if (c->error) {
            return;
        }
        chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
        chunk_emit_u8(c->chunk, base_slot);
        c->local_struct[base_slot] = (uint8_t)li;
    }

    field_idx = 0;
    for (fl = AS_STRUCT_DECL(decl).fields; fl && !c->error; fl = fl->next, field_idx++) {
        const char *fname = AS_STRUCT_FIELD(fl->item).name;
        bool got = false;
        for (in = AS_STRUCT_INIT(n).fields; in; in = in->next) {
            if (strcmp(AS_FIELD_INIT(in->item).name, fname) == 0) {
                compile_expr(c, AS_FIELD_INIT(in->item).value);
                got = true;
                break;
            }
        }
        if (!got) {
            if (!base_expr) {
                compile_fail(c, "missing struct field in bytecode initializer");
                return;
            }
            if (field_idx < 0 || field_idx >= 256) {
                compile_fail(c, "too many struct fields for bytecode");
                return;
            }
            chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
            chunk_emit_u8(c->chunk, base_slot);
            chunk_emit_u8(c->chunk, OP_GET_FIELD);
            chunk_emit_u8(c->chunk, (uint8_t)field_idx);
        }
    }
    if (c->error) {
        return;
    }
    if (li > 65535) {
        compile_fail(c, "too many struct layouts");
        return;
    }
    chunk_emit_u8(c->chunk, OP_STRUCT_NEW);
    chunk_emit_u16(c->chunk, (uint16_t)li);
}

static size_t count_struct_decls(AstNode *program) {
    size_t n = 0;
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM) {
        return 0;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_STRUCT_DECL) {
            n++;
        }
    }
    return n;
}

static bool fill_struct_layouts(CompileProgramResult *r, AstNode *program) {
    size_t ns = count_struct_decls(program);
    size_t si = 0;
    AstList *d;

    r->prog.nstructs = 0;
    r->prog.structs = NULL;
    if (ns == 0) {
        return true;
    }
    r->prog.structs = (BcStructLayout *)calloc(ns, sizeof(BcStructLayout));
    if (!r->prog.structs) {
        return false;
    }
    r->prog.nstructs = ns;

    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind != NODE_STRUCT_DECL) {
            continue;
        }
        {
            AstNode *st = d->item;
            size_t nf = ast_list_len(AS_STRUCT_DECL(st).fields);
            AstList *fl;
            size_t i;
            const char **fnames;

            r->prog.structs[si].type_name = AS_STRUCT_DECL(st).name;
            r->prog.structs[si].nfields = nf;
            if (nf > 0) {
                fnames = (const char **)malloc(nf * sizeof(char *));
                if (!fnames) {
                    size_t j;
                    for (j = 0; j < si; j++) {
                        free((void *)r->prog.structs[j].field_names);
                        r->prog.structs[j].field_names = NULL;
                    }
                    free(r->prog.structs);
                    r->prog.structs = NULL;
                    r->prog.nstructs = 0;
                    return false;
                }
                i = 0;
                for (fl = AS_STRUCT_DECL(st).fields; fl; fl = fl->next) {
                    fnames[i++] = AS_STRUCT_FIELD(fl->item).name;
                }
                r->prog.structs[si].field_names = fnames;
            } else {
                r->prog.structs[si].field_names = NULL;
            }
            si++;
        }
    }
    return true;
}

static int64_t lookup_int_const(AstNode *program, const char *name, int *found) {
    AstList *d;
    *found = 0;
    if (!program || program->kind != NODE_PROGRAM) {
        return 0;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_CONST_DECL && strcmp(AS_CONST_DECL(d->item).name, name) == 0) {
            AstNode *val = AS_CONST_DECL(d->item).value;
            if (val && val->kind == NODE_LIT_INT) {
                *found = 1;
                return AS_LIT_INT(val).value;
            }
            return 0;
        }
    }
    return 0;
}

static bool lookup_bool_const(AstNode *program, const char *name, int *found) {
    AstList *d;
    *found = 0;
    if (!program || program->kind != NODE_PROGRAM) {
        return false;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_CONST_DECL && strcmp(AS_CONST_DECL(d->item).name, name) == 0) {
            AstNode *val = AS_CONST_DECL(d->item).value;
            if (val && val->kind == NODE_LIT_BOOL) {
                *found = 1;
                return AS_LIT_BOOL(val).value;
            }
            return false;
        }
    }
    return false;
}

static int resolve_local(Ctx *c, const char *name) {
    uint8_t s;
    for (s = 0; s < c->next_slot; s++) {
        if (c->local_names[s] && strcmp(c->local_names[s], name) == 0) {
            return (int)s;
        }
    }
    return -1;
}

static int add_upvalue(Ctx *c, uint8_t is_local, uint8_t idx) {
    size_t i;
    for (i = 0; i < (size_t)c->nupvals; i++) {
        if (c->upvals[i].is_local == is_local && c->upvals[i].index == idx) {
            return (int)i;
        }
    }
    if (c->nupvals >= 64) {
        compile_fail(c, "too many upvalues");
        return -1;
    }
    c->upvals[c->nupvals].is_local = is_local;
    c->upvals[c->nupvals].index = idx;
    c->nupvals++;
    return (int)(c->nupvals - 1);
}

static int resolve_upvalue(Ctx *c, const char *name) {
    int loc;
    int up;
    if (!c->enclosing) {
        return -1;
    }
    loc = resolve_local(c->enclosing, name);
    if (loc >= 0) {
        return add_upvalue(c, 1, (uint8_t)loc);
    }
    up = resolve_upvalue(c->enclosing, name);
    if (up >= 0) {
        return add_upvalue(c, 0, (uint8_t)up);
    }
    return -1;
}

static void compile_lambda(Ctx *parent, AstNode *lam);
static void compile_for_stmt(Ctx *c, AstNode *st);

/* `trailing_return`: function body ends with OP_RETURN (value optional); otherwise tail expr is discarded. */
static void compile_block_ex(Ctx *c, AstNode *blk, bool trailing_return) {
    AstList *sl;
    if (!blk || blk->kind != NODE_BLOCK) {
        compile_fail(c, "internal: expected block");
        return;
    }
    for (sl = AS_BLOCK(blk).stmts; sl && !c->error; sl = sl->next) {
        AstNode *st = sl->item;
        switch (st->kind) {
            case NODE_LET: {
                uint8_t slot = c->next_slot++;
                if (c->next_slot > 255) {
                    compile_fail(c, "too many locals");
                    break;
                }
                if (c->chunk->nlocals < c->next_slot) {
                    c->chunk->nlocals = c->next_slot;
                }
                c->local_names[slot] = AS_LET(st).name;
                compile_expr(c, AS_LET(st).init);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
                chunk_emit_u8(c->chunk, slot);
                if (!c->error && AS_LET(st).init && AS_LET(st).init->kind == NODE_STRUCT_INIT) {
                    int sli = struct_layout_named(c->bc, AS_STRUCT_INIT(AS_LET(st).init).struct_name);
                    if (sli >= 0 && sli < (int)BC_NO_STRUCT) {
                        c->local_struct[slot] = (uint8_t)sli;
                    }
                }
                break;
            }
            case NODE_RETURN:
                compile_expr(c, AS_RETURN(st).value);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_RETURN);
                return;
            case NODE_EXPR_STMT:
                compile_expr(c, AS_EXPR_STMT(st).expr);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_POP);
                break;
            case NODE_FOR:
                compile_for_stmt(c, st);
                if (c->error) {
                    return;
                }
                break;
            default:
                compile_fail(c, "statement not supported in bytecode");
                return;
        }
    }
    if (c->error) {
        return;
    }
    if (AS_BLOCK(blk).tail_expr) {
        compile_expr(c, AS_BLOCK(blk).tail_expr);
        if (c->error) {
            return;
        }
        if (!trailing_return) {
            chunk_emit_u8(c->chunk, OP_POP);
        }
    }
    if (trailing_return) {
        chunk_emit_u8(c->chunk, OP_RETURN);
    }
}

static void compile_block(Ctx *c, AstNode *blk) {
    compile_block_ex(c, blk, true);
}

static void compile_for_stmt(Ctx *c, AstNode *st) {
    uint8_t arr_slot;
    uint8_t i_slot;
    uint8_t v_slot;
    size_t loop_start;
    size_t at_exit_patch;
    size_t at_back_patch;
    Value zv;
    int zidx;
    AstNode *body;

    if (!st || st->kind != NODE_FOR) {
        compile_fail(c, "internal: for");
        return;
    }
    body = AS_FOR(st).body;
    if (!body || body->kind != NODE_BLOCK) {
        compile_fail(c, "for body must be a block in bytecode");
        return;
    }

    arr_slot = c->next_slot++;
    i_slot = c->next_slot++;
    v_slot = c->next_slot++;
    if (c->next_slot > 255) {
        compile_fail(c, "too many locals");
        return;
    }
    if (c->chunk->nlocals < c->next_slot) {
        c->chunk->nlocals = c->next_slot;
    }
    c->local_names[v_slot] = AS_FOR(st).var;

    compile_expr(c, AS_FOR(st).iter);
    if (c->error) {
        return;
    }
    chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
    chunk_emit_u8(c->chunk, arr_slot);

    zv.kind = VAL_INT;
    zv.as.i = 0;
    zidx = chunk_add_constant(c->chunk, zv);
    if (zidx < 0) {
        compile_fail(c, "out of memory");
        return;
    }
    chunk_emit_u8(c->chunk, OP_PUSH_CONST);
    chunk_emit_u16(c->chunk, (uint16_t)zidx);
    chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
    chunk_emit_u8(c->chunk, i_slot);

    loop_start = c->chunk->len;
    chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
    chunk_emit_u8(c->chunk, i_slot);
    chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
    chunk_emit_u8(c->chunk, arr_slot);
    chunk_emit_u8(c->chunk, OP_ARRAY_LEN);
    chunk_emit_u8(c->chunk, OP_LT_INT);
    at_exit_patch = c->chunk->len;
    chunk_emit_u8(c->chunk, OP_POP_JUMP_IF_FALSE);
    chunk_emit_u16(c->chunk, 0);

    chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
    chunk_emit_u8(c->chunk, arr_slot);
    chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
    chunk_emit_u8(c->chunk, i_slot);
    chunk_emit_u8(c->chunk, OP_INDEX_INT);
    chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
    chunk_emit_u8(c->chunk, v_slot);

    compile_block_ex(c, body, false);
    if (c->error) {
        return;
    }

    chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
    chunk_emit_u8(c->chunk, i_slot);
    zv.as.i = 1;
    zidx = chunk_add_constant(c->chunk, zv);
    if (zidx < 0) {
        compile_fail(c, "out of memory");
        return;
    }
    chunk_emit_u8(c->chunk, OP_PUSH_CONST);
    chunk_emit_u16(c->chunk, (uint16_t)zidx);
    chunk_emit_u8(c->chunk, OP_ADD_INT);
    chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
    chunk_emit_u8(c->chunk, i_slot);

    at_back_patch = c->chunk->len;
    chunk_emit_u8(c->chunk, OP_JUMP);
    chunk_emit_u16(c->chunk, 0);

    {
        int16_t back = (int16_t)((int64_t)loop_start - (int64_t)(at_back_patch + 3u));
        chunk_patch_u16(c->chunk, at_back_patch + 1u, (uint16_t)back);
    }
    {
        int16_t ex = (int16_t)((int64_t)c->chunk->len - (int64_t)(at_exit_patch + 3u));
        chunk_patch_u16(c->chunk, at_exit_patch + 1u, (uint16_t)ex);
    }
}

/* Block as a value (e.g. `if` branch): must end with a tail expression; no `return`. */
static void compile_block_as_value(Ctx *c, AstNode *blk) {
    AstList *sl;
    if (!blk || blk->kind != NODE_BLOCK) {
        compile_fail(c, "internal: expected block");
        return;
    }
    for (sl = AS_BLOCK(blk).stmts; sl && !c->error; sl = sl->next) {
        AstNode *st = sl->item;
        switch (st->kind) {
            case NODE_LET: {
                uint8_t slot = c->next_slot++;
                if (c->next_slot > 255) {
                    compile_fail(c, "too many locals");
                    break;
                }
                if (c->chunk->nlocals < c->next_slot) {
                    c->chunk->nlocals = c->next_slot;
                }
                c->local_names[slot] = AS_LET(st).name;
                compile_expr(c, AS_LET(st).init);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
                chunk_emit_u8(c->chunk, slot);
                if (!c->error && AS_LET(st).init && AS_LET(st).init->kind == NODE_STRUCT_INIT) {
                    int sli = struct_layout_named(c->bc, AS_STRUCT_INIT(AS_LET(st).init).struct_name);
                    if (sli >= 0 && sli < (int)BC_NO_STRUCT) {
                        c->local_struct[slot] = (uint8_t)sli;
                    }
                }
                break;
            }
            case NODE_RETURN:
                compile_fail(c, "return in branch block not supported in bytecode");
                return;
            case NODE_EXPR_STMT:
                compile_expr(c, AS_EXPR_STMT(st).expr);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_POP);
                break;
            case NODE_FOR:
                compile_for_stmt(c, st);
                if (c->error) {
                    return;
                }
                break;
            default:
                compile_fail(c, "statement not supported in bytecode");
                return;
        }
    }
    if (c->error) {
        return;
    }
    if (!AS_BLOCK(blk).tail_expr) {
        compile_fail(c, "branch block needs a tail expression");
        return;
    }
    compile_expr(c, AS_BLOCK(blk).tail_expr);
}

static void compile_if_expr(Ctx *c, AstNode *n) {
    AstList *br;
    AstNode *els;
    size_t exit_patches[48];
    size_t n_exit = 0;

    if (!n || n->kind != NODE_IF) {
        compile_fail(c, "internal: if");
        return;
    }
    els = AS_IF(n).else_body;
    if (!els) {
        compile_fail(c, "bytecode if needs else branch");
        return;
    }
    for (br = AS_IF(n).branches; br && !c->error; ) {
        AstNode *cond;
        AstNode *body;
        size_t at_popjf;
        size_t jmp_out;

        cond = br->item;
        br = br->next;
        if (!br) {
            compile_fail(c, "internal: if branches");
            return;
        }
        body = br->item;
        br = br->next;

        compile_expr(c, cond);
        if (c->error) {
            return;
        }
        at_popjf = c->chunk->len;
        chunk_emit_u8(c->chunk, OP_POP_JUMP_IF_FALSE);
        chunk_emit_u16(c->chunk, 0);
        compile_block_as_value(c, body);
        if (c->error) {
            return;
        }
        jmp_out = c->chunk->len;
        chunk_emit_u8(c->chunk, OP_JUMP);
        chunk_emit_u16(c->chunk, 0);
        if (n_exit >= sizeof(exit_patches) / sizeof(exit_patches[0])) {
            compile_fail(c, "too many else-if branches for bytecode");
            return;
        }
        exit_patches[n_exit++] = jmp_out;
        chunk_patch_u16(c->chunk, at_popjf + 1u, (uint16_t)(c->chunk->len - (at_popjf + 3u)));
    }
    compile_block_as_value(c, els);
    if (c->error) {
        return;
    }
    {
        size_t end_ip = c->chunk->len;
        size_t i;
        for (i = 0; i < n_exit; i++) {
            size_t j = exit_patches[i];
            chunk_patch_u16(c->chunk, j + 1u, (uint16_t)(end_ip - (j + 3u)));
        }
    }
}

static void compile_lambda(Ctx *parent, AstNode *lam) {
    int chi;
    Ctx child;
    AstList *pl;
    size_t i;
    size_t nparams;

    chi = bc_append_chunk(parent->bc);
    if (chi < 0) {
        compile_fail(parent, "out of memory");
        return;
    }
    ctx_refresh_chunk(parent);

    memset(&child, 0, sizeof(child));
    memset(child.local_struct, BC_NO_STRUCT, sizeof(child.local_struct));
    child.bc = parent->bc;
    child.self_idx = chi;
    child.chunk = &parent->bc->prog.chunks[(size_t)chi];
    child.program = parent->program;
    child.enclosing = parent;

    nparams = ast_list_len(AS_LAMBDA(lam).params);
    if (nparams > 255u) {
        compile_fail(parent, "too many lambda parameters");
        return;
    }
    for (pl = AS_LAMBDA(lam).params, i = 0; pl; pl = pl->next, i++) {
        AstNode *pm = pl->item;
        child.local_names[i] = AS_PARAM(pm).name;
    }
    child.next_slot = (uint8_t)nparams;
    child.chunk->nparams = (uint8_t)nparams;
    if (child.next_slot != nparams) {
        compile_fail(parent, "too many lambda parameters");
        return;
    }
    child.chunk->nlocals = child.next_slot;

    if (AS_LAMBDA(lam).body->kind == NODE_BLOCK) {
        compile_block(&child, AS_LAMBDA(lam).body);
    } else {
        compile_expr(&child, AS_LAMBDA(lam).body);
        if (!child.error) {
            chunk_emit_u8(child.chunk, OP_RETURN);
        }
    }

    if (child.error) {
        compile_fail(parent, child.error);
        return;
    }

    child.chunk->nlocals = child.next_slot;
    child.chunk->nupvalues = child.nupvals;

    ctx_refresh_chunk(parent);

    if (chi > 65535) {
        compile_fail(parent, "too many chunks for bytecode");
        return;
    }
    chunk_emit_u8(parent->chunk, OP_CLOSURE);
    chunk_emit_u16(parent->chunk, (uint16_t)chi);
    chunk_emit_u8(parent->chunk, child.nupvals);
    for (i = 0; i < (size_t)child.nupvals; i++) {
        chunk_emit_u8(parent->chunk, child.upvals[i].is_local ? 1u : 0u);
        chunk_emit_u8(parent->chunk, child.upvals[i].index);
    }
}

static void compile_expr(Ctx *c, AstNode *n) {
    if (!n || c->error) {
        return;
    }

    switch (n->kind) {
        case NODE_LIT_INT: {
            Value v;
            int idx;
            v.kind = VAL_INT;
            v.as.i = AS_LIT_INT(n).value;
            idx = chunk_add_constant(c->chunk, v);
            if (idx < 0) {
                compile_fail(c, "out of memory");
                return;
            }
            chunk_emit_u8(c->chunk, OP_PUSH_CONST);
            chunk_emit_u16(c->chunk, (uint16_t)idx);
            return;
        }
        case NODE_LIT_FLOAT: {
            Value v;
            int idx;
            v.kind = VAL_FLOAT;
            v.as.f = AS_LIT_FLOAT(n).value;
            idx = chunk_add_constant(c->chunk, v);
            if (idx < 0) {
                compile_fail(c, "out of memory");
                return;
            }
            chunk_emit_u8(c->chunk, OP_PUSH_CONST);
            chunk_emit_u16(c->chunk, (uint16_t)idx);
            return;
        }
        case NODE_LIT_DOUBLE: {
            Value v;
            int idx;
            v.kind = VAL_DOUBLE;
            v.as.d = AS_LIT_DOUBLE(n).value;
            idx = chunk_add_constant(c->chunk, v);
            if (idx < 0) {
                compile_fail(c, "out of memory");
                return;
            }
            chunk_emit_u8(c->chunk, OP_PUSH_CONST);
            chunk_emit_u16(c->chunk, (uint16_t)idx);
            return;
        }
        case NODE_LIT_BOOL:
            chunk_emit_u8(c->chunk, AS_LIT_BOOL(n).value ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            return;
        case NODE_BLOCK:
            compile_block_as_value(c, n);
            return;
        case NODE_ASSIGN: {
            AstNode *tgt = AS_ASSIGN(n).target;
            AstNode *val = AS_ASSIGN(n).value;
            if (!tgt || tgt->kind != NODE_IDENT) {
                compile_fail(c, "bytecode assign needs ident target");
                return;
            }
            compile_expr(c, val);
            if (c->error) {
                return;
            }
            chunk_emit_u8(c->chunk, OP_DUP);
            {
                const char *nm = AS_IDENT(tgt).name;
                int loc = resolve_local(c, nm);
                if (loc >= 0) {
                    chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
                    chunk_emit_u8(c->chunk, (uint8_t)loc);
                    return;
                }
                {
                    int up = resolve_upvalue(c, nm);
                    if (up >= 0) {
                        chunk_emit_u8(c->chunk, OP_SET_UPVALUE);
                        chunk_emit_u8(c->chunk, (uint8_t)up);
                        return;
                    }
                }
            }
            compile_fail(c, "bytecode assign to unknown binding");
            return;
        }
        case NODE_IDENT: {
            const char *nm = AS_IDENT(n).name;
            int loc = resolve_local(c, nm);
            if (loc >= 0) {
                chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
                chunk_emit_u8(c->chunk, (uint8_t)loc);
                return;
            }
            {
                int up = resolve_upvalue(c, nm);
                if (up >= 0) {
                    chunk_emit_u8(c->chunk, OP_GET_UPVALUE);
                    chunk_emit_u8(c->chunk, (uint8_t)up);
                    return;
                }
            }
            {
                int fb = 0;
                bool bconst = lookup_bool_const(c->program, nm, &fb);
                if (fb) {
                    chunk_emit_u8(c->chunk, bconst ? OP_PUSH_TRUE : OP_PUSH_FALSE);
                    return;
                }
            }
            {
                int fc = 0;
                int64_t iv = lookup_int_const(c->program, nm, &fc);
                if (fc) {
                    Value v;
                    int idx;
                    v.kind = VAL_INT;
                    v.as.i = iv;
                    idx = chunk_add_constant(c->chunk, v);
                    if (idx < 0) {
                        compile_fail(c, "out of memory");
                        return;
                    }
                    chunk_emit_u8(c->chunk, OP_PUSH_CONST);
                    chunk_emit_u16(c->chunk, (uint16_t)idx);
                    return;
                }
            }
            {
                int gfi = fn_name_to_index(c->program, nm);
                if (gfi >= 0) {
                    if (gfi > 65535) {
                        compile_fail(c, "too many functions for bytecode");
                        return;
                    }
                    chunk_emit_u8(c->chunk, OP_PUSH_FN);
                    chunk_emit_u16(c->chunk, (uint16_t)gfi);
                    return;
                }
            }
            compile_fail(c, "unknown identifier for bytecode");
            return;
        }
        case NODE_UNARY:
            if (AS_UNARY(n).op == UNOP_NEG) {
                compile_expr(c, AS_UNARY(n).operand);
                if (c->error) {
                    return;
                }
                switch (AS_UNARY(n).operand->bc_ty) {
                    case AST_BC_TY_INT:
                        chunk_emit_u8(c->chunk, OP_NEG_INT);
                        return;
                    case AST_BC_TY_FLOAT:
                        chunk_emit_u8(c->chunk, OP_NEG_FLOAT);
                        return;
                    case AST_BC_TY_DOUBLE:
                        chunk_emit_u8(c->chunk, OP_NEG_DOUBLE);
                        return;
                    default:
                        compile_fail(c, "bytecode unary '-' needs numeric operand");
                        return;
                }
            }
            if (AS_UNARY(n).op == UNOP_NOT) {
                compile_expr(c, AS_UNARY(n).operand);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_NOT);
                return;
            }
            compile_fail(c, "unsupported unary");
            return;
        case NODE_IF:
            compile_if_expr(c, n);
            return;
        case NODE_LAMBDA:
            compile_lambda(c, n);
            return;
        case NODE_CALL: {
            AstNode *cal = AS_CALL(n).callee;
            size_t got = ast_list_len(AS_CALL(n).args);
            AstList *arg;

            if (!cal) {
                compile_fail(c, "internal: call");
                return;
            }

            if (cal->kind == NODE_IDENT) {
                const char *nm = AS_IDENT(cal).name;
                int loc = resolve_local(c, nm);
                int up = loc < 0 ? resolve_upvalue(c, nm) : -1;
                int fidx = fn_name_to_index(c->program, nm);

                if (fidx >= 0 && loc < 0 && up < 0) {
                    AstNode *callee_fn = fn_decl_at_index(c->program, fidx);
                    size_t expect;
                    if (!callee_fn) {
                        compile_fail(c, "internal: bad function index");
                        return;
                    }
                    expect = ast_list_len(AS_FN_DECL(callee_fn).params);
                    if (expect != got) {
                        compile_fail(c, "wrong argument count in call");
                        return;
                    }
                    for (arg = AS_CALL(n).args; arg && !c->error; arg = arg->next) {
                        compile_expr(c, arg->item);
                    }
                    if (c->error) {
                        return;
                    }
                    if (fidx > 65535) {
                        compile_fail(c, "too many functions for bytecode");
                        return;
                    }
                    chunk_emit_u8(c->chunk, OP_CALL);
                    chunk_emit_u16(c->chunk, (uint16_t)fidx);
                    chunk_emit_u8(c->chunk, (uint8_t)got);
                    return;
                }
                if (loc >= 0) {
                    chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
                    chunk_emit_u8(c->chunk, (uint8_t)loc);
                } else if (up >= 0) {
                    chunk_emit_u8(c->chunk, OP_GET_UPVALUE);
                    chunk_emit_u8(c->chunk, (uint8_t)up);
                } else {
                    compile_fail(c, "unknown call target");
                    return;
                }

                for (arg = AS_CALL(n).args; arg && !c->error; arg = arg->next) {
                    compile_expr(c, arg->item);
                }
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_CALL_CLOSURE);
                chunk_emit_u8(c->chunk, (uint8_t)got);
                return;
            }

            compile_expr(c, cal);
            for (arg = AS_CALL(n).args; arg && !c->error; arg = arg->next) {
                compile_expr(c, arg->item);
            }
            if (c->error) {
                return;
            }
            chunk_emit_u8(c->chunk, OP_CALL_CLOSURE);
            chunk_emit_u8(c->chunk, (uint8_t)got);
            return;
        }
        case NODE_BINARY: {
            BinOp op = AS_BINARY(n).op;
            if (op == BINOP_AND) {
                size_t at_jf;
                size_t at_j;
                size_t fl;
                size_t el;
                compile_expr(c, AS_BINARY(n).left);
                if (c->error) {
                    return;
                }
                at_jf = c->chunk->len;
                chunk_emit_u8(c->chunk, OP_POP_JUMP_IF_FALSE);
                chunk_emit_u16(c->chunk, 0);
                compile_expr(c, AS_BINARY(n).right);
                if (c->error) {
                    return;
                }
                at_j = c->chunk->len;
                chunk_emit_u8(c->chunk, OP_JUMP);
                chunk_emit_u16(c->chunk, 0);
                fl = c->chunk->len;
                chunk_emit_u8(c->chunk, OP_PUSH_FALSE);
                el = c->chunk->len;
                chunk_patch_u16(c->chunk, at_jf + 1u, (uint16_t)(fl - (at_jf + 3u)));
                chunk_patch_u16(c->chunk, at_j + 1u, (uint16_t)(el - (at_j + 3u)));
                return;
            }
            if (op == BINOP_OR) {
                size_t at_jt;
                size_t at_j;
                size_t tl;
                size_t el;
                compile_expr(c, AS_BINARY(n).left);
                if (c->error) {
                    return;
                }
                at_jt = c->chunk->len;
                chunk_emit_u8(c->chunk, OP_POP_JUMP_IF_TRUE);
                chunk_emit_u16(c->chunk, 0);
                compile_expr(c, AS_BINARY(n).right);
                if (c->error) {
                    return;
                }
                at_j = c->chunk->len;
                chunk_emit_u8(c->chunk, OP_JUMP);
                chunk_emit_u16(c->chunk, 0);
                tl = c->chunk->len;
                chunk_emit_u8(c->chunk, OP_PUSH_TRUE);
                el = c->chunk->len;
                chunk_patch_u16(c->chunk, at_jt + 1u, (uint16_t)(tl - (at_jt + 3u)));
                chunk_patch_u16(c->chunk, at_j + 1u, (uint16_t)(el - (at_j + 3u)));
                return;
            }
            compile_expr(c, AS_BINARY(n).left);
            compile_expr(c, AS_BINARY(n).right);
            if (c->error) {
                return;
            }
            switch (op) {
                case BINOP_ADD:
                    switch (n->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_ADD_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_ADD_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_ADD_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '+' needs numeric operands");
                            return;
                    }
                case BINOP_SUB:
                    switch (n->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_SUB_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_SUB_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_SUB_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '-' needs numeric operands");
                            return;
                    }
                case BINOP_MUL:
                    switch (n->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_MUL_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_MUL_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_MUL_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '*' needs numeric operands");
                            return;
                    }
                case BINOP_DIV:
                    switch (n->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_DIV_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_DIV_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_DIV_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '/' needs numeric operands");
                            return;
                    }
                case BINOP_MOD:
                    chunk_emit_u8(c->chunk, OP_MOD_INT);
                    return;
                case BINOP_EQ:
                    chunk_emit_u8(c->chunk, OP_EQ);
                    return;
                case BINOP_NEQ:
                    chunk_emit_u8(c->chunk, OP_NE);
                    return;
                case BINOP_LT:
                    switch (AS_BINARY(n).left->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_LT_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_LT_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_LT_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '<' needs ordered operands");
                            return;
                    }
                case BINOP_GT:
                    switch (AS_BINARY(n).left->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_GT_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_GT_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_GT_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '>' needs ordered operands");
                            return;
                    }
                case BINOP_LTE:
                    switch (AS_BINARY(n).left->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_LTE_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_LTE_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_LTE_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '<=' needs ordered operands");
                            return;
                    }
                case BINOP_GTE:
                    switch (AS_BINARY(n).left->bc_ty) {
                        case AST_BC_TY_INT:
                            chunk_emit_u8(c->chunk, OP_GTE_INT);
                            return;
                        case AST_BC_TY_FLOAT:
                            chunk_emit_u8(c->chunk, OP_GTE_FLOAT);
                            return;
                        case AST_BC_TY_DOUBLE:
                            chunk_emit_u8(c->chunk, OP_GTE_DOUBLE);
                            return;
                        default:
                            compile_fail(c, "bytecode '>=' needs ordered operands");
                            return;
                    }
                default:
                    compile_fail(c, "binary operator not supported in bytecode");
                    return;
            }
        }
        case NODE_ARRAY: {
            AstList *el;
            size_t na = ast_list_len(AS_ARRAY(n).elements);
            if (na > 65535u) {
                compile_fail(c, "array too large for bytecode");
                return;
            }
            for (el = AS_ARRAY(n).elements; el && !c->error; el = el->next) {
                compile_expr(c, el->item);
            }
            if (c->error) {
                return;
            }
            chunk_emit_u8(c->chunk, OP_ARRAY_NEW);
            chunk_emit_u16(c->chunk, (uint16_t)na);
            return;
        }
        case NODE_TUPLE: {
            AstList *el;
            size_t nt = ast_list_len(AS_TUPLE(n).elements);
            if (nt > 65535u) {
                compile_fail(c, "tuple too large for bytecode");
                return;
            }
            for (el = AS_TUPLE(n).elements; el && !c->error; el = el->next) {
                compile_expr(c, el->item);
            }
            if (c->error) {
                return;
            }
            chunk_emit_u8(c->chunk, OP_TUPLE_NEW);
            chunk_emit_u16(c->chunk, (uint16_t)nt);
            return;
        }
        case NODE_INDEX: {
            compile_expr(c, AS_INDEX(n).object);
            if (c->error) {
                return;
            }
            compile_expr(c, AS_INDEX(n).index);
            if (c->error) {
                return;
            }
            chunk_emit_u8(c->chunk, OP_INDEX_INT);
            return;
        }
        case NODE_STRUCT_INIT:
            compile_struct_init(c, n);
            return;
        case NODE_FIELD_ACCESS: {
            AstNode *obj = AS_FIELD_ACCESS(n).object;
            const char *fname = AS_FIELD_ACCESS(n).field;
            int fidx;
            compile_expr(c, obj);
            if (c->error) {
                return;
            }
            if (obj->kind == NODE_IDENT) {
                int loc = resolve_local(c, AS_IDENT(obj).name);
                if (loc >= 0 && c->local_struct[loc] != BC_NO_STRUCT) {
                    const BcStructLayout *L = &c->bc->prog.structs[c->local_struct[loc]];
                    fidx = field_index_for_layout(L, fname);
                    if (fidx >= 0 && fidx < 256) {
                        chunk_emit_u8(c->chunk, OP_GET_FIELD);
                        chunk_emit_u8(c->chunk, (uint8_t)fidx);
                        return;
                    }
                }
            }
            {
                uint16_t sym;
                if (!bc_intern_symbol(c->bc, fname, &sym)) {
                    compile_fail(c, "out of memory or too many bytecode symbols");
                    return;
                }
                chunk_emit_u8(c->chunk, OP_GET_FIELD_NAMED);
                chunk_emit_u16(c->chunk, sym);
            }
            return;
        }
        case NODE_METHOD_CALL: {
            const char *mname = AS_METHOD_CALL(n).method;
            int fidx = fn_name_to_index(c->program, mname);
            AstNode *mfn;
            size_t expect;
            size_t nargs;
            AstList *arg;
            if (fidx < 0) {
                compile_fail(c, "unknown method in bytecode");
                return;
            }
            mfn = fn_decl_at_index(c->program, fidx);
            if (!mfn) {
                compile_fail(c, "internal: method index");
                return;
            }
            expect = ast_list_len(AS_FN_DECL(mfn).params);
            nargs = 1u + ast_list_len(AS_METHOD_CALL(n).args);
            if (expect != nargs) {
                compile_fail(c, "wrong argument count in method call");
                return;
            }
            compile_expr(c, AS_METHOD_CALL(n).receiver);
            if (c->error) {
                return;
            }
            for (arg = AS_METHOD_CALL(n).args; arg && !c->error; arg = arg->next) {
                compile_expr(c, arg->item);
            }
            if (c->error) {
                return;
            }
            if (fidx > 65535) {
                compile_fail(c, "too many functions for bytecode");
                return;
            }
            chunk_emit_u8(c->chunk, OP_CALL);
            chunk_emit_u16(c->chunk, (uint16_t)fidx);
            chunk_emit_u8(c->chunk, (uint8_t)nargs);
            return;
        }
        default:
            compile_fail(c, "expression not supported in bytecode");
            return;
    }
}

/* NULL on success, else static error string. */
static const char *compile_one_fn(
    CompileProgramResult *r,
    int fn_idx,
    AstNode *program,
    AstNode *fn,
    int self_struct_layout_idx) {
    Ctx c;
    AstList *pl;
    size_t i;
    size_t nparams;

    memset(&c, 0, sizeof(c));
    memset(c.local_struct, BC_NO_STRUCT, sizeof(c.local_struct));
    c.bc = r;
    c.self_idx = fn_idx;
    c.chunk = &r->prog.chunks[(size_t)fn_idx];
    c.program = program;
    c.enclosing = NULL;

    nparams = ast_list_len(AS_FN_DECL(fn).params);
    if (nparams > 255u) {
        return "too many parameters";
    }
    for (pl = AS_FN_DECL(fn).params, i = 0; pl; pl = pl->next, i++) {
        c.local_names[i] = AS_PARAM(pl->item).name;
    }
    c.next_slot = (uint8_t)nparams;
    c.chunk->nparams = (uint8_t)nparams;
    if (c.next_slot != nparams) {
        return "too many parameters";
    }
    c.chunk->nlocals = c.next_slot;

    if (self_struct_layout_idx >= 0 && self_struct_layout_idx < (int)BC_NO_STRUCT && nparams > 0 &&
        AS_FN_DECL(fn).params && AS_FN_DECL(fn).params->item &&
        strcmp(AS_PARAM(AS_FN_DECL(fn).params->item).name, "self") == 0) {
        c.local_struct[0] = (uint8_t)self_struct_layout_idx;
    }

    compile_block(&c, AS_FN_DECL(fn).body);
    c.chunk->nlocals = c.next_slot;
    return c.error;
}

CompileProgramResult compile_program_bc(AstNode *program) {
    CompileProgramResult r;
    int nf;
    int fi;

    memset(&r, 0, sizeof(r));
    if (!program || program->kind != NODE_PROGRAM) {
        r.error_message = "internal: not a program";
        return r;
    }

    nf = count_fn_decls(program);
    if (nf == 0) {
        r.error_message = "no functions to compile";
        return r;
    }

    r.prog.chunks = (Chunk *)calloc((size_t)nf, sizeof(Chunk));
    if (!r.prog.chunks) {
        r.error_message = "out of memory";
        return r;
    }
    r.prog.nchunks = (size_t)nf;

    if (!fill_struct_layouts(&r, program)) {
        r.error_message = "out of memory";
        compile_program_result_free(&r);
        return r;
    }

    for (fi = 0; fi < nf; fi++) {
        AstNode *fn = fn_decl_at_index(program, fi);
        if (!fn) {
            r.error_message = "internal: fn index";
            compile_program_result_free(&r);
            return r;
        }
        chunk_init(&r.prog.chunks[fi]);
        {
            int ssl = fn_self_struct_layout(program, fn, &r);
            const char *ce = compile_one_fn(&r, fi, program, fn, ssl);
            if (ce) {
                r.error_message = ce;
                compile_program_result_free(&r);
                return r;
            }
        }
    }

    r.ok = true;
    return r;
}

void compile_program_result_free(CompileProgramResult *r) {
    size_t i;
    if (!r) {
        return;
    }
    if (r->prog.chunks) {
        for (i = 0; i < r->prog.nchunks; i++) {
            chunk_free(&r->prog.chunks[i]);
        }
        free(r->prog.chunks);
        r->prog.chunks = NULL;
        r->prog.nchunks = 0;
    }
    if (r->prog.structs) {
        for (i = 0; i < r->prog.nstructs; i++) {
            free((void *)r->prog.structs[i].field_names);
            r->prog.structs[i].field_names = NULL;
        }
        free(r->prog.structs);
        r->prog.structs = NULL;
        r->prog.nstructs = 0;
    }
    if (r->prog.symbol_pool) {
        for (i = 0; i < r->prog.nsymbols; i++) {
            free(r->prog.symbol_pool[i]);
        }
        free(r->prog.symbol_pool);
        r->prog.symbol_pool = NULL;
        r->prog.nsymbols = 0;
        r->prog.symbol_cap = 0;
    }
    r->ok = false;
    r->error_message = NULL;
}
