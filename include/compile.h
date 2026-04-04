#ifndef MONS_COMPILE_H
#define MONS_COMPILE_H

#include "ast.h"
#include "bytecode.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Chunk  *chunks;
    size_t  nchunks;
} BcProgram;

typedef struct {
    bool        ok;
    BcProgram   prog;
    const char *error_message;
} CompileProgramResult;

/* Compile every top-level `fn` in source order into `prog.chunks[i]`. */
CompileProgramResult compile_program_bc(AstNode *program);

void compile_program_result_free(CompileProgramResult *r);

/* Index of `name` among `fn` declarations only, or -1. */
int bc_fn_index(AstNode *program, const char *name);

#endif /* MONS_COMPILE_H */
