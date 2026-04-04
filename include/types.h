#ifndef MONS_TYPES_H
#define MONS_TYPES_H

#include "ast.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ok;
    const char *error_message;
    uint32_t error_line;
    uint32_t error_col;
} TypeCheckResult;

/* Hindley-Milner-style unification over a simple type algebra (Phase 1 subset). */
TypeCheckResult type_check_program(AstNode *program);

#endif /* MONS_TYPES_H */
