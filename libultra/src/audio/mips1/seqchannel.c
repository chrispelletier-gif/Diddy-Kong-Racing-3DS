#include <libaudio.h>
#include "plat/plat.h"
#ifdef PICA_ALL
int port_music_post(void *seqp, ALEvent *evt);
#endif

void alSeqChOff(ALCSPlayer *seqp, u8 chan) {
    ALEvent evt;

    evt.type = AL_SEQP_MIDI_EVT;
    evt.msg.midi.ticks  = 0;
    evt.msg.midi.status = AL_MIDI_ControlChange;
    evt.msg.midi.byte1  = AL_MIDI_UNK_6A;
    evt.msg.midi.byte2  = chan;
    #ifdef PICA_ALL
    if (port_music_post(seqp, &evt)) {
        return; // the native music player (port/src/audio/music.c)
    }
#endif
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}

void alSeqChOn(ALCSPlayer *seqp, u8 chan) {
    ALEvent evt;

    evt.type = AL_SEQP_MIDI_EVT;
    evt.msg.midi.ticks  = 0;
    evt.msg.midi.status = AL_MIDI_ControlChange;
    evt.msg.midi.byte1  = AL_MIDI_UNK_6C;
    evt.msg.midi.byte2  = chan;
    #ifdef PICA_ALL
    if (port_music_post(seqp, &evt)) {
        return; // the native music player (port/src/audio/music.c)
    }
#endif
    alEvtqPostEvent(&seqp->evtq, &evt, 0);
}
