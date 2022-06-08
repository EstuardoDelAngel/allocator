#pragma once

#include <stddef.h>

// MAX_SMALL_CHUNK_BYTES == 1 << MAX_SMALL_CHUNK_SIZE, e.g. 9 corresponds to 512 bytes

void *malloc_(size_t size);
void *calloc_(size_t num, size_t size);
void *realloc_(void *ptr, size_t size);
void free_(void *ptr);