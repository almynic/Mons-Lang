#ifndef MONS_LEXER_H
#define MONS_LEXER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    TOK_IDENT,
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_DOUBLE_LIT,
    TOK_STRING_LIT,

    TOK_FN,
    TOK_STRUCT,
    TOK_TRAIT,
    TOK_IMPL,
    TOK_MACRO,
    TOK_USE,
    TOK_CONST,
    TOK_LET,
    TOK_MUT,
    TOK_PUB,
    TOK_IF,
    TOK_ELSE,
    TOK_FOR,
    TOK_IN,
    TOK_MATCH,
    TOK_RETURN,
    TOK_TRY,
    TOK_CATCH,
    TOK_FINALLY,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NONE,

    TOK_INT,
    TOK_FLOAT,
    TOK_DOUBLE,
    TOK_BOOL,
    TOK_STRING,

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_DOT,
    TOK_COLON,
    TOK_SEMI,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_BANG,
    TOK_AMP,
    TOK_PIPE,
    TOK_LT,
    TOK_GT,
    TOK_EQ,

    TOK_ARROW,       /* -> */
    TOK_FAT_ARROW,   /* => */
    TOK_PATH_SEP,    /* :: */
    TOK_DOT_DOT,     /* .. */
    TOK_EQ_EQ,       /* == */
    TOK_BANG_EQ,     /* != */
    TOK_LT_EQ,       /* <= */
    TOK_GT_EQ,       /* >= */
    TOK_AMP_AMP,     /* && */
    TOK_PIPE_PIPE,   /* || */
    TOK_DOLLAR       /* $ */
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *lexeme;
    size_t length;
    uint32_t line;
    uint32_t col;
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenArray;

TokenArray lexer_tokenize(const char *source, const char *filename);
void token_array_free(TokenArray *tokens);
const char *token_kind_name(TokenKind kind);

#endif /* MONS_LEXER_H */
