/* The backend replay (replay.c) and its stubs. */
#ifndef PORT_C3D_REPLAY_H
#define PORT_C3D_REPLAY_H

#include <stdint.h>

void replay_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int replay_old_3ds(void);
/* A command list the backend handed to the GPU (GX_ProcessCommandList): decoded, and its draws written out. */
void replay_command_list(const uint32_t *words, uint32_t count);
/* Any other GPU job (memory fill, display transfer, texture copy), written to the words output as one line. */
void replay_gx_job(const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                   uint32_t h);
/* The replay's memory at a physical address (stubs_ctru.c), NULL when [phys, phys + bytes) is not all in one arena. */
void *replay_phys_to_virt(uint32_t phys, uint32_t bytes);

#endif
