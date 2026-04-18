#include "vm.h"

#include "ast.h"
#include "eval.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VM_STACK_CAP 512u
#define VM_MAX_DEPTH 256u
#define VM_TRY_CAP 256u

static void vm_fail(VmResult *out, const char *msg) {
    out->ok = false;
    out->result.kind = VAL_VOID;
    out->error_message = msg;
}

static bool values_equal_scalar(const Value *a, const Value *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case VAL_INT:
            return a->as.i == b->as.i;
        case VAL_BOOL:
            return a->as.b == b->as.b;
        case VAL_FLOAT:
            return a->as.f == b->as.f;
        case VAL_DOUBLE:
            return a->as.d == b->as.d;
        case VAL_NONE:
            return true;
        case VAL_STRING:
            return a->as.s && b->as.s && strcmp(a->as.s, b->as.s) == 0;
        default:
            return false;
    }
}

static int cmp_int_values(const Value *a, const Value *b) {
    if (a->kind != VAL_INT || b->kind != VAL_INT) {
        return 0;
    }
    if (a->as.i < b->as.i) {
        return -1;
    }
    if (a->as.i > b->as.i) {
        return 1;
    }
    return 0;
}

typedef struct {
    const Chunk *chunk;
    size_t       ip;
    Value       *locals;
    size_t       nlocals;
    Value       *upvalues; /* borrowed from closure cells; length chunk->nupvalues */
    size_t       nupvalues;
    ValClosure  *closure_held; /* retained while frame is active */
} VmFrame;

typedef struct {
    int    frame_depth;
    size_t handler_ip;
    size_t stack_sp;
} VmTryHandler;

static void stack_push(Value *stack, size_t *sp, size_t cap, Value v, VmResult *out, int *err) {
    if (*sp >= cap) {
        vm_fail(out, "vm stack overflow");
        *err = 1;
        return;
    }
    stack[(*sp)++] = v;
}

static Value stack_pop(Value *stack, size_t *sp, VmResult *out, int *err) {
    Value z;
    z.kind = VAL_VOID;
    if (*sp == 0) {
        vm_fail(out, "vm stack underflow");
        *err = 1;
        return z;
    }
    return stack[--(*sp)];
}

static void free_frame_locals(VmFrame *fr) {
    size_t i;
    if (fr->closure_held) {
        Value w;
        w.kind = VAL_CLOSURE;
        w.as.closure = fr->closure_held;
        value_release(&w);
        fr->closure_held = NULL;
    }
    if (!fr->locals) {
        return;
    }
    for (i = 0; i < fr->nlocals; i++) {
        value_release(&fr->locals[i]);
    }
    free(fr->locals);
    fr->locals = NULL;
    fr->upvalues = NULL;
    fr->nupvalues = 0;
}

VmResult vm_run_program(
    const Chunk *chunks,
    size_t nchunks,
    size_t entry,
    const Value *args,
    size_t nargs,
    const BcStructLayout *structs,
    size_t nstructs,
    const char *const *symbol_pool,
    size_t nsymbols) {
    VmResult out;
    VmFrame fr[VM_MAX_DEPTH];
    VmTryHandler try_stack[VM_TRY_CAP];
    Value stack[VM_STACK_CAP];

    memset(fr, 0, sizeof(fr));
    size_t sp = 0;
    int depth = 0;
    size_t ntry = 0;
    int err = 0;
    const Chunk *chunk;
    size_t ip;
    Value *locals;
    size_t nloc;
    size_t fi;

    out.ok = false;
    out.result.kind = VAL_VOID;
    out.error_message = NULL;

    if (!chunks || nchunks == 0 || entry >= nchunks) {
        vm_fail(&out, "vm: bad program");
        return out;
    }
    if (nargs > 255u || nargs != chunks[entry].nparams) {
        vm_fail(&out, "vm: bad argument count");
        return out;
    }

    fr[0].chunk = &chunks[entry];
    fr[0].ip = 0;
    fr[0].nlocals = chunks[entry].nlocals;
    fr[0].upvalues = NULL;
    fr[0].nupvalues = 0;
    fr[0].closure_held = NULL;
    fr[0].locals = (Value *)calloc(fr[0].nlocals, sizeof(Value));
    if (!fr[0].locals) {
        vm_fail(&out, "vm: out of memory");
        return out;
    }
    for (fi = 0; fi < nargs; fi++) {
        fr[0].locals[fi] = value_retain(args[fi]);
    }

    for (;;) {
reload:
        if (depth < 0) {
            break;
        }

        chunk = fr[depth].chunk;
        ip = fr[depth].ip;
        locals = fr[depth].locals;
        nloc = fr[depth].nlocals;

        while (ip < chunk->len && !err) {
            Value *upvals = fr[depth].upvalues;
            size_t nupv = fr[depth].nupvalues;
            uint8_t op = chunk->code[ip++];

            switch ((OpCode)op) {
                case OP_PUSH_CONST: {
                    uint16_t idx = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    ip += 2u;
                    if (idx >= chunk->nconst) {
                        vm_fail(&out, "vm: bad constant index");
                        err = 1;
                        break;
                    }
                    stack_push(stack, &sp, VM_STACK_CAP, value_retain(chunk->constants[idx]), &out, &err);
                    break;
                }
                case OP_PUSH_TRUE:
                    stack_push(stack, &sp, VM_STACK_CAP, (Value){ .kind = VAL_BOOL, .as.b = true }, &out, &err);
                    break;
                case OP_PUSH_FALSE:
                    stack_push(stack, &sp, VM_STACK_CAP, (Value){ .kind = VAL_BOOL, .as.b = false }, &out, &err);
                    break;
                case OP_LOAD_LOCAL: {
                    uint8_t slot = chunk->code[ip++];
                    if ((size_t)slot >= nloc) {
                        vm_fail(&out, "vm: bad local index");
                        err = 1;
                        break;
                    }
                    stack_push(stack, &sp, VM_STACK_CAP, value_retain(locals[slot]), &out, &err);
                    break;
                }
                case OP_STORE_LOCAL: {
                    uint8_t slot = chunk->code[ip++];
                    Value v = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if ((size_t)slot >= nloc) {
                        value_release(&v);
                        vm_fail(&out, "vm: bad local index");
                        err = 1;
                        break;
                    }
                    value_release(&locals[slot]);
                    locals[slot] = v;
                    break;
                }
                case OP_GET_UPVALUE: {
                    uint8_t uslot = chunk->code[ip++];
                    if (!upvals || (size_t)uslot >= nupv) {
                        vm_fail(&out, "vm: bad upvalue index");
                        err = 1;
                        break;
                    }
                    stack_push(stack, &sp, VM_STACK_CAP, value_retain(upvals[uslot]), &out, &err);
                    break;
                }
                case OP_POP: {
                    Value v = stack_pop(stack, &sp, &out, &err);
                    if (!err) {
                        value_release(&v);
                    }
                    break;
                }
                case OP_ADD_INT:
                case OP_SUB_INT:
                case OP_MUL_INT:
                case OP_DIV_INT:
                case OP_MOD_INT: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    if (l.kind != VAL_INT || r.kind != VAL_INT) {
                        value_release(&l);
                        value_release(&r);
                        vm_fail(&out, "vm: int op expects int operands");
                        err = 1;
                        break;
                    }
                    {
                        Value o;
                        o.kind = VAL_INT;
                        switch ((OpCode)op) {
                            case OP_ADD_INT:
                                o.as.i = l.as.i + r.as.i;
                                break;
                            case OP_SUB_INT:
                                o.as.i = l.as.i - r.as.i;
                                break;
                            case OP_MUL_INT:
                                o.as.i = l.as.i * r.as.i;
                                break;
                            case OP_DIV_INT:
                                if (r.as.i == 0) {
                                    value_release(&l);
                                    value_release(&r);
                                    vm_fail(&out, "division by zero");
                                    err = 1;
                                    goto int_op_done;
                                }
                                o.as.i = l.as.i / r.as.i;
                                break;
                            case OP_MOD_INT:
                                if (r.as.i == 0) {
                                    value_release(&l);
                                    value_release(&r);
                                    vm_fail(&out, "division by zero");
                                    err = 1;
                                    goto int_op_done;
                                }
                                o.as.i = l.as.i % r.as.i;
                                break;
                            default:
                                o.as.i = 0;
                                break;
                        }
                        value_release(&l);
                        value_release(&r);
                        stack_push(stack, &sp, VM_STACK_CAP, o, &out, &err);
                    }
int_op_done:
                    break;
                }
                case OP_NEG_INT: {
                    Value x = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (x.kind != VAL_INT) {
                        value_release(&x);
                        vm_fail(&out, "vm: NEG expects int");
                        err = 1;
                        break;
                    }
                    x.as.i = -x.as.i;
                    stack_push(stack, &sp, VM_STACK_CAP, x, &out, &err);
                    break;
                }
                case OP_EQ:
                case OP_NE: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    {
                        bool eq = values_equal_scalar(&l, &r);
                        Value b;
                        b.kind = VAL_BOOL;
                        b.as.b = (op == OP_EQ) ? eq : !eq;
                        value_release(&l);
                        value_release(&r);
                        stack_push(stack, &sp, VM_STACK_CAP, b, &out, &err);
                    }
                    break;
                }
                case OP_LT_INT:
                case OP_GT_INT:
                case OP_LTE_INT:
                case OP_GTE_INT: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    if (l.kind != VAL_INT || r.kind != VAL_INT) {
                        value_release(&l);
                        value_release(&r);
                        vm_fail(&out, "vm: ordered compare expects int");
                        err = 1;
                        break;
                    }
                    {
                        int c = cmp_int_values(&l, &r);
                        bool res;
                        if (op == OP_LT_INT) {
                            res = c < 0;
                        } else if (op == OP_GT_INT) {
                            res = c > 0;
                        } else if (op == OP_LTE_INT) {
                            res = c <= 0;
                        } else {
                            res = c >= 0;
                        }
                        value_release(&l);
                        value_release(&r);
                        {
                            Value b;
                            b.kind = VAL_BOOL;
                            b.as.b = res;
                            stack_push(stack, &sp, VM_STACK_CAP, b, &out, &err);
                        }
                    }
                    break;
                }
                case OP_JUMP: {
                    uint16_t raw = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    int16_t soff = (int16_t)raw;
                    int64_t nip;
                    ip += 2u;
                    nip = (int64_t)ip + (int64_t)soff;
                    if (nip < 0 || (size_t)nip > chunk->len) {
                        vm_fail(&out, "vm: jump out of range");
                        err = 1;
                        break;
                    }
                    ip = (size_t)nip;
                    break;
                }
                case OP_POP_JUMP_IF_FALSE: {
                    uint16_t raw = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    int16_t soff = (int16_t)raw;
                    int64_t nip;
                    Value v = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (v.kind != VAL_BOOL) {
                        value_release(&v);
                        vm_fail(&out, "vm: pop_jump_if_false expects bool");
                        err = 1;
                        break;
                    }
                    ip += 2u;
                    if (!v.as.b) {
                        nip = (int64_t)ip + (int64_t)soff;
                        if (nip < 0 || (size_t)nip > chunk->len) {
                            vm_fail(&out, "vm: jump out of range");
                            err = 1;
                            break;
                        }
                        ip = (size_t)nip;
                    }
                    break;
                }
                case OP_POP_JUMP_IF_TRUE: {
                    uint16_t raw = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    int16_t soff = (int16_t)raw;
                    int64_t nip;
                    Value v = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (v.kind != VAL_BOOL) {
                        value_release(&v);
                        vm_fail(&out, "vm: pop_jump_if_true expects bool");
                        err = 1;
                        break;
                    }
                    ip += 2u;
                    if (v.as.b) {
                        nip = (int64_t)ip + (int64_t)soff;
                        if (nip < 0 || (size_t)nip > chunk->len) {
                            vm_fail(&out, "vm: jump out of range");
                            err = 1;
                            break;
                        }
                        ip = (size_t)nip;
                    }
                    break;
                }
                case OP_NOT: {
                    Value x = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (x.kind != VAL_BOOL) {
                        value_release(&x);
                        vm_fail(&out, "vm: NOT expects bool");
                        err = 1;
                        break;
                    }
                    x.as.b = !x.as.b;
                    stack_push(stack, &sp, VM_STACK_CAP, x, &out, &err);
                    break;
                }
                case OP_DUP: {
                    if (sp == 0) {
                        vm_fail(&out, "vm: dup stack underflow");
                        err = 1;
                        break;
                    }
                    stack_push(stack, &sp, VM_STACK_CAP, value_retain(stack[sp - 1u]), &out, &err);
                    break;
                }
                case OP_SET_UPVALUE: {
                    uint8_t uslot = chunk->code[ip++];
                    Value v = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (!upvals || (size_t)uslot >= nupv) {
                        value_release(&v);
                        vm_fail(&out, "vm: bad set_upvalue index");
                        err = 1;
                        break;
                    }
                    value_release(&upvals[uslot]);
                    upvals[uslot] = v;
                    break;
                }
                case OP_CALL: {
                    uint16_t fni = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    uint8_t na = chunk->code[ip + 2u];
                    size_t j;

                    ip += 3u;
                    fr[depth].ip = ip;

                    if (sp < (size_t)na) {
                        vm_fail(&out, "vm: stack underflow at call");
                        err = 1;
                        break;
                    }
                    if ((size_t)fni >= nchunks) {
                        vm_fail(&out, "vm: bad call target");
                        err = 1;
                        break;
                    }
                    if (chunks[fni].nparams != na) {
                        vm_fail(&out, "vm: wrong argument count in call");
                        err = 1;
                        break;
                    }
                    if (chunks[fni].nlocals < na) {
                        vm_fail(&out, "vm: callee has too few locals for args");
                        err = 1;
                        break;
                    }
                    if (depth + 1 >= (int)VM_MAX_DEPTH) {
                        vm_fail(&out, "vm: call depth exceeded");
                        err = 1;
                        break;
                    }

                    depth++;
                    fr[depth].chunk = &chunks[fni];
                    fr[depth].ip = 0;
                    fr[depth].nlocals = chunks[fni].nlocals;
                    fr[depth].upvalues = NULL;
                    fr[depth].nupvalues = 0;
                    fr[depth].closure_held = NULL;
                    fr[depth].locals = (Value *)calloc(fr[depth].nlocals, sizeof(Value));
                    if (!fr[depth].locals) {
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        depth--;
                        break;
                    }

                    for (j = 0; j < (size_t)na; j++) {
                        Value v = stack[--sp];
                        fr[depth].locals[(size_t)na - 1u - j] = v;
                    }

                    goto reload;
                }
                case OP_CLOSURE: {
                    uint16_t chi = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    uint8_t nup = chunk->code[ip + 2u];
                    ValClosure *cl;
                    Value *cells;
                    Value cv;
                    size_t u;

                    ip += 3u;
                    if (ip + (size_t)nup * 2u > chunk->len) {
                        vm_fail(&out, "vm: truncated OP_CLOSURE");
                        err = 1;
                        break;
                    }
                    if ((size_t)chi >= nchunks) {
                        vm_fail(&out, "vm: bad closure chunk index");
                        err = 1;
                        break;
                    }
                    cl = value_closure_new();
                    cells = nup > 0 ? (Value *)calloc(nup, sizeof(Value)) : NULL;
                    if (nup > 0 && !cells) {
                        Value dead;
                        dead.kind = VAL_CLOSURE;
                        dead.as.closure = cl;
                        value_release(&dead);
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    cl->lambda = NULL;
                    cl->cap_names = NULL;
                    cl->cap_vals = cells;
                    cl->ncap = nup;
                    cl->is_bytecode = true;
                    cl->bc_chunk_idx = chi;

                    for (u = 0; u < (size_t)nup && !err; u++) {
                        uint8_t is_loc = chunk->code[ip++];
                        uint8_t idx = chunk->code[ip++];
                        if (is_loc) {
                            if ((size_t)idx >= nloc) {
                                vm_fail(&out, "vm: bad closure capture (local)");
                                err = 1;
                                break;
                            }
                            cells[u] = value_retain(locals[idx]);
                        } else {
                            if (!upvals || (size_t)idx >= nupv) {
                                vm_fail(&out, "vm: bad closure capture (upvalue)");
                                err = 1;
                                break;
                            }
                            cells[u] = value_retain(upvals[idx]);
                        }
                    }
                    if (err) {
                        for (u = 0; u < (size_t)nup; u++) {
                            value_release(&cells[u]);
                        }
                        free(cells);
                        free(cl);
                        break;
                    }
                    fr[depth].ip = ip;
                    cv.kind = VAL_CLOSURE;
                    cv.as.closure = cl;
                    stack_push(stack, &sp, VM_STACK_CAP, cv, &out, &err);
                    break;
                }
                case OP_PUSH_FN: {
                    uint16_t fidx = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    ValClosure *cl;
                    Value cv;

                    ip += 2u;
                    if ((size_t)fidx >= nchunks) {
                        vm_fail(&out, "vm: bad OP_PUSH_FN index");
                        err = 1;
                        break;
                    }
                    cl = value_closure_new();
                    if (!cl) {
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    cl->lambda = NULL;
                    cl->cap_names = NULL;
                    cl->cap_vals = NULL;
                    cl->ncap = 0;
                    cl->is_bytecode = true;
                    cl->bc_chunk_idx = fidx;
                    cv.kind = VAL_CLOSURE;
                    cv.as.closure = cl;
                    stack_push(stack, &sp, VM_STACK_CAP, cv, &out, &err);
                    break;
                }
                case OP_STRUCT_NEW: {
                    uint16_t sidx = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    const BcStructLayout *L;
                    ValStruct *vs;
                    Value *vals;
                    const char **fnames_copy;
                    Value sv;
                    size_t nf;
                    size_t j;

                    ip += 2u;
                    if (!structs || (size_t)sidx >= nstructs) {
                        vm_fail(&out, "vm: bad struct layout index");
                        err = 1;
                        break;
                    }
                    L = &structs[sidx];
                    nf = L->nfields;
                    if (sp < nf) {
                        vm_fail(&out, "vm: stack underflow at struct new");
                        err = 1;
                        break;
                    }
                    vals = nf > 0 ? (Value *)malloc(nf * sizeof(Value)) : NULL;
                    fnames_copy = nf > 0 ? (const char **)malloc(nf * sizeof(char *)) : NULL;
                    vs = value_struct_new();
                    if ((nf > 0 && (!vals || !fnames_copy)) || !vs) {
                        free(vals);
                        free(fnames_copy);
                        if (vs) {
                            Value dv;
                            dv.kind = VAL_STRUCT;
                            dv.as.st = vs;
                            value_release(&dv);
                        }
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    for (j = nf; j-- > 0u;) {
                        vals[j] = stack_pop(stack, &sp, &out, &err);
                    }
                    if (err) {
                        for (j = 0; j < nf; j++) {
                            value_release(&vals[j]);
                        }
                        free(vals);
                        free(fnames_copy);
                        {
                            Value dv;
                            dv.kind = VAL_STRUCT;
                            dv.as.st = vs;
                            value_release(&dv);
                        }
                        break;
                    }
                    memcpy(fnames_copy, L->field_names, nf * sizeof(char *));
                    vs->type_name = L->type_name;
                    vs->n = nf;
                    vs->field_names = fnames_copy;
                    vs->values = vals;
                    sv.kind = VAL_STRUCT;
                    sv.as.st = vs;
                    stack_push(stack, &sp, VM_STACK_CAP, sv, &out, &err);
                    break;
                }
                case OP_GET_FIELD: {
                    uint8_t fidx = chunk->code[ip++];
                    Value obj = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (obj.kind != VAL_STRUCT || !obj.as.st) {
                        value_release(&obj);
                        vm_fail(&out, "vm: get_field needs struct");
                        err = 1;
                        break;
                    }
                    if ((size_t)fidx >= obj.as.st->n) {
                        value_release(&obj);
                        vm_fail(&out, "vm: bad field index");
                        err = 1;
                        break;
                    }
                    stack_push(stack, &sp, VM_STACK_CAP, value_retain(obj.as.st->values[fidx]), &out, &err);
                    value_release(&obj);
                    break;
                }
                case OP_GET_FIELD_NAMED: {
                    uint16_t sym = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    Value obj = stack_pop(stack, &sp, &out, &err);
                    const char *want;
                    size_t k;
                    int found = 0;

                    ip += 2u;
                    if (err) {
                        break;
                    }
                    if (!symbol_pool || (size_t)sym >= nsymbols || !symbol_pool[sym]) {
                        value_release(&obj);
                        vm_fail(&out, "vm: bad field name pool index");
                        err = 1;
                        break;
                    }
                    want = symbol_pool[sym];
                    if (obj.kind != VAL_STRUCT || !obj.as.st) {
                        value_release(&obj);
                        vm_fail(&out, "vm: get_field_named needs struct");
                        err = 1;
                        break;
                    }
                    for (k = 0; k < obj.as.st->n; k++) {
                        if (obj.as.st->field_names[k] && strcmp(obj.as.st->field_names[k], want) == 0) {
                            stack_push(stack, &sp, VM_STACK_CAP, value_retain(obj.as.st->values[k]), &out, &err);
                            found = 1;
                            break;
                        }
                    }
                    value_release(&obj);
                    if (!found) {
                        vm_fail(&out, "vm: unknown struct field");
                        err = 1;
                        break;
                    }
                    break;
                }
                case OP_ARRAY_NEW: {
                    uint16_t nf = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    ValSeq *seq;
                    Value av;
                    size_t j;

                    ip += 2u;
                    if (sp < (size_t)nf) {
                        vm_fail(&out, "vm: stack underflow at array new");
                        err = 1;
                        break;
                    }
                    seq = value_seq_new();
                    if (!seq) {
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    seq->len = (size_t)nf;
                    if (nf > 0) {
                        seq->items = (Value *)malloc((size_t)nf * sizeof(Value));
                        if (!seq->items) {
                            {
                                Value ds;
                                ds.kind = VAL_ARRAY;
                                ds.as.seq = seq;
                                value_release(&ds);
                            }
                            vm_fail(&out, "vm: out of memory");
                            err = 1;
                            break;
                        }
                        for (j = (size_t)nf; j-- > 0u;) {
                            seq->items[j] = stack_pop(stack, &sp, &out, &err);
                        }
                        if (err) {
                            for (j = 0; j < seq->len; j++) {
                                value_release(&seq->items[j]);
                            }
                            free(seq->items);
                            {
                                Value ds;
                                ds.kind = VAL_ARRAY;
                                ds.as.seq = seq;
                                value_release(&ds);
                            }
                            break;
                        }
                    } else {
                        seq->items = NULL;
                    }
                    av.kind = VAL_ARRAY;
                    av.as.seq = seq;
                    stack_push(stack, &sp, VM_STACK_CAP, av, &out, &err);
                    break;
                }
                case OP_TUPLE_NEW: {
                    uint16_t nf = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    ValSeq *seq;
                    Value tv;
                    size_t j;

                    ip += 2u;
                    if (sp < (size_t)nf) {
                        vm_fail(&out, "vm: stack underflow at tuple new");
                        err = 1;
                        break;
                    }
                    seq = value_seq_new();
                    if (!seq) {
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    seq->len = (size_t)nf;
                    if (nf > 0) {
                        seq->items = (Value *)malloc((size_t)nf * sizeof(Value));
                        if (!seq->items) {
                            {
                                Value ds;
                                ds.kind = VAL_TUPLE;
                                ds.as.seq = seq;
                                value_release(&ds);
                            }
                            vm_fail(&out, "vm: out of memory");
                            err = 1;
                            break;
                        }
                        for (j = (size_t)nf; j-- > 0u;) {
                            seq->items[j] = stack_pop(stack, &sp, &out, &err);
                        }
                        if (err) {
                            for (j = 0; j < seq->len; j++) {
                                value_release(&seq->items[j]);
                            }
                            free(seq->items);
                            {
                                Value ds;
                                ds.kind = VAL_TUPLE;
                                ds.as.seq = seq;
                                value_release(&ds);
                            }
                            break;
                        }
                    } else {
                        seq->items = NULL;
                    }
                    tv.kind = VAL_TUPLE;
                    tv.as.seq = seq;
                    stack_push(stack, &sp, VM_STACK_CAP, tv, &out, &err);
                    break;
                }
                case OP_ARRAY_LEN: {
                    Value a = stack_pop(stack, &sp, &out, &err);
                    Value o;
                    if (err) {
                        break;
                    }
                    if ((a.kind != VAL_ARRAY && a.kind != VAL_TUPLE) || !a.as.seq) {
                        value_release(&a);
                        vm_fail(&out, "vm: array_len needs array or tuple");
                        err = 1;
                        break;
                    }
                    o.kind = VAL_INT;
                    o.as.i = (int64_t)a.as.seq->len;
                    value_release(&a);
                    stack_push(stack, &sp, VM_STACK_CAP, o, &out, &err);
                    break;
                }
                case OP_INDEX_INT: {
                    Value ki = stack_pop(stack, &sp, &out, &err);
                    Value a;
                    int64_t idx;
                    if (err) {
                        break;
                    }
                    a = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&ki);
                        break;
                    }
                    if (ki.kind != VAL_INT) {
                        value_release(&ki);
                        value_release(&a);
                        vm_fail(&out, "vm: index expects int");
                        err = 1;
                        break;
                    }
                    if ((a.kind != VAL_ARRAY && a.kind != VAL_TUPLE) || !a.as.seq) {
                        value_release(&ki);
                        value_release(&a);
                        vm_fail(&out, "vm: index expects array or tuple");
                        err = 1;
                        break;
                    }
                    idx = ki.as.i;
                    if (idx < 0 || (size_t)idx >= a.as.seq->len) {
                        value_release(&ki);
                        value_release(&a);
                        vm_fail(&out, "vm: index out of bounds");
                        err = 1;
                        break;
                    }
                    stack_push(stack, &sp, VM_STACK_CAP, value_retain(a.as.seq->items[(size_t)idx]), &out, &err);
                    value_release(&ki);
                    value_release(&a);
                    break;
                }
                case OP_ADD_FLOAT:
                case OP_SUB_FLOAT:
                case OP_MUL_FLOAT:
                case OP_DIV_FLOAT: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    if (l.kind != VAL_FLOAT || r.kind != VAL_FLOAT) {
                        value_release(&l);
                        value_release(&r);
                        vm_fail(&out, "vm: float op expects float operands");
                        err = 1;
                        break;
                    }
                    {
                        Value o;
                        o.kind = VAL_FLOAT;
                        switch ((OpCode)op) {
                            case OP_ADD_FLOAT:
                                o.as.f = l.as.f + r.as.f;
                                break;
                            case OP_SUB_FLOAT:
                                o.as.f = l.as.f - r.as.f;
                                break;
                            case OP_MUL_FLOAT:
                                o.as.f = l.as.f * r.as.f;
                                break;
                            case OP_DIV_FLOAT:
                                if (r.as.f == 0.0f) {
                                    value_release(&l);
                                    value_release(&r);
                                    vm_fail(&out, "division by zero");
                                    err = 1;
                                    goto float_op_done;
                                }
                                o.as.f = l.as.f / r.as.f;
                                break;
                            default:
                                o.as.f = 0.0f;
                                break;
                        }
                        value_release(&l);
                        value_release(&r);
                        stack_push(stack, &sp, VM_STACK_CAP, o, &out, &err);
                    }
float_op_done:
                    break;
                }
                case OP_NEG_FLOAT: {
                    Value x = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (x.kind != VAL_FLOAT) {
                        value_release(&x);
                        vm_fail(&out, "vm: NEG_FLOAT expects float");
                        err = 1;
                        break;
                    }
                    x.as.f = -x.as.f;
                    stack_push(stack, &sp, VM_STACK_CAP, x, &out, &err);
                    break;
                }
                case OP_ADD_DOUBLE:
                case OP_SUB_DOUBLE:
                case OP_MUL_DOUBLE:
                case OP_DIV_DOUBLE: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    if (l.kind != VAL_DOUBLE || r.kind != VAL_DOUBLE) {
                        value_release(&l);
                        value_release(&r);
                        vm_fail(&out, "vm: double op expects double operands");
                        err = 1;
                        break;
                    }
                    {
                        Value o;
                        o.kind = VAL_DOUBLE;
                        switch ((OpCode)op) {
                            case OP_ADD_DOUBLE:
                                o.as.d = l.as.d + r.as.d;
                                break;
                            case OP_SUB_DOUBLE:
                                o.as.d = l.as.d - r.as.d;
                                break;
                            case OP_MUL_DOUBLE:
                                o.as.d = l.as.d * r.as.d;
                                break;
                            case OP_DIV_DOUBLE:
                                if (r.as.d == 0.0) {
                                    value_release(&l);
                                    value_release(&r);
                                    vm_fail(&out, "division by zero");
                                    err = 1;
                                    goto double_op_done;
                                }
                                o.as.d = l.as.d / r.as.d;
                                break;
                            default:
                                o.as.d = 0.0;
                                break;
                        }
                        value_release(&l);
                        value_release(&r);
                        stack_push(stack, &sp, VM_STACK_CAP, o, &out, &err);
                    }
double_op_done:
                    break;
                }
                case OP_NEG_DOUBLE: {
                    Value x = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (x.kind != VAL_DOUBLE) {
                        value_release(&x);
                        vm_fail(&out, "vm: NEG_DOUBLE expects double");
                        err = 1;
                        break;
                    }
                    x.as.d = -x.as.d;
                    stack_push(stack, &sp, VM_STACK_CAP, x, &out, &err);
                    break;
                }
                case OP_LT_FLOAT:
                case OP_GT_FLOAT:
                case OP_LTE_FLOAT:
                case OP_GTE_FLOAT: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    if (l.kind != VAL_FLOAT || r.kind != VAL_FLOAT) {
                        value_release(&l);
                        value_release(&r);
                        vm_fail(&out, "vm: float compare expects float operands");
                        err = 1;
                        break;
                    }
                    {
                        int c = (l.as.f < r.as.f) ? -1 : ((l.as.f > r.as.f) ? 1 : 0);
                        bool res;
                        if (op == OP_LT_FLOAT) {
                            res = c < 0;
                        } else if (op == OP_GT_FLOAT) {
                            res = c > 0;
                        } else if (op == OP_LTE_FLOAT) {
                            res = c <= 0;
                        } else {
                            res = c >= 0;
                        }
                        value_release(&l);
                        value_release(&r);
                        {
                            Value b;
                            b.kind = VAL_BOOL;
                            b.as.b = res;
                            stack_push(stack, &sp, VM_STACK_CAP, b, &out, &err);
                        }
                    }
                    break;
                }
                case OP_LT_DOUBLE:
                case OP_GT_DOUBLE:
                case OP_LTE_DOUBLE:
                case OP_GTE_DOUBLE: {
                    Value r = stack_pop(stack, &sp, &out, &err);
                    Value l = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        value_release(&l);
                        value_release(&r);
                        break;
                    }
                    if (l.kind != VAL_DOUBLE || r.kind != VAL_DOUBLE) {
                        value_release(&l);
                        value_release(&r);
                        vm_fail(&out, "vm: double compare expects double operands");
                        err = 1;
                        break;
                    }
                    {
                        int c = (l.as.d < r.as.d) ? -1 : ((l.as.d > r.as.d) ? 1 : 0);
                        bool res;
                        if (op == OP_LT_DOUBLE) {
                            res = c < 0;
                        } else if (op == OP_GT_DOUBLE) {
                            res = c > 0;
                        } else if (op == OP_LTE_DOUBLE) {
                            res = c <= 0;
                        } else {
                            res = c >= 0;
                        }
                        value_release(&l);
                        value_release(&r);
                        {
                            Value b;
                            b.kind = VAL_BOOL;
                            b.as.b = res;
                            stack_push(stack, &sp, VM_STACK_CAP, b, &out, &err);
                        }
                    }
                    break;
                }
                case OP_SWAP: {
                    Value t;
                    if (sp < 2u) {
                        vm_fail(&out, "vm: swap needs two stack values");
                        err = 1;
                        break;
                    }
                    t = stack[sp - 2u];
                    stack[sp - 2u] = stack[sp - 1u];
                    stack[sp - 1u] = t;
                    break;
                }
                case OP_TRY_ENTER: {
                    uint16_t raw = (uint16_t)chunk->code[ip] | ((uint16_t)chunk->code[ip + 1u] << 8);
                    int16_t soff = (int16_t)raw;
                    int64_t hip;
                    ip += 2u;
                    hip = (int64_t)ip + (int64_t)soff;
                    if (hip < 0 || (size_t)hip > chunk->len) {
                        vm_fail(&out, "vm: try handler jump out of range");
                        err = 1;
                        break;
                    }
                    if (ntry >= VM_TRY_CAP) {
                        vm_fail(&out, "vm: try stack overflow");
                        err = 1;
                        break;
                    }
                    try_stack[ntry].frame_depth = depth;
                    try_stack[ntry].handler_ip = (size_t)hip;
                    try_stack[ntry].stack_sp = sp;
                    ntry++;
                    break;
                }
                case OP_TRY_EXIT:
                    if (ntry == 0) {
                        vm_fail(&out, "vm: try stack underflow");
                        err = 1;
                        break;
                    }
                    ntry--;
                    break;
                case OP_THROW: {
                    Value ex = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }
                    if (ntry == 0) {
                        value_release(&ex);
                        vm_fail(&out, "uncaught exception");
                        err = 1;
                        break;
                    }
                    {
                        VmTryHandler h = try_stack[ntry - 1u];
                        ntry--;
                        while (depth > h.frame_depth) {
                            free_frame_locals(&fr[depth]);
                            depth--;
                        }
                        while (sp > h.stack_sp) {
                            Value drop = stack[--sp];
                            value_release(&drop);
                        }
                        fr[depth].ip = h.handler_ip;
                        stack_push(stack, &sp, VM_STACK_CAP, ex, &out, &err);
                        if (err) {
                            value_release(&ex);
                            break;
                        }
                    }
                    goto reload;
                }
                case OP_EXN_IS_PRIM: {
                    uint8_t tag = chunk->code[ip++];
                    Value ex = stack_pop(stack, &sp, &out, &err);
                    Value b;
                    bool ok = false;
                    if (err) {
                        break;
                    }
                    switch (tag) {
                        case PRIM_INT:
                            ok = ex.kind == VAL_INT;
                            break;
                        case PRIM_FLOAT:
                            ok = ex.kind == VAL_FLOAT;
                            break;
                        case PRIM_DOUBLE:
                            ok = ex.kind == VAL_DOUBLE;
                            break;
                        case PRIM_BOOL:
                            ok = ex.kind == VAL_BOOL;
                            break;
                        case PRIM_STRING:
                            ok = ex.kind == VAL_STRING;
                            break;
                        default:
                            value_release(&ex);
                            vm_fail(&out, "vm: unknown primitive tag in OP_EXN_IS_PRIM");
                            err = 1;
                            break;
                    }
                    if (err) {
                        break;
                    }
                    value_release(&ex);
                    b.kind = VAL_BOOL;
                    b.as.b = ok;
                    stack_push(stack, &sp, VM_STACK_CAP, b, &out, &err);
                    break;
                }
                case OP_CALL_CLOSURE: {
                    uint8_t na = chunk->code[ip++];
                    Value *argv;
                    Value clos_v;
                    ValClosure *cl;
                    uint16_t cidx;
                    size_t j2;

                    fr[depth].ip = ip;
                    if (sp < (size_t)na + 1u) {
                        vm_fail(&out, "vm: stack underflow at call_closure");
                        err = 1;
                        break;
                    }
                    argv = (Value *)malloc((size_t)na * sizeof(Value));
                    if (!argv && na > 0) {
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    for (j2 = 0; j2 < (size_t)na; j2++) {
                        argv[(size_t)na - 1u - j2] = stack[--sp];
                    }
                    clos_v = stack[--sp];
                    if (clos_v.kind != VAL_CLOSURE || !clos_v.as.closure || !clos_v.as.closure->is_bytecode) {
                        if (argv) {
                            for (j2 = 0; j2 < (size_t)na; j2++) {
                                value_release(&argv[j2]);
                            }
                            free(argv);
                        }
                        value_release(&clos_v);
                        vm_fail(&out, "vm: call_closure needs bytecode closure");
                        err = 1;
                        break;
                    }
                    cl = clos_v.as.closure;
                    cidx = cl->bc_chunk_idx;
                    if ((size_t)cidx >= nchunks) {
                        for (j2 = 0; j2 < (size_t)na; j2++) {
                            value_release(&argv[j2]);
                        }
                        free(argv);
                        value_release(&clos_v);
                        vm_fail(&out, "vm: bad closure target chunk");
                        err = 1;
                        break;
                    }
                    if (chunks[cidx].nparams != na) {
                        for (j2 = 0; j2 < (size_t)na; j2++) {
                            value_release(&argv[j2]);
                        }
                        free(argv);
                        value_release(&clos_v);
                        vm_fail(&out, "vm: wrong argument count in call_closure");
                        err = 1;
                        break;
                    }
                    if (chunks[cidx].nlocals < na) {
                        for (j2 = 0; j2 < (size_t)na; j2++) {
                            value_release(&argv[j2]);
                        }
                        free(argv);
                        value_release(&clos_v);
                        vm_fail(&out, "vm: callee has too few locals");
                        err = 1;
                        break;
                    }
                    if (depth + 1 >= (int)VM_MAX_DEPTH) {
                        for (j2 = 0; j2 < (size_t)na; j2++) {
                            value_release(&argv[j2]);
                        }
                        free(argv);
                        value_release(&clos_v);
                        vm_fail(&out, "vm: call depth exceeded");
                        err = 1;
                        break;
                    }

                    depth++;
                    fr[depth].chunk = &chunks[cidx];
                    fr[depth].ip = 0;
                    fr[depth].nlocals = chunks[cidx].nlocals;
                    fr[depth].upvalues = cl->cap_vals;
                    fr[depth].nupvalues = cl->ncap;
                    {
                        Value hold;
                        hold.kind = VAL_CLOSURE;
                        hold.as.closure = cl;
                        fr[depth].closure_held = value_retain(hold).as.closure;
                    }
                    fr[depth].locals = (Value *)calloc(fr[depth].nlocals, sizeof(Value));
                    if (!fr[depth].locals) {
                        Value hold2;
                        hold2.kind = VAL_CLOSURE;
                        hold2.as.closure = fr[depth].closure_held;
                        value_release(&hold2);
                        fr[depth].closure_held = NULL;
                        for (j2 = 0; j2 < (size_t)na; j2++) {
                            value_release(&argv[j2]);
                        }
                        free(argv);
                        value_release(&clos_v);
                        depth--;
                        vm_fail(&out, "vm: out of memory");
                        err = 1;
                        break;
                    }
                    for (j2 = 0; j2 < (size_t)na; j2++) {
                        fr[depth].locals[j2] = argv[j2];
                    }
                    free(argv);
                    value_release(&clos_v);

                    goto reload;
                }
                case OP_RETURN: {
                    Value v = stack_pop(stack, &sp, &out, &err);
                    int old_depth = depth;
                    if (err) {
                        break;
                    }

                    free_frame_locals(&fr[depth]);
                    depth--;
                    while (ntry > 0 && try_stack[ntry - 1u].frame_depth >= old_depth) {
                        ntry--;
                    }
                    if (depth < 0) {
                        out.result = v;
                        out.ok = true;
                        goto vm_cleanup;
                    }

                    stack_push(stack, &sp, VM_STACK_CAP, v, &out, &err);
                    if (err) {
                        value_release(&v);
                        goto vm_cleanup;
                    }

                    goto reload;
                }
                default:
                    vm_fail(&out, "vm: unknown opcode");
                    err = 1;
                    break;
            }
        }

        if (err) {
            break;
        }

        if (ip >= chunk->len) {
            vm_fail(&out, "vm: fell off end of chunk");
            err = 1;
            break;
        }
    }

vm_cleanup:
    {
        size_t i;
        for (i = 0; i < sp; i++) {
            value_release(&stack[i]);
        }
    }

    for (fi = 0; fi < VM_MAX_DEPTH; fi++) {
        if (fr[fi].locals || fr[fi].closure_held) {
            free_frame_locals(&fr[fi]);
        }
    }

    if (!out.ok) {
        value_release(&out.result);
        out.result.kind = VAL_VOID;
        value_gc_collect(NULL, 0);
    } else {
        value_gc_collect(&out.result, 1);
    }

    return out;
}

VmResult vm_run_chunk(const Chunk *chunk, const Value *args, size_t nargs) {
    return vm_run_program(chunk, 1, 0, args, nargs, NULL, 0, NULL, 0);
}
