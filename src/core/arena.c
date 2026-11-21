#include "finte/object.h"

#define ARENA_MAGIC 0xF1A7E000u
#define BLOCK_HDR FINTE_ARENA_BLOCK_HDR

static fret_t arena_alloc_range(farena_t *a, uint64_t start, uint64_t end,
                                size_t size, size_t align, uint64_t *offset,
                                int scoped)
{
    if (!a || !a->in_use || size == 0 || !offset || end <= start ||
        end > a->size) {
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
    if (need > end - start) {
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }

    uint8_t *lo = a->base + start;
    uint8_t *hi = a->base + end;
    for (fblock_t *b = a->free_list; b; b = b->next) {
        if (!b->free || b->size < need || (!!b->scoped != scoped) ||
            (uint8_t *)b < lo || (uint8_t *)b + b->size > hi) {
            continue;
        }
        /* Split only when the remainder is a useful block. */
        if (b->size >= need + BLOCK_HDR + 16u) {
            fblock_t *rest = (fblock_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->next = b->next;
            rest->free = 1;
            rest->magic = ARENA_MAGIC;
            rest->scoped = b->scoped;
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

fret_t finte_arena_alloc(farena_t *a, size_t size, size_t align, uint64_t *offset)
{
    if (!a || !a->in_use) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    return arena_alloc_range(a, 0, a->size, size, align, offset, 0);
}

fret_t finte_arena_alloc_scoped(farena_t *a, uint64_t offset, uint64_t length,
                                size_t size, size_t align, uint64_t *out)
{
    if (!a || !a->in_use || length == 0 || offset + length < offset ||
        offset + length > a->size) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    return arena_alloc_range(a, offset, offset + length, size, align, out, 1);
}

fret_t finte_arena_reserve(farena_t *a, uint64_t offset, uint64_t length)
{
    if (!a || !a->in_use || length == 0 || offset + length < offset ||
        offset + length > a->size) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    uint8_t *start = a->base + offset;
    uint8_t *end = start + length;

    fblock_t *b = a->free_list;
    fblock_t *prev = NULL;
    while (b && (uint8_t *)b + b->size <= start) {
        prev = b;
        b = b->next;
    }
    if (!b || (uint8_t *)b > start || !b->free) {
        return finte_ret(FINTE_ERR_BUSY, 0);
    }

    uint8_t *block_end = (uint8_t *)b + b->size;
    if (end > block_end) {
        return finte_ret(FINTE_ERR_RANGE, 0);
    }
    if ((uint8_t *)b == start && b->size == length) {
        b->scoped = 1;
        return finte_ret_ok(0);
    }
    if (block_end > end && (size_t)(block_end - end) < BLOCK_HDR) {
        return finte_ret(FINTE_ERR_RANGE, 0);
    }
    if ((uint8_t *)b < start &&
        (size_t)(start - (uint8_t *)b) < BLOCK_HDR) {
        return finte_ret(FINTE_ERR_RANGE, 0);
    }

    fblock_t *next = b->next;
    if ((uint8_t *)b < start) {
        size_t head = (size_t)(start - (uint8_t *)b);
        b->size = head;
        b->scoped = 0;
        prev = b;
        b = (fblock_t *)start;
        b->size = length;
        b->next = next;
        b->free = 1;
        b->magic = ARENA_MAGIC;
        b->scoped = 1;
        if (prev) {
            prev->next = b;
        } else {
            a->free_list = b;
        }
    } else {
        b->size = length;
        b->scoped = 1;
    }

    if (end < block_end) {
        size_t tail = (size_t)(block_end - end);
        fblock_t *rest = (fblock_t *)end;
        rest->size = tail;
        rest->next = next;
        rest->free = 1;
        rest->magic = ARENA_MAGIC;
        rest->scoped = 0;
        b->next = rest;
    } else {
        b->next = next;
    }
    return finte_ret_ok(0);
}

fret_t finte_arena_free(farena_t *a, uint64_t offset)
{
    return finte_arena_free_scoped(a, offset, 0);
}

fret_t finte_arena_free_scoped(farena_t *a, uint64_t offset, int scoped)
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
    if (!!b->scoped != scoped) {
        return finte_ret(FINTE_ERR_PERM, 0);
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
        if (it->free && it->next->free && !it->scoped && !it->next->scoped &&
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
