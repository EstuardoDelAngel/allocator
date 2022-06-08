#include <stdbool.h> // bool
#include <stdint.h>  // uintptr_t, size_t
#include <string.h>  // memcpy, memmove, memset

#include <sys/mman.h> // mmap, munmap
#include <unistd.h>   // sysconf, _SC_PAGESIZE

#include "alloc.h"


#define MAX_SMALL_CHUNK_SIZE = 12,
#define MAX_BIN_SIZE = 31,
#define MMAP_PAGE_THRESHOLD = 1,
#define MUNMAP_PAGE_THRESHOLD = 1,

#define WORD_MAX = UINTPTR_MAX,
#define WORD_BYTES = sizeof(word_t),
#define MAX_SMALL_CHUNK_BYTES = 1 << MAX_SMALL_CHUNK_SIZE,
#define MAX_SMALL_CHUNK_WORDS = MAX_SMALL_CHUNK_BYTES / WORD_BYTES,
#define N_BINS = MAX_BIN_SIZE - MAX_SMALL_CHUNK_SIZE + MAX_SMALL_CHUNK_WORDS - 1,

#define SIZE_MASK = WORD_MAX >> 2,
#define USED_MASK = ~(WORD_MAX >> 1),
#define BOUNDARY_MASK = ~(SIZE_MASK | USED_MASK),

#define USED = USED_MASK,
#define UNUSED = 0,
#define BOUNDARY = BOUNDARY_MASK,
#define NOT_BOUNDARY = 0

#define PTR_TO_CHUNK(ptr) ((chunk_ptr_t)(ptr)-1)
#define DATA(chunk) ((chunk_ptr_t)(chunk) + 1)
#define NEXT(chunk) (*(chunk_ptr_t *)((chunk_ptr_t)(chunk) + 1))
#define PREV(chunk) (*(chunk_ptr_t *)((chunk_ptr_t)(chunk) + 2))

#define PAGE_BYTES() (sysconf(_SC_PAGESIZE))
#define PAGE_WORDS() (PAGE_BYTES() / WORD_BYTES)
#define MMAP(ptr, pages)                                                                \
    (chunk_ptr_t) mmap((void *)(ptr), max((pages), MMAP_PAGE_THRESHOLD) * PAGE_BYTES(), \
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
#define MUNMAP(ptr, pages) munmap((void *)(ptr), (pages)*PAGE_BYTES())


typedef uintptr_t word_t;
typedef word_t *chunk_ptr_t;


static chunk_ptr_t bins[N_BINS] = {0}; // assuming the bit representation of null is 0
static chunk_ptr_t last_alloc = NULL;
static word_t last_alloc_words = 0;

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


static void shrink_chunk(chunk_ptr_t chunk, word_t size) {
    // assumes chunk is already used
    word_t old_size = get_size(chunk);
    word_t end = boundary(tail(chunk));
    if (old_size - size > 4) {
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


static chunk_ptr_t malloc_internal(word_t size, bool zero) {
    // find chunk of at least the required number of words
    chunk_ptr_t chunk;
    for (size_t i = words_to_bin_index(size); i < N_BINS; i++) {
        chunk = bins[i];
        while (chunk) {
            if (get_size(chunk) >= size) {
                borrow_chunk(chunk);
                set_used(chunk, USED);
                shrink_chunk(chunk, size);
                if (zero) memset((void *)DATA(chunk), 0, size / WORD_BYTES);
                return DATA(chunk);
            }
            chunk = NEXT(chunk);
        }
    }

    // no chunk found, mmap
    word_t pages = ceil_div(size + 2, PAGE_WORDS());
    word_t alloced = pages * PAGE_WORDS() - 2;
    chunk = (chunk_ptr_t)MMAP(last_alloc, pages);
    if (chunk == MAP_FAILED) return NULL;

    last_alloc = chunk;
    last_alloc_words = pages * PAGE_WORDS();

    if (last_alloc && last_alloc + last_alloc_words == chunk) {
        if (!used(prev_tail(chunk))) {
            chunk = prev_adj(chunk);
            alloced += get_size(chunk) + 2;
            if (zero) memset((void *)DATA(chunk), 0, (get_size(chunk) + 2) / WORD_BYTES);
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


void *realloc_(void *ptr, size_t size) {
    if (!ptr) return malloc_(size);
    if (!size) {
        free_(ptr);
        return NULL;
    }

    size = bytes_to_words(size);
    chunk_ptr_t chunk = PTR_TO_CHUNK(ptr); // start of chunk

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
        return (void *)prev;
    }

    chunk_ptr_t new_ptr = malloc_internal(size, false);
    memcpy((void *)new_ptr, (void *)ptr, size * WORD_BYTES);
    return (void *)new_ptr;
}

void free_(void *ptr) {
    if (!ptr) return;
    set_used(ptr, UNUSED);
    chunk_ptr_t chunk = PTR_TO_CHUNK(ptr);

    if (!boundary(tail(chunk))) {
        chunk_ptr_t next = next_adj(chunk);
        if (!used(next)) {
            borrow_chunk(next);
            create_chunk(chunk, get_size(chunk) + get_size(next) + 2, UNUSED, boundary(chunk), boundary(tail(next)));
        }
    }

    if (!boundary(chunk)) {
        chunk_ptr_t prev = prev_adj(chunk);
        if (!used(prev)) {
            borrow_chunk(prev);
            create_chunk(prev, get_size(prev) + get_size(chunk) + 2, UNUSED, boundary(prev), boundary(tail(chunk)));
        }
    }

    if (boundary(chunk) && boundary(tail(chunk))) {
        word_t pages = (get_size(chunk) + 2) / PAGE_WORDS();
        if (pages >= MUNMAP_PAGE_THRESHOLD) {
            MUNMAP(chunk, pages);
            return;
        }
    }

    return_chunk(chunk);
}

void *malloc_(size_t size) {
    if (!size) return NULL;
    return (void *)malloc_internal(bytes_to_words(size), false);
}

void *calloc_(size_t num, size_t size) {
    word_t new_size = (word_t)num * size;
    if (!num || !size || new_size / num != size) return NULL; // overflow check
    return (void *)malloc_internal(bytes_to_words(new_size), true);
}