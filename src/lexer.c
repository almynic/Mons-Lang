/*
 * lexer.c — scan UTF-8 source into TokenKind + lexeme slices.
 * Keywords are recognized after identifiers; numeric and string literals are validated here.
 */
#include "lexer.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *source;
    const char *filename;
    const char *start;
    const char *current;
    uint32_t line;
    uint32_t col;
    uint32_t token_line;
    uint32_t token_col;
    TokenArray out;
} Lexer;

static bool is_at_end(const Lexer *lx) {
    return *lx->current == '\0';
}

static char advance(Lexer *lx) {
    char c = *lx->current++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static char peek(const Lexer *lx) {
    return *lx->current;
}

static char peek_next(const Lexer *lx) {
    if (is_at_end(lx)) {
        return '\0';
    }
    return lx->current[1];
}

static bool match_char(Lexer *lx, char expected) {
    if (is_at_end(lx) || *lx->current != expected) {
        return false;
    }
    (void)advance(lx);
    return true;
}

static bool token_array_push(TokenArray *arr, Token tok) {
    if (arr->count == arr->capacity) {
        size_t new_capacity = arr->capacity == 0 ? 64 : arr->capacity * 2;
        Token *new_items = (Token *)realloc(arr->items, new_capacity * sizeof(Token));
        if (!new_items) {
            return false;
        }
        arr->items = new_items;
        arr->capacity = new_capacity;
    }
    arr->items[arr->count++] = tok;
    return true;
}

static bool emit_token(Lexer *lx, TokenKind kind) {
    Token tok;
    tok.kind = kind;
    tok.lexeme = lx->start;
    tok.length = (size_t)(lx->current - lx->start);
    tok.line = lx->token_line;
    tok.col = lx->token_col;
    return token_array_push(&lx->out, tok);
}

static bool emit_static_error_token(Lexer *lx, const char *message) {
    Token tok;
    tok.kind = TOK_ERROR;
    tok.lexeme = message;
    tok.length = strlen(message);
    tok.line = lx->line;
    tok.col = lx->col;
    return token_array_push(&lx->out, tok);
}

static void skip_whitespace_and_comments(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
            case '\n':
                (void)advance(lx);
                break;
            case '/':
                if (peek_next(lx) == '/') {
                    while (!is_at_end(lx) && peek(lx) != '\n') {
                        (void)advance(lx);
                    }
                } else if (peek_next(lx) == '*') {
                    (void)advance(lx);
                    (void)advance(lx);
                    while (!is_at_end(lx)) {
                        if (peek(lx) == '*' && peek_next(lx) == '/') {
                            (void)advance(lx);
                            (void)advance(lx);
                            break;
                        }
                        (void)advance(lx);
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static TokenKind keyword_kind(const char *start, size_t len) {
    struct Keyword {
        const char *text;
        TokenKind kind;
    };
    static const struct Keyword keywords[] = {
        {"fn", TOK_FN},
        {"struct", TOK_STRUCT},
        {"trait", TOK_TRAIT},
        {"impl", TOK_IMPL},
        {"macro", TOK_MACRO},
        {"use", TOK_USE},
        {"const", TOK_CONST},
        {"let", TOK_LET},
        {"mut", TOK_MUT},
        {"pub", TOK_PUB},
        {"if", TOK_IF},
        {"else", TOK_ELSE},
        {"for", TOK_FOR},
        {"in", TOK_IN},
        {"match", TOK_MATCH},
        {"return", TOK_RETURN},
        {"try", TOK_TRY},
        {"catch", TOK_CATCH},
        {"finally", TOK_FINALLY},
        {"true", TOK_TRUE},
        {"false", TOK_FALSE},
        {"None", TOK_NONE},
        {"int", TOK_INT},
        {"float", TOK_FLOAT},
        {"double", TOK_DOUBLE},
        {"bool", TOK_BOOL},
        {"string", TOK_STRING}
    };

    size_t i;
    for (i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strlen(keywords[i].text) == len &&
            strncmp(start, keywords[i].text, len) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

static bool lex_identifier(Lexer *lx) {
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') {
        (void)advance(lx);
    }
    return emit_token(lx, keyword_kind(lx->start, (size_t)(lx->current - lx->start)));
}

static bool lex_number(Lexer *lx) {
    bool saw_dot = false;

    while (isdigit((unsigned char)peek(lx))) {
        (void)advance(lx);
    }

    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        saw_dot = true;
        (void)advance(lx);
        while (isdigit((unsigned char)peek(lx))) {
            (void)advance(lx);
        }
    }

    if (saw_dot && peek(lx) == 'f') {
        (void)advance(lx);
        return emit_token(lx, TOK_FLOAT_LIT);
    }

    return emit_token(lx, saw_dot ? TOK_DOUBLE_LIT : TOK_INT_LIT);
}

static bool lex_string(Lexer *lx) {
    while (!is_at_end(lx) && peek(lx) != '"') {
        if (peek(lx) == '\\' && !is_at_end(lx)) {
            (void)advance(lx);
        }
        (void)advance(lx);
    }

    if (is_at_end(lx)) {
        return emit_token(lx, TOK_ERROR);
    }

    (void)advance(lx); /* closing quote */
    return emit_token(lx, TOK_STRING_LIT);
}

static bool lex_next_token(Lexer *lx) {
    skip_whitespace_and_comments(lx);
    lx->start = lx->current;
    lx->token_line = lx->line;
    lx->token_col = lx->col;

    if (is_at_end(lx)) {
        return emit_token(lx, TOK_EOF);
    }

    char c = advance(lx);
    if (isalpha((unsigned char)c) || c == '_') {
        return lex_identifier(lx);
    }
    if (isdigit((unsigned char)c)) {
        return lex_number(lx);
    }

    switch (c) {
        case '(': return emit_token(lx, TOK_LPAREN);
        case ')': return emit_token(lx, TOK_RPAREN);
        case '{': return emit_token(lx, TOK_LBRACE);
        case '}': return emit_token(lx, TOK_RBRACE);
        case '[': return emit_token(lx, TOK_LBRACKET);
        case ']': return emit_token(lx, TOK_RBRACKET);
        case ',': return emit_token(lx, TOK_COMMA);
        case ';': return emit_token(lx, TOK_SEMI);
        case '+': return emit_token(lx, TOK_PLUS);
        case '*': return emit_token(lx, TOK_STAR);
        case '/': return emit_token(lx, TOK_SLASH);
        case '%': return emit_token(lx, TOK_PERCENT);
        case '.':
            return emit_token(lx, match_char(lx, '.') ? TOK_DOT_DOT : TOK_DOT);
        case ':':
            return emit_token(lx, match_char(lx, ':') ? TOK_PATH_SEP : TOK_COLON);
        case '-':
            return emit_token(lx, match_char(lx, '>') ? TOK_ARROW : TOK_MINUS);
        case '!':
            return emit_token(lx, match_char(lx, '=') ? TOK_BANG_EQ : TOK_BANG);
        case '=':
            if (match_char(lx, '=')) {
                return emit_token(lx, TOK_EQ_EQ);
            }
            if (match_char(lx, '>')) {
                return emit_token(lx, TOK_FAT_ARROW);
            }
            return emit_token(lx, TOK_EQ);
        case '<':
            return emit_token(lx, match_char(lx, '=') ? TOK_LT_EQ : TOK_LT);
        case '>':
            return emit_token(lx, match_char(lx, '=') ? TOK_GT_EQ : TOK_GT);
        case '&':
            return emit_token(lx, match_char(lx, '&') ? TOK_AMP_AMP : TOK_AMP);
        case '|':
            return emit_token(lx, match_char(lx, '|') ? TOK_PIPE_PIPE : TOK_PIPE);
        case '$':
            return emit_token(lx, TOK_DOLLAR);
        case '"':
            return lex_string(lx);
        default:
            return emit_token(lx, TOK_ERROR);
    }
}

TokenArray lexer_tokenize(const char *source, const char *filename) {
    Lexer lx;
    lx.source = source;
    lx.filename = filename;
    lx.start = source;
    lx.current = source;
    lx.line = 1;
    lx.col = 1;
    lx.token_line = 1;
    lx.token_col = 1;
    lx.out.items = NULL;
    lx.out.count = 0;
    lx.out.capacity = 0;

    (void)lx.source;
    (void)lx.filename;

    for (;;) {
        if (!lex_next_token(&lx)) {
            token_array_free(&lx.out);
            lx.out.items = NULL;
            lx.out.count = 0;
            lx.out.capacity = 0;
            if (!emit_static_error_token(&lx, "out of memory")) {
                return lx.out;
            }
            return lx.out;
        }

        if (lx.out.count == 0) {
            token_array_free(&lx.out);
            lx.out.items = NULL;
            lx.out.count = 0;
            lx.out.capacity = 0;
            if (!emit_static_error_token(&lx, "out of memory")) {
                return lx.out;
            }
            return lx.out;
        }

        TokenKind last = lx.out.items[lx.out.count - 1].kind;
        if (last == TOK_EOF || last == TOK_ERROR) {
            break;
        }
    }

    return lx.out;
}

void token_array_free(TokenArray *tokens) {
    if (!tokens) {
        return;
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
    tokens->capacity = 0;
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_IDENT: return "IDENT";
        case TOK_INT_LIT: return "INT_LIT";
        case TOK_FLOAT_LIT: return "FLOAT_LIT";
        case TOK_DOUBLE_LIT: return "DOUBLE_LIT";
        case TOK_STRING_LIT: return "STRING_LIT";
        case TOK_FN: return "FN";
        case TOK_STRUCT: return "STRUCT";
        case TOK_TRAIT: return "TRAIT";
        case TOK_IMPL: return "IMPL";
        case TOK_MACRO: return "MACRO";
        case TOK_USE: return "USE";
        case TOK_CONST: return "CONST";
        case TOK_LET: return "LET";
        case TOK_MUT: return "MUT";
        case TOK_PUB: return "PUB";
        case TOK_IF: return "IF";
        case TOK_ELSE: return "ELSE";
        case TOK_FOR: return "FOR";
        case TOK_IN: return "IN";
        case TOK_MATCH: return "MATCH";
        case TOK_RETURN: return "RETURN";
        case TOK_TRY: return "TRY";
        case TOK_CATCH: return "CATCH";
        case TOK_FINALLY: return "FINALLY";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NONE: return "NONE";
        case TOK_INT: return "INT";
        case TOK_FLOAT: return "FLOAT";
        case TOK_DOUBLE: return "DOUBLE";
        case TOK_BOOL: return "BOOL";
        case TOK_STRING: return "STRING";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_COMMA: return "COMMA";
        case TOK_DOT: return "DOT";
        case TOK_COLON: return "COLON";
        case TOK_SEMI: return "SEMI";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_BANG: return "BANG";
        case TOK_AMP: return "AMP";
        case TOK_PIPE: return "PIPE";
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_EQ: return "EQ";
        case TOK_ARROW: return "ARROW";
        case TOK_FAT_ARROW: return "FAT_ARROW";
        case TOK_PATH_SEP: return "PATH_SEP";
        case TOK_DOT_DOT: return "DOT_DOT";
        case TOK_EQ_EQ: return "EQ_EQ";
        case TOK_BANG_EQ: return "BANG_EQ";
        case TOK_LT_EQ: return "LT_EQ";
        case TOK_GT_EQ: return "GT_EQ";
        case TOK_AMP_AMP: return "AMP_AMP";
        case TOK_PIPE_PIPE: return "PIPE_PIPE";
        case TOK_DOLLAR: return "DOLLAR";
        default: return "UNKNOWN";
    }
}
