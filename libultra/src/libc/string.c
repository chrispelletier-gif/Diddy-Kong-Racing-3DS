#include "PR/ultratypes.h"
#include "string.h"

// TODO: this comes from a header
#ident "$Revision: 1.23 $"

#if defined(TARGET_PORT) && defined(__arm__)
/*
 * The port's memcpy for the whole program (this file's replaces the C library's). The N64 version copies one byte at a
 * time, forwards, which is also what it does to overlapping regions; that is kept exactly: when the destination starts
 * inside the source, the bytes are copied one at a time forwards. Everything else is copied forwards in blocks of 32
 * bytes with the ARM11's load and store multiple (both 4-byte aligned), else words, which leaves the same bytes behind
 * (a forward copy to a lower address can't overwrite bytes it hasn't read yet).
 */
int gPortOldMemcpy; // DKR_OLD_MEMCPY=1: the N64's byte at a time copy (set each frame, port_game_native_frame)

void* memcpy(void* s1, const void* s2, size_t n) {
    unsigned char* d = (unsigned char*)s1;
    const unsigned char* s = (const unsigned char*)s2;
    if (gPortOldMemcpy || (s < d && d < s + n)) {
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
        return s1;
    }
    if (n >= 8 && (((unsigned int)d ^ (unsigned int)s) & 3) == 0) {
        while (((unsigned int)d & 3) != 0) {
            *d++ = *s++;
            n--;
        }
        while (n >= 32) {
            __asm__ volatile("ldmia %0!, {r3, r4, r5, r6, r7, r8, r9, r12}\n\t"
                             "stmia %1!, {r3, r4, r5, r6, r7, r8, r9, r12}"
                             : "+r"(s), "+r"(d)
                             :
                             : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r12", "memory");
            n -= 32;
        }
        while (n >= 4) {
            *(unsigned int*)d = *(const unsigned int*)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    } else {
        // Different alignments: words read and written unaligned (the ARM11 does that in hardware).
        while (n >= 4) {
            unsigned int w;
            __builtin_memcpy(&w, s, 4);
            __builtin_memcpy(d, &w, 4);
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n > 0) {
        *d++ = *s++;
        n--;
    }
    return s1;
}
#else
void* memcpy(void* s1, const void* s2, size_t n) {
    char* su1 = (char*)s1;
    const char* su2 = (const char*)s2;
    while (n > 0) {
        *su1 = *su2;
        su1++;
        su2++;
        n--;
    }
    return (void*)s1;
}
#endif

size_t strlen(const char* s) {
    const char* sc = s;
    while (*sc != 0) {
        sc++;
    }
    return sc - s;
}

char* strchr(const char* s, int c) {
    const char ch = c;
    while (*s != ch) {
        if (*s == 0) {
            return NULL;
        }
        s++;
    }
    return (char*)s;
}
