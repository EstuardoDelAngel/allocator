#pragma once

#include <stddef.h>

void *malloc_(size_t size);
void *calloc_(size_t num, size_t size);
void *realloc_(void *ptr, size_t size);
void free_(void *ptr);