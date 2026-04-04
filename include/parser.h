#ifndef MONS_PARSER_H
#define MONS_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    AstNode *program; /* NODE_PROGRAM or NULL on failure */
    const char *error_message;
    uint32_t error_line;
    uint32_t error_col;
} ParseResult;

/* Builds an AST in the global arena (`ast_alloc` / `ast_copy_string`).
 * Does not free token memory. Caller should `ast_free_all()` when done. */
ParseResult parse_tokens(TokenArray tokens, const char *filename);

#endif /* MONS_PARSER_H */
