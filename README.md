# allocator

Simple single-threaded implementation of malloc, calloc, realloc and free for Linux/MacOS.
At best it's the same speed as glibc's malloc (according to my tests), and at worst it is ~2x as slow.
