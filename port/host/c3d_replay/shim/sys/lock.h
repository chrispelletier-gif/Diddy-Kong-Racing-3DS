/* newlib's devkitARM lock types, which libctru's synchronization.h includes; the replay never takes a lock. */
#ifndef REPLAY_SYS_LOCK_H
#define REPLAY_SYS_LOCK_H
#include <stdint.h>
typedef int32_t _LOCK_T;
typedef struct __lock_t {
    _LOCK_T lock;
    uint32_t thread_tag;
    uint32_t counter;
} _LOCK_RECURSIVE_T;
typedef uint32_t _COND_T;
#endif
