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
    OP_CALL, /* u16 callee chunk index, u8 nargs (args on stack: first arg deepest) */
    OP_RETURN
} OpCode;

typedef struct Chunk {
    uint8_t *code;
    size_t    len;
    size_t    cap;
    Value    *constants;
    size_t    nconst;
    size_t    capconst;
    uint8_t   nlocals; /* slots 0..nlocals-1 (params + lets) */
} Chunk;

void chunk_init(Chunk *c);
void chunk_free(Chunk *c);

/* Returns constant pool index, or -1 on OOM. */
int chunk_add_constant(Chunk *c, Value v);

void chunk_emit_u8(Chunk *c, uint8_t b);
void chunk_emit_u16(Chunk *c, uint16_t u);

#endif /* MONS_BYTECODE_H */
