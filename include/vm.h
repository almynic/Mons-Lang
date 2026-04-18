#ifndef MONS_VM_H
#define MONS_VM_H

#include "bytecode.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool        ok;
    Value       result;
    const char *error_message;
} VmResult;

/*
 * Run function `chunks[entry]` with `args`. Other chunks are targets of OP_CALL.
 * `chunks` must stay alive for the duration of the call.
 * `structs` / `symbol_pool` are required for OP_STRUCT_NEW / OP_GET_FIELD_NAMED (may be NULL if unused).
 */
VmResult vm_run_program(
    const Chunk *chunks,
    size_t nchunks,
    size_t entry,
    const Value *args,
    size_t nargs,
    const BcStructLayout *structs,
    size_t nstructs,
    const char *const *symbol_pool,
    size_t nsymbols);

/* Single-chunk program (no OP_CALL to other chunks, or only self if inlined). */
VmResult vm_run_chunk(const Chunk *chunk, const Value *args, size_t nargs);

#endif /* MONS_VM_H */
