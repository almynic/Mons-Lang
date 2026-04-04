/*
 * arena.c — bump-pointer allocator for the whole AST.
 *
 * All AstNode / AstList / copied lexer strings come from here. The parser never
 * calls free() on individual nodes; ast_free_all() walks the block list and
 * frees everything in one shot after parse + typecheck + eval are done.
 */
#include "ast.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AST_ARENA_BLOCK_SIZE (64 * 1024)

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t capacity;
    unsigned char data[];
} ArenaBlock;

static ArenaBlock *g_arena_head = NULL;

static size_t align_up(size_t value, size_t alignment) {
    size_t remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }
    return value + (alignment - remainder);
}

static ArenaBlock *arena_block_new(size_t min_capacity) {
    size_t capacity = AST_ARENA_BLOCK_SIZE;
    if (min_capacity > capacity) {
        capacity = min_capacity;
    }

    size_t total_size = sizeof(ArenaBlock) + capacity;
    ArenaBlock *block = (ArenaBlock *)malloc(total_size);
    if (!block) {
        return NULL;
    }

    block->next = NULL;
    block->used = 0;
    block->capacity = capacity;
    return block;
}

static void *arena_alloc_raw(size_t size, size_t alignment) {
    if (size == 0) {
        size = 1;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    if (!g_arena_head) {
        g_arena_head = arena_block_new(size + alignment);
        if (!g_arena_head) {
            return NULL;
        }
    }

    ArenaBlock *block = g_arena_head;
    size_t offset = align_up(block->used, alignment);

    if (offset + size > block->capacity) {
        ArenaBlock *new_block = arena_block_new(size + alignment);
        if (!new_block) {
            return NULL;
        }

        new_block->next = g_arena_head;
        g_arena_head = new_block;
        block = new_block;
        offset = align_up(block->used, alignment);
    }

    void *ptr = (void *)(block->data + offset);
    block->used = offset + size;
    return ptr;
}

AstNode *ast_alloc(NodeKind kind, SrcLoc loc) {
    AstNode *node = (AstNode *)arena_alloc_raw(sizeof(AstNode), _Alignof(AstNode));
    if (!node) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->loc = loc;
    return node;
}

AstList *ast_list_append(AstList *list, AstNode *item) {
    AstList *new_item = (AstList *)arena_alloc_raw(sizeof(AstList), _Alignof(AstList));
    if (!new_item) {
        return list;
    }

    new_item->item = item;
    new_item->next = NULL;

    if (!list) {
        return new_item;
    }

    AstList *tail = list;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = new_item;
    return list;
}

void ast_free_all(void) {
    ArenaBlock *block = g_arena_head;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    g_arena_head = NULL;
}

const char *ast_copy_string(const char *src, size_t len) {
    char *buf = (char *)arena_alloc_raw(len + 1, 1);
    if (!buf) {
        return NULL;
    }
    if (len > 0 && src) {
        memcpy(buf, src, len);
    }
    buf[len] = '\0';
    return buf;
}
