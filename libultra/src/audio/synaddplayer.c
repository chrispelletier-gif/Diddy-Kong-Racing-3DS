/*====================================================================
 * synaddplayer.c
 *
 * Copyright 1995, Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Silicon Graphics,
 * Inc.; the contents of this file may not be disclosed to third
 * parties, copied or duplicated in any form, in whole or in part,
 * without the prior written permission of Silicon Graphics, Inc.
 *
 * RESTRICTED RIGHTS LEGEND:
 * Use, duplication or disclosure by the Government is subject to
 * restrictions as set forth in subdivision (c)(1)(ii) of the Rights
 * in Technical Data and Computer Software clause at DFARS
 * 252.227-7013, and/or in similar or successor clauses in the FAR,
 * DOD or NASA FAR Supplement. Unpublished - rights reserved under the
 * Copyright Laws of the United States.
 *====================================================================*/

#include "synthInternals.h"
#include <os_internal.h>
#include "plat/plat.h"
#ifdef PICA_ALL
/* The sound code's interrupt-disabled sections take the sound lock instead (port/src/os/audio_lock.c). */
OSIntMask port_audio_set_mask(OSIntMask mask);
#define osSetIntMask port_audio_set_mask
#endif

void alSynAddPlayer(ALSynth *drvr, ALPlayer *client)
{
    OSIntMask mask = osSetIntMask(OS_IM_NONE);

    client->samplesLeft = drvr->curSamples;
    client->next = drvr->head;
    drvr->head   = client;

    osSetIntMask(mask);
}

