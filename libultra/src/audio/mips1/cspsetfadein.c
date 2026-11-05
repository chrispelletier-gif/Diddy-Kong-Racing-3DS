#include <libaudio.h>
#include "plat/plat.h"
#ifdef PICA_ALL
int port_music_post(void *seqp, ALEvent *evt);
#endif

void alCSPSetFadeIn(ALCSPlayer *seqp, u8 chan, ALPan pan) {
    ALEvent       evt;

    evt.type            = AL_SEQP_MIDI_EVT;
    evt.msg.midi.ticks  = 0;
    evt.msg.midi.status = AL_MIDI_ControlChange | chan;
    evt.msg.midi.byte1  = AL_SEQP_PROG_EVT;
    evt.msg.midi.byte2  = pan;

    #ifdef PICA_ALL
    if (port_music_post(seqp, &evt)) {
        return; // the native music player (port/src/audio/music.c)
    }
#endif
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}
