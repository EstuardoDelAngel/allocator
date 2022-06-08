# allocator

Single-threaded implementation of malloc, calloc, realloc and free for Linux/MacOS.
Simple and easy to understand implementation but considerably (up to ~100x for small allocations) slower than glibc's malloc in my tests.
