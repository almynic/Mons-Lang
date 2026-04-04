/*
 * Demo driver: tokenize → parse → print AST → type_check_program → eval_call_by_name
 * for a few public functions.
 */
#include "ast.h"
#include "eval.h"
#include "lexer.h"
#include "parser.h"
#include "types.h"

#include <stdio.h>

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

int main(void) {
    TokenArray tokens = lexer_tokenize(embedded_sample, "<sample>");
    if (!tokens.items) {
        fprintf(stderr, "error: lexer failed to allocate token buffer\n");
        return 1;
    }

    ParseResult pr = parse_tokens(tokens, "<sample>");
    if (pr.error_message) {
        fprintf(stderr, "parse error at %u:%u: %s\n",
                pr.error_line,
                pr.error_col,
                pr.error_message);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    ast_print(pr.program, 0);

    TypeCheckResult tc = type_check_program(pr.program);
    if (!tc.ok) {
        fprintf(stderr, "type error at %u:%u: %s\n",
                tc.error_line,
                tc.error_col,
                tc.error_message);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }
    printf("type check: ok\n");

    if (run_demo_evals(pr.program) != 0) {
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    token_array_free(&tokens);
    ast_free_all();
    return 0;
}
