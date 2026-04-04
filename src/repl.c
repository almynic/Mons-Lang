/*
 * REPL: grow a session buffer, re-lex/parse/typecheck the full program each turn.
 * Lines that are not valid top-level declarations are wrapped in `fn __monsrepl_N() { ... }`
 * and that function is evaluated so the last expression is printed.
 */
#include "repl.h"

#include "ast.h"
#include "eval.h"
#include "lexer.h"
#include "parser.h"
#include "types.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static int line_brace_delta(const char *s) {
    int d = 0;
    for (; *s; s++) {
        if (*s == '{') {
            d++;
        } else if (*s == '}') {
            d--;
        }
    }
    return d;
}

/* Read until `{`/`}` balance hits zero; skip leading blank "paragraphs". */
static char *repl_read_paragraph(void) {
    char *acc = NULL;
    size_t acc_len = 0;
    int depth = 0;
    int line_cont = 0;
    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        printf((depth > 0 || line_cont) ? "... " : "mons> ");
        fflush(stdout);

        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            free(line);
            free(acc);
            return NULL;
        }

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }

        const char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (depth == 0 && acc_len == 0 && *p == '\0') {
            continue;
        }

        size_t line_len = strlen(line);
        char *nacc = (char *)realloc(acc, acc_len + line_len + 2u);
        if (!nacc) {
            free(line);
            free(acc);
            return NULL;
        }
        acc = nacc;
        if (acc_len > 0) {
            acc[acc_len++] = '\n';
        }
        memcpy(acc + acc_len, line, line_len + 1u);
        acc_len += line_len;

        depth += line_brace_delta(line);
        if (depth < 0) {
            depth = 0;
        }

        if (acc_len > 0 && acc[acc_len - 1u] == '\\') {
            acc[acc_len - 1u] = '\0';
            acc_len--;
            line_cont = 1;
            continue;
        }
        line_cont = 0;

        if (depth == 0) {
            free(line);
            return acc;
        }
    }
}

static char *join_session(const char *session, const char *pending) {
    if (!session || session[0] == '\0') {
        return strdup(pending);
    }
    size_t a = strlen(session);
    size_t b = strlen(pending);
    char *o = (char *)malloc(a + b + 2u);
    if (!o) {
        return NULL;
    }
    memcpy(o, session, a);
    o[a] = '\n';
    memcpy(o + a + 1u, pending, b + 1u);
    return o;
}

static char *build_wrapped(const char *session, const char *pending, const char *fn) {
    int ret;
    char *buf = NULL;
    size_t sz = strlen(pending) + strlen(fn) + 128u;
    if (session && session[0] != '\0') {
        sz += strlen(session) + 1u;
    }
    buf = (char *)malloc(sz);
    if (!buf) {
        return NULL;
    }
    if (session && session[0] != '\0') {
        ret = snprintf(buf, sz, "%s\nfn %s() {\n%s\n}\n", session, fn, pending);
    } else {
        ret = snprintf(buf, sz, "fn %s() {\n%s\n}\n", fn, pending);
    }
    if (ret < 0 || (size_t)ret >= sz) {
        free(buf);
        return NULL;
    }
    return buf;
}

static char *trim_inplace(char *s) {
    char *a = s;
    while (*a && isspace((unsigned char)*a)) {
        a++;
    }
    if (a != s) {
        memmove(s, a, strlen(a) + 1u);
    }
    char *b = s + strlen(s);
    while (b > s && isspace((unsigned char)b[-1])) {
        *--b = '\0';
    }
    return s;
}

int repl_run(void) {
    char *session = strdup("");
    unsigned repl_seq = 0;

    if (!session) {
        fprintf(stderr, "repl: out of memory\n");
        return 1;
    }

    printf("Mons REPL — accumulated program is re-typechecked each line.\n");
    printf("  Top-level: struct, fn, const, pub ...\n");
    printf("  Other input is wrapped in a temporary fn and evaluated.\n");
    printf("  Multi-line: unbalanced { } until closing brace, or \\ at end of line.\n");
    printf("  Commands: :help  :clear  :quit  (or EOF)\n\n");

    for (;;) {
        char *pending = repl_read_paragraph();
        if (!pending) {
            printf("\n");
            break;
        }

        trim_inplace(pending);
        if (pending[0] == '\0') {
            free(pending);
            continue;
        }

        if (strcmp(pending, ":q") == 0 || strcmp(pending, ":quit") == 0) {
            free(pending);
            break;
        }
        if (strcmp(pending, ":help") == 0) {
            printf("Enter Mons declarations or expression/statements.\n");
            printf("Unbalanced { } continues on the next line ('...' prompt).\n");
            free(pending);
            continue;
        }
        if (strcmp(pending, ":clear") == 0) {
            free(session);
            session = strdup("");
            repl_seq = 0;
            if (!session) {
                fprintf(stderr, "repl: out of memory\n");
                free(pending);
                return 1;
            }
            printf("(session cleared)\n");
            free(pending);
            continue;
        }

        char fnbuf[48];
        char *trial = join_session(session, pending);
        if (!trial) {
            fprintf(stderr, "repl: out of memory\n");
            free(pending);
            free(session);
            return 1;
        }

        ast_free_all();
        TokenArray tokens = lexer_tokenize(trial, "<repl>");
        if (!tokens.items) {
            fprintf(stderr, "repl: lexer failed\n");
            free(trial);
            free(pending);
            free(session);
            return 1;
        }

        ParseResult pr = parse_tokens(tokens, "<repl>");
        int wrapped = 0;
        char *commit_src = NULL;

        if (pr.error_message) {
            token_array_free(&tokens);
            ast_free_all();

            (void)snprintf(fnbuf, sizeof(fnbuf), "__monsrepl_%u", repl_seq);
            char *w = build_wrapped(session, pending, fnbuf);
            if (!w) {
                fprintf(stderr, "repl: out of memory\n");
                free(trial);
                free(pending);
                free(session);
                return 1;
            }

            tokens = lexer_tokenize(w, "<repl>");
            if (!tokens.items) {
                fprintf(stderr, "repl: lexer failed\n");
                free(w);
                free(trial);
                free(pending);
                free(session);
                return 1;
            }
            pr = parse_tokens(tokens, "<repl>");
            if (pr.error_message) {
                fprintf(stderr, "parse error at %u:%u: %s\n",
                        pr.error_line,
                        pr.error_col,
                        pr.error_message);
                token_array_free(&tokens);
                ast_free_all();
                free(w);
                free(trial);
                free(pending);
                continue;
            }
            wrapped = 1;
            commit_src = w;
            free(trial);
            trial = NULL;
        } else {
            commit_src = trial;
            trial = NULL;
        }

        TypeCheckResult tc = type_check_program(pr.program);
        if (!tc.ok) {
            fprintf(stderr, "type error at %u:%u: %s\n",
                    tc.error_line,
                    tc.error_col,
                    tc.error_message);
            token_array_free(&tokens);
            ast_free_all();
            free(commit_src);
            free(pending);
            continue;
        }

        free(session);
        session = strdup(commit_src);
        free(commit_src);
        if (!session) {
            fprintf(stderr, "repl: out of memory\n");
            token_array_free(&tokens);
            ast_free_all();
            free(pending);
            return 1;
        }

        if (wrapped) {
            EvalResult er = eval_call_by_name(pr.program, fnbuf, NULL, 0);
            if (!er.ok) {
                fprintf(stderr, "eval: %s\n", er.error_message ? er.error_message : "unknown");
            } else {
                value_fprint(stdout, &er.result);
                printf("\n");
                value_release(&er.result);
            }
            repl_seq++;
        } else {
            printf("ok\n");
        }

        token_array_free(&tokens);
        ast_free_all();
        free(pending);
    }

    free(session);
    return 0;
}
