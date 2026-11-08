#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *port_memcpy(void *, const void *, size_t);
static void ref(unsigned char *d, const unsigned char *s, size_t n) { while (n--) *d++ = *s++; }
int main(void) {
    static unsigned char a[4096], b[4096];
    unsigned long long tests = 0;
    srand(1);
    for (int it = 0; it < 2000000; it++) {
        for (int i = 0; i < 1024; i++) a[i] = b[i] = rand();
        size_t n = rand() % 300; if (rand() % 8 == 0) n = rand() % 900;
        int so = rand() % 100, dof = rand() % 100;
        if (rand() % 2) dof = so + (rand() % 70) - 35; if (dof < 0) dof = 0;
        ref(a + dof, a + so, n);
        void *r = port_memcpy(b + dof, b + so, n);
        if (r != b + dof || memcmp(a, b, 1024)) { printf("FAIL it %d n %zu so %d do %d\n", it, n, so, dof); return 1; }
        tests++;
    }
    printf("ok %llu\n", tests);
}
