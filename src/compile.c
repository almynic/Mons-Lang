#include "compile.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    Chunk          *chunk;
    AstNode        *program;
    const char    **param_names;
    size_t          nparams;
    uint8_t         next_slot;
    const char     *error;
} Ctx;

static void compile_fail(Ctx *c, const char *msg) {
    if (!c->error) {
        c->error = msg;
    }
}

static int count_fn_decls(AstNode *program) {
    int n = 0;
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM) {
        return 0;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind == NODE_FN_DECL) {
            n++;
        }
    }
    return n;
}

static AstNode *fn_decl_at_index(AstNode *program, int idx) {
    int n = 0;
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM) {
        return NULL;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind != NODE_FN_DECL) {
            continue;
        }
        if (n == idx) {
            return d->item;
        }
        n++;
    }
    return NULL;
}

int bc_fn_index(AstNode *program, const char *name) {
    int idx = 0;
    AstList *d;
    if (!program || program->kind != NODE_PROGRAM) {
        return -1;
    }
    for (d = AS_PROGRAM(program).decls; d; d = d->next) {
        if (d->item->kind != NODE_FN_DECL) {
            continue;
        }
        if (strcmp(AS_FN_DECL(d->item).name, name) == 0) {
            return idx;
        }
        idx++;
    }
    return -1;
}

static int fn_name_to_index(AstNode *program, const char *name) {
    return bc_fn_index(program, name);
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

static int find_param_index(Ctx *c, const char *name) {
    size_t i;
    for (i = 0; i < c->nparams; i++) {
        if (c->param_names[i] && strcmp(c->param_names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void compile_expr(Ctx *c, AstNode *n);

static void compile_block(Ctx *c, AstNode *blk) {
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
                compile_expr(c, AS_LET(st).init);
                if (c->error) {
                    return;
                }
                chunk_emit_u8(c->chunk, OP_STORE_LOCAL);
                chunk_emit_u8(c->chunk, slot);
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
    }
    chunk_emit_u8(c->chunk, OP_RETURN);
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
        case NODE_LIT_BOOL:
            chunk_emit_u8(c->chunk, AS_LIT_BOOL(n).value ? OP_PUSH_TRUE : OP_PUSH_FALSE);
            return;
        case NODE_IDENT: {
            const char *nm = AS_IDENT(n).name;
            int pi = find_param_index(c, nm);
            if (pi >= 0) {
                chunk_emit_u8(c->chunk, OP_LOAD_LOCAL);
                chunk_emit_u8(c->chunk, (uint8_t)pi);
                return;
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
            compile_fail(c, "unknown identifier for bytecode (need param or int const)");
            return;
        }
        case NODE_UNARY:
            if (AS_UNARY(n).op == UNOP_NEG) {
                compile_expr(c, AS_UNARY(n).operand);
                chunk_emit_u8(c->chunk, OP_NEG_INT);
                return;
            }
            if (AS_UNARY(n).op == UNOP_NOT) {
                compile_fail(c, "unary ! not supported in bytecode yet");
                return;
            }
            compile_fail(c, "unsupported unary");
            return;
        case NODE_LAMBDA:
            compile_fail(c, "lambdas / closures not supported in bytecode yet");
            return;
        case NODE_CALL: {
            AstNode *cal = AS_CALL(n).callee;
            int fidx;
            AstNode *callee_fn;
            size_t expect;
            size_t got;
            AstList *arg;

            if (!cal || cal->kind != NODE_IDENT) {
                compile_fail(c, "bytecode: only direct calls to named functions");
                return;
            }
            fidx = fn_name_to_index(c->program, AS_IDENT(cal).name);
            if (fidx < 0) {
                compile_fail(c, "call to unknown function");
                return;
            }
            callee_fn = fn_decl_at_index(c->program, fidx);
            if (!callee_fn) {
                compile_fail(c, "internal: bad function index");
                return;
            }
            expect = ast_list_len(AS_FN_DECL(callee_fn).params);
            got = ast_list_len(AS_CALL(n).args);
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
        case NODE_BINARY: {
            BinOp op = AS_BINARY(n).op;
            if (op == BINOP_AND || op == BINOP_OR) {
                compile_fail(c, "&& and || not supported in bytecode yet");
                return;
            }
            compile_expr(c, AS_BINARY(n).left);
            compile_expr(c, AS_BINARY(n).right);
            if (c->error) {
                return;
            }
            switch (op) {
                case BINOP_ADD:
                    chunk_emit_u8(c->chunk, OP_ADD_INT);
                    return;
                case BINOP_SUB:
                    chunk_emit_u8(c->chunk, OP_SUB_INT);
                    return;
                case BINOP_MUL:
                    chunk_emit_u8(c->chunk, OP_MUL_INT);
                    return;
                case BINOP_DIV:
                    chunk_emit_u8(c->chunk, OP_DIV_INT);
                    return;
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
                    chunk_emit_u8(c->chunk, OP_LT_INT);
                    return;
                case BINOP_GT:
                    chunk_emit_u8(c->chunk, OP_GT_INT);
                    return;
                case BINOP_LTE:
                    chunk_emit_u8(c->chunk, OP_LTE_INT);
                    return;
                case BINOP_GTE:
                    chunk_emit_u8(c->chunk, OP_GTE_INT);
                    return;
                default:
                    compile_fail(c, "binary operator not supported in bytecode");
                    return;
            }
        }
        default:
            compile_fail(c, "expression not supported in bytecode");
            return;
    }
}

/* NULL on success, else static error string. */
static const char *compile_one_fn(AstNode *program, AstNode *fn, Chunk *out) {
    Ctx c;
    AstList *pl;
    size_t i;

    memset(&c, 0, sizeof(c));
    c.chunk = out;
    c.program = program;
    c.nparams = ast_list_len(AS_FN_DECL(fn).params);
    c.param_names = (const char **)malloc(c.nparams * sizeof(char *));
    if (c.nparams > 0 && !c.param_names) {
        return "out of memory";
    }
    for (pl = AS_FN_DECL(fn).params, i = 0; pl; pl = pl->next, i++) {
        c.param_names[i] = AS_PARAM(pl->item).name;
    }
    c.nparams = i;
    c.next_slot = (uint8_t)c.nparams;
    if (c.next_slot != c.nparams) {
        free((void *)c.param_names);
        return "too many parameters";
    }
    out->nlocals = c.next_slot;

    compile_block(&c, AS_FN_DECL(fn).body);
    free((void *)c.param_names);

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

    for (fi = 0; fi < nf; fi++) {
        AstNode *fn = fn_decl_at_index(program, fi);
        if (!fn) {
            r.error_message = "internal: fn index";
            compile_program_result_free(&r);
            return r;
        }
        chunk_init(&r.prog.chunks[fi]);
        {
            const char *ce = compile_one_fn(program, fn, &r.prog.chunks[fi]);
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
    r->ok = false;
    r->error_message = NULL;
}
