# allocator

## about
Simple single-threaded implementation of malloc, calloc, realloc and free for Linux/MacOS.

## implementation
The implementation is based on dlmalloc, with some modifications. Memory is acquired using `mmap` and returned using `munmap`. An array (`bins`) of linked lists to free chunks of different size ranges is used to keep track of memory.

```
size: 16     24     32        512    1024
bins: +------+------+--     --+------+--
      | NULL | NULL |   ...   | .    |   ...
      +------+------+--     --+-|----+--
        |-----------------------|
        v
heap: +-----------+---------+------------+--                --+-----------+--
      | size: 536 | next: . | prev: NULL |   ... (data) ...   | size: 580 |   ...
      | unused    |       | |            |                    | unused    |
      +-----------+-------|-+------------+--                --+-----------+--
                          |                                     ^
                          |-------------------------------------|

## performance
According to some (probably not very representative) tests, the provided functions are 1-2x slower than glibc's implementations.
