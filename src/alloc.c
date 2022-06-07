#include "alloc.h"

#include <stdint.h>

#include <unistd.h>
#include <sys/mman.h>

#include <stdio.h> // remove

// TODO:
//   - test page linked list
//   - merge adjacent pages whenever space is made
//   - try a minimum number of pages allocated, test to see fastest
//   - free
//   - calloc
//   - realloc

// by default:
//   - MAX_SMALL_CHUNK_BYTES is 512
//   - MAX_BIN_BYTES is 2^31
// bin sizes (bytes) are as such (assuming default defines):
// 16, 24, 32, 40, ..., 504, 512, 1024, 2048, 4096, ..., 1073741824, 2147483648

#define MAX_BIN_SIZE 31
#define WORD_MAX UINTPTR_MAX
#define WORD_BYTES sizeof(word_t)
#define MAX_SMALL_CHUNK_BYTES (1 << MAX_SMALL_CHUNK_SIZE)
#define MAX_SMALL_CHUNK_WORDS (MAX_SMALL_CHUNK_BYTES / WORD_BYTES)
#define N_BINS (MAX_BIN_SIZE - MAX_SMALL_CHUNK_SIZE + MAX_SMALL_CHUNK_WORDS - 1)

#define SIZE_MASK (WORD_MAX >> 2)
#define USED_MASK (~(WORD_MAX >> 1))
#define BOUNDARY_MASK (~(SIZE_MASK | USED_MASK))
#define USED USED_MASK
#define UNUSED 0
#define BOUNDARY BOUNDARY_MASK
#define NOT_BOUNDARY 0

#define DATA(chunk) ((chunk) + 1)
#define NEXT(chunk) (*(chunk_ptr_t *)((chunk) + 1))
#define PREV(chunk) (*(chunk_ptr_t *)((chunk) + 2))

#define PAGE_BYTES (sysconf(_SC_PAGESIZE))
#define PAGE_WORDS (PAGE_BYTES / WORD_BYTES)
#define MMAP(ptr, pages) mmap(ptr, pages * PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
#define MUNMAP(ptr, pages) munmap(ptr, pages * PAGE_BYTES)

typedef uintptr_t word_t;
typedef word_t *chunk_ptr_t;

// this assumes the bit representation of null is 0
static chunk_ptr_t bins[N_BINS] = {0};
static chunk_ptr_t first_page = NULL; // linked list
static chunk_ptr_t last_page = NULL;

static void print_n(uintptr_t *arr, word_t n) { // remove
    printf("{");
    if (n >= 1) {
        printf("%lu", arr[0]);

        for (word_t i = 1; i < n; i++) {
            printf(", %lu", arr[i]);
        }
    }
    printf("}\n");
}

static inline word_t ceil_div(word_t x, word_t y) { return (x + y - 1) / y; }

static inline word_t get_size(chunk_ptr_t chunk) { return *chunk & SIZE_MASK; }
static inline word_t     used(chunk_ptr_t chunk) { return *chunk & USED_MASK; }
static inline word_t boundary(chunk_ptr_t chunk) { return *chunk & BOUNDARY_MASK; }

static inline chunk_ptr_t      tail(chunk_ptr_t chunk) { return chunk + (*chunk & SIZE_MASK )+ 1; }
static inline chunk_ptr_t prev_tail(chunk_ptr_t chunk) { return chunk - 1; }
static inline chunk_ptr_t  next_adj(chunk_ptr_t chunk) { return chunk + (*chunk & SIZE_MASK) + 2; }
static inline chunk_ptr_t  prev_adj(chunk_ptr_t chunk) { return chunk - (*(chunk - 1) & SIZE_MASK) - 2; }

static inline void set_used(chunk_ptr_t chunk, word_t used) {
    return *chunk = (*chunk & ~USED_MASK) | (used & USED_MASK);
}

static inline void create_chunk(chunk_ptr_t chunk, word_t size, word_t used, word_t start, word_t end) {
    *chunk = (size & SIZE_MASK) | (used & USED_MASK);
    *(chunk + (size & SIZE_MASK) + 1) = *chunk | end;
    *chunk |= start;
}

static inline word_t words_to_bin_index(word_t words) {
    if (words < MAX_SMALL_CHUNK_WORDS) return words - 2;
    word_t i = 0;
    while (words >>= 1) i++;
    return MAX_SMALL_CHUNK_WORDS + i + 1 - MAX_SMALL_CHUNK_SIZE;
}

static void return_chunk(chunk_ptr_t chunk) {
    word_t words = get_size(chunk);
    
    word_t i = words_to_bin_index(words);
    chunk_ptr_t ptr = bins[i];
    chunk_ptr_t next;

    if (!ptr || get_size(ptr) >= words) {
        // insert at beginning
        next = ptr;
        bins[i] = chunk;
    }
    else {
        while (NEXT(ptr) && get_size(NEXT(ptr)) < words) ptr = NEXT(ptr);
        // insert into list
        next = NEXT(ptr);
        NEXT(ptr) = chunk;
    }

    NEXT(chunk) = next;
    PREV(chunk) = ptr;
    if (next) PREV(next) = chunk;
}

static void borrow_chunk(chunk_ptr_t chunk) {
    if (PREV(chunk)) NEXT(PREV(chunk)) = NEXT(chunk);
    else bins[words_to_bin_index(get_size(chunk))] = NEXT(chunk);
}

void *malloc_(word_t size) {
    if (size == 0) return NULL;

    // convert bytes to words, with a minimum of 2 words allocated
    size = ceil_div(size, WORD_BYTES);
    if (size < 2) size = 2;

    // find chunk of at least the required number of words
    chunk_ptr_t chunk;
    for (word_t i = words_to_bin_index(size); i < N_BINS; i++) {
        chunk = bins[i];
        while (chunk) {
            if (get_size(chunk) >= size) {
                borrow_chunk(chunk);
                word_t old_size = get_size(chunk);
                word_t is_end = boundary(tail(chunk));

                printf("found space\n");

                if (old_size - size > 4) {
                    printf("(extra)\n");

                    create_chunk(chunk, size, USED, boundary(chunk), NOT_BOUNDARY);
                    chunk_ptr_t new_chunk = next_adj(chunk);
                    create_chunk(new_chunk, old_size - size - 2, UNUSED, NOT_BOUNDARY, is_end);

                    chunk_ptr_t next_chunk = next_adj(chunk);
                    if (!is_end && !used(next_chunk)) {
                        // merge
                        borrow_chunk(new_chunk);
                        borrow_chunk(next_chunk);
                        create_chunk(new_chunk, get_size(new_chunk) + get_size(next_chunk) + 2,
                            UNUSED, NOT_BOUNDARY, boundary(tail(next_chunk)));
                        return_chunk(new_chunk);
                    }
                    else {
                        return_chunk(new_chunk);
                    }
                }
                else {
                    set_used(chunk, USED);
                }

                return (void *)DATA(chunk);
            }
            chunk = NEXT(chunk);
        }
    }

    printf("mmap\n");

    // no chunk found, mmap
    word_t pages = ceil_div(size + 3, PAGE_WORDS); // pages required
    word_t alloced = pages * PAGE_WORDS - 3;
    chunk = MMAP(NULL, pages);
    if (chunk == MAP_FAILED) return NULL;

    if (first_page == NULL) first_page = chunk;
    else *last_page = (word_t)chunk;
    last_page = chunk;
    chunk++;

    // add excess to bin
    if (alloced - size > 4) {
        create_chunk(chunk, size, USED, BOUNDARY, NOT_BOUNDARY);
        create_chunk(next_adj(chunk), alloced - size - 2, UNUSED, NOT_BOUNDARY, BOUNDARY);
        return_chunk(next_adj(chunk));
    }
    else {
        create_chunk(chunk, alloced, USED, BOUNDARY, BOUNDARY);
    }

    return (void *)DATA(chunk);
}

void *calloc_(word_t num, word_t size) {
    return NULL;
}

void *realloc_(void *ptr, word_t size) {
    return NULL;
}

void free_(void *ptr) {
    
}