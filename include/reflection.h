#ifndef MONS_REFLECTION_H
#define MONS_REFLECTION_H

#include "ast.h"

#include <stdio.h>

/*
 * Print a stable, line-oriented summary of public structs, functions, and
 * constants for tooling (Phase 2C). Types are pretty-printed from the AST.
 */
void reflection_fprint_program(FILE *fp, const AstNode *program);

#endif /* MONS_REFLECTION_H */
