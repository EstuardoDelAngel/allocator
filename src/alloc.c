#include "alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h> // for memcpy and memmove

#include <sys/mman.h>
#include <unistd.h>

#include <stdio.h> // remove

// TODO:
//   - if not linked list, use mmap like sbrk(0)
//     or:
//       - test page linked list ?
//       - merge adjacent pages whenever space is made ?
//   - try a minimum number of pages allocated, test to see fastest
//   - free
//   - calloc
//   - realloc

// bin sizes (bytes) are as such (assuming default defines):
// 16, 24, 32, 40, ..., 4080, 4088, 4096, 8192, 16384, ..., 1073741824, 2147483648

#define DATA(chunk) ((chunk) + 1)
#define NEXT(chunk) (*(chunk_ptr_t *)((chunk) + 1))
#define PREV(chunk) (*(chunk_ptr_t *)((chunk) + 2))

#define PAGE_BYTES() (sysconf(_SC_PAGESIZE))
#define PAGE_WORDS() (PAGE_BYTES() / WORD_BYTES)
#define MMAP(ptr, pages)                                                                  \
    mmap((ptr), max((pages), MMAP_PAGE_THRESHOLD) * PAGE_BYTES(), PROT_READ | PROT_WRITE, \
         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
#define MUNMAP(ptr, pages) munmap((ptr), (pages)*PAGE_BYTES())

enum {
    MAX_SMALL_CHUNK_SIZE = 12,
    MAX_BIN_SIZE = 31,
    MMAP_PAGE_THRESHOLD = 1,
    MUNMAP_PAGE_THRESHOLD = 1,

    WORD_MAX = UINTPTR_MAX,
    WORD_BYTES = sizeof(word_t),
    MAX_SMALL_CHUNK_BYTES = 1 << MAX_SMALL_CHUNK_SIZE,
    MAX_SMALL_CHUNK_WORDS = MAX_SMALL_CHUNK_BYTES / WORD_BYTES,
    N_BINS = MAX_BIN_SIZE - MAX_SMALL_CHUNK_SIZE + MAX_SMALL_CHUNK_WORDS - 1,

    SIZE_MASK = WORD_MAX >> 2,
    USED_MASK = ~(WORD_MAX >> 1),
    BOUNDARY_MASK = ~(SIZE_MASK | USED_MASK),

    USED = USED_MASK,
    UNUSED = 0,
    BOUNDARY = BOUNDARY_MASK,
    NOT_BOUNDARY = 0
};

typedef uintptr_t word_t;
typedef word_t *chunk_ptr_t;

// this assumes the bit representation of null is 0
static chunk_ptr_t bins[N_BINS] = {0};
static chunk_ptr_t last_alloc = NULL;
static word_t last_alloc_words = 0;

static void print_n(uintptr_t *arr, word_t n) { // remove
    printf("{");
    if (n >= 1) {
        printf("%lu", arr[0]);

        for (size_t i = 1; i < n; i++) {
            printf(", %lu", arr[i]);
        }
    }
    printf("}\n");
}

static inline word_t max(word_t x, word_t y) { return (x > y) ? x : y; }
static inline word_t ceil_div(word_t x, word_t y) { return (x + y - 1) / y; }
static inline word_t bytes_to_words(size_t bytes) { return max(ceil_div((word_t)bytes, WORD_BYTES), 2); }

static inline word_t get_size(const chunk_ptr_t chunk) { return *chunk & SIZE_MASK; }
static inline word_t used(const chunk_ptr_t chunk) { return *chunk & USED_MASK; }
static inline word_t boundary(const chunk_ptr_t chunk) { return *chunk & BOUNDARY_MASK; }

static inline chunk_ptr_t tail(const chunk_ptr_t chunk) { return chunk + get_size(chunk) + 1; }
static inline chunk_ptr_t prev_tail(const chunk_ptr_t chunk) { return chunk - 1; }
static inline chunk_ptr_t next_adj(const chunk_ptr_t chunk) { return chunk + get_size(chunk) + 2; }
static inline chunk_ptr_t prev_adj(const chunk_ptr_t chunk) { return chunk - get_size(prev_tail(chunk)) - 2; }

static inline void set_used(chunk_ptr_t chunk, word_t used) {
    *chunk = (*chunk & ~USED_MASK) | (used & USED_MASK);
    *tail(chunk) = (*tail(chunk) & ~USED_MASK) | (used & USED_MASK);
}

static inline void set_boundary(chunk_ptr_t chunk, word_t boundary) {
    *chunk = (*chunk & ~BOUNDARY_MASK) | (boundary & BOUNDARY_MASK);
}

static inline void create_chunk(chunk_ptr_t chunk, word_t size, word_t used, word_t start, word_t end) {
    *chunk = (size & SIZE_MASK) | (used & USED_MASK);
    *(chunk + (size & SIZE_MASK) + 1) = *chunk | end;
    *chunk |= start;
}

static inline size_t words_to_bin_index(word_t words) {
    if (words < MAX_SMALL_CHUNK_WORDS) return words - 2;
    size_t i = 0;
    while (words >>= 1) i++;
    return i + MAX_SMALL_CHUNK_WORDS + 1 - MAX_SMALL_CHUNK_SIZE;
}

static void return_chunk(chunk_ptr_t chunk) {
    word_t words = get_size(chunk);

    size_t i = words_to_bin_index(words);
    chunk_ptr_t ptr = bins[i];
    chunk_ptr_t next;

    if (!ptr || get_size(ptr) >= words) {
        // insert at beginning
        next = ptr;
        bins[i] = chunk;
    } else {
        while (NEXT(ptr) && get_size(NEXT(ptr)) < words) ptr = NEXT(ptr);
        // insert into list
        next = NEXT(ptr);
        NEXT(ptr) = chunk;
    }

    NEXT(chunk) = next;
    PREV(chunk) = ptr;
    if (next) PREV(next) = chunk;
}

static inline void borrow_chunk(chunk_ptr_t chunk) {
    if (PREV(chunk))
        NEXT(PREV(chunk)) = NEXT(chunk);
    else
        bins[words_to_bin_index(get_size(chunk))] = NEXT(chunk);
}

// assumes chunk is already used
static void shrink_chunk(chunk_ptr_t chunk, word_t new_size) {
    word_t old_size = get_size(chunk);
    word_t end = boundary(tail(chunk));
    if (old_size - new_size > 4) {
        chunk_ptr_t next_chunk = next_adj(chunk);
        create_chunk(chunk, size, USED, boundary(chunk), NOT_BOUNDARY);
        chunk_ptr_t new_chunk = next_adj(chunk);

        if (!end && !used(next_chunk)) {
            borrow_chunk(next_chunk);
            create_chunk(new_chunk, old_size - size + get_size(next_chunk), UNUSED, NOT_BOUNDARY, end);
        } else {
            create_chunk(new_chunk, old_size - size - 2, UNUSED, NOT_BOUNDARY, end);
        }

        return_chunk(new_chunk);
    }
}

static chunk_ptr_t malloc_internal(word_t size) {
    // find chunk of at least the required number of words
    chunk_ptr_t chunk;
    for (size_t i = words_to_bin_index(size); i < N_BINS; i++) {
        chunk = bins[i];
        while (chunk) {
            if (get_size(chunk) >= size) {
                borrow_chunk(chunk);
                set_used(chunk, USED);
                shrink_chunk(chunk, size);
                return DATA(chunk);
            }
            chunk = NEXT(chunk);
        }
    }

    // no chunk found, mmap
    word_t pages = ceil_div(size + 2, PAGE_WORDS());
    word_t alloced = pages * PAGE_WORDS() - 2;
    chunk = MMAP(last_alloc, pages);
    if (chunk == MAP_FAILED) return NULL;

    last_alloc = chunk;
    last_alloc_words = pages * PAGE_WORDS();

    if (last_alloc && last_alloc + last_alloc_words == chunk) {
        if (!used(prev_tail(chunk))) {
            chunk = prev_adj(chunk);
            alloced += get_size(chunk) + 2;
            borrow_chunk(chunk);
            create_chunk(chunk, alloced, USED, boundary(chunk), BOUNDARY);
        } else {
            set_boundary(prev_tail(chunk), NOT_BOUNDARY);
            create_chunk(chunk, alloced, USED, NOT_BOUNDARY, BOUNDARY);
        }
    } else {
        create_chunk(chunk, alloced, USED, BOUNDARY, BOUNDARY);
    }

    shrink_chunk(chunk, size);
    return DATA(chunk);
}

static chunk_ptr_t realloc_internal(chunk_ptr_t ptr, word_t size) {
    chunk_ptr_t chunk = ptr - 1; // start of chunk

    // free the end bit
    if (get_size(chunk) >= size) {
        shrink_chunk(chunk, size);
        return ptr;
    }

    // expand into next chunk
    chunk_ptr_t next = next_adj(chunk);
    word_t available = get_size(chunk);
    if (!boundary(tail(chunk)) && !used(next)) {
        available += get_size(next) + 2;
        if (available >= size) {
            borrow_chunk(next);
            create_chunk(chunk, available, USED, boundary(chunk), boundary(tail(next)));
            shrink_chunk(chunk, size);
            return ptr;
        }
        next_unused = true;
    }

    // expand into previous
    chunk_ptr_t prev = prev_adj(chunk);
    if (!boundary(chunk) && !used(prev)) {
        available += get_size(prev) + 2;
        // minor hack
        create_chunk(prev, available, USED, boundary(prev), boundary(chunk + get_size(chunk) + 2));
        shrink_chunk(prev, size);
        prev++;
        memmove((void *)prev, (void *)ptr, size * WORD_BYTES);
        return prev;
    }

    chunk_ptr_t new_ptr = malloc_internal(size);
    memcpy((void *)new_ptr, (void *)ptr, size * WORD_BYTES);
    return new_ptr;
}

void *malloc_(size_t size) {
    if (!size) return NULL;
    return (void *)malloc_internal(bytes_to_words(size));
}

void *realloc_(void *ptr, size_t size) {
    if (!size) {
        free_(ptr);
        return NULL;
    }
    return (void *)realloc_internal((chunk_ptr_t)ptr, bytes_to_words(size));
}

void *calloc_(size_t num, size_t size) {
    word_t new_size = (word_t)num * size;
    if (!num || !size || new_size / num != size) return NULL;
    return (void *)malloc_internal(bytes_to_words(new_size));
}

void free_(void *ptr) {}