#include "bytecode.h"

#include <stdlib.h>
#include <string.h>

void chunk_init(Chunk *c) {
    c->code = NULL;
    c->len = 0;
    c->cap = 0;
    c->constants = NULL;
    c->nconst = 0;
    c->capconst = 0;
    c->nparams = 0;
    c->nlocals = 0;
    c->nupvalues = 0;
}

void chunk_free(Chunk *c) {
    size_t i;
    if (!c) {
        return;
    }
    free(c->code);
    c->code = NULL;
    c->len = 0;
    c->cap = 0;
    for (i = 0; i < c->nconst; i++) {
        Value *v = &c->constants[i];
        value_release(v);
    }
    free(c->constants);
    c->constants = NULL;
    c->nconst = 0;
    c->capconst = 0;
    c->nparams = 0;
    c->nlocals = 0;
    c->nupvalues = 0;
}

int chunk_add_constant(Chunk *c, Value v) {
    Value *nv;
    if (c->nconst >= c->capconst) {
        size_t ncap = c->capconst ? c->capconst * 2u : 8u;
        nv = (Value *)realloc(c->constants, ncap * sizeof(Value));
        if (!nv) {
            return -1;
        }
        c->constants = nv;
        c->capconst = ncap;
    }
    c->constants[c->nconst] = v;
    c->nconst++;
    return (int)(c->nconst - 1u);
}

static int chunk_grow(Chunk *c, size_t need) {
    size_t ncap = c->cap ? c->cap : 64u;
    while (ncap < c->len + need) {
        ncap *= 2u;
    }
    if (ncap != c->cap) {
        uint8_t *p = (uint8_t *)realloc(c->code, ncap);
        if (!p) {
            return 0;
        }
        c->code = p;
        c->cap = ncap;
    }
    return 1;
}

void chunk_emit_u8(Chunk *c, uint8_t b) {
    if (!chunk_grow(c, 1u)) {
        return;
    }
    c->code[c->len++] = b;
}

void chunk_emit_u16(Chunk *c, uint16_t u) {
    if (!chunk_grow(c, 2u)) {
        return;
    }
    c->code[c->len++] = (uint8_t)(u & 0xffu);
    c->code[c->len++] = (uint8_t)((u >> 8) & 0xffu);
}

void chunk_patch_u16(Chunk *c, size_t at, uint16_t v) {
    if (!c || at + 2u > c->len) {
        return;
    }
    c->code[at] = (uint8_t)(v & 0xffu);
    c->code[at + 1u] = (uint8_t)((v >> 8) & 0xffu);
}
