#include "vm.h"

#include "eval.h"

#include <stdlib.h>
#include <string.h>

#define VM_STACK_CAP 512u
#define VM_MAX_DEPTH 256u

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
} VmFrame;

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
    if (!fr->locals) {
        return;
    }
    for (i = 0; i < fr->nlocals; i++) {
        value_release(&fr->locals[i]);
    }
    free(fr->locals);
    fr->locals = NULL;
}

VmResult vm_run_program(const Chunk *chunks, size_t nchunks, size_t entry, const Value *args, size_t nargs) {
    VmResult out;
    VmFrame fr[VM_MAX_DEPTH];
    Value stack[VM_STACK_CAP];

    memset(fr, 0, sizeof(fr));
    size_t sp = 0;
    int depth = 0;
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
    if (nargs > 255u || chunks[entry].nlocals < nargs) {
        vm_fail(&out, "vm: bad argument count");
        return out;
    }

    fr[0].chunk = &chunks[entry];
    fr[0].ip = 0;
    fr[0].nlocals = chunks[entry].nlocals;
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
                case OP_RETURN: {
                    Value v = stack_pop(stack, &sp, &out, &err);
                    if (err) {
                        break;
                    }

                    free_frame_locals(&fr[depth]);
                    depth--;
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
        if (fr[fi].locals) {
            free_frame_locals(&fr[fi]);
        }
    }

    if (!out.ok) {
        value_release(&out.result);
        out.result.kind = VAL_VOID;
    }

    return out;
}

VmResult vm_run_chunk(const Chunk *chunk, const Value *args, size_t nargs) {
    return vm_run_program(chunk, 1, 0, args, nargs);
}
