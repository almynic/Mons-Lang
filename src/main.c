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
#include <dirent.h>

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
    "}\n"
    "pub fn try_catch_smoke() -> int {\n"
    "    let mut r = 0;\n"
    "    try {\n"
    "        throw 7;\n"
    "    } catch (e: int) {\n"
    "        r = e + 2;\n"
    "    };\n"
    "    r\n"
    "}\n"
    "pub fn try_finally_overrides_return() -> int {\n"
    "    try {\n"
    "        return 2;\n"
    "    } catch (e: int) { 0 }\n"
    "    finally {\n"
    "        return 9;\n"
    "    };\n"
    "    0\n"
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

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrVec;

static void strvec_free(StrVec *v) {
    size_t i;
    if (!v || !v->items) {
        return;
    }
    for (i = 0; i < v->len; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static int strvec_push_copy(StrVec *v, const char *s) {
    char *cp;
    if (!v || !s) {
        return 0;
    }
    if (v->len >= v->cap) {
        size_t ncap = v->cap == 0 ? 8u : v->cap * 2u;
        char **ni = (char **)realloc(v->items, ncap * sizeof(char *));
        if (!ni) {
            return 0;
        }
        v->items = ni;
        v->cap = ncap;
    }
    cp = (char *)malloc(strlen(s) + 1u);
    if (!cp) {
        return 0;
    }
    strcpy(cp, s);
    v->items[v->len++] = cp;
    return 1;
}

static int strvec_contains(const StrVec *v, const char *s) {
    size_t i;
    if (!v || !s) {
        return 0;
    }
    for (i = 0; i < v->len; i++) {
        if (strcmp(v->items[i], s) == 0) {
            return 1;
        }
    }
    return 0;
}

static void strvec_pop(StrVec *v) {
    if (!v || v->len == 0) {
        return;
    }
    free(v->items[v->len - 1u]);
    v->len--;
}

static const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? (s + 1) : path;
}

static char *path_dirname_copy(const char *path) {
    const char *s;
    size_t n;
    char *d;
    if (!path) {
        return NULL;
    }
    s = strrchr(path, '/');
    if (!s) {
        d = (char *)malloc(2u);
        if (!d) {
            return NULL;
        }
        d[0] = '.';
        d[1] = '\0';
        return d;
    }
    n = (size_t)(s - path);
    d = (char *)malloc(n + 1u);
    if (!d) {
        return NULL;
    }
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static char *path_join2(const char *a, const char *b) {
    size_t na;
    size_t nb;
    char *out;
    if (!a || !b) {
        return NULL;
    }
    na = strlen(a);
    nb = strlen(b);
    out = (char *)malloc(na + 1u + nb + 1u);
    if (!out) {
        return NULL;
    }
    memcpy(out, a, na);
    out[na] = '/';
    memcpy(out + na + 1u, b, nb);
    out[na + 1u + nb] = '\0';
    return out;
}

static char *module_path_to_file(const char *module_path) {
    size_t i;
    size_t j = 0;
    size_t n;
    char *f;
    if (!module_path) {
        return NULL;
    }
    n = strlen(module_path);
    f = (char *)malloc(n + 6u);
    if (!f) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (module_path[i] == ':' && i + 1u < n && module_path[i + 1u] == ':') {
            f[j++] = '/';
            i++;
        } else {
            f[j++] = module_path[i];
        }
    }
    strcpy(f + j, ".mons");
    return f;
}

static int file_exists(const char *path) {
    FILE *fp;
    if (!path) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static int append_text(char **dst, size_t *len, const char *src) {
    size_t sn = strlen(src);
    char *nd = (char *)realloc(*dst, *len + sn + 1u);
    if (!nd) {
        return 0;
    }
    *dst = nd;
    memcpy(*dst + *len, src, sn);
    *len += sn;
    (*dst)[*len] = '\0';
    return 1;
}

static int is_ident_start_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_char(char c) {
    return is_ident_start_char(c) || (c >= '0' && c <= '9');
}

static void skip_ws(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *pp = p;
}

static int append_module_item(StrVec *v, const char *s, size_t n) {
    char tmp[256];
    if (n == 0 || n >= sizeof(tmp)) {
        return 0;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    return strvec_push_copy(v, tmp);
}

/* Returns: 0=not use line, 1=parsed use, -1=invalid/unsupported syntax. */
static int parse_use_decl_from_line(const char *line, char *base_out, size_t cap, StrVec *items_out, int *glob_out) {
    const char *p = line;
    size_t bn = 0;
    int saw_part = 0;

    if (!base_out || cap == 0 || !items_out || !glob_out) {
        return -1;
    }
    *glob_out = 0;
    base_out[0] = '\0';
    skip_ws(&p);
    if (strncmp(p, "use", 3) != 0 || !(*((p + 3)) == ' ' || *((p + 3)) == '\t')) {
        return 0;
    }
    p += 3;
    skip_ws(&p);

    while (1) {
        const char *s = p;
        size_t n = 0;
        if (!is_ident_start_char(*p)) {
            return -1;
        }
        while (is_ident_char(*p)) {
            p++;
            n++;
        }
        if (bn != 0) {
            if (bn + 2u >= cap) {
                return -1;
            }
            base_out[bn++] = ':';
            base_out[bn++] = ':';
        }
        if (bn + n + 1u >= cap) {
            return -1;
        }
        memcpy(base_out + bn, s, n);
        bn += n;
        base_out[bn] = '\0';
        saw_part = 1;
        if (p[0] == ':' && p[1] == ':') {
            p += 2;
            if (*p == '*') {
                p++;
                *glob_out = 1;
                break;
            }
            if (*p == '{') {
                p++;
                skip_ws(&p);
                if (*p == '*') {
                    p++;
                    skip_ws(&p);
                    if (*p != '}') {
                        return -1;
                    }
                    p++;
                    *glob_out = 1;
                    break;
                }
                while (1) {
                    const char *is = p;
                    size_t in = 0;
                    if (!is_ident_start_char(*p)) {
                        return -1;
                    }
                    while (is_ident_char(*p)) {
                        p++;
                        in++;
                    }
                    if (!append_module_item(items_out, is, in)) {
                        return -1;
                    }
                    skip_ws(&p);
                    if (*p == ',') {
                        p++;
                        skip_ws(&p);
                        continue;
                    }
                    if (*p == '}') {
                        p++;
                        break;
                    }
                    return -1;
                }
                break;
            }
            continue;
        }
        break;
    }

    if (!saw_part) {
        return -1;
    }
    skip_ws(&p);
    if (*p != ';') {
        return -1;
    }
    p++;
    skip_ws(&p);
    if (*p != '\0' && *p != '\r' && *p != '\n') {
        return -1;
    }
    if (*glob_out && items_out->len > 0) {
        return -1;
    }
    return 1;
}

static char *module_path_to_dir(const char *module_path) {
    size_t i;
    size_t j = 0;
    size_t n;
    char *d;
    if (!module_path) {
        return NULL;
    }
    n = strlen(module_path);
    d = (char *)malloc(n + 1u);
    if (!d) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (module_path[i] == ':' && i + 1u < n && module_path[i + 1u] == ':') {
            d[j++] = '/';
            i++;
        } else {
            d[j++] = module_path[i];
        }
    }
    d[j] = '\0';
    return d;
}

static int mod_name_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static int collect_glob_modules(const char *dir_path, const char *base_module, StrVec *out_mods) {
    DIR *dp;
    struct dirent *de;
    int found = 0;
    dp = opendir(dir_path);
    if (!dp) {
        return 0;
    }
    while ((de = readdir(dp)) != NULL) {
        const char *nm = de->d_name;
        size_t ln = strlen(nm);
        char full[512];
        if (ln < 6 || strcmp(nm + ln - 5u, ".mons") != 0) {
            continue;
        }
        if (!is_ident_start_char(nm[0])) {
            continue;
        }
        {
            size_t i;
            int ok = 1;
            for (i = 1; i + 5u < ln; i++) {
                if (!is_ident_char(nm[i])) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                continue;
            }
        }
        if (snprintf(full, sizeof(full), "%s::%.*s", base_module, (int)(ln - 5u), nm) >= (int)sizeof(full)) {
            closedir(dp);
            return -1;
        }
        if (!strvec_push_copy(out_mods, full)) {
            closedir(dp);
            return -1;
        }
        found = 1;
    }
    closedir(dp);
    if (out_mods->len > 1) {
        qsort(out_mods->items, out_mods->len, sizeof(char *), mod_name_cmp);
    }
    return found;
}

static int load_with_uses_rec(
    const char *path,
    StrVec *stack,
    StrVec *loaded,
    char **out,
    size_t *out_len,
    char *err,
    size_t err_cap);

static int resolve_and_load_module(
    const char *module_name,
    const char *dir,
    const char *from_path,
    StrVec *stack,
    StrVec *loaded,
    char **out,
    size_t *out_len,
    char *err,
    size_t err_cap) {
    char *mf = module_path_to_file(module_name);
    char *cand = NULL;
    char *cand2 = NULL;
    int ok = 0;
    if (!mf) {
        (void)snprintf(err, err_cap, "out of memory");
        return 0;
    }
    cand = path_join2(dir, mf);
    if (cand && file_exists(cand)) {
        ok = load_with_uses_rec(cand, stack, loaded, out, out_len, err, err_cap);
    } else {
        cand2 = path_join2(".", mf);
        if (cand2 && file_exists(cand2)) {
            ok = load_with_uses_rec(cand2, stack, loaded, out, out_len, err, err_cap);
        } else {
            (void)snprintf(err, err_cap, "cannot resolve module \"%s\" from \"%s\"",
                           module_name,
                           path_basename(from_path));
            ok = 0;
        }
    }
    free(cand);
    free(cand2);
    free(mf);
    return ok;
}

static int load_with_uses_rec(
    const char *path,
    StrVec *stack,
    StrVec *loaded,
    char **out,
    size_t *out_len,
    char *err,
    size_t err_cap) {
    char *src;
    char *dir;

    if (strvec_contains(stack, path)) {
        (void)snprintf(err, err_cap, "use cycle detected involving \"%s\"", path_basename(path));
        return 0;
    }
    if (strvec_contains(loaded, path)) {
        return 1;
    }
    if (!strvec_push_copy(stack, path)) {
        (void)snprintf(err, err_cap, "out of memory");
        return 0;
    }
    src = read_file_contents(path);
    if (!src) {
        (void)snprintf(err, err_cap, "could not read \"%s\"", path);
        strvec_pop(stack);
        return 0;
    }
    dir = path_dirname_copy(path);
    if (!dir) {
        free(src);
        strvec_pop(stack);
        (void)snprintf(err, err_cap, "out of memory");
        return 0;
    }

    {
        size_t n = strlen(src);
        size_t start = 0;
        while (start < n) {
            size_t end = start;
            char base_mod[256];
            StrVec use_items = {0};
            int use_kind = 0;
            int use_glob = 0;
            while (end < n && src[end] != '\n') {
                end++;
            }
            {
                size_t ln = end - start;
                char line[512];
                if (ln >= sizeof(line)) {
                    ln = sizeof(line) - 1u;
                }
                memcpy(line, src + start, ln);
                line[ln] = '\0';
                use_kind = parse_use_decl_from_line(line, base_mod, sizeof(base_mod), &use_items, &use_glob);
            }
            if (use_kind < 0) {
                strvec_free(&use_items);
                free(dir);
                free(src);
                strvec_pop(stack);
                (void)snprintf(err, err_cap, "unsupported use syntax in \"%s\"", path_basename(path));
                return 0;
            } else if (use_kind > 0) {
                int ok = 1;
                if (use_glob) {
                    StrVec mods = {0};
                    char *mdir = module_path_to_dir(base_mod);
                    char *cand_dir = NULL;
                    char *cand_dir2 = NULL;
                    int found = 0;
                    if (!mdir) {
                        strvec_free(&use_items);
                        free(dir);
                        free(src);
                        strvec_pop(stack);
                        (void)snprintf(err, err_cap, "out of memory");
                        return 0;
                    }
                    cand_dir = path_join2(dir, mdir);
                    if (cand_dir) {
                        found = collect_glob_modules(cand_dir, base_mod, &mods);
                    }
                    if (found == 0) {
                        cand_dir2 = path_join2(".", mdir);
                        if (cand_dir2) {
                            found = collect_glob_modules(cand_dir2, base_mod, &mods);
                        }
                    }
                    free(mdir);
                    free(cand_dir);
                    free(cand_dir2);
                    if (found <= 0) {
                        strvec_free(&mods);
                        strvec_free(&use_items);
                        free(dir);
                        free(src);
                        strvec_pop(stack);
                        if (found < 0) {
                            (void)snprintf(err, err_cap, "out of memory");
                        } else {
                            (void)snprintf(err, err_cap, "cannot resolve module \"%s::*\" from \"%s\"",
                                           base_mod,
                                           path_basename(path));
                        }
                        return 0;
                    }
                    {
                        size_t mi;
                        for (mi = 0; mi < mods.len; mi++) {
                            if (!resolve_and_load_module(
                                    mods.items[mi], dir, path, stack, loaded, out, out_len, err, err_cap)) {
                                ok = 0;
                                break;
                            }
                        }
                    }
                    strvec_free(&mods);
                } else if (use_items.len > 0) {
                    size_t mi;
                    for (mi = 0; mi < use_items.len; mi++) {
                        char mod[512];
                        if (snprintf(mod, sizeof(mod), "%s::%s", base_mod, use_items.items[mi]) >= (int)sizeof(mod)) {
                            ok = 0;
                            (void)snprintf(err, err_cap, "module path too long in \"%s\"", path_basename(path));
                            break;
                        }
                        if (!resolve_and_load_module(mod, dir, path, stack, loaded, out, out_len, err, err_cap)) {
                            ok = 0;
                            break;
                        }
                    }
                } else {
                    ok = resolve_and_load_module(base_mod, dir, path, stack, loaded, out, out_len, err, err_cap);
                }
                strvec_free(&use_items);
                if (!ok) {
                    free(dir);
                    free(src);
                    strvec_pop(stack);
                    return 0;
                }
            } else {
                strvec_free(&use_items);
            }
            if (end < n && src[end] == '\n') {
                end++;
            }
            start = end;
        }
    }

    if (!append_text(out, out_len, "\n/* --- module: ")) {
        free(dir);
        free(src);
        strvec_pop(stack);
        (void)snprintf(err, err_cap, "out of memory");
        return 0;
    }
    if (!append_text(out, out_len, path) ||
        !append_text(out, out_len, " --- */\n") ||
        !append_text(out, out_len, src) ||
        !append_text(out, out_len, "\n")) {
        free(dir);
        free(src);
        strvec_pop(stack);
        (void)snprintf(err, err_cap, "out of memory");
        return 0;
    }

    free(dir);
    free(src);
    strvec_pop(stack);
    if (!strvec_push_copy(loaded, path)) {
        (void)snprintf(err, err_cap, "out of memory");
        return 0;
    }
    return 1;
}

static char *load_source_with_uses(const char *entry_path, char *err, size_t err_cap) {
    StrVec stack = {0};
    StrVec loaded = {0};
    char *out = NULL;
    size_t out_len = 0;
    int ok = load_with_uses_rec(entry_path, &stack, &loaded, &out, &out_len, err, err_cap);
    strvec_free(&stack);
    strvec_free(&loaded);
    if (!ok) {
        free(out);
        return NULL;
    }
    return out;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-i|--repl|--vm-test|--reflect] [file.mons]\n"
            "  no args          run embedded demo (AST + typecheck + eval)\n"
            "  -i, --repl       interactive read-eval-print loop\n"
            "  --vm-test        resolve `use`, typecheck vm_smoke, run VM smokes\n"
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
        {"smoke_use_stdlib", 'i', 12, 0.0f, 0.0},
        {"smoke_stdlib_helpers", 'i', 33, 0.0f, 0.0},
        {"smoke_hof", 'i', 12, 0.0f, 0.0},
        {"smoke_hof_capture", 'i', 17, 0.0f, 0.0},
        {"smoke_infer_unary", 'i', 42, 0.0f, 0.0},
        {"smoke_infer_pair", 'i', 42, 0.0f, 0.0},
        {"smoke_pipe_closure", 'i', 12, 0.0f, 0.0},
        {"smoke_nested", 'i', 13, 0.0f, 0.0},
        {"smoke_ctrl", 'i', 29, 0.0f, 0.0},
        {"smoke_if_return_then", 'i', 10, 0.0f, 0.0},
        {"smoke_if_return_else", 'i', 20, 0.0f, 0.0},
        {"smoke_if_return_mixed", 'i', 5, 0.0f, 0.0},
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
        {"smoke_match_bool", 'i', 7, 0.0f, 0.0},
        {"smoke_match_int", 'i', 13, 0.0f, 0.0},
        {"smoke_match_option_none", 'i', 2, 0.0f, 0.0},
        {"smoke_trait_bump", 'i', 5, 0.0f, 0.0},
        {"smoke_try_catch_basic", 'i', 9, 0.0f, 0.0},
        {"smoke_try_catch_cross_call", 'i', 12, 0.0f, 0.0},
        {"smoke_try_finally", 'i', 7, 0.0f, 0.0},
        {"smoke_try_catch_finally", 'i', 16, 0.0f, 0.0},
        {"smoke_try_return", 'i', 40, 0.0f, 0.0},
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

/* Build unreachable cycles directly and ensure tracing sweep frees them. */
static int run_gc_stress_test(void) {
    size_t base = value_gc_live_count();
    int it;
    for (it = 0; it < 200; it++) {
        ValSeq *a = value_seq_new();
        ValSeq *b = value_seq_new();
        Value va;
        Value vb;
        size_t mid;
        size_t after;

        if (!a || !b) {
            fprintf(stderr, "gc stress: out of memory creating cycle\n");
            return 1;
        }
        a->len = 1;
        b->len = 1;
        a->items = (Value *)calloc(1, sizeof(Value));
        b->items = (Value *)calloc(1, sizeof(Value));
        if (!a->items || !b->items) {
            free(a->items);
            free(b->items);
            va.kind = VAL_ARRAY;
            va.as.seq = a;
            vb.kind = VAL_ARRAY;
            vb.as.seq = b;
            value_release(&va);
            value_release(&vb);
            fprintf(stderr, "gc stress: out of memory creating cycle payload\n");
            return 1;
        }

        va.kind = VAL_ARRAY;
        va.as.seq = a;
        vb.kind = VAL_ARRAY;
        vb.as.seq = b;
        a->items[0] = value_retain(vb);
        b->items[0] = value_retain(va);
        value_release(&va);
        value_release(&vb);

        mid = value_gc_live_count();
        if (mid < base + 2u) {
            fprintf(stderr, "gc stress: expected cycle objects to remain before collect\n");
            return 1;
        }
        value_gc_collect(NULL, 0);
        after = value_gc_live_count();
        if (after != base) {
            fprintf(stderr, "gc stress: leak after collect (base=%lu after=%lu)\n",
                    (unsigned long)base,
                    (unsigned long)after);
            return 1;
        }
    }
    return 0;
}

static int run_vm_smoke_test(void) {
    const char *user_path = "tests/vm_smoke.mons";
    char load_err[256];
    char *src = load_source_with_uses(user_path, load_err, sizeof(load_err));
    TokenArray tokens;
    ParseResult pr;
    TypeCheckResult tc;
    CompileProgramResult bc;

    if (!src) {
        fprintf(stderr, "error: %s\n", load_err[0] ? load_err : "could not load vm smoke sources");
        return 1;
    }

    tokens = lexer_tokenize(src, user_path);
    if (!tokens.items) {
        free(src);
        fprintf(stderr, "error: lexer failed\n");
        return 1;
    }

    pr = parse_tokens(tokens, user_path);
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
    if (run_gc_stress_test() != 0) {
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
    char load_err[256] = {0};
    char *src = load_source_with_uses(path, load_err, sizeof(load_err));
    TokenArray tokens;
    ParseResult pr;
    TypeCheckResult tc;

    if (!src) {
        fprintf(stderr, "error: %s\n", load_err[0] ? load_err : "could not load source");
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

    er = eval_call_by_name(program, "try_catch_smoke", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval try_catch_smoke() = ");
    value_fprint(stdout, &er.result);
    printf("\n");
    value_release(&er.result);

    er = eval_call_by_name(program, "try_finally_overrides_return", NULL, 0);
    if (!er.ok) {
        fprintf(stderr, "eval error: %s\n", er.error_message ? er.error_message : "unknown");
        return 1;
    }
    printf("eval try_finally_overrides_return() = ");
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
        char load_err[256] = {0};
        file_buf = load_source_with_uses(argv[1], load_err, sizeof(load_err));
        if (!file_buf) {
            fprintf(stderr, "error: %s\n", load_err[0] ? load_err : "could not read source file");
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
