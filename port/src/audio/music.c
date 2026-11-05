/*
 * The native music player (PICA_ALL test build, DKR_NATIVE_MUSIC, on by default there).
 *
 * Replaces libultra's compact sequence player (csplayer.c, cseq.c, event.c's queue and the seqplayer.c voice helpers)
 * for the game's two players (music and jingles), behind the same alCSP and alCSeq calls the game makes:
 *
 * - The sequence is decoded once, when the game hands it over (alCSeqNew): each track's compact MIDI byte stream
 *   (running status, backup blocks, variable-length deltas, loop markers with their byte offsets) becomes a table of
 *   ready records, and loops become jumps to a record. Playing reads the next record of the earliest track.
 * - The player's timeline is a time-ordered array of 64-bit microsecond times instead of libultra's linked list of
 *   relative deltas guarded by the N64's interrupt mask; the game thread and the sound thread share it through a
 *   lock held only for the queue operations themselves.
 *
 * What it plays is meant to be exactly the old player's: same events at the same times in the same order, same voice
 * choices, same volumes. DKR_SYNTH_TRACE=<file> (voices.c) logs every voice change for a comparison with
 * DKR_NATIVE_MUSIC=0. A sequence the decoder can't turn into records plays through the old reader instead.
 */
#include <ultra64.h>
#include <stdlib.h>
#include "synthInternals.h"
#include "seqp.h"
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#ifdef PICA_ALL

/* libultra's reader, for a sequence the decoder refused. */
void alCSeqNextEvent(ALCSeq *seq, ALEvent *evt);
char __alCSeqNextDelta(ALCSeq *seq, s32 *pDeltaTicks);
void func_80065A80(ALSynth *arg0, struct PVoice_s *arg1, s16 arg2);
int port_lockstep_enabled(void);

/******************************** Decoded sequences ********************************/

enum { REC_MIDI, REC_TEMPO, REC_EOT, REC_LOOPSTART, REC_LOOPEND };

typedef struct {
    u32 delta;          /* ticks from the track's previous event */
    u8 kind;            /* REC_* */
    u8 status, b1, b2;  /* MIDI status and data bytes; for a tempo, b1..b3 are the tempo bytes */
    u8 b3;
    u32 duration;       /* note on: ticks to the note off */
    u8 *loop;           /* loop end: its loop count bytes, counted down in the sequence data as on the N64 */
    s32 jump;           /* loop end: the record the loop goes back to */
} Rec;

#define MAX_SEQS 4
#define MAX_TRACK_RECS 32768

typedef struct {
    ALCSeq *seq;
    u8 *base;
    s32 ok;
    Rec *rec[16];
    s32 count[16];
    s32 idx[16];   /* the track's pending record */
    u32 next[16];  /* the pending record's tick time */
    u32 valid;     /* tracks still playing */
    u32 lastTicks; /* tick time of the last event */
} NSeq;

static NSeq sSeqs[MAX_SEQS];
static s32 sMusicOn = -1;

int port_music_native_on(void) {
    if (sMusicOn < 0) {
        sMusicOn = plat_switch("DKR_NATIVE_MUSIC", 1) != 0;
    }
    return sMusicOn;
}

static NSeq *seq_find(ALCSeq *seq) {
    s32 i;
    for (i = 0; i < MAX_SEQS; i++) {
        if (sSeqs[i].seq == seq) {
            return &sSeqs[i];
        }
    }
    return NULL;
}

/* The compact format's byte reader (cseq.c __getTrackByte), with its backup blocks. */
typedef struct {
    u8 *loc;
    u8 *buPtr;
    u32 buLen;
} Reader;

static u8 rd_byte(Reader *r) {
    u8 b;
    if (r->buLen) {
        b = *r->buPtr++;
        r->buLen--;
        return b;
    }
    b = *r->loc++;
    if (b == AL_CMIDI_BLOCK_CODE) {
        u8 next = *r->loc++;
        if (next != AL_CMIDI_BLOCK_CODE) {
            u32 backup = ((u32) next << 8) + *r->loc++;
            u8 len = *r->loc++;
            r->buPtr = r->loc - (backup + 4);
            r->buLen = len;
            b = *r->buPtr++;
            r->buLen--;
        }
    }
    return b;
}

static u32 rd_varlen(Reader *r) {
    u32 value = rd_byte(r), c;
    if (value & 0x80) {
        value &= 0x7F;
        do {
            c = rd_byte(r);
            value = (value << 7) + (c & 0x7F);
        } while (c & 0x80);
    }
    return value;
}

/* Decodes one track; FALSE when it uses something the records can't express (the old reader plays it then). */
static s32 decode_track(NSeq *ns, s32 t, u8 *start) {
    static u8 *sPos[MAX_TRACK_RECS];
    static u8 sRunning[MAX_TRACK_RECS]; /* the record took its status from the running status */
    static u8 sStatusBefore[MAX_TRACK_RECS];
    static u8 *sTarget[MAX_TRACK_RECS];
    Reader r;
    Rec *recs;
    s32 n = 0, cap = 256, i, k;
    u8 lastStatus = 0;

    r.loc = start;
    r.buPtr = NULL;
    r.buLen = 0;
    recs = (Rec *) malloc(sizeof(Rec) * cap);
    if (recs == NULL) {
        return FALSE;
    }
    for (;;) {
        Rec *rec;
        u8 status;
        if (n == MAX_TRACK_RECS) {
            free(recs);
            return FALSE;
        }
        if (n == cap) {
            Rec *more = (Rec *) realloc(recs, sizeof(Rec) * cap * 2);
            if (more == NULL) {
                free(recs);
                return FALSE;
            }
            recs = more;
            cap *= 2;
        }
        rec = &recs[n];
        __builtin_memset(rec, 0, sizeof(*rec));
        sPos[n] = (r.buLen == 0) ? r.loc : NULL;
        sRunning[n] = FALSE;
        sStatusBefore[n] = lastStatus;
        sTarget[n] = NULL;
        rec->delta = rd_varlen(&r);
        status = rd_byte(&r);
        if (status == AL_MIDI_Meta) {
            u8 type = rd_byte(&r);
            if (type == AL_MIDI_META_TEMPO) {
                rec->kind = REC_TEMPO;
                rec->b1 = rd_byte(&r);
                rec->b2 = rd_byte(&r);
                rec->b3 = rd_byte(&r);
                lastStatus = 0;
            } else if (type == AL_MIDI_META_EOT) {
                rec->kind = REC_EOT;
                n++;
                break;
            } else if (type == AL_CMIDI_LOOPSTART_CODE) {
                rd_byte(&r);
                rd_byte(&r);
                lastStatus = 0;
                rec->kind = REC_LOOPSTART;
            } else if (type == AL_CMIDI_LOOPEND_CODE) {
                u8 *p = r.loc;
                u32 offset;
                if (r.buLen != 0) {
                    free(recs); // the old reader takes the loop bytes from the stream, not the backup block
                    return FALSE;
                }
                offset = ((u32) p[2] << 24) | ((u32) p[3] << 16) | ((u32) p[4] << 8) | p[5];
                rec->kind = REC_LOOPEND;
                rec->loop = p;
                sTarget[n] = p + 6 - offset;
                r.loc = p + 6;
                lastStatus = 0;
            } else {
                free(recs); // a meta event the old reader doesn't know either
                return FALSE;
            }
        } else {
            rec->kind = REC_MIDI;
            if (status & 0x80) {
                rec->status = status;
                rec->b1 = rd_byte(&r);
                lastStatus = status;
            } else {
                rec->status = lastStatus;
                rec->b1 = status;
                sRunning[n] = TRUE;
            }
            if ((rec->status & 0xF0) != AL_MIDI_ProgramChange && (rec->status & 0xF0) != AL_MIDI_ChannelPressure) {
                rec->b2 = rd_byte(&r);
                if ((rec->status & 0xF0) == AL_MIDI_NoteOn) {
                    rec->duration = rd_varlen(&r);
                }
            }
        }
        n++;
    }
    // Loops: each loop end goes back to the record whose delta starts at its target. After the jump the running status
    // is cleared, so a target that leans on the running status left by the record before it doesn't decode the same.
    for (i = 0; i < n; i++) {
        if (recs[i].kind != REC_LOOPEND) {
            continue;
        }
        recs[i].jump = -1;
        for (k = 0; k < n; k++) {
            if (sPos[k] == sTarget[i]) {
                recs[i].jump = k;
                break;
            }
        }
        if (recs[i].jump < 0 || (sRunning[k] && sStatusBefore[k] != 0)) {
            free(recs);
            return FALSE;
        }
    }
    ns->rec[t] = recs;
    ns->count[t] = n;
    return TRUE;
}

static void seq_free(NSeq *ns) {
    s32 t;
    for (t = 0; t < 16; t++) {
        free(ns->rec[t]);
        ns->rec[t] = NULL;
        ns->count[t] = 0;
    }
    ns->ok = FALSE;
}

/* After libultra's alCSeqNew has set up the ALCSeq (the game reads its qnpt through alCSPGetTempo). */
void port_music_seq_new(ALCSeq *seq, u8 *ptr) {
    NSeq *ns;
    s32 t;

    if (!port_music_native_on()) {
        return;
    }
    ns = seq_find(seq);
    if (ns == NULL) {
        ns = seq_find(NULL);
        if (ns == NULL) {
            return;
        }
    }
    seq_free(ns);
    ns->seq = seq;
    ns->base = ptr;
    ns->valid = 0;
    ns->lastTicks = 0;
    for (t = 0; t < 16; t++) {
        u32 off = seq->base->trackOffset[t];
        ns->idx[t] = 0;
        if (off == 0) {
            continue;
        }
        if (!decode_track(ns, t, ptr + off)) {
            plat_log("music: sequence track %d not decoded, the old reader plays it", (int) t);
            seq_free(ns);
            return;
        }
        ns->valid |= 1 << t;
        ns->next[t] = ns->rec[t][0].delta;
    }
    ns->ok = TRUE;
}

/* __alCSeqNextDelta */
static s32 seq_next_delta(ALCSeq *seq, s32 *deltaTicks) {
    NSeq *ns = seq_find(seq);
    u32 first = 0xFFFFFFFF;
    s32 t;

    if (ns == NULL || !ns->ok) {
        return __alCSeqNextDelta(seq, deltaTicks);
    }
    if (!ns->valid) {
        return FALSE;
    }
    for (t = 0; t < 16; t++) {
        if ((ns->valid >> t) & 1) {
            u32 rel = ns->next[t] - ns->lastTicks;
            if (rel < first) {
                first = rel;
            }
        }
    }
    *deltaTicks = (s32) first;
    return TRUE;
}

/* alCSeqNextEvent */
static void seq_next_event(ALCSeq *seq, ALEvent *evt) {
    NSeq *ns = seq_find(seq);
    u32 first = 0xFFFFFFFF, now;
    s32 t, track = 0, nextIdx;
    Rec *rec;

    if (ns == NULL || !ns->ok) {
        alCSeqNextEvent(seq, evt);
        return;
    }
    for (t = 0; t < 16; t++) {
        if ((ns->valid >> t) & 1) {
            u32 rel = ns->next[t] - ns->lastTicks;
            if (rel < first) {
                first = rel;
                track = t;
            }
        }
    }
    rec = &ns->rec[track][ns->idx[track]];
    now = ns->lastTicks + first;
    nextIdx = ns->idx[track] + 1;
    switch (rec->kind) {
        case REC_MIDI:
            evt->type = AL_SEQ_MIDI_EVT;
            evt->msg.midi.status = rec->status;
            evt->msg.midi.byte1 = rec->b1;
            evt->msg.midi.byte2 = rec->b2;
            evt->msg.midi.duration = rec->duration;
            break;
        case REC_TEMPO:
            evt->type = AL_TEMPO_EVT;
            evt->msg.tempo.status = AL_MIDI_Meta;
            evt->msg.tempo.type = AL_MIDI_META_TEMPO;
            evt->msg.tempo.byte1 = rec->b1;
            evt->msg.tempo.byte2 = rec->b2;
            evt->msg.tempo.byte3 = rec->b3;
            break;
        case REC_EOT:
            ns->valid ^= 1 << track;
            evt->type = ns->valid ? AL_TRACK_END : AL_SEQ_END_EVT;
            break;
        case REC_LOOPSTART:
            evt->type = AL_CSP_LOOPSTART;
            break;
        default: { // REC_LOOPEND: the loop count lives in the sequence data, as on the N64
            u8 *p = rec->loop;
            if (p[1] == 0) {
                p[1] = p[0]; // done looping: reset the count, go on past the loop end
            } else {
                if (p[1] != 0xFF) {
                    p[1]--;
                }
                nextIdx = rec->jump;
            }
            evt->type = AL_CSP_LOOPEND;
            break;
        }
    }
    evt->msg.midi.ticks = (s32) first;
    ns->lastTicks = now;
    seq->lastTicks += first;
    seq->lastDeltaTicks = first;
    if (rec->kind != REC_EOT) {
        ns->idx[track] = nextIdx;
        ns->next[track] = now + ns->rec[track][nextIdx].delta;
    }
}

/******************************** The player's timeline ********************************/

#define MAX_PLAYERS 4
#define QUEUE_CAP 1024

typedef struct {
    s64 time;
    ALEvent evt;
} Item;

typedef struct {
    ALCSPlayer_Custom *seqp;
    Item items[QUEUE_CAP];
    s32 head, count;
    s64 clock; /* the time of the event in seqp->nextEvent: what a delta is counted from */
    u32 dropped;
} NPlayer;

static NPlayer sPlayers[MAX_PLAYERS];
static PlatSem *sLock;

static void q_lock(void) {
    plat_sem_wait(sLock);
}
static void q_unlock(void) {
    plat_sem_post(sLock);
}

static NPlayer *player_find(void *seqp) {
    s32 i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (sPlayers[i].seqp == seqp) {
            return &sPlayers[i];
        }
    }
    return NULL;
}

static Item *q_at(NPlayer *np, s32 i) {
    return &np->items[np->head + i];
}

/* Removes entry i of the queue. */
static void q_remove(NPlayer *np, s32 i) {
    __builtin_memmove(q_at(np, i), q_at(np, i + 1), sizeof(Item) * (np->count - i - 1));
    np->count--;
}

/* alEvtqPostEvent: after any event of the same time. AL_EVTQ_END goes at the time of the last one. */
static void q_post_locked(NPlayer *np, const ALEvent *evt, ALMicroTime delta) {
    s64 time;
    s32 i;

    if (np->count == QUEUE_CAP) {
        np->dropped++;
        return;
    }
    if (np->head + np->count == QUEUE_CAP) {
        __builtin_memmove(np->items, q_at(np, 0), sizeof(Item) * np->count);
        np->head = 0;
    }
    if (delta == AL_EVTQ_END) {
        time = np->count ? q_at(np, np->count - 1)->time : np->clock;
    } else {
        time = np->clock + delta;
    }
    for (i = np->count; i > 0 && q_at(np, i - 1)->time > time; i--) {
    }
    __builtin_memmove(q_at(np, i + 1), q_at(np, i), sizeof(Item) * (np->count - i));
    q_at(np, i)->time = time;
    q_at(np, i)->evt = *evt;
    np->count++;
}

static void q_post(NPlayer *np, const ALEvent *evt, ALMicroTime delta) {
    q_lock();
    q_post_locked(np, evt, delta);
    q_unlock();
}

/* alEvtqNextEvent: the delta from the last event, type -1 when empty. */
static ALMicroTime q_next(NPlayer *np, ALEvent *evt) {
    ALMicroTime delta = 0;
    q_lock();
    if (np->count == 0) {
        evt->type = -1;
    } else {
        Item *it = q_at(np, 0);
        *evt = it->evt;
        delta = (ALMicroTime) (it->time - np->clock);
        np->clock = it->time;
        np->head++;
        np->count--;
        if (np->count == 0) {
            np->head = 0;
        }
    }
    q_unlock();
    return delta;
}

static void q_flush_type(NPlayer *np, s16 type) {
    s32 i;
    q_lock();
    for (i = 0; i < np->count;) {
        if (q_at(np, i)->evt.type == type) {
            q_remove(np, i);
        } else {
            i++;
        }
    }
    q_unlock();
}

/******************************** Voices and channels (seqplayer.c's helpers) ********************************/

#define CHAN(seqp, c) ((seqp)->chanState[c])

static ALVoiceState *map_voice(ALCSPlayer_Custom *seqp, u8 key, u8 vel, u8 channel) {
    ALVoiceState *vs = seqp->vFreeList;
    if (seqp->voiceLimit < seqp->mappedVoices) {
        return NULL;
    }
    if (vs) {
        seqp->vFreeList = vs->next;
        vs->next = 0;
        if (!seqp->vAllocHead) {
            seqp->vAllocHead = vs;
        } else {
            seqp->vAllocTail->next = vs;
        }
        seqp->vAllocTail = vs;
        vs->channel = channel;
        vs->key = key;
        vs->velocity = vel;
        vs->voice.clientPrivate = vs;
        seqp->mappedVoices++;
    }
    return vs;
}

static void unmap_voice(ALCSPlayer_Custom *seqp, ALVoice *voice) {
    ALVoiceState *prev = 0, *vs;
    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
        if (&vs->voice == voice) {
            if (prev) {
                prev->next = vs->next;
            } else {
                seqp->vAllocHead = vs->next;
            }
            if (vs == seqp->vAllocTail) {
                seqp->vAllocTail = prev;
            }
            vs->next = seqp->vFreeList;
            seqp->vFreeList = vs;
            seqp->mappedVoices--;
            return;
        }
        prev = vs;
    }
}

static ALVoiceState *lookup_voice(ALCSPlayer_Custom *seqp, u8 key, u8 channel) {
    ALVoiceState *vs;
    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
        if (vs->key == key && vs->channel == channel && vs->phase != AL_PHASE_RELEASE &&
            vs->phase != AL_PHASE_SUSTREL) {
            return vs;
        }
    }
    return 0;
}

static ALSound *lookup_sound(ALCSPlayer_Custom *seqp, u8 key, u8 vel, u8 chan) {
    ALInstrument *inst = CHAN(seqp, chan).instrument;
    s32 l = 1, r = inst->soundCount, i;
    while (r >= l) {
        ALKeyMap *keymap;
        i = (l + r) / 2;
        keymap = inst->soundArray[i - 1]->keyMap;
        if (key >= keymap->keyMin && key <= keymap->keyMax && vel >= keymap->velocityMin &&
            vel <= keymap->velocityMax) {
            return inst->soundArray[i - 1];
        } else if (key < keymap->keyMin || (vel < keymap->velocityMin && key <= keymap->keyMax)) {
            r = i - 1;
        } else {
            l = i + 1;
        }
    }
    return 0;
}

static s16 vs_vol(ALVoiceState *vs, ALCSPlayer_Custom *seqp) {
    u32 t1 = (vs->tremelo * vs->velocity * vs->envGain) >> 6;
    u32 t2 = (vs->sound->sampleVolume * seqp->vol * CHAN(seqp, vs->channel).vol) >> 14;
    t1 *= t2;
    t1 >>= 15;
    return (s16) t1;
}

static ALMicroTime vs_delta(ALVoiceState *vs, ALMicroTime t) {
    s32 delta = vs->envEndTime - t;
    return (delta >= 0) ? delta : AL_GAIN_CHANGE_TIME;
}

static ALPan vs_pan(ALVoiceState *vs, ALCSPlayer_Custom *seqp) {
    s32 tmp = CHAN(seqp, vs->channel).pan - AL_PAN_CENTER + vs->sound->samplePan;
    tmp = MAX(tmp, AL_PAN_LEFT);
    tmp = MIN(tmp, AL_PAN_RIGHT);
    return (ALPan) tmp;
}

static void release_voice(NPlayer *np, ALVoice *voice, ALMicroTime deltaTime) {
    ALCSPlayer_Custom *seqp = np->seqp;
    ALVoiceState *vs = (ALVoiceState *) voice->clientPrivate;
    ALEvent evt;

    if (vs->envPhase == AL_PHASE_ATTACK) {
        // Its pending envelope steps go.
        s32 i;
        q_lock();
        for (i = 0; i < np->count;) {
            Item *it = q_at(np, i);
            if (it->evt.type == AL_SEQP_ENV_EVT && it->evt.msg.vol.voice == voice) {
                q_remove(np, i);
            } else {
                i++;
            }
        }
        q_unlock();
    }
    vs->velocity = 0;
    vs->envPhase = AL_PHASE_RELEASE;
    vs->envGain = 0;
    vs->envEndTime = seqp->curTime + deltaTime;
    alSynSetPriority(seqp->drvr, voice, 0);
    alSynSetVol(seqp->drvr, voice, 0, deltaTime);
    evt.type = AL_NOTE_END_EVT;
    evt.msg.note.voice = voice;
    q_post(np, &evt, deltaTime);
}

/* __voiceNeedsNoteKill: a voice whose note end comes after killTime loses that note end and must be killed. */
static s32 voice_needs_kill(NPlayer *np, ALVoice *voice, ALMicroTime killTime) {
    s32 i, needs = TRUE;
    q_lock();
    for (i = 0; i < np->count; i++) {
        Item *it = q_at(np, i);
        if (it->evt.type == AL_NOTE_END_EVT && it->evt.msg.note.voice == voice) {
            if ((ALMicroTime) (it->time - np->clock) > killTime) {
                q_remove(np, i);
            } else {
                needs = FALSE;
            }
            break;
        }
    }
    q_unlock();
    return needs;
}

static void set_inst_chan(ALCSPlayer_Custom *seqp, ALInstrument *inst, s32 chan) {
    CHAN(seqp, chan).instrument = inst;
    CHAN(seqp, chan).pan = inst->pan;
    CHAN(seqp, chan).vol = inst->volume;
    CHAN(seqp, chan).priority = inst->priority;
    CHAN(seqp, chan).bendRange = inst->bendRange;
}

static void reset_chan(ALCSPlayer_Custom *seqp, s32 chan) {
    CHAN(seqp, chan).fxId = AL_FX_NONE;
    CHAN(seqp, chan).fxmix = AL_DEFAULT_FXMIX;
    CHAN(seqp, chan).pan = AL_PAN_CENTER;
    CHAN(seqp, chan).vol = AL_VOL_FULL;
    CHAN(seqp, chan).fade = AL_VOL_FULL;
    CHAN(seqp, chan).priority = AL_DEFAULT_PRIORITY;
    CHAN(seqp, chan).sustain = 0;
    CHAN(seqp, chan).bendRange = 200;
    CHAN(seqp, chan).pitchBend = 1.0f;
}

static void init_from_bank(ALCSPlayer_Custom *seqp, ALBank *b) {
    ALInstrument *inst = 0;
    s32 i;
    for (i = 0; !inst; i++) {
        inst = b->instArray[i];
    }
    for (i = 0; i < seqp->maxChannels; i++) {
        reset_chan(seqp, i);
        set_inst_chan(seqp, inst, i);
    }
    if (b->percussion) {
        reset_chan(seqp, i); // one past the channels, as libultra does (the channel table has room for it)
        set_inst_chan(seqp, b->percussion, 9);
    }
}

/******************************** Playing ********************************/

static void post_next_seq_event(NPlayer *np) {
    ALCSPlayer_Custom *seqp = np->seqp;
    ALEvent evt;
    s32 deltaTicks;

    if (seqp->state != AL_PLAYING || seqp->target == NULL) {
        return;
    }
    if (!seq_next_delta(seqp->target, &deltaTicks)) {
        return;
    }
    evt.type = AL_SEQ_REF_EVT;
    q_post(np, &evt, deltaTicks * seqp->uspt);
}

static void handle_midi(NPlayer *np, ALEvent *event) {
    ALCSPlayer_Custom *seqp = np->seqp;
    ALMIDIEvent *midi = &event->msg.midi;
    ALVoiceState *vs, *vstate;
    ALVoice *voice;
    ALVoiceConfig config;
    ALSound *sound;
    ALInstrument *inst;
    ALEvent evt;
    ALMicroTime deltaTime;
    f32 pitch, oscValue;
    s16 vol;
    ALPan pan;
    s32 i, status = midi->status & AL_MIDI_StatusMask;
    u8 chan = midi->status & AL_MIDI_ChannelMask, byte1 = midi->byte1, byte2 = midi->byte2, fxmix;

    if (status == AL_MIDI_ChannelModeSelect) {
        if (byte1 == 0x6A) { // the game's channel off: its notes released
            seqp->chanMask &= ~(1 << byte2);
            for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                if (vs->channel == byte2) {
                    release_voice(np, &vs->voice, vs->sound->envelope->releaseTime);
                }
            }
            return;
        } else if (byte1 == 0x6C) { // channel on
            seqp->chanMask |= 1 << byte2;
            return;
        }
    }
    if (!(seqp->chanMask & (1 << chan)) && status != AL_MIDI_ProgramChange && status != AL_MIDI_ControlChange &&
        status != AL_MIDI_PitchBendChange) {
        return;
    }

    switch (status) {
        case AL_MIDI_NoteOn:
            if (byte2 != 0) {
                if (seqp->state != AL_PLAYING) {
                    break;
                }
                sound = lookup_sound(seqp, byte1, byte2, chan);
                if (!sound) {
                    return;
                }
                config.priority = CHAN(seqp, chan).priority;
                config.fxBus = 0;
                config.unityPitch = 0;
                vstate = map_voice(seqp, byte1, byte2, chan);
                if (!vstate) {
                    return;
                }
                voice = &vstate->voice;
                if (alSynAllocVoice(seqp->drvr, voice, &config)) {
                    func_80065A80(seqp->drvr, voice->pvoice, 0);
                }
                vstate->sound = sound;
                vstate->envPhase = AL_PHASE_ATTACK;
                vstate->phase = (CHAN(seqp, chan).sustain > AL_SUSTAIN) ? AL_PHASE_SUSTAIN : AL_PHASE_NOTEON;
                vstate->pitch =
                    alCents2Ratio((s16) ((byte1 - sound->keyMap->keyBase) * 100 + sound->keyMap->detune));
                vstate->envGain = sound->envelope->attackVolume;
                vstate->envEndTime = seqp->curTime + sound->envelope->attackTime;
                // No tremolo or vibrato: the game gives its players no oscillators (only those use them).
                vstate->flags = 0;
                oscValue = (f32) AL_VOL_FULL;
                vstate->tremelo = (u8) oscValue;
                vstate->vibrato = 1.0f;

                pitch = vstate->pitch * CHAN(seqp, chan).pitchBend * vstate->vibrato;
                fxmix = CHAN(seqp, chan).fxmix;
                pan = vs_pan(vstate, seqp);
                vol = vs_vol(vstate, seqp);
                deltaTime = sound->envelope->attackTime;
                alSynStartVoiceParams(seqp->drvr, voice, sound->wavetable, pitch, vol, pan, fxmix, deltaTime);

                evt.type = AL_SEQP_ENV_EVT;
                evt.msg.vol.voice = voice;
                evt.msg.vol.vol = sound->envelope->decayVolume;
                evt.msg.vol.delta = sound->envelope->decayTime;
                q_post(np, &evt, deltaTime);

                if (midi->duration) {
                    evt.type = AL_CSP_NOTEOFF_EVT;
                    evt.msg.midi.status = chan | AL_MIDI_NoteOff;
                    evt.msg.midi.byte1 = byte1;
                    evt.msg.midi.byte2 = 0;
                    q_post(np, &evt, seqp->uspt * midi->duration);
                }
                break;
            }
            // fall through: a note on with no velocity is a note off
        case AL_MIDI_NoteOff:
            vstate = lookup_voice(seqp, byte1, chan);
            if (!vstate) {
                return;
            }
            if (vstate->phase == AL_PHASE_SUSTAIN) {
                vstate->phase = AL_PHASE_SUSTREL;
            } else {
                vstate->phase = AL_PHASE_RELEASE;
                release_voice(np, &vstate->voice, vstate->sound->envelope->releaseTime);
            }
            break;

        case AL_MIDI_PolyKeyPressure:
            vstate = lookup_voice(seqp, byte1, chan);
            if (!vstate) {
                return;
            }
            vstate->velocity = byte2;
            alSynSetVol(seqp->drvr, &vstate->voice, vs_vol(vstate, seqp), vs_delta(vstate, seqp->curTime));
            break;

        case AL_MIDI_ChannelPressure:
            for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                if (vs->channel == chan) {
                    vs->velocity = byte1;
                    alSynSetVol(seqp->drvr, &vs->voice, vs_vol(vs, seqp), vs_delta(vs, seqp->curTime));
                }
            }
            break;

        case AL_MIDI_ControlChange:
            switch (byte1) {
                case AL_MIDI_PAN_CTRL:
                    CHAN(seqp, chan).pan = byte2;
                    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                        if (vs->channel == chan) {
                            alSynSetPan(seqp->drvr, &vs->voice, vs_pan(vs, seqp));
                        }
                    }
                    break;
                case AL_MIDI_VOLUME_CTRL:
                    CHAN(seqp, chan).unk11 = byte2;
                    CHAN(seqp, chan).vol = ((f32) CHAN(seqp, chan).fade * byte2) / 127.0f;
                    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                        if (vs->channel == chan && vs->envPhase != AL_PHASE_RELEASE) {
                            alSynSetVol(seqp->drvr, &vs->voice, vs_vol(vs, seqp), vs_delta(vs, seqp->curTime));
                        }
                    }
                    break;
                case AL_MIDI_UNK_8: // the game's fade-in level
                    CHAN(seqp, chan).fade = byte2;
                    CHAN(seqp, chan).vol = (CHAN(seqp, chan).unk11 * CHAN(seqp, chan).fade) / 127.0f;
                    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                        if (vs->channel == chan && vs->envPhase != AL_PHASE_RELEASE) {
                            alSynSetVol(seqp->drvr, &vs->voice, vs_vol(vs, seqp), vs_delta(vs, seqp->curTime));
                        }
                    }
                    break;
                case AL_MIDI_PRIORITY_CTRL:
                    CHAN(seqp, chan).priority = byte2;
                    break;
                case AL_MIDI_SUSTAIN_CTRL:
                    CHAN(seqp, chan).sustain = byte2;
                    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                        if (vs->channel == chan && vs->phase != AL_PHASE_RELEASE) {
                            if (byte2 > AL_SUSTAIN) {
                                if (vs->phase == AL_PHASE_NOTEON) {
                                    vs->phase = AL_PHASE_SUSTAIN;
                                }
                            } else if (vs->phase == AL_PHASE_SUSTAIN) {
                                vs->phase = AL_PHASE_NOTEON;
                            } else if (vs->phase == AL_PHASE_SUSTREL) {
                                vs->phase = AL_PHASE_RELEASE;
                                release_voice(np, &vs->voice, vs->sound->envelope->releaseTime);
                            }
                        }
                    }
                    break;
                case AL_MIDI_FX1_CTRL:
                    CHAN(seqp, chan).fxmix = byte2;
                    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                        if (vs->channel == chan) {
                            alSynSetFXMix(seqp->drvr, &vs->voice, byte2);
                        }
                    }
                    break;
                case AL_MIDI_UNK_5F: // the game's effect mix ceiling
                    seqp->unk36 = byte2;
                    for (i = 0; i < seqp->maxChannels; i++) {
                        if (CHAN(seqp, i).fxmix < byte2) {
                            CHAN(seqp, i).fxmix = 0;
                            for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                                if (vs->channel == i) {
                                    alSynSetFXMix(seqp->drvr, &vs->voice, byte2);
                                }
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
            break;

        case AL_MIDI_ProgramChange:
            if (byte1 < seqp->bank->instCount) {
                inst = seqp->bank->instArray[byte1];
                set_inst_chan(seqp, inst, chan);
            }
            break;

        case AL_MIDI_PitchBendChange: {
            s32 bendVal = ((byte2 << 7) + byte1) - 8192;
            f32 bendRatio = alCents2Ratio((CHAN(seqp, chan).bendRange * bendVal) / 8192);
            CHAN(seqp, chan).pitchBend = bendRatio;
            for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                if (vs->channel == chan) {
                    alSynSetPitch(seqp->drvr, &vs->voice, vs->pitch * bendRatio * vs->vibrato);
                }
            }
            break;
        }
        default:
            break;
    }
}

static void set_uspt(ALCSPlayer_Custom *seqp, f32 tempo) {
    seqp->uspt = seqp->target ? (s32) ((f32) tempo * seqp->target->qnpt) : 488;
}

/* A tempo change: the pending note offs move to the same tick count at the new tempo. */
static void handle_meta(NPlayer *np, ALEvent *event) {
    ALCSPlayer_Custom *seqp = np->seqp;
    ALTempoEvent *tevt = &event->msg.tempo;
    static Item sOffs[QUEUE_CAP];
    s32 oldUspt, tempo, i, n = 0;

    if (tevt->status != AL_MIDI_Meta || tevt->type != AL_MIDI_META_TEMPO) {
        return;
    }
    oldUspt = seqp->uspt;
    tempo = (tevt->byte1 << 16) | (tevt->byte2 << 8) | (tevt->byte3 << 0);
    set_uspt(seqp, (f32) tempo);

    q_lock();
    for (i = 0; i < np->count;) {
        Item *it = q_at(np, i);
        if (it->evt.type == AL_CSP_NOTEOFF_EVT) {
            sOffs[n++] = *it;
            q_remove(np, i);
        } else {
            i++;
        }
    }
    for (i = 0; i < n; i++) {
        ALMicroTime rel = (ALMicroTime) (sOffs[i].time - np->clock);
        u32 ticks = rel / oldUspt;
        q_post_locked(np, &sOffs[i].evt, (ALMicroTime) (ticks * seqp->uspt));
    }
    q_unlock();
}

static void handle_next_seq_event(NPlayer *np) {
    ALCSPlayer_Custom *seqp = np->seqp;
    ALEvent evt;

    if (seqp->target == NULL) {
        return;
    }
    seq_next_event(seqp->target, &evt);
    switch (evt.type) {
        case AL_SEQ_MIDI_EVT:
            handle_midi(np, &evt);
            post_next_seq_event(np);
            break;
        case AL_TEMPO_EVT:
            handle_meta(np, &evt);
            post_next_seq_event(np);
            break;
        case AL_SEQ_END_EVT:
            seqp->state = AL_STOPPING;
            evt.type = AL_SEQP_STOP_EVT;
            q_post(np, &evt, AL_EVTQ_END);
            break;
        case AL_TRACK_END:
        case AL_CSP_LOOPSTART:
        case AL_CSP_LOOPEND:
            post_next_seq_event(np);
            break;
        default:
            break;
    }
}

extern const char *volatile gPortAudioStage;

/* The synthesizer's call for this player (the old __CSPVoiceHandler): everything due now, then the time to the next. */
static ALMicroTime voice_handler(void *node) {
    ALCSPlayer_Custom *seqp = (ALCSPlayer_Custom *) node;
    NPlayer *np = player_find(seqp);
    ALEvent evt;
    ALVoice *voice;
    ALVoiceState *vs;
    ALMicroTime delta;
    u8 chan;

    gPortAudioStage = "handling music events (native)";
    do {
        switch (seqp->nextEvent.type) {
            case AL_SEQ_REF_EVT:
                handle_next_seq_event(np);
                break;
            case AL_SEQP_API_EVT:
                evt.type = AL_SEQP_API_EVT;
                q_post(np, &evt, seqp->frameTime);
                break;
            case AL_NOTE_END_EVT:
                voice = seqp->nextEvent.msg.note.voice;
                alSynStopVoice(seqp->drvr, voice);
                alSynFreeVoice(seqp->drvr, voice);
                unmap_voice(seqp, voice);
                break;
            case AL_SEQP_ENV_EVT:
                voice = seqp->nextEvent.msg.vol.voice;
                vs = (ALVoiceState *) voice->clientPrivate;
                if (vs->envPhase == AL_PHASE_ATTACK) {
                    vs->envPhase = AL_PHASE_DECAY;
                }
                delta = seqp->nextEvent.msg.vol.delta;
                vs->envEndTime = seqp->curTime + delta;
                vs->envGain = seqp->nextEvent.msg.vol.vol;
                alSynSetVol(seqp->drvr, voice, vs_vol(vs, seqp), delta);
                break;
            case AL_SEQP_MIDI_EVT:
            case AL_CSP_NOTEOFF_EVT:
                handle_midi(np, &seqp->nextEvent);
                break;
            case AL_SEQP_META_EVT:
                handle_meta(np, &seqp->nextEvent);
                break;
            case AL_SEQP_VOL_EVT:
                seqp->vol = seqp->nextEvent.msg.spvol.vol;
                for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                    alSynSetVol(seqp->drvr, &vs->voice, vs_vol(vs, seqp), vs_delta(vs, seqp->curTime));
                }
                break;
            case AL_SEQP_PLAY_EVT:
                if (seqp->state != AL_PLAYING) {
                    seqp->state = AL_PLAYING;
                    post_next_seq_event(np);
                }
                break;
            case AL_SEQP_STOP_EVT:
                if (seqp->state == AL_STOPPING) {
                    seqp->chanMask = 0xFFFF;
                    for (chan = 0; chan < seqp->maxChannels; chan++) {
                        CHAN(seqp, chan).fade = 0x7F;
                        CHAN(seqp, chan).vol = CHAN(seqp, chan).unk11;
                    }
                    for (vs = seqp->vAllocHead; vs != 0; vs = seqp->vAllocHead) {
                        alSynStopVoice(seqp->drvr, &vs->voice);
                        alSynFreeVoice(seqp->drvr, &vs->voice);
                        unmap_voice(seqp, &vs->voice);
                    }
                    seqp->state = AL_STOPPED;
                }
                break;
            case AL_SEQP_STOPPING_EVT:
                if (seqp->state == AL_PLAYING) {
                    q_flush_type(np, AL_SEQ_REF_EVT);
                    q_flush_type(np, AL_CSP_NOTEOFF_EVT);
                    q_flush_type(np, AL_SEQP_MIDI_EVT);
                    for (vs = seqp->vAllocHead; vs != 0; vs = vs->next) {
                        if (voice_needs_kill(np, &vs->voice, KILL_TIME)) {
                            release_voice(np, &vs->voice, KILL_TIME);
                        }
                    }
                    seqp->state = AL_STOPPING;
                    evt.type = AL_SEQP_STOP_EVT;
                    q_post(np, &evt, AL_EVTQ_END);
                }
                break;
            case AL_SEQP_PRIORITY_EVT:
                chan = seqp->nextEvent.msg.sppriority.chan;
                CHAN(seqp, chan).priority = seqp->nextEvent.msg.sppriority.priority;
                break;
            case AL_SEQP_SEQ_EVT:
                seqp->target = seqp->nextEvent.msg.spseq.seq;
                if (seqp->bank) {
                    init_from_bank(seqp, seqp->bank);
                }
                break;
            case AL_SEQP_BANK_EVT:
                seqp->bank = seqp->nextEvent.msg.spbank.bank;
                init_from_bank(seqp, seqp->bank);
                break;
            default:
                break;
        }
        seqp->nextDelta = q_next(np, &seqp->nextEvent);
        if (seqp->nextEvent.type == (s16) -1) {
            // An empty queue (the API event couldn't be reposted): start again a frame from now.
            seqp->nextEvent.type = AL_SEQP_API_EVT;
            seqp->nextDelta = seqp->frameTime;
            q_lock();
            np->clock += seqp->frameTime;
            q_unlock();
        }
    } while (seqp->nextDelta == 0);
    gPortAudioStage = "synthesizing a frame";
    seqp->curTime += seqp->nextDelta;
    return seqp->nextDelta;
}

/******************************** The game's calls ********************************/

/* alCSPNew: the same allocations from the audio heap as libultra's, so everything after keeps its place. */
int port_music_player_new(ALCSPlayer *player, ALSeqpConfig *c) {
    ALCSPlayer_Custom *seqp = (ALCSPlayer_Custom *) player;
    ALHeap *hp = c->heap;
    ALVoiceState *voices;
    NPlayer *np;
    s32 i;

    if (!port_music_native_on() || c->initOsc != NULL) {
        return FALSE;
    }
    np = player_find(NULL);
    if (np == NULL) {
        return FALSE;
    }
    if (sLock == NULL) {
        sLock = plat_sem_create(1);
    }
    __builtin_memset(np, 0, sizeof(*np));
    np->seqp = seqp;

    seqp->bank = 0;
    seqp->target = NULL;
    seqp->drvr = &alGlobals->drvr;
    seqp->chanMask = 0xffff;
    seqp->uspt = 488;
    seqp->nextDelta = 0;
    seqp->state = AL_STOPPED;
    seqp->vol = 0x7FFF;
    seqp->frameTime = AL_USEC_PER_FRAME;
    seqp->curTime = 0;
    seqp->initOsc = c->initOsc;
    seqp->updateOsc = c->updateOsc;
    seqp->stopOsc = c->stopOsc;
    seqp->mappedVoices = 0;
    seqp->voiceLimit = c->voiceLimit;
    seqp->nextEvent.type = AL_SEQP_API_EVT;

    seqp->maxChannels = c->maxChannels;
    seqp->chanState = alHeapAlloc(hp, c->maxChannels, sizeof(ALChanState) + 4);
    for (i = 0; i < seqp->maxChannels; i++) {
        CHAN(seqp, i).instrument = 0;
        reset_chan(seqp, i);
    }
    voices = alHeapAlloc(hp, c->maxVoices, sizeof(ALVoiceState));
    seqp->vFreeList = 0;
    for (i = 0; i < c->maxVoices; i++) {
        voices[i].next = seqp->vFreeList;
        seqp->vFreeList = &voices[i];
    }
    seqp->vAllocHead = 0;
    seqp->vAllocTail = 0;
    // libultra's event items, unused here: allocated anyway so the heap's later blocks land where they always did.
    alHeapAlloc(hp, c->maxEvents, sizeof(ALEventListItem));
    alEvtqNew(&seqp->evtq, NULL, 0);

    seqp->node.next = NULL;
    seqp->node.handler = voice_handler;
    seqp->node.clientData = seqp;
    alSynAddPlayer(&alGlobals->drvr, &seqp->node);
    plat_log("music: native player %p (%d voices, %d channels)", (void *) seqp, (int) c->maxVoices,
             (int) c->maxChannels);
    return TRUE;
}

/* The game's player calls that post an event: TRUE when the native player took it. */
int port_music_post(void *seqp, ALEvent *evt) {
    NPlayer *np = player_find(seqp);
    if (np == NULL) {
        return FALSE;
    }
    q_post(np, evt, 0);
    return TRUE;
}

#endif
