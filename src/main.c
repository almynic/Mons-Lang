/*
 * Driver: lex → parse → typecheck; optional AST print and eval smoke tests.
 *
 * With no arguments: embedded sample, print AST, typecheck, evaluate several pub fns.
 * With a file path: read source, typecheck only (no AST dump, no eval) — for CI and `make test`.
 * With -i / --repl: interactive REPL.
 */
#include "ast.h"
#include "eval.h"
#include "lexer.h"
#include "parser.h"
#include "types.h"
#include "repl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *embedded_sample =
    "struct Point {\n"
    "    x: int,\n"
    "    y: int,\n"
    "}\n"
    "fn area(self: Point) -> int {\n"
    "    self.x * self.y\n"
    "}\n"
    "const answer: int = 40 + 2;\n"
    "pub fn add(a: int, b: int) -> bool {\n"
    "    // comment\n"
    "    let x = 1.5f;\n"
    "    let y = 2.0;\n"
    "    a + b * 2 == 0 && true || false\n"
    "}\n"
    "pub fn mid() -> int {\n"
    "    let a = [1, 2, 3];\n"
    "    a[1]\n"
    "}\n"
    "pub fn sum_arr() -> int {\n"
    "    let a = [1, 2, 3];\n"
    "    let s = 0;\n"
    "    for x in a {\n"
    "        s = s + x;\n"
    "    };\n"
    "    s\n"
    "}\n"
    "pub fn tup() -> int {\n"
    "    let t = (10, 20);\n"
    "    t[1]\n"
    "}\n"
    "pub fn use_point() -> int {\n"
    "    let p = Point { x: 3, y: 4, };\n"
    "    p.x + p.y\n"
    "}\n"
    "pub fn rect_area() -> int {\n"
    "    let p = Point { x: 3, y: 4, };\n"
    "    p.area()\n"
    "}\n"
    "pub fn life() -> int {\n"
    "    answer\n"
    "}\n"
    "pub fn shifted() -> int {\n"
    "    let p = Point { x: 1, y: 2, };\n"
    "    let q = Point { x: 10, ..p, };\n"
    "    q.x + q.y\n"
    "}\n";

static char *read_file_contents(const char *path) {
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;

    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1u, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-i|--repl] [file.mons]\n"
            "  no args      run embedded demo (AST + typecheck + eval)\n"
            "  -i, --repl   interactive read-eval-print loop\n"
            "  file.mons    lex, parse, and typecheck only\n",
            argv0);
}

static int run_demo_evals(AstNode *program) {
    Value call_args[2];
    EvalResult er;

    call_args[0].kind = VAL_INT;
    call_args[0].as.i = 0;
    call_args[1].kind = VAL_INT;
    call_args[1].as.i = 0;

    er = eval_call_by_name(program, "add", call_args, 2);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval add(0,0) = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "mid", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval mid() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "sum_arr", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval sum_arr() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "tup", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval tup() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "use_point", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval use_point() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "rect_area", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval rect_area() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "life", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval life() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "shifted", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval shifted() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    return 0;
}

int main(int argc, char **argv) {
    char *file_buf = NULL;
    const char *source;
    const char *fname;
    int demo_mode;
    TokenArray tokens;
    ParseResult pr;
    TypeCheckResult tc;
    int rc = 1;

    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (argc >= 2 && (strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "--repl") == 0)) {
        return repl_run();
    }

    if (argc >= 2) {
        file_buf = read_file_contents(argv[1]);
        if (!file_buf) {
            fprintf(stderr, "error: could not read \"%s\"\n", argv[1]);
            return 1;
        }
        source = file_buf;
        fname = argv[1];
        demo_mode = 0;
    } else {
        source = embedded_sample;
        fname = "<sample>";
        demo_mode = 1;
    }

    tokens = lexer_tokenize(source, fname);
    if (!tokens.items) {
        fprintf(stderr, "error: lexer failed to allocate token buffer\n");
        goto done;
    }

    pr = parse_tokens(tokens, fname);
    if (pr.error_message) {
        fprintf(stderr, "parse error at %u:%u: %s\n",
                pr.error_line,
                pr.error_col,
                pr.error_message);
        goto done_tokens;
    }

    if (demo_mode) {
        ast_print(pr.program, 0);
    }

    tc = type_check_program(pr.program);
    if (!tc.ok) {
        fprintf(stderr, "type error at %u:%u: %s\n",
                tc.error_line,
                tc.error_col,
                tc.error_message);
        goto done_tokens;
    }
    printf("type check: ok\n");

    if (demo_mode) {
        if (run_demo_evals(pr.program) != 0) {
            goto done_tokens;
        }
    }

    rc = 0;

done_tokens:
    token_array_free(&tokens);
    ast_free_all();
done:
    free(file_buf);
    return rc;
}
