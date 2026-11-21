#include "finte/object.h"

#define ARENA_MAGIC 0xF1A7E000u
#define BLOCK_HDR ((sizeof(fblock_t) + 15u) & ~(size_t)15u)

fret_t finte_arena_alloc(farena_t *a, size_t size, size_t align, uint64_t *offset)
{
    if (!a || !a->in_use || size == 0 || !offset) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    if (align < 16) {
        align = 16;
    }
    if (align > 16) {
        /* Block payloads are 16-byte aligned; finer alignment would require
         * a different allocator and is not promised by the Finte ABI. */
        return finte_ret(FINTE_ERR_INVAL, 0);
    }

    size_t payload = (size + 15u) & ~(size_t)15u;
    size_t need = BLOCK_HDR + payload;

    for (fblock_t *b = a->free_list; b; b = b->next) {
        if (!b->free || b->size < need) {
            continue;
        }
        /* Split only when the remainder is a useful block. */
        if (b->size >= need + BLOCK_HDR + 16u) {
            fblock_t *rest = (fblock_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            rest->free = 1;
            rest->magic = ARENA_MAGIC;
            b->size = need;
            b->next = rest;
        }
        b->free = 0;
        a->used += b->size;
        if (a->used > a->peak_bytes) {
            a->peak_bytes = a->used;
        }
        a->live_bytes += payload;
        a->allocs++;
        *offset = (uint64_t)((uint8_t *)b - a->base) + BLOCK_HDR;
        return finte_ret_ok(*offset);
    }
    return finte_ret(FINTE_ERR_NOMEM, 0);
}

fret_t finte_arena_free(farena_t *a, uint64_t offset)
{
    if (!a || !a->in_use || offset < BLOCK_HDR) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    if (offset >= a->size) {
        return finte_ret(FINTE_ERR_RANGE, 0);
    }
    fblock_t *b = (fblock_t *)(a->base + offset - BLOCK_HDR);
    if (b->magic != ARENA_MAGIC || b->free) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    b->free = 1;
    if (a->used >= b->size) {
        a->used -= b->size;
    } else {
        a->used = 0;
    }
    if (a->live_bytes >= b->size - BLOCK_HDR) {
        a->live_bytes -= b->size - BLOCK_HDR;
    } else {
        a->live_bytes = 0;
    }
    a->frees++;

    /* The block list always contains every block, allocated or free, in
     * address order. Coalesce adjacent free blocks in one pass. */
    fblock_t *it = a->free_list;
    while (it && it->next) {
        if (it->free && it->next->free &&
            (uint8_t *)it + it->size == (uint8_t *)it->next) {
            it->size += it->next->size;
            it->next = it->next->next;
        } else {
            it = it->next;
        }
    }
    return finte_ret_ok(0);
}

void finte_arena_destroy(farena_t *a)
{
    if (!a || !a->in_use) {
        return;
    }
    if (a->base) {
        hal_pages_free(a->base, a->size);
    }
    a->base = NULL;
    a->size = 0;
    a->used = 0;
    a->free_list = NULL;
    a->in_use = 0;
}
