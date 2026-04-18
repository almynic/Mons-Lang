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
#include "compile.h"
#include "vm.h"
#include "reflection.h"

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
    "}\n"
    "pub fn closure_add() -> int {\n"
    "    let add = |a: int, b: int| a + b;\n"
    "    add(2, 3)\n"
    "}\n"
    "pub fn closure_capture() -> int {\n"
    "    let x = 10;\n"
    "    let g = || x + 1;\n"
    "    g()\n"
    "}\n"
    "pub fn closure_hof() -> int {\n"
    "    let mk = |base: int| |y: int| base + y;\n"
    "    let add5 = mk(5);\n"
    "    add5(7)\n"
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

/* Concatenate two sources for a single parse (stdlib prelude + user file). */
static char *combine_file_contents(const char *first_path, const char *second_path) {
    char *a = read_file_contents(first_path);
    char *b = read_file_contents(second_path);
    char *out;
    size_t la;
    size_t lb;

    if (!a || !b) {
        free(a);
        free(b);
        return NULL;
    }
    la = strlen(a);
    lb = strlen(b);
    out = (char *)malloc(la + 2u + lb + 1u);
    if (!out) {
        free(a);
        free(b);
        return NULL;
    }
    memcpy(out, a, la);
    out[la] = '\n';
    out[la + 1u] = '\n';
    memcpy(out + la + 2u, b, lb);
    out[la + 2u + lb] = '\0';
    free(a);
    free(b);
    return out;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-i|--repl|--vm-test|--reflect] [file.mons]\n"
            "  no args          run embedded demo (AST + typecheck + eval)\n"
            "  -i, --repl       interactive read-eval-print loop\n"
            "  --vm-test        typecheck stdlib+vm_smoke, run bytecode VM smokes\n"
            "  --reflect FILE   print public API summary (after typecheck)\n"
            "  file.mons        lex, parse, and typecheck only\n",
            argv0);
}

static int vm_run_bc_entry(AstNode *program, CompileProgramResult *bc, const char *fn_name, VmResult *out_vr) {
    int idx = bc_fn_index(program, fn_name);

    if (idx < 0) {
        fprintf(stderr, "compile error: no function \"%s\"\n", fn_name);
        return 1;
    }

    *out_vr = vm_run_program(
        bc->prog.chunks,
        bc->prog.nchunks,
        (size_t)idx,
        NULL,
        0,
        bc->prog.structs,
        bc->prog.nstructs,
        (const char *const *)bc->prog.symbol_pool,
        bc->prog.nsymbols);
    if (!out_vr->ok) {
        fprintf(stderr, "vm error (%s): %s\n",
                fn_name,
                out_vr->error_message ? out_vr->error_message : "unknown");
        return 1;
    }
    return 0;
}

static float vm_absf(float x) {
    return x < 0.0f ? -x : x;
}

static int vm_expect_int(VmResult *vr, const char *fn_name, int64_t expect_i) {
    if (vr->result.kind != VAL_INT || vr->result.as.i != expect_i) {
        fprintf(stderr, "vm smoke \"%s\": expected %lld, got ",
                fn_name,
                (long long)expect_i);
        value_fprint(stderr, &vr->result);
        fprintf(stderr, "\n");
        value_release(&vr->result);
        return 1;
    }
    printf("bytecode %s() = ", fn_name);
    value_fprint(stdout, &vr->result);
    printf("\n");
    value_release(&vr->result);
    return 0;
}

static int vm_expect_float(VmResult *vr, const char *fn_name, float expect_f) {
    if (vr->result.kind != VAL_FLOAT) {
        fprintf(stderr, "vm smoke \"%s\": expected float %f, got ", fn_name, (double)expect_f);
        value_fprint(stderr, &vr->result);
        fprintf(stderr, "\n");
        value_release(&vr->result);
        return 1;
    }
    if (vm_absf(vr->result.as.f - expect_f) > 1e-4f) {
        fprintf(stderr, "vm smoke \"%s\": expected float ~%f, got %f\n",
                fn_name, (double)expect_f, (double)vr->result.as.f);
        value_release(&vr->result);
        return 1;
    }
    printf("bytecode %s() = ", fn_name);
    value_fprint(stdout, &vr->result);
    printf("\n");
    value_release(&vr->result);
    return 0;
}

static int vm_expect_double(VmResult *vr, const char *fn_name, double expect_d) {
    if (vr->result.kind != VAL_DOUBLE) {
        fprintf(stderr, "vm smoke \"%s\": expected double %f, got ", fn_name, expect_d);
        value_fprint(stderr, &vr->result);
        fprintf(stderr, "\n");
        value_release(&vr->result);
        return 1;
    }
    {
        double g = vr->result.as.d;
        double d = g > expect_d ? g - expect_d : expect_d - g;
        if (d > 1e-10) {
            fprintf(stderr, "vm smoke \"%s\": expected double ~%f, got %f\n",
                    fn_name, expect_d, g);
            value_release(&vr->result);
            return 1;
        }
    }
    printf("bytecode %s() = ", fn_name);
    value_fprint(stdout, &vr->result);
    printf("\n");
    value_release(&vr->result);
    return 0;
}

static int vm_run_smoke_table(AstNode *program, CompileProgramResult *bc) {
    static const struct {
        const char *name;
        char        kind; /* 'i' int, 'f' float, 'd' double */
        int64_t     i;
        float       f;
        double      d;
    } rows[] = {
        {"smoke", 'i', 14, 0.0f, 0.0},
        {"smoke_hof", 'i', 12, 0.0f, 0.0},
        {"smoke_hof_capture", 'i', 17, 0.0f, 0.0},
        {"smoke_nested", 'i', 13, 0.0f, 0.0},
        {"smoke_ctrl", 'i', 29, 0.0f, 0.0},
        {"smoke_assign", 'i', 5, 0.0f, 0.0},
        {"smoke_assign_capture", 'i', 8, 0.0f, 0.0},
        {"smoke_block_expr", 'i', 22, 0.0f, 0.0},
        {"smoke_struct", 'i', 42, 0.0f, 0.0},
        {"smoke_struct_field_named", 'i', 12, 0.0f, 0.0},
        {"smoke_struct_spread", 'i', 14, 0.0f, 0.0},
        {"smoke_for_sum", 'i', 10, 0.0f, 0.0},
        {"smoke_array_index", 'i', 40, 0.0f, 0.0},
        {"smoke_tuple_index", 'i', 25, 0.0f, 0.0},
        {"smoke_float_sum", 'f', 0, 5.0f, 0.0},
        {"smoke_double_half", 'd', 0, 0.0f, 5.0},
        {"smoke_for_tuple_sum", 'i', 6, 0.0f, 0.0},
    };
    size_t i;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        VmResult vr;
        if (vm_run_bc_entry(program, bc, rows[i].name, &vr) != 0) {
            return 1;
        }
        if (rows[i].kind == 'i') {
            if (vm_expect_int(&vr, rows[i].name, rows[i].i) != 0) {
                return 1;
            }
        } else if (rows[i].kind == 'f') {
            if (vm_expect_float(&vr, rows[i].name, rows[i].f) != 0) {
                return 1;
            }
        } else if (rows[i].kind == 'd') {
            if (vm_expect_double(&vr, rows[i].name, rows[i].d) != 0) {
                return 1;
            }
        } else {
            value_release(&vr.result);
            return 1;
        }
    }
    return 0;
}

static int run_vm_smoke_test(void) {
    const char *prelude_path = "stdlib/core.mons";
    const char *user_path = "tests/vm_smoke.mons";
    const char *virt = "stdlib/core.mons+tests/vm_smoke.mons";
    char *src = combine_file_contents(prelude_path, user_path);
    TokenArray tokens;
    ParseResult pr;
    TypeCheckResult tc;
    CompileProgramResult bc;

    if (!src) {
        fprintf(stderr, "error: could not read \"%s\" and/or \"%s\"\n", prelude_path, user_path);
        return 1;
    }

    tokens = lexer_tokenize(src, virt);
    if (!tokens.items) {
        free(src);
        fprintf(stderr, "error: lexer failed\n");
        return 1;
    }

    pr = parse_tokens(tokens, virt);
    free(src);
    if (pr.error_message) {
        fprintf(stderr, "parse error at %u:%u: %s\n",
                pr.error_line,
                pr.error_col,
                pr.error_message);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    tc = type_check_program(pr.program);
    if (!tc.ok) {
        fprintf(stderr, "type error at %u:%u: %s\n",
                tc.error_line,
                tc.error_col,
                tc.error_message);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    bc = compile_program_bc(pr.program);
    if (!bc.ok) {
        fprintf(stderr, "compile error: %s\n", bc.error_message ? bc.error_message : "unknown");
        compile_program_result_free(&bc);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    if (vm_run_smoke_table(pr.program, &bc) != 0) {
        compile_program_result_free(&bc);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    compile_program_result_free(&bc);
    token_array_free(&tokens);
    ast_free_all();
    return 0;
}

static int run_reflect(const char *path) {
    char *src = read_file_contents(path);
    TokenArray tokens;
    ParseResult pr;
    TypeCheckResult tc;

    if (!src) {
        fprintf(stderr, "error: could not read \"%s\"\n", path);
        return 1;
    }

    tokens = lexer_tokenize(src, path);
    if (!tokens.items) {
        free(src);
        fprintf(stderr, "error: lexer failed\n");
        return 1;
    }

    pr = parse_tokens(tokens, path);
    free(src);
    if (pr.error_message) {
        fprintf(stderr, "parse error at %u:%u: %s\n",
                pr.error_line,
                pr.error_col,
                pr.error_message);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    tc = type_check_program(pr.program);
    if (!tc.ok) {
        fprintf(stderr, "type error at %u:%u: %s\n",
                tc.error_line,
                tc.error_col,
                tc.error_message);
        token_array_free(&tokens);
        ast_free_all();
        return 1;
    }

    reflection_fprint_program(stdout, pr.program);
    token_array_free(&tokens);
    ast_free_all();
    return 0;
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

    er = eval_call_by_name(program, "closure_add", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval closure_add() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "closure_capture", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval closure_capture() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "closure_hof", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval closure_hof() = ");
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

    if (argc >= 2 && strcmp(argv[1], "--vm-test") == 0) {
        return run_vm_smoke_test();
    }

    if (argc >= 2 && strcmp(argv[1], "--reflect") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: --reflect requires a file path\n");
            usage(argv[0]);
            return 1;
        }
        return run_reflect(argv[2]);
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
