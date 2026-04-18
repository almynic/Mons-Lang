#ifndef MONS_BYTECODE_H
#define MONS_BYTECODE_H

#include "eval.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Phase 2A: stack bytecode. Opcodes may take multi-byte operands (little-endian).
 * See compile.c / vm.c for encoding details.
 */
typedef enum {
    OP_PUSH_CONST = 1, /* u16 constant index */
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_LOAD_LOCAL,  /* u8 slot */
    OP_STORE_LOCAL, /* u8 slot */
    OP_POP,
    OP_ADD_INT,
    OP_SUB_INT,
    OP_MUL_INT,
    OP_DIV_INT,
    OP_MOD_INT,
    OP_NEG_INT,
    OP_EQ,
    OP_NE,
    OP_LT_INT,
    OP_GT_INT,
    OP_LTE_INT,
    OP_GTE_INT,
    OP_JUMP,               /* u16: add to ip after operand (forward relative) */
    OP_POP_JUMP_IF_FALSE,  /* u16: pop bool; if false, ip += off; if true, fall through */
    OP_POP_JUMP_IF_TRUE,   /* u16: pop bool; if true, ip += off; if false, fall through */
    OP_NOT,                /* pop bool, push logical not */
    OP_DUP,                /* duplicate stack top (retain if refcounted) */
    OP_SET_UPVALUE,        /* u8 slot — pop value into closure cell */
    OP_CALL, /* u16 callee chunk index, u8 nargs (args on stack: first arg deepest) */
    OP_RETURN,
    OP_GET_UPVALUE,   /* u8 upvalue index in current closure */
    OP_CLOSURE,       /* u16 chunk_idx, u8 nup, then nup × (u8 is_local, u8 idx) */
    OP_PUSH_FN,       /* u16 chunk_idx — push zero-capture closure (top-level fn) */
    OP_CALL_CLOSURE,  /* u8 nargs — stack: closure deepest, then args */
    OP_STRUCT_NEW,       /* u16 struct layout index; pops nfields values (first field deepest) */
    OP_GET_FIELD,        /* u8 field index; pops struct, pushes field value (retained) */
    OP_GET_FIELD_NAMED,  /* u16 symbol-pool index; pops struct, finds field by name */
    OP_ARRAY_NEW,        /* u16 n; pops n values (first deepest) → VAL_ARRAY */
    OP_TUPLE_NEW,        /* u16 n; pops n values (first deepest) → VAL_TUPLE */
    OP_ARRAY_LEN,        /* pops array or tuple, pushes int length (releases popped ref) */
    OP_INDEX_INT,         /* pops int index, pops array or tuple, pushes elem (releases seq ref) */
    OP_ADD_FLOAT,
    OP_SUB_FLOAT,
    OP_MUL_FLOAT,
    OP_DIV_FLOAT,
    OP_NEG_FLOAT,
    OP_ADD_DOUBLE,
    OP_SUB_DOUBLE,
    OP_MUL_DOUBLE,
    OP_DIV_DOUBLE,
    OP_NEG_DOUBLE,
    OP_LT_FLOAT,
    OP_GT_FLOAT,
    OP_LTE_FLOAT,
    OP_GTE_FLOAT,
    OP_LT_DOUBLE,
    OP_GT_DOUBLE,
    OP_LTE_DOUBLE,
    OP_GTE_DOUBLE,
    OP_SWAP, /* swap two stack tops (deepest = sp-2, top = sp-1) */
    OP_TRY_ENTER,  /* u16 handler target ip */
    OP_TRY_EXIT,   /* pop one active try handler */
    OP_THROW,      /* pop value and raise exception */
    OP_EXN_IS_PRIM /* u8 PrimitiveType tag; pop value, push bool(kind matches) */
} OpCode;

/* Decl-order struct layouts for bytecode; `field_names` is owned (array of pointers into AST). */
typedef struct BcStructLayout {
    const char     *type_name;
    size_t          nfields;
    const char    **field_names;
} BcStructLayout;

typedef struct Chunk {
    uint8_t *code;
    size_t    len;
    size_t    cap;
    Value    *constants;
    size_t    nconst;
    size_t    capconst;
    uint8_t   nparams;   /* arity (parameter slots 0..nparams-1) */
    uint8_t   nlocals;   /* total local slots (params + lets) */
    uint8_t   nupvalues; /* closed-over cells (GET_UPVALUE indices) */
} Chunk;

void chunk_init(Chunk *c);
void chunk_free(Chunk *c);

/* Returns constant pool index, or -1 on OOM. */
int chunk_add_constant(Chunk *c, Value v);

void chunk_emit_u8(Chunk *c, uint8_t b);
void chunk_emit_u16(Chunk *c, uint16_t u);

/* Patch a u16 at byte offset `at` in `c->code` (e.g. forward jump displacement). */
void chunk_patch_u16(Chunk *c, size_t at, uint16_t v);

#endif /* MONS_BYTECODE_H */
