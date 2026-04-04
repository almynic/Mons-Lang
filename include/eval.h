/*
 * Runtime values and eval_call_by_name(). Composite values (array, tuple, struct)
 * use refcounting; call value_release on EvalResult.result when done if it may be composite.
 */
#ifndef MONS_EVAL_H
#define MONS_EVAL_H

#include "ast.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    VAL_INT,
    VAL_FLOAT,
    VAL_DOUBLE,
    VAL_BOOL,
    VAL_STRING,
    VAL_NONE,
    VAL_VOID,
    VAL_ARRAY,
    VAL_TUPLE,
    VAL_STRUCT
} ValKind;

typedef struct ValSeq ValSeq;
typedef struct ValStruct ValStruct;

typedef struct Value {
    ValKind kind;
    union {
        int64_t i;
        float f;
        double d;
        bool b;
        const char *s;
        ValSeq *seq;
        ValStruct *st;
    } as;
} Value;

struct ValSeq {
    size_t len;
    size_t refc;
    Value *items;
};

struct ValStruct {
    size_t refc;
    const char *type_name;
    size_t n;
    const char **field_names;
    Value *values;
};

typedef struct {
    bool ok;
    Value result;
    const char *error_message;
} EvalResult;

void value_fprint(FILE *fp, const Value *v);
void value_release(Value *v);

/* Run a type-checked top-level function by name. `args` length must match parameters. */
EvalResult eval_call_by_name(AstNode *program, const char *fn_name, const Value *args, size_t nargs);

#endif /* MONS_EVAL_H */
