/*
 * parser.c — build AstNode trees from TokenArray.
 *
 * - Declarations / statements / types: classic recursive descent (expect, match).
 * - Expressions: Pratt parser (parse_expr_pratt) for precedence; atoms from parse_atom;
 *   then parse_postfix chains .field, .method(), [], (), and Type { } struct literals.
 * - Struct literal vs block: Ident '{' is NODE_STRUCT_INIT only if the name looks like a
 *   type (PascalCase: first char upper and either one char, or some lowercase later).
 *   All-caps ids (e.g. const FLAG) do not consume `{`, so `if FLAG {` is not `FLAG { ... }`.
 */
#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Parser {
    TokenArray tokens;
    size_t i;
    const char *filename;
    const char *error;
    uint32_t err_line;
    uint32_t err_col;
    char errbuf[192]; /* expect() formats here so p->error is not a dangling stack pointer */
    AstList *generic_tyargs_pending; /* from `name::<...>` before `{` struct literal */
} Parser;

static SrcLoc tok_loc(const Parser *p, const Token *t) {
    SrcLoc loc;
    loc.file = p->filename;
    loc.line = t->line;
    loc.col = t->col;
    return loc;
}

static void parser_error(Parser *p, const Token *t, const char *msg) {
    if (p->error) {
        return;
    }
    p->error = msg;
    if (t) {
        p->err_line = t->line;
        p->err_col = t->col;
    } else {
        p->err_line = 0;
        p->err_col = 0;
    }
}

static Token *peek(Parser *p) {
    if (p->i >= p->tokens.count) {
        return NULL;
    }
    return &p->tokens.items[p->i];
}

static Token *prev(Parser *p) {
    if (p->i == 0) {
        return NULL;
    }
    return &p->tokens.items[p->i - 1];
}

static bool check(Parser *p, TokenKind k) {
    Token *t = peek(p);
    return t && t->kind == k;
}

static bool match(Parser *p, TokenKind k) {
    if (check(p, k)) {
        p->i++;
        return true;
    }
    return false;
}

static void expect(Parser *p, TokenKind k, const char *what) {
    Token *t = peek(p);
    if (!t || t->kind != k) {
        (void)snprintf(p->errbuf, sizeof(p->errbuf), "expected %s", what);
        parser_error(p, t ? t : prev(p), p->errbuf);
        return;
    }
    p->i++;
}

static const char *expect_ident_copy(Parser *p) {
    Token *t = peek(p);
    if (!t || t->kind != TOK_IDENT) {
        parser_error(p, t ? t : prev(p), "expected identifier");
        return NULL;
    }
    const char *s = ast_copy_string(t->lexeme, t->length);
    if (!s) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    p->i++;
    return s;
}

/* After `Type::`, variant names may be identifiers or the keyword `None` (lexed as TOK_NONE). */
static const char *expect_enum_variant_name(Parser *p) {
    Token *t = peek(p);
    if (t && t->kind == TOK_NONE) {
        const char *s = ast_copy_string("None", 4);
        if (!s) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        p->i++;
        return s;
    }
    return expect_ident_copy(p);
}

static bool ident_text_is(const char *s, const char *lit) {
    return s && strcmp(s, lit) == 0;
}

static AstNode *parse_expr(Parser *p);

static AstList *parse_generic_params(Parser *p) {
    if (!match(p, TOK_LBRACKET)) {
        return NULL;
    }

    AstList *list = NULL;
    do {
        Token *t_name = peek(p);
        const char *nm = expect_ident_copy(p);
        if (p->error) {
            return NULL;
        }

        AstList *bounds = NULL;
        if (match(p, TOK_COLON)) {
            do {
                const char *bn = expect_ident_copy(p);
                if (p->error) {
                    return NULL;
                }
                SrcLoc bl = tok_loc(p, t_name);
                AstNode *bid = ast_alloc(NODE_IDENT, bl);
                if (!bid) {
                    parser_error(p, t_name, "out of memory");
                    return NULL;
                }
                AS_IDENT(bid).name = bn;
                bounds = ast_list_append(bounds, bid);
            } while (match(p, TOK_PLUS));
        }

        AstNode *gp = ast_alloc(NODE_GENERIC_PARAM, tok_loc(p, t_name));
        if (!gp) {
            parser_error(p, t_name, "out of memory");
            return NULL;
        }
        AS_GENERIC_PARAM(gp).name = nm;
        AS_GENERIC_PARAM(gp).bounds = bounds;
        list = ast_list_append(list, gp);
    } while (match(p, TOK_COMMA));

    expect(p, TOK_RBRACKET, "']' after generic parameters");
    return list;
}

static AstNode *parse_type(Parser *p);

static AstNode *parse_fn_type(Parser *p, Token *t_fn) {
    SrcLoc loc = tok_loc(p, t_fn);
    expect(p, TOK_LPAREN, "'(' after fn in function type");
    if (p->error) {
        return NULL;
    }

    AstList *param_types = NULL;
    if (!check(p, TOK_RPAREN)) {
        do {
            AstNode *pt = parse_type(p);
            if (p->error || !pt) {
                return NULL;
            }
            param_types = ast_list_append(param_types, pt);
        } while (match(p, TOK_COMMA));
    }

    expect(p, TOK_RPAREN, "')' after function type parameters");
    expect(p, TOK_ARROW, "'->' in function type");
    AstNode *ret = parse_type(p);
    if (p->error || !ret) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_TYPE_FN, loc);
    if (!n) {
        parser_error(p, t_fn, "out of memory");
        return NULL;
    }
    AS_TYPE_FN(n).param_types = param_types;
    AS_TYPE_FN(n).ret_type = ret;
    return n;
}

static AstNode *parse_type(Parser *p) {
    Token *t0 = peek(p);
    if (!t0) {
        parser_error(p, prev(p), "expected type");
        return NULL;
    }

    if (match(p, TOK_AMP)) {
        SrcLoc loc = tok_loc(p, t0);
        bool is_mut = match(p, TOK_MUT);
        AstNode *inner = parse_type(p);
        if (p->error || !inner) {
            return NULL;
        }
        AstNode *n = ast_alloc(NODE_TYPE_REF, loc);
        if (!n) {
            parser_error(p, t0, "out of memory");
            return NULL;
        }
        AS_TYPE_REF(n).inner = inner;
        AS_TYPE_REF(n).is_mut = is_mut;
        return n;
    }

    if (match(p, TOK_LBRACKET)) {
        SrcLoc loc = tok_loc(p, t0);
        AstNode *elem = parse_type(p);
        if (p->error || !elem) {
            return NULL;
        }
        expect(p, TOK_RBRACKET, "']' after array element type");
        if (p->error) {
            return NULL;
        }
        AstNode *n = ast_alloc(NODE_TYPE_ARRAY, loc);
        if (!n) {
            parser_error(p, t0, "out of memory");
            return NULL;
        }
        AS_TYPE_ARRAY(n).elem_type = elem;
        return n;
    }

    if (match(p, TOK_LPAREN)) {
        SrcLoc loc = tok_loc(p, t0);
        AstList *elem_types = NULL;
        if (!check(p, TOK_RPAREN)) {
            do {
                AstNode *et = parse_type(p);
                if (p->error || !et) {
                    return NULL;
                }
                elem_types = ast_list_append(elem_types, et);
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN, "')' after tuple type");
        if (p->error) {
            return NULL;
        }
        AstNode *n = ast_alloc(NODE_TYPE_TUPLE, loc);
        if (!n) {
            parser_error(p, t0, "out of memory");
            return NULL;
        }
        AS_TYPE_TUPLE(n).elem_types = elem_types;
        return n;
    }

    if (check(p, TOK_FN)) {
        Token *tf = peek(p);
        p->i++;
        return parse_fn_type(p, tf);
    }

    PrimKind prim;
    bool is_prim = true;
    switch (t0->kind) {
        case TOK_INT: prim = PRIM_INT; break;
        case TOK_FLOAT: prim = PRIM_FLOAT; break;
        case TOK_DOUBLE: prim = PRIM_DOUBLE; break;
        case TOK_BOOL: prim = PRIM_BOOL; break;
        case TOK_STRING: prim = PRIM_STRING; break;
        default: is_prim = false; break;
    }
    if (is_prim) {
        SrcLoc loc = tok_loc(p, t0);
        p->i++;
        AstNode *n = ast_alloc(NODE_TYPE_PRIMITIVE, loc);
        if (!n) {
            parser_error(p, t0, "out of memory");
            return NULL;
        }
        AS_TYPE_PRIM(n).prim = prim;
        return n;
    }

    if (check(p, TOK_IDENT)) {
        Token *ti = peek(p);
        SrcLoc loc = tok_loc(p, ti);
        const char *nm = expect_ident_copy(p);
        if (p->error || !nm) {
            return NULL;
        }

        if (ident_text_is(nm, "Self")) {
            if (match(p, TOK_LBRACKET)) {
                parser_error(p, peek(p), "`Self` does not take type arguments");
                return NULL;
            }
            {
                AstNode *sn = ast_alloc(NODE_TYPE_SELF, loc);
                if (!sn) {
                    parser_error(p, ti, "out of memory");
                    return NULL;
                }
                AS_TYPE_SELF(sn)._reserved = 0;
                return sn;
            }
        }

        if (ident_text_is(nm, "Option") && match(p, TOK_LBRACKET)) {
            AstNode *inner = parse_type(p);
            if (p->error || !inner) {
                return NULL;
            }
            expect(p, TOK_RBRACKET, "']' after Option type argument");
            if (p->error) {
                return NULL;
            }
            AstNode *n = ast_alloc(NODE_TYPE_OPTION, loc);
            if (!n) {
                parser_error(p, ti, "out of memory");
                return NULL;
            }
            AS_TYPE_OPTION(n).inner = inner;
            return n;
        }

        if (ident_text_is(nm, "Result") && match(p, TOK_LBRACKET)) {
            AstNode *ok_t = parse_type(p);
            if (p->error || !ok_t) {
                return NULL;
            }
            expect(p, TOK_COMMA, "',' between Result type arguments");
            AstNode *err_t = parse_type(p);
            if (p->error || !err_t) {
                return NULL;
            }
            expect(p, TOK_RBRACKET, "']' after Result type arguments");
            if (p->error) {
                return NULL;
            }
            AstNode *n = ast_alloc(NODE_TYPE_RESULT, loc);
            if (!n) {
                parser_error(p, ti, "out of memory");
                return NULL;
            }
            AS_TYPE_RESULT(n).ok_type = ok_t;
            AS_TYPE_RESULT(n).err_type = err_t;
            return n;
        }

        AstList *args = NULL;
        if (match(p, TOK_LBRACKET)) {
            if (!check(p, TOK_RBRACKET)) {
                do {
                    AstNode *a = parse_type(p);
                    if (p->error || !a) {
                        return NULL;
                    }
                    args = ast_list_append(args, a);
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RBRACKET, "']' after type arguments");
            if (p->error) {
                return NULL;
            }
        }

        AstNode *n = ast_alloc(NODE_TYPE_NAMED, loc);
        if (!n) {
            parser_error(p, ti, "out of memory");
            return NULL;
        }
        AS_TYPE_NAMED(n).name = nm;
        AS_TYPE_NAMED(n).type_args = args;
        return n;
    }

    parser_error(p, t0, "expected type");
    return NULL;
}

static AstList *parse_param_list(Parser *p) {
    AstList *params = NULL;
    if (check(p, TOK_RPAREN)) {
        return NULL;
    }

    do {
        Token *t_param = peek(p);
        bool is_mut = match(p, TOK_MUT);
        const char *name = expect_ident_copy(p);
        if (p->error || !name) {
            return NULL;
        }
        expect(p, TOK_COLON, "':' in parameter");
        if (p->error) {
            return NULL;
        }
        AstNode *ty = parse_type(p);
        if (p->error || !ty) {
            return NULL;
        }

        AstNode *param = ast_alloc(NODE_PARAM, tok_loc(p, t_param));
        if (!param) {
            parser_error(p, t_param, "out of memory");
            return NULL;
        }
        AS_PARAM(param).name = name;
        AS_PARAM(param).type = ty;
        AS_PARAM(param).is_mut = is_mut;
        AS_PARAM(param).is_self = false;

        params = ast_list_append(params, param);
    } while (match(p, TOK_COMMA));

    return params;
}

/* Parameters between `|` and closing `|`; each param is `[mut] IDENT [':' type]`. */
static AstList *parse_lambda_params_until_pipe(Parser *p) {
    AstList *params = NULL;

    while (!check(p, TOK_PIPE) && !p->error) {
        Token *t_param = peek(p);
        bool is_mut = match(p, TOK_MUT);
        const char *name = expect_ident_copy(p);
        if (p->error || !name) {
            return NULL;
        }
        AstNode *ty = NULL;
        if (match(p, TOK_COLON)) {
            ty = parse_type(p);
            if (p->error || !ty) {
                return NULL;
            }
        }

        AstNode *param = ast_alloc(NODE_PARAM, tok_loc(p, t_param));
        if (!param) {
            parser_error(p, t_param, "out of memory");
            return NULL;
        }
        AS_PARAM(param).name = name;
        AS_PARAM(param).type = ty;
        AS_PARAM(param).is_mut = is_mut;
        AS_PARAM(param).is_self = false;
        params = ast_list_append(params, param);

        if (check(p, TOK_PIPE)) {
            break;
        }
        if (!match(p, TOK_COMMA)) {
            Token *tbad = peek(p);
            parser_error(p, tbad ? tbad : t_param, "expected ',' or '|' after lambda parameter");
            return NULL;
        }
    }
    return params;
}

static AstNode *parse_block(Parser *p);

static AstNode *parse_stmt(Parser *p);

static AstNode *parse_let_after_kw(Parser *p, Token *t_let) {
    bool is_mut = match(p, TOK_MUT);
    const char *name = expect_ident_copy(p);
    if (p->error || !name) {
        return NULL;
    }

    AstNode *type_ann = NULL;
    if (match(p, TOK_COLON)) {
        type_ann = parse_type(p);
        if (p->error || !type_ann) {
            return NULL;
        }
    }

    expect(p, TOK_EQ, "'=' in let initializer");
    if (p->error) {
        return NULL;
    }

    AstNode *init = parse_expr(p);
    if (p->error || !init) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_LET, tok_loc(p, t_let));
    if (!n) {
        parser_error(p, t_let, "out of memory");
        return NULL;
    }
    AS_LET(n).name = name;
    AS_LET(n).type = type_ann;
    AS_LET(n).init = init;
    AS_LET(n).is_mut = is_mut;
    return n;
}

static AstNode *parse_return_after_kw(Parser *p, Token *t_ret) {
    if (check(p, TOK_SEMI) || check(p, TOK_RBRACE)) {
        AstNode *n = ast_alloc(NODE_RETURN, tok_loc(p, t_ret));
        if (!n) {
            parser_error(p, t_ret, "out of memory");
            return NULL;
        }
        AS_RETURN(n).value = NULL;
        return n;
    }

    AstNode *val = parse_expr(p);
    if (p->error || !val) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_RETURN, tok_loc(p, t_ret));
    if (!n) {
        parser_error(p, t_ret, "out of memory");
        return NULL;
    }
    AS_RETURN(n).value = val;
    return n;
}

static AstNode *parse_try_after_kw(Parser *p, Token *t_try) {
    AstNode *body;
    AstList *catches = NULL;
    AstNode *finally_body = NULL;
    AstNode *n;

    body = parse_block(p);
    if (p->error || !body) {
        return NULL;
    }
    while (match(p, TOK_CATCH)) {
        AstNode *cl;
        const char *vname;
        AstNode *ty;
        AstNode *cbody;

        expect(p, TOK_LPAREN, "'(' after catch");
        if (p->error) {
            return NULL;
        }
        vname = expect_ident_copy(p);
        if (p->error || !vname) {
            return NULL;
        }
        expect(p, TOK_COLON, "':' in catch binding");
        if (p->error) {
            return NULL;
        }
        ty = parse_type(p);
        if (p->error || !ty) {
            return NULL;
        }
        expect(p, TOK_RPAREN, "')' after catch binding");
        if (p->error) {
            return NULL;
        }
        cbody = parse_block(p);
        if (p->error || !cbody) {
            return NULL;
        }
        cl = ast_alloc(NODE_CATCH_CLAUSE, tok_loc(p, t_try));
        if (!cl) {
            parser_error(p, t_try, "out of memory");
            return NULL;
        }
        AS_CATCH(cl).var = vname;
        AS_CATCH(cl).type = ty;
        AS_CATCH(cl).body = cbody;
        catches = ast_list_append(catches, cl);
        if (!catches) {
            parser_error(p, t_try, "out of memory");
            return NULL;
        }
    }
    if (match(p, TOK_FINALLY)) {
        finally_body = parse_block(p);
        if (p->error || !finally_body) {
            return NULL;
        }
    }
    n = ast_alloc(NODE_TRY, tok_loc(p, t_try));
    if (!n) {
        parser_error(p, t_try, "out of memory");
        return NULL;
    }
    AS_TRY(n).body = body;
    AS_TRY(n).catch_clauses = catches;
    AS_TRY(n).finally_body = finally_body;
    return n;
}

static AstNode *parse_throw_after_kw(Parser *p, Token *t_throw) {
    AstNode *e;
    AstNode *n;

    e = parse_expr(p);
    if (p->error || !e) {
        return NULL;
    }
    n = ast_alloc(NODE_THROW, tok_loc(p, t_throw));
    if (!n) {
        parser_error(p, t_throw, "out of memory");
        return NULL;
    }
    AS_THROW(n).expr = e;
    return n;
}

static AstNode *parse_for_after_kw(Parser *p, Token *t_for) {
    const char *var = expect_ident_copy(p);
    if (p->error || !var) {
        return NULL;
    }
    expect(p, TOK_IN, "'in' in for-loop");
    if (p->error) {
        return NULL;
    }
    AstNode *iter = parse_expr(p);
    if (p->error || !iter) {
        return NULL;
    }
    AstNode *body = parse_block(p);
    if (p->error || !body) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_FOR, tok_loc(p, t_for));
    if (!n) {
        parser_error(p, t_for, "out of memory");
        return NULL;
    }
    AS_FOR(n).var = var;
    AS_FOR(n).iter = iter;
    AS_FOR(n).body = body;
    return n;
}

static AstNode *parse_stmt(Parser *p) {
    Token *t0 = peek(p);
    if (!t0) {
        parser_error(p, prev(p), "expected statement");
        return NULL;
    }

    if (t0->kind == TOK_LET) {
        p->i++;
        return parse_let_after_kw(p, t0);
    }
    if (t0->kind == TOK_RETURN) {
        p->i++;
        return parse_return_after_kw(p, t0);
    }
    if (t0->kind == TOK_FOR) {
        p->i++;
        return parse_for_after_kw(p, t0);
    }
    if (t0->kind == TOK_TRY) {
        p->i++;
        return parse_try_after_kw(p, t0);
    }
    if (t0->kind == TOK_THROW) {
        p->i++;
        return parse_throw_after_kw(p, t0);
    }

    parser_error(p, t0, "expected statement");
    return NULL;
}

static bool stmt_starts(TokenKind k) {
    return k == TOK_LET || k == TOK_RETURN || k == TOK_FOR || k == TOK_TRY || k == TOK_THROW;
}

static AstNode *parse_block(Parser *p) {
    Token *t_brace = peek(p);
    expect(p, TOK_LBRACE, "'{' to start block");
    if (p->error) {
        return NULL;
    }

    AstList *stmts = NULL;
    AstNode *tail = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token *t = peek(p);
        if (!t) {
            break;
        }

        if (stmt_starts(t->kind)) {
            AstNode *s = parse_stmt(p);
            if (p->error || !s) {
                return NULL;
            }
            expect(p, TOK_SEMI, "';' after statement");
            if (p->error) {
                return NULL;
            }
            stmts = ast_list_append(stmts, s);
            continue;
        }

        AstNode *e = parse_expr(p);
        if (p->error || !e) {
            return NULL;
        }

        if (match(p, TOK_SEMI)) {
            AstNode *es = ast_alloc(NODE_EXPR_STMT, e->loc);
            if (!es) {
                parser_error(p, t, "out of memory");
                return NULL;
            }
            AS_EXPR_STMT(es).expr = e;
            stmts = ast_list_append(stmts, es);
            continue;
        }

        if (check(p, TOK_RBRACE)) {
            tail = e;
            break;
        }

        parser_error(p, peek(p), "expected ';' or '}' after expression");
        return NULL;
    }

    expect(p, TOK_RBRACE, "'}' to end block");
    if (p->error) {
        return NULL;
    }

    AstNode *blk = ast_alloc(NODE_BLOCK, tok_loc(p, t_brace));
    if (!blk) {
        parser_error(p, t_brace, "out of memory");
        return NULL;
    }
    AS_BLOCK(blk).stmts = stmts;
    AS_BLOCK(blk).tail_expr = tail;
    return blk;
}

static AstNode *parse_if_expr(Parser *p, Token *t_if) {
    AstNode *cond = parse_expr(p);
    if (p->error || !cond) {
        return NULL;
    }
    AstNode *then_body = parse_block(p);
    if (p->error || !then_body) {
        return NULL;
    }

    AstList *branches = NULL;
    branches = ast_list_append(branches, cond);
    branches = ast_list_append(branches, then_body);

    AstNode *else_body = NULL;

    while (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF)) {
            Token *t_elseif = prev(p);
            (void)t_elseif;
            AstNode *econd = parse_expr(p);
            if (p->error || !econd) {
                return NULL;
            }
            AstNode *ebody = parse_block(p);
            if (p->error || !ebody) {
                return NULL;
            }
            branches = ast_list_append(branches, econd);
            branches = ast_list_append(branches, ebody);
            continue;
        }

        else_body = parse_block(p);
        if (p->error || !else_body) {
            return NULL;
        }
        break;
    }

    AstNode *n = ast_alloc(NODE_IF, tok_loc(p, t_if));
    if (!n) {
        parser_error(p, t_if, "out of memory");
        return NULL;
    }
    AS_IF(n).branches = branches;
    AS_IF(n).else_body = else_body;
    return n;
}

/* Inside `{ ... }` after a type name: `name: expr,` fields, then optional `..base` (spread). */
static AstNode *parse_field_inits(Parser *p, AstNode *struct_init) {
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (match(p, TOK_DOT_DOT)) {
            AstNode *base = parse_expr(p);
            if (p->error || !base) {
                return NULL;
            }
            AS_STRUCT_INIT(struct_init).base = base;
            (void)match(p, TOK_COMMA); /* allow `..base,` before `}` */
            break;
        }

        const char *fname = expect_ident_copy(p);
        if (p->error || !fname) {
            return NULL;
        }
        expect(p, TOK_COLON, "':' in struct field initializer");
        if (p->error) {
            return NULL;
        }
        AstNode *val = parse_expr(p);
        if (p->error || !val) {
            return NULL;
        }
        expect(p, TOK_COMMA, "',' after struct field initializer");
        if (p->error) {
            return NULL;
        }

        AstNode *fi = ast_alloc(NODE_FIELD_INIT, val->loc);
        if (!fi) {
            parser_error(p, peek(p), "out of memory");
            return NULL;
        }
        AS_FIELD_INIT(fi).name = fname;
        AS_FIELD_INIT(fi).value = val;
        AS_STRUCT_INIT(struct_init).fields =
            ast_list_append(AS_STRUCT_INIT(struct_init).fields, fi);
    }

    expect(p, TOK_RBRACE, "'}' after struct initializer");
    if (p->error) {
        return NULL;
    }
    return struct_init;
}

/* Precedence for Pratt parser (higher = tighter). Unary operand uses min > PREC_UNARY. */
enum {
    PREC_ASSIGN = 10,
    PREC_OR = 20,
    PREC_AND = 25,
    PREC_EQ = 30,
    PREC_CMP = 35,
    PREC_TERM = 40,
    PREC_FACTOR = 50,
    PREC_UNARY = 60
};

static AstNode *parse_atom(Parser *p);
static AstNode *parse_expr_pratt(Parser *p, int min_prec);

/* True if `name` should parse as `name { fields }` (struct init), not leave `{` for a block. */
static bool ident_looks_like_struct_type_name(const char *name) {
    size_t i;
    size_t len;
    if (!name || !name[0] || !isupper((unsigned char)name[0])) {
        return false;
    }
    len = strlen(name);
    if (len == 1u) {
        return true;
    }
    for (i = 1u; name[i]; i++) {
        if (islower((unsigned char)name[i])) {
            return true;
        }
    }
    return false;
}

static AstNode *parse_postfix(Parser *p, AstNode *n) {
    p->generic_tyargs_pending = NULL;
    for (;;) {
        if (p->generic_tyargs_pending && !check(p, TOK_LBRACE)) {
            parser_error(p, peek(p), "expected '{' after `::<...>` for generic struct literal (or use `f::<T>(...)`)");
            return NULL;
        }
        if (n->kind == NODE_IDENT && check(p, TOK_PATH_SEP)) {
            Token *tsep = peek(p);
            (void)tsep;
            p->i++;
            if (!match(p, TOK_LT)) {
                parser_error(p, peek(p), "expected '<' after `::` (generic syntax is `name::<T>(...)` or `Type::<T> { ... }`)");
                return NULL;
            }
            {
                AstList *tyargs = NULL;
                if (!check(p, TOK_GT)) {
                    do {
                        AstNode *ta = parse_type(p);
                        if (p->error || !ta) {
                            return NULL;
                        }
                        tyargs = ast_list_append(tyargs, ta);
                        if (!tyargs) {
                            parser_error(p, peek(p), "out of memory");
                            return NULL;
                        }
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_GT, "'>' after type arguments");
                if (p->error) {
                    return NULL;
                }
                if (match(p, TOK_LPAREN)) {
                    AstList *args = NULL;
                    if (!check(p, TOK_RPAREN)) {
                        do {
                            AstNode *a = parse_expr(p);
                            if (p->error || !a) {
                                return NULL;
                            }
                            args = ast_list_append(args, a);
                            if (!args) {
                                parser_error(p, peek(p), "out of memory");
                                return NULL;
                            }
                        } while (match(p, TOK_COMMA));
                    }
                    expect(p, TOK_RPAREN, "')' after call arguments");
                    if (p->error) {
                        return NULL;
                    }
                    {
                        AstNode *call = ast_alloc(NODE_CALL, n->loc);
                        if (!call) {
                            parser_error(p, peek(p), "out of memory");
                            return NULL;
                        }
                        AS_CALL(call).callee = n;
                        AS_CALL(call).args = args;
                        AS_CALL(call).type_args = tyargs;
                        n = call;
                        p->generic_tyargs_pending = NULL;
                        continue;
                    }
                }
                p->generic_tyargs_pending = tyargs;
                continue;
            }
        }
        if (match(p, TOK_DOT)) {
            const char *field = expect_ident_copy(p);
            if (p->error || !field) {
                return NULL;
            }

            if (match(p, TOK_LPAREN)) {
                AstList *args = NULL;
                if (!check(p, TOK_RPAREN)) {
                    do {
                        AstNode *a = parse_expr(p);
                        if (p->error || !a) {
                            return NULL;
                        }
                        args = ast_list_append(args, a);
                    } while (match(p, TOK_COMMA));
                }
                expect(p, TOK_RPAREN, "')' after method arguments");
                if (p->error) {
                    return NULL;
                }

                AstNode *mc = ast_alloc(NODE_METHOD_CALL, n->loc);
                if (!mc) {
                    parser_error(p, peek(p), "out of memory");
                    return NULL;
                }
                AS_METHOD_CALL(mc).receiver = n;
                AS_METHOD_CALL(mc).method = field;
                AS_METHOD_CALL(mc).args = args;
                AS_METHOD_CALL(mc).type_args = NULL;
                n = mc;
                continue;
            }

            AstNode *fa = ast_alloc(NODE_FIELD_ACCESS, n->loc);
            if (!fa) {
                parser_error(p, peek(p), "out of memory");
                return NULL;
            }
            AS_FIELD_ACCESS(fa).object = n;
            AS_FIELD_ACCESS(fa).field = field;
            n = fa;
            continue;
        }

        if (match(p, TOK_LBRACKET)) {
            AstNode *idx = parse_expr(p);
            if (p->error || !idx) {
                return NULL;
            }
            expect(p, TOK_RBRACKET, "']' after index expression");
            if (p->error) {
                return NULL;
            }
            AstNode *ix = ast_alloc(NODE_INDEX, n->loc);
            if (!ix) {
                parser_error(p, peek(p), "out of memory");
                return NULL;
            }
            AS_INDEX(ix).object = n;
            AS_INDEX(ix).index = idx;
            n = ix;
            continue;
        }

        if (match(p, TOK_LPAREN)) {
            AstList *args = NULL;
            if (!check(p, TOK_RPAREN)) {
                do {
                    AstNode *a = parse_expr(p);
                    if (p->error || !a) {
                        return NULL;
                    }
                    args = ast_list_append(args, a);
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN, "')' after call arguments");
            if (p->error) {
                return NULL;
            }
            AstNode *call = ast_alloc(NODE_CALL, n->loc);
            if (!call) {
                parser_error(p, peek(p), "out of memory");
                return NULL;
            }
            AS_CALL(call).callee = n;
            AS_CALL(call).args = args;
            AS_CALL(call).type_args = NULL;
            n = call;
            continue;
        }

        /* PascalCase type names (Point, T); not ALL_CAPS consts (FLAG). `for x in a {` still safe (a lower). */
        if (n->kind == NODE_IDENT && check(p, TOK_LBRACE)) {
            const char *sname = AS_IDENT(n).name;
            if (!ident_looks_like_struct_type_name(sname)) {
                break;
            }
            p->i++;
            AstNode *si = ast_alloc(NODE_STRUCT_INIT, n->loc);
            if (!si) {
                parser_error(p, peek(p), "out of memory");
                return NULL;
            }
            AS_STRUCT_INIT(si).struct_name = sname;
            AS_STRUCT_INIT(si).fields = NULL;
            AS_STRUCT_INIT(si).base = NULL;
            AS_STRUCT_INIT(si).type_args = p->generic_tyargs_pending;
            p->generic_tyargs_pending = NULL;
            if (!parse_field_inits(p, si)) {
                return NULL;
            }
            n = si;
            continue;
        }

        break;
    }
    if (p->generic_tyargs_pending) {
        parser_error(p, peek(p), "incomplete `::<...>` (expected '{' or '(')");
        return NULL;
    }
    return n;
}

static AstNode *parse_int_literal(Parser *p, Token *t) {
    const char *tmp = ast_copy_string(t->lexeme, t->length);
    if (!tmp) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(tmp, &end, 10);
    if (errno != 0 || (size_t)(end - tmp) != strlen(tmp)) {
        parser_error(p, t, "invalid integer literal");
        return NULL;
    }
    AstNode *n = ast_alloc(NODE_LIT_INT, tok_loc(p, t));
    if (!n) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    AS_LIT_INT(n).value = (int64_t)v;
    return n;
}

static AstNode *parse_float_literal(Parser *p, Token *t) {
    size_t len = t->length;
    if (len == 0) {
        parser_error(p, t, "invalid float literal");
        return NULL;
    }
    const char *s = t->lexeme;
    if (s[len - 1] == 'f') {
        len--;
    }
    const char *tmp = ast_copy_string(s, len);
    if (!tmp) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    errno = 0;
    char *end = NULL;
    float v = strtof(tmp, &end);
    if (errno != 0 || (size_t)(end - tmp) != strlen(tmp)) {
        parser_error(p, t, "invalid float literal");
        return NULL;
    }
    AstNode *n = ast_alloc(NODE_LIT_FLOAT, tok_loc(p, t));
    if (!n) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    AS_LIT_FLOAT(n).value = v;
    return n;
}

static AstNode *parse_double_literal(Parser *p, Token *t) {
    const char *tmp = ast_copy_string(t->lexeme, t->length);
    if (!tmp) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    errno = 0;
    char *end = NULL;
    double v = strtod(tmp, &end);
    if (errno != 0 || (size_t)(end - tmp) != strlen(tmp)) {
        parser_error(p, t, "invalid double literal");
        return NULL;
    }
    AstNode *n = ast_alloc(NODE_LIT_DOUBLE, tok_loc(p, t));
    if (!n) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    AS_LIT_DOUBLE(n).value = v;
    return n;
}

static AstNode *parse_string_literal(Parser *p, Token *t) {
    if (t->length < 2 || t->lexeme[0] != '"' || t->lexeme[t->length - 1] != '"') {
        parser_error(p, t, "invalid string literal");
        return NULL;
    }
    const char *inner = t->lexeme + 1;
    size_t inner_len = t->length - 2;

    size_t cap = inner_len + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    size_t w = 0;
    size_t r = 0;
    while (r < inner_len) {
        char c = inner[r++];
        if (c != '\\') {
            buf[w++] = c;
            continue;
        }
        if (r >= inner_len) {
            free(buf);
            parser_error(p, t, "invalid escape in string literal");
            return NULL;
        }
        char e = inner[r++];
        switch (e) {
            case 'n': buf[w++] = '\n'; break;
            case 't': buf[w++] = '\t'; break;
            case 'r': buf[w++] = '\r'; break;
            case '\\': buf[w++] = '\\'; break;
            case '"': buf[w++] = '"'; break;
            case '0': buf[w++] = '\0'; break;
            default:
                free(buf);
                parser_error(p, t, "invalid escape in string literal");
                return NULL;
        }
    }
    buf[w] = '\0';

    const char *interned = ast_copy_string(buf, w);
    free(buf);
    if (!interned) {
        parser_error(p, t, "out of memory");
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_LIT_STRING, tok_loc(p, t));
    if (!n) {
        parser_error(p, t, "out of memory");
        return NULL;
    }
    AS_LIT_STRING(n).value = interned;
    return n;
}

static AstNode *parse_paren_or_tuple(Parser *p, Token *t_lpar) {
    if (check(p, TOK_RPAREN)) {
        parser_error(p, t_lpar, "empty parentheses");
        return NULL;
    }

    AstNode *first = parse_expr(p);
    if (p->error || !first) {
        return NULL;
    }

    if (match(p, TOK_COMMA)) {
        AstList *els = NULL;
        els = ast_list_append(els, first);
        if (!check(p, TOK_RPAREN)) {
            do {
                AstNode *e = parse_expr(p);
                if (p->error || !e) {
                    return NULL;
                }
                els = ast_list_append(els, e);
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN, "')' after tuple");
        if (p->error) {
            return NULL;
        }
        AstNode *tu = ast_alloc(NODE_TUPLE, tok_loc(p, t_lpar));
        if (!tu) {
            parser_error(p, t_lpar, "out of memory");
            return NULL;
        }
        AS_TUPLE(tu).elements = els;
        return tu;
    }

    expect(p, TOK_RPAREN, "')' after expression");
    if (p->error) {
        return NULL;
    }
    return first;
}

static AstNode *make_binary(AstNode *left, BinOp op, AstNode *right) {
    AstNode *n = ast_alloc(NODE_BINARY, left->loc);
    if (!n) {
        return NULL;
    }
    AS_BINARY(n).op = op;
    AS_BINARY(n).left = left;
    AS_BINARY(n).right = right;
    return n;
}

static AstNode *make_pat_or(Parser *p, AstNode *left, AstNode *right) {
    AstNode *n = ast_alloc(NODE_PAT_OR, left->loc);
    if (!n) {
        parser_error(p, peek(p), "out of memory");
        return NULL;
    }
    (void)p;
    AS_PAT_OR(n).left = left;
    AS_PAT_OR(n).right = right;
    return n;
}

static AstNode *wrap_pat_literal(Parser *p, AstNode *lit) {
    AstNode *n = ast_alloc(NODE_PAT_LITERAL, lit->loc);
    if (!n) {
        parser_error(p, peek(p), "out of memory");
        return NULL;
    }
    AS_PAT_LIT(n).lit = lit;
    return n;
}

static AstNode *parse_pattern(Parser *p);

static AstNode *parse_pattern_atom(Parser *p) {
    Token *t = peek(p);
    if (!t) {
        parser_error(p, prev(p), "expected pattern");
        return NULL;
    }

    if (t->kind == TOK_TRUE || t->kind == TOK_FALSE) {
        p->i++;
        {
            AstNode *lit = ast_alloc(NODE_LIT_BOOL, tok_loc(p, t));
            if (!lit) {
                parser_error(p, t, "out of memory");
                return NULL;
            }
            AS_LIT_BOOL(lit).value = (t->kind == TOK_TRUE);
            return wrap_pat_literal(p, lit);
        }
    }
    if (t->kind == TOK_NONE) {
        p->i++;
        {
            AstNode *lit = ast_alloc(NODE_LIT_NONE, tok_loc(p, t));
            if (!lit) {
                parser_error(p, t, "out of memory");
                return NULL;
            }
            return wrap_pat_literal(p, lit);
        }
    }
    if (t->kind == TOK_INT_LIT) {
        p->i++;
        {
            AstNode *lit = parse_int_literal(p, t);
            if (p->error || !lit) {
                return NULL;
            }
            return wrap_pat_literal(p, lit);
        }
    }
    if (t->kind == TOK_FLOAT_LIT || t->kind == TOK_DOUBLE_LIT) {
        parser_error(p, t, "float/double patterns not supported yet");
        return NULL;
    }
    if (t->kind == TOK_STRING_LIT) {
        p->i++;
        {
            AstNode *lit = parse_string_literal(p, t);
            if (p->error || !lit) {
                return NULL;
            }
            return wrap_pat_literal(p, lit);
        }
    }

    if (t->kind == TOK_IDENT) {
        const char *nm = expect_ident_copy(p);
        if (p->error || !nm) {
            return NULL;
        }
        if (strcmp(nm, "_") == 0) {
            AstNode *w = ast_alloc(NODE_PAT_WILDCARD, tok_loc(p, t));
            if (!w) {
                parser_error(p, t, "out of memory");
                return NULL;
            }
            return w;
        }
        if (check(p, TOK_PATH_SEP)) {
            const char *tn = nm;
            p->i++;
            {
                const char *vn = expect_enum_variant_name(p);
                AstList *flds = NULL;
                if (p->error || !vn) {
                    return NULL;
                }
                if (match(p, TOK_LPAREN)) {
                    if (!check(p, TOK_RPAREN)) {
                        do {
                            AstNode *sub = parse_pattern(p);
                            if (p->error || !sub) {
                                return NULL;
                            }
                            flds = ast_list_append(flds, sub);
                            if (!flds) {
                                parser_error(p, peek(p), "out of memory");
                                return NULL;
                            }
                        } while (match(p, TOK_COMMA));
                    }
                    expect(p, TOK_RPAREN, "')' after enum pattern fields");
                    if (p->error) {
                        return NULL;
                    }
                }
                {
                    AstNode *en = ast_alloc(NODE_PAT_ENUM, tok_loc(p, t));
                    if (!en) {
                        parser_error(p, t, "out of memory");
                        return NULL;
                    }
                    AS_PAT_ENUM(en).type_name = tn;
                    AS_PAT_ENUM(en).variant = vn;
                    AS_PAT_ENUM(en).fields = flds;
                    return en;
                }
            }
        }
        if (check(p, TOK_LBRACE) && ident_looks_like_struct_type_name(nm)) {
            AstList *fields = NULL;
            bool rest = false;
            p->i++;
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                const char *fnm = expect_ident_copy(p);
                if (p->error || !fnm) {
                    return NULL;
                }
                if (match(p, TOK_COLON)) {
                    AstNode *sub = parse_pattern(p);
                    AstNode *pf;
                    if (p->error || !sub) {
                        return NULL;
                    }
                    pf = ast_alloc(NODE_PAT_FIELD, tok_loc(p, t));
                    if (!pf) {
                        parser_error(p, t, "out of memory");
                        return NULL;
                    }
                    AS_PAT_FIELD(pf).field = fnm;
                    AS_PAT_FIELD(pf).pattern = sub;
                    fields = ast_list_append(fields, pf);
                } else {
                    AstNode *pf = ast_alloc(NODE_PAT_FIELD, tok_loc(p, t));
                    if (!pf) {
                        parser_error(p, t, "out of memory");
                        return NULL;
                    }
                    AS_PAT_FIELD(pf).field = fnm;
                    AS_PAT_FIELD(pf).pattern = NULL;
                    fields = ast_list_append(fields, pf);
                }
                if (!fields) {
                    parser_error(p, peek(p), "out of memory");
                    return NULL;
                }
                if (match(p, TOK_COMMA)) {
                    continue;
                }
                break;
            }
            if (match(p, TOK_DOT_DOT)) {
                rest = true;
            }
            expect(p, TOK_RBRACE, "'}' after struct pattern");
            if (p->error) {
                return NULL;
            }
            {
                AstNode *st = ast_alloc(NODE_PAT_STRUCT, tok_loc(p, t));
                if (!st) {
                    parser_error(p, t, "out of memory");
                    return NULL;
                }
                AS_PAT_STRUCT(st).name = nm;
                AS_PAT_STRUCT(st).field_pats = fields;
                AS_PAT_STRUCT(st).rest = rest;
                return st;
            }
        }
        {
            AstNode *b = ast_alloc(NODE_PAT_BIND, tok_loc(p, t));
            if (!b) {
                parser_error(p, t, "out of memory");
                return NULL;
            }
            AS_PAT_BIND(b).name = nm;
            AS_PAT_BIND(b).is_mut = false;
            return b;
        }
    }

    if (t->kind == TOK_LPAREN) {
        p->i++;
        {
            AstNode *inner = parse_pattern(p);
            if (p->error || !inner) {
                return NULL;
            }
            if (match(p, TOK_COMMA)) {
                AstList *els = ast_list_append(NULL, inner);
                if (!els) {
                    parser_error(p, t, "out of memory");
                    return NULL;
                }
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    AstNode *el = parse_pattern(p);
                    if (p->error || !el) {
                        return NULL;
                    }
                    els = ast_list_append(els, el);
                    if (!els) {
                        parser_error(p, peek(p), "out of memory");
                        return NULL;
                    }
                    if (!match(p, TOK_COMMA)) {
                        break;
                    }
                }
                expect(p, TOK_RPAREN, "')' after tuple pattern");
                if (p->error) {
                    return NULL;
                }
                {
                    AstNode *tu = ast_alloc(NODE_PAT_TUPLE, tok_loc(p, t));
                    if (!tu) {
                        parser_error(p, t, "out of memory");
                        return NULL;
                    }
                    AS_PAT_TUPLE(tu).elements = els;
                    return tu;
                }
            }
            expect(p, TOK_RPAREN, "')' after pattern");
            if (p->error) {
                return NULL;
            }
            return inner;
        }
    }

    if (t->kind == TOK_LBRACKET) {
        p->i++;
        {
            AstList *els = NULL;
            if (!check(p, TOK_RBRACKET)) {
                do {
                    AstNode *el = parse_pattern(p);
                    if (p->error || !el) {
                        return NULL;
                    }
                    els = ast_list_append(els, el);
                    if (!els) {
                        parser_error(p, peek(p), "out of memory");
                        return NULL;
                    }
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RBRACKET, "']' after array pattern");
            if (p->error) {
                return NULL;
            }
            {
                AstNode *ar = ast_alloc(NODE_PAT_ARRAY, tok_loc(p, t));
                if (!ar) {
                    parser_error(p, t, "out of memory");
                    return NULL;
                }
                AS_PAT_ARRAY(ar).elements = els;
                return ar;
            }
        }
    }

    parser_error(p, t, "expected pattern");
    return NULL;
}

static AstNode *parse_pattern(Parser *p) {
    AstNode *lhs = parse_pattern_atom(p);
    if (p->error || !lhs) {
        return NULL;
    }
    while (match(p, TOK_PIPE)) {
        AstNode *rhs = parse_pattern_atom(p);
        if (p->error || !rhs) {
            return NULL;
        }
        lhs = make_pat_or(p, lhs, rhs);
        if (p->error || !lhs) {
            return NULL;
        }
    }
    return lhs;
}

static AstNode *parse_match_arm(Parser *p) {
    AstNode *pat = parse_pattern(p);
    AstNode *guard = NULL;
    AstNode *body;
    AstNode *arm;
    Token *t0 = peek(p);

    if (p->error || !pat) {
        return NULL;
    }
    if (match(p, TOK_IF)) {
        guard = parse_expr(p);
        if (p->error || !guard) {
            return NULL;
        }
    }
    expect(p, TOK_FAT_ARROW, "'=>' in match arm");
    if (p->error) {
        return NULL;
    }
    if (check(p, TOK_LBRACE)) {
        body = parse_block(p);
    } else {
        body = parse_expr(p);
        if (p->error || !body) {
            return NULL;
        }
        expect(p, TOK_COMMA, "comma after match arm expression");
    }
    if (p->error || !body) {
        return NULL;
    }
    arm = ast_alloc(NODE_MATCH_ARM, pat->loc);
    if (!arm) {
        parser_error(p, t0 ? t0 : peek(p), "out of memory");
        return NULL;
    }
    AS_MATCH_ARM(arm).pattern = pat;
    AS_MATCH_ARM(arm).guard = guard;
    AS_MATCH_ARM(arm).body = body;
    return arm;
}

static AstNode *parse_match_expr(Parser *p, Token *t_match) {
    AstNode *subj;
    AstList *arms = NULL;
    AstNode *n;

    subj = parse_expr(p);
    if (p->error || !subj) {
        return NULL;
    }
    expect(p, TOK_LBRACE, "'{' after match subject");
    if (p->error) {
        return NULL;
    }
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        AstNode *a = parse_match_arm(p);
        if (p->error || !a) {
            return NULL;
        }
        arms = ast_list_append(arms, a);
        if (!arms) {
            parser_error(p, peek(p), "out of memory");
            return NULL;
        }
    }
    expect(p, TOK_RBRACE, "'}' after match arms");
    if (p->error) {
        return NULL;
    }
    n = ast_alloc(NODE_MATCH, tok_loc(p, t_match));
    if (!n) {
        parser_error(p, t_match, "out of memory");
        return NULL;
    }
    AS_MATCH(n).subject = subj;
    AS_MATCH(n).arms = arms;
    return n;
}

/* Returns false if token is not a binary infix operator at expression level. */
static bool pratt_infix(TokenKind k, int *left_prec, BinOp *bop, bool *is_assign) {
    *is_assign = false;
    switch (k) {
        case TOK_EQ:
            *left_prec = PREC_ASSIGN;
            *is_assign = true;
            return true;
        case TOK_PIPE_PIPE:
            *left_prec = PREC_OR;
            *bop = BINOP_OR;
            return true;
        case TOK_AMP_AMP:
            *left_prec = PREC_AND;
            *bop = BINOP_AND;
            return true;
        case TOK_EQ_EQ:
            *left_prec = PREC_EQ;
            *bop = BINOP_EQ;
            return true;
        case TOK_BANG_EQ:
            *left_prec = PREC_EQ;
            *bop = BINOP_NEQ;
            return true;
        case TOK_LT:
            *left_prec = PREC_CMP;
            *bop = BINOP_LT;
            return true;
        case TOK_GT:
            *left_prec = PREC_CMP;
            *bop = BINOP_GT;
            return true;
        case TOK_LT_EQ:
            *left_prec = PREC_CMP;
            *bop = BINOP_LTE;
            return true;
        case TOK_GT_EQ:
            *left_prec = PREC_CMP;
            *bop = BINOP_GTE;
            return true;
        case TOK_PLUS:
            *left_prec = PREC_TERM;
            *bop = BINOP_ADD;
            return true;
        case TOK_MINUS:
            *left_prec = PREC_TERM;
            *bop = BINOP_SUB;
            return true;
        case TOK_STAR:
            *left_prec = PREC_FACTOR;
            *bop = BINOP_MUL;
            return true;
        case TOK_SLASH:
            *left_prec = PREC_FACTOR;
            *bop = BINOP_DIV;
            return true;
        case TOK_PERCENT:
            *left_prec = PREC_FACTOR;
            *bop = BINOP_MOD;
            return true;
        default:
            return false;
    }
}

static AstNode *parse_atom(Parser *p) {
    Token *t = peek(p);
    if (!t) {
        parser_error(p, prev(p), "expected expression");
        return NULL;
    }

    if (t->kind == TOK_TRUE || t->kind == TOK_FALSE) {
        p->i++;
        AstNode *n = ast_alloc(NODE_LIT_BOOL, tok_loc(p, t));
        if (!n) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        AS_LIT_BOOL(n).value = (t->kind == TOK_TRUE);
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_NONE) {
        p->i++;
        AstNode *n = ast_alloc(NODE_LIT_NONE, tok_loc(p, t));
        if (!n) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_INT_LIT) {
        p->i++;
        AstNode *n = parse_int_literal(p, t);
        if (p->error || !n) {
            return NULL;
        }
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_FLOAT_LIT) {
        p->i++;
        AstNode *n = parse_float_literal(p, t);
        if (p->error || !n) {
            return NULL;
        }
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_DOUBLE_LIT) {
        p->i++;
        AstNode *n = parse_double_literal(p, t);
        if (p->error || !n) {
            return NULL;
        }
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_STRING_LIT) {
        p->i++;
        AstNode *n = parse_string_literal(p, t);
        if (p->error || !n) {
            return NULL;
        }
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_IDENT) {
        const char *nm = expect_ident_copy(p);
        if (p->error || !nm) {
            return NULL;
        }
        AstNode *id = ast_alloc(NODE_IDENT, tok_loc(p, t));
        if (!id) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        AS_IDENT(id).name = nm;
        return parse_postfix(p, id);
    }

    if (t->kind == TOK_IF) {
        p->i++;
        AstNode *n = parse_if_expr(p, t);
        if (p->error || !n) {
            return NULL;
        }
        return parse_postfix(p, n);
    }

    if (t->kind == TOK_MATCH) {
        p->i++;
        {
            AstNode *n = parse_match_expr(p, t);
            if (p->error || !n) {
                return NULL;
            }
            return parse_postfix(p, n);
        }
    }

    if (t->kind == TOK_LBRACE) {
        AstNode *blk = parse_block(p);
        if (p->error || !blk) {
            return NULL;
        }
        return parse_postfix(p, blk);
    }

    if (t->kind == TOK_LPAREN) {
        p->i++;
        AstNode *inner = parse_paren_or_tuple(p, t);
        if (p->error || !inner) {
            return NULL;
        }
        return parse_postfix(p, inner);
    }

    if (t->kind == TOK_LBRACKET) {
        p->i++;
        AstList *els = NULL;
        if (!check(p, TOK_RBRACKET)) {
            do {
                AstNode *e = parse_expr(p);
                if (p->error || !e) {
                    return NULL;
                }
                els = ast_list_append(els, e);
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RBRACKET, "']' after array literal");
        if (p->error) {
            return NULL;
        }
        AstNode *arr = ast_alloc(NODE_ARRAY, tok_loc(p, t));
        if (!arr) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        AS_ARRAY(arr).elements = els;
        return parse_postfix(p, arr);
    }

    /* Lambda: `|| body` or `| params | [ -> ret ] body` (body = block or expr). */
    if (t->kind == TOK_PIPE_PIPE || t->kind == TOK_PIPE) {
        Token *t_start = t;
        AstList *lam_params = NULL;

        if (t->kind == TOK_PIPE_PIPE) {
            p->i++;
        } else {
            p->i++;
            lam_params = parse_lambda_params_until_pipe(p);
            if (p->error) {
                return NULL;
            }
            expect(p, TOK_PIPE, "closing '|' in lambda");
            if (p->error) {
                return NULL;
            }
        }

        AstNode *ret_ann = NULL;
        if (match(p, TOK_ARROW)) {
            ret_ann = parse_type(p);
            if (p->error || !ret_ann) {
                return NULL;
            }
        }

        AstNode *body;
        if (check(p, TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            body = parse_expr(p);
        }
        if (p->error || !body) {
            return NULL;
        }

        AstNode *lam = ast_alloc(NODE_LAMBDA, tok_loc(p, t_start));
        if (!lam) {
            parser_error(p, t_start, "out of memory");
            return NULL;
        }
        AS_LAMBDA(lam).params = lam_params;
        AS_LAMBDA(lam).ret_type = ret_ann;
        AS_LAMBDA(lam).body = body;
        return parse_postfix(p, lam);
    }

    parser_error(p, t, "expected expression");
    return NULL;
}

static AstNode *parse_expr_pratt(Parser *p, int min_prec) {
    Token *t = peek(p);
    if (!t) {
        parser_error(p, prev(p), "expected expression");
        return NULL;
    }

    AstNode *lhs;

    if (t->kind == TOK_BANG) {
        p->i++;
        AstNode *inner = parse_expr_pratt(p, PREC_UNARY + 1);
        if (p->error || !inner) {
            return NULL;
        }
        lhs = ast_alloc(NODE_UNARY, tok_loc(p, t));
        if (!lhs) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        AS_UNARY(lhs).op = UNOP_NOT;
        AS_UNARY(lhs).operand = inner;
    } else if (t->kind == TOK_MINUS) {
        p->i++;
        AstNode *inner = parse_expr_pratt(p, PREC_UNARY + 1);
        if (p->error || !inner) {
            return NULL;
        }
        lhs = ast_alloc(NODE_UNARY, tok_loc(p, t));
        if (!lhs) {
            parser_error(p, t, "out of memory");
            return NULL;
        }
        AS_UNARY(lhs).op = UNOP_NEG;
        AS_UNARY(lhs).operand = inner;
    } else {
        lhs = parse_atom(p);
    }

    if (p->error || !lhs) {
        return NULL;
    }

    for (;;) {
        t = peek(p);
        if (!t) {
            break;
        }

        int lprec = 0;
        BinOp bop = BINOP_ADD;
        bool is_assign = false;
        if (!pratt_infix(t->kind, &lprec, &bop, &is_assign)) {
            break;
        }
        if (lprec < min_prec) {
            break;
        }

        p->i++;

        AstNode *rhs;
        if (is_assign) {
            /* Right-associative */
            rhs = parse_expr_pratt(p, PREC_ASSIGN);
        } else {
            rhs = parse_expr_pratt(p, lprec + 1);
        }
        if (p->error || !rhs) {
            return NULL;
        }

        if (is_assign) {
            AstNode *a = ast_alloc(NODE_ASSIGN, lhs->loc);
            if (!a) {
                parser_error(p, peek(p), "out of memory");
                return NULL;
            }
            AS_ASSIGN(a).target = lhs;
            AS_ASSIGN(a).value = rhs;
            lhs = a;
        } else {
            lhs = make_binary(lhs, bop, rhs);
            if (!lhs) {
                parser_error(p, peek(p), "out of memory");
                return NULL;
            }
        }
    }

    return lhs;
}

/* Entry: minimum precedence 0 so top-level expr sees all infix operators. */
static AstNode *parse_expr(Parser *p) {
    return parse_expr_pratt(p, 0);
}

static AstNode *parse_fn_decl_rest(Parser *p, bool is_pub, Token *t_fn);

static AstNode *parse_struct_decl_rest(Parser *p, bool is_pub, Token *t_struct) {
    const char *name = expect_ident_copy(p);
    if (p->error || !name) {
        return NULL;
    }
    AstList *generics = parse_generic_params(p);
    if (p->error) {
        return NULL;
    }

    expect(p, TOK_LBRACE, "'{' in struct declaration");
    if (p->error) {
        return NULL;
    }

    AstList *fields = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token *tf = peek(p);
        bool f_pub = match(p, TOK_PUB);
        bool f_mut = match(p, TOK_MUT);
        const char *fname = expect_ident_copy(p);
        if (p->error || !fname) {
            return NULL;
        }
        expect(p, TOK_COLON, "':' in struct field");
        if (p->error) {
            return NULL;
        }
        AstNode *fty = parse_type(p);
        if (p->error || !fty) {
            return NULL;
        }
        expect(p, TOK_COMMA, "',' after struct field");
        if (p->error) {
            return NULL;
        }

        AstNode *sf = ast_alloc(NODE_STRUCT_FIELD, tok_loc(p, tf));
        if (!sf) {
            parser_error(p, tf, "out of memory");
            return NULL;
        }
        AS_STRUCT_FIELD(sf).name = fname;
        AS_STRUCT_FIELD(sf).type = fty;
        AS_STRUCT_FIELD(sf).is_pub = f_pub;
        AS_STRUCT_FIELD(sf).is_mut = f_mut;
        fields = ast_list_append(fields, sf);
    }

    expect(p, TOK_RBRACE, "'}' after struct body");
    if (p->error) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_STRUCT_DECL, tok_loc(p, t_struct));
    if (!n) {
        parser_error(p, t_struct, "out of memory");
        return NULL;
    }
    AS_STRUCT_DECL(n).name = name;
    AS_STRUCT_DECL(n).generic_params = generics;
    AS_STRUCT_DECL(n).fields = fields;
    AS_STRUCT_DECL(n).is_pub = is_pub;
    return n;
}

static AstNode *parse_const_decl_rest(Parser *p, bool is_pub, Token *t_const) {
    const char *name = expect_ident_copy(p);
    if (p->error || !name) {
        return NULL;
    }
    expect(p, TOK_COLON, "':' after const name");
    if (p->error) {
        return NULL;
    }
    AstNode *ty = parse_type(p);
    if (p->error || !ty) {
        return NULL;
    }
    expect(p, TOK_EQ, "'=' in const declaration");
    if (p->error) {
        return NULL;
    }
    AstNode *val = parse_expr(p);
    if (p->error || !val) {
        return NULL;
    }
    expect(p, TOK_SEMI, "';' after const initializer");
    if (p->error) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_CONST_DECL, tok_loc(p, t_const));
    if (!n) {
        parser_error(p, t_const, "out of memory");
        return NULL;
    }
    AS_CONST_DECL(n).name = name;
    AS_CONST_DECL(n).type = ty;
    AS_CONST_DECL(n).value = val;
    AS_CONST_DECL(n).is_pub = is_pub;
    return n;
}

static AstNode *parse_use_decl_rest(Parser *p, Token *t_use) {
    Token *t0;
    const char *part;
    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    AstList *items = NULL;
    bool glob = false;
    AstNode *n;

    if (!buf) {
        parser_error(p, t_use, "out of memory");
        return NULL;
    }
    buf[0] = '\0';

    t0 = peek(p);
    part = expect_ident_copy(p);
    if (p->error || !part) {
        free(buf);
        return NULL;
    }
    {
        size_t pn = strlen(part);
        while (len + pn + 1u > cap) {
            size_t ncap = cap * 2u;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                parser_error(p, t_use, "out of memory");
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, part, pn);
        len += pn;
        buf[len] = '\0';
    }

    while (match(p, TOK_PATH_SEP)) {
        if (match(p, TOK_STAR)) {
            glob = true;
            break;
        }
        if (match(p, TOK_LBRACE)) {
            if (match(p, TOK_STAR)) {
                glob = true;
            } else {
                do {
                    Token *ti = peek(p);
                    const char *it = expect_ident_copy(p);
                    AstNode *id;
                    if (p->error || !it) {
                        free(buf);
                        return NULL;
                    }
                    id = ast_alloc(NODE_IDENT, tok_loc(p, ti ? ti : t_use));
                    if (!id) {
                        free(buf);
                        parser_error(p, t_use, "out of memory");
                        return NULL;
                    }
                    AS_IDENT(id).name = it;
                    items = ast_list_append(items, id);
                    if (!items) {
                        free(buf);
                        parser_error(p, t_use, "out of memory");
                        return NULL;
                    }
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RBRACE, "'}' after use tree");
            if (p->error) {
                free(buf);
                return NULL;
            }
            break;
        }
        part = expect_ident_copy(p);
        if (p->error || !part) {
            free(buf);
            return NULL;
        }
        {
            size_t pn = strlen(part);
            while (len + 2u + pn + 1u > cap) {
                size_t ncap = cap * 2u;
                char *nb = (char *)realloc(buf, ncap);
                if (!nb) {
                    free(buf);
                    parser_error(p, t_use, "out of memory");
                    return NULL;
                }
                buf = nb;
                cap = ncap;
            }
            buf[len++] = ':';
            buf[len++] = ':';
            memcpy(buf + len, part, pn);
            len += pn;
            buf[len] = '\0';
        }
    }

    expect(p, TOK_SEMI, "';' after use declaration");
    if (p->error) {
        free(buf);
        return NULL;
    }

    n = ast_alloc(NODE_USE_DECL, tok_loc(p, t0 ? t0 : t_use));
    if (!n) {
        free(buf);
        parser_error(p, t_use, "out of memory");
        return NULL;
    }
    AS_USE_DECL(n).path = ast_copy_string(buf, len);
    AS_USE_DECL(n).items = items;
    AS_USE_DECL(n).glob = glob;
    free(buf);
    if (!AS_USE_DECL(n).path) {
        parser_error(p, t_use, "out of memory");
        return NULL;
    }
    return n;
}

static AstNode *parse_impl_decl_rest(Parser *p, bool is_pub, Token *t_impl) {
    AstList *gp_impl;
    AstList *gp_type = NULL;
    AstList *methods = NULL;
    const char *trait_name = NULL;
    const char *struct_name = NULL;
    const char *name_a;
    AstNode *n;

    (void)t_impl;

    gp_impl = parse_generic_params(p);
    if (p->error) {
        return NULL;
    }

    name_a = expect_ident_copy(p);
    if (p->error || !name_a) {
        return NULL;
    }

    if (match(p, TOK_FOR)) {
        trait_name = name_a;
        struct_name = expect_ident_copy(p);
        if (p->error || !struct_name) {
            return NULL;
        }
    } else {
        struct_name = name_a;
    }

    gp_type = parse_generic_params(p);
    if (p->error) {
        return NULL;
    }

    expect(p, TOK_LBRACE, "'{' to start impl body");
    if (p->error) {
        return NULL;
    }

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        bool mpub = match(p, TOK_PUB);
        Token *t_fn = peek(p);
        if (!t_fn || t_fn->kind != TOK_FN) {
            parser_error(p, t_fn ? t_fn : prev(p), "expected fn in impl block");
            break;
        }
        p->i++;
        {
            AstNode *f = parse_fn_decl_rest(p, mpub, t_fn);
            if (p->error || !f) {
                break;
            }
            methods = ast_list_append(methods, f);
        }
    }

    if (p->error) {
        return NULL;
    }

    expect(p, TOK_RBRACE, "'}' after impl body");
    if (p->error) {
        return NULL;
    }

    n = ast_alloc(NODE_IMPL_DECL, tok_loc(p, t_impl));
    if (!n) {
        parser_error(p, t_impl, "out of memory");
        return NULL;
    }
    AS_IMPL_DECL(n).struct_name = struct_name;
    AS_IMPL_DECL(n).trait_name = trait_name;
    AS_IMPL_DECL(n).generic_params = gp_impl;
    AS_IMPL_DECL(n).type_generic_params = gp_type;
    AS_IMPL_DECL(n).methods = methods;
    AS_IMPL_DECL(n).is_pub = is_pub;
    return n;
}

static AstNode *parse_fn_decl_rest(Parser *p, bool is_pub, Token *t_fn) {
    const char *name = expect_ident_copy(p);
    if (p->error || !name) {
        return NULL;
    }
    AstList *generics = parse_generic_params(p);
    if (p->error) {
        return NULL;
    }

    expect(p, TOK_LPAREN, "'(' after function name");
    if (p->error) {
        return NULL;
    }
    AstList *params = parse_param_list(p);
    if (p->error) {
        return NULL;
    }
    expect(p, TOK_RPAREN, "')' after parameters");
    if (p->error) {
        return NULL;
    }

    AstNode *ret_type = NULL;
    if (match(p, TOK_ARROW)) {
        ret_type = parse_type(p);
        if (p->error || !ret_type) {
            return NULL;
        }
    }

    AstNode *body = parse_block(p);
    if (p->error || !body) {
        return NULL;
    }

    AstNode *n = ast_alloc(NODE_FN_DECL, tok_loc(p, t_fn));
    if (!n) {
        parser_error(p, t_fn, "out of memory");
        return NULL;
    }
    AS_FN_DECL(n).name = name;
    AS_FN_DECL(n).generic_params = generics;
    AS_FN_DECL(n).params = params;
    AS_FN_DECL(n).ret_type = ret_type;
    AS_FN_DECL(n).body = body;
    AS_FN_DECL(n).is_pub = is_pub;
    return n;
}

static AstNode *parse_trait_fn_sig_after_kw(Parser *p, Token *t_fn) {
    const char *name = expect_ident_copy(p);
    AstList *generics;
    AstList *params;
    AstNode *ret_type = NULL;
    AstNode *n;

    if (p->error || !name) {
        return NULL;
    }
    generics = parse_generic_params(p);
    if (p->error) {
        return NULL;
    }
    if (generics) {
        parser_error(p, t_fn, "generic trait methods are not supported yet");
        return NULL;
    }

    expect(p, TOK_LPAREN, "'(' after function name");
    if (p->error) {
        return NULL;
    }
    params = parse_param_list(p);
    if (p->error) {
        return NULL;
    }
    expect(p, TOK_RPAREN, "')' after parameters");
    if (p->error) {
        return NULL;
    }

    if (match(p, TOK_ARROW)) {
        ret_type = parse_type(p);
        if (p->error || !ret_type) {
            return NULL;
        }
    }

    expect(p, TOK_SEMI, "';' after trait method signature");
    if (p->error) {
        return NULL;
    }

    n = ast_alloc(NODE_TRAIT_FN_SIG, tok_loc(p, t_fn));
    if (!n) {
        parser_error(p, t_fn, "out of memory");
        return NULL;
    }
    AS_TRAIT_FN_SIG(n).name = name;
    AS_TRAIT_FN_SIG(n).generic_params = NULL;
    AS_TRAIT_FN_SIG(n).params = params;
    AS_TRAIT_FN_SIG(n).ret_type = ret_type;
    return n;
}

static AstNode *parse_trait_decl_rest(Parser *p, bool is_pub, Token *t_trait) {
    const char *name = expect_ident_copy(p);
    AstList *generics;
    AstList *items = NULL;
    AstNode *n;

    if (p->error || !name) {
        return NULL;
    }

    generics = parse_generic_params(p);
    if (p->error) {
        return NULL;
    }
    if (generics) {
        parser_error(p, t_trait, "generic traits are not supported yet");
        return NULL;
    }

    if (match(p, TOK_COLON)) {
        parser_error(p, peek(p), "trait super-bounds are not supported yet");
        return NULL;
    }

    expect(p, TOK_LBRACE, "'{' to start trait body");
    if (p->error) {
        return NULL;
    }

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Token *tf = peek(p);
        if (!tf || tf->kind != TOK_FN) {
            parser_error(p, tf ? tf : prev(p), "expected fn in trait body");
            break;
        }
        p->i++;
        {
            AstNode *it = parse_trait_fn_sig_after_kw(p, tf);
            if (p->error || !it) {
                break;
            }
            items = ast_list_append(items, it);
            if (!items) {
                parser_error(p, tf, "out of memory");
                break;
            }
        }
    }

    if (p->error) {
        return NULL;
    }

    expect(p, TOK_RBRACE, "'}' after trait body");
    if (p->error) {
        return NULL;
    }

    n = ast_alloc(NODE_TRAIT_DECL, tok_loc(p, t_trait));
    if (!n) {
        parser_error(p, t_trait, "out of memory");
        return NULL;
    }
    AS_TRAIT_DECL(n).name = name;
    AS_TRAIT_DECL(n).generic_params = NULL;
    AS_TRAIT_DECL(n).super_traits = NULL;
    AS_TRAIT_DECL(n).items = items;
    AS_TRAIT_DECL(n).is_pub = is_pub;
    return n;
}

static AstNode *parse_top_level_decl(Parser *p) {
    bool is_pub = false;
    if (match(p, TOK_PUB)) {
        is_pub = true;
    }

    Token *t = peek(p);
    if (!t) {
        parser_error(p, prev(p), "expected declaration");
        return NULL;
    }

    if (t->kind == TOK_FN) {
        p->i++;
        return parse_fn_decl_rest(p, is_pub, t);
    }
    if (t->kind == TOK_STRUCT) {
        p->i++;
        return parse_struct_decl_rest(p, is_pub, t);
    }

    if (t->kind == TOK_TRAIT) {
        p->i++;
        return parse_trait_decl_rest(p, is_pub, t);
    }

    if (t->kind == TOK_IMPL) {
        p->i++;
        return parse_impl_decl_rest(p, is_pub, t);
    }

    if (t->kind == TOK_USE) {
        p->i++;
        if (is_pub) {
            parser_error(p, t, "`pub use` is not supported yet");
            return NULL;
        }
        return parse_use_decl_rest(p, t);
    }

    if (t->kind == TOK_CONST) {
        p->i++;
        return parse_const_decl_rest(p, is_pub, t);
    }

    if (is_pub) {
        parser_error(p, t, "expected fn, struct, trait, or impl after pub");
        return NULL;
    }

    parser_error(p, t, "unexpected token at top level");
    return NULL;
}

static AstNode *parse_program(Parser *p) {
    AstList *decls = NULL;
    while (!check(p, TOK_EOF)) {
        Token *t = peek(p);
        if (t && t->kind == TOK_ERROR) {
            parser_error(p, t, "lex error");
            return NULL;
        }
        AstNode *d = parse_top_level_decl(p);
        if (p->error || !d) {
            return NULL;
        }
        decls = ast_list_append(decls, d);
    }

    Token *t0 = p->tokens.count > 0 ? &p->tokens.items[0] : peek(p);
    SrcLoc ploc;
    if (t0) {
        ploc = tok_loc(p, t0);
    } else {
        ploc.file = p->filename;
        ploc.line = 1;
        ploc.col = 1;
    }

    AstNode *prog = ast_alloc(NODE_PROGRAM, ploc);
    if (!prog) {
        parser_error(p, peek(p), "out of memory");
        return NULL;
    }
    AS_PROGRAM(prog).decls = decls;
    return prog;
}

ParseResult parse_tokens(TokenArray tokens, const char *filename) {
    /* Copy parser error text out of Parser.errbuf before Parser stack frame is destroyed. */
    static char err_storage[256];

    ParseResult r;
    r.program = NULL;
    r.error_message = NULL;
    r.error_line = 0;
    r.error_col = 0;

    Parser p = {0};
    p.tokens = tokens;
    p.i = 0;
    p.filename = filename ? filename : "<input>";
    p.error = NULL;
    p.err_line = 0;
    p.err_col = 0;

    AstNode *prog = parse_program(&p);
    if (p.error) {
        strncpy(err_storage, p.error, sizeof(err_storage) - 1);
        err_storage[sizeof(err_storage) - 1] = '\0';
        r.error_message = err_storage;
        r.error_line = p.err_line;
        r.error_col = p.err_col;
        return r;
    }

    r.program = prog;
    return r;
}
