# allocator

Single-threaded implementation of malloc, calloc, realloc and free for Linux/MacOS.
The code should be simple and easy to understand, but it is considerably (up to ~100x for small allocations) slower than glibc's malloc in my tests.
