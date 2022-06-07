
#include "alloc.h"

#include <stdint.h>
#include <stdio.h>

#include <unistd.h>
#include <sys/mman.h>

void print_n(uintptr_t *arr, size_t n) {
    printf("{");
    if (n >= 1) {
        printf("%lu", arr[0]);

        for (size_t i = 1; i < n; i++) {
            printf(", %lu", arr[i]);
        }
    }
    printf("}\n");
}

int main() {
    /*const N = 100;
    uintptr_t *v = malloc_(sizeof(uintptr_t) * N);
    v[0] = 1;
    v[1] = 1;
    for (size_t i = 0; i < N; i++) v[i + 2] = v[i] + v[i + 1];
    // print_n(v, N);*/

    size_t page_words = sysconf(_SC_PAGESIZE)/sizeof(uintptr_t);

    uintptr_t* v = (uintptr_t *)malloc_(2048) - 2; print_n(v, page_words); printf("\n");
    malloc_(2000); print_n(v, page_words); printf("\n");
    v = (uintptr_t *)malloc_(2048) - 2; print_n(v, page_words); printf("\n");
    malloc_(1024); print_n(v, page_words); printf("\n");
    malloc_(512); print_n(v, page_words); printf("\n");
}