/* SPDX-License-Identifier: GPL-3.0-only */
/* MIDI OUT: the tracks play external gear (a MiniFreak, a P-6, a sampler, a DAW) over USB MIDI.
 *
 * GLO > MIDI OUT, for the selected track: OUT  INT  the FM-1's own engine (as ever; the keys still echo
 *                                                  on the track's channel, as before 2.4)
 *                                              MIDI  MIDI only: the engine stays silent
 *                                              BOTH  the engine and MIDI
 *                                         CH   the channel it plays (default 1, 2, 3, drums 10)
 *                                         CLK  MIDI clock out (24 a beat, START / CONTINUE / STOP)
 * What a track plays goes out, whatever started it: the steps, ratchets, ties and slides, the arp, note
 * repeat, the keys, the TRS MIDI IN jack, song sections. A note that came in over USB is not sent back out
 * over USB (a DAW with MIDI thru would loop). The drum track sends each hit as a note-on and its note-off
 * MO_DGATE later (drum machines and samplers in gate mode).
 *
 * No note left hanging: every note-on sent is remembered per track (with its channel) until its note-off
 * went out. A note-off that finds the queue full waits and goes first. Released on: the note's own end,
 * STOP, MUTE / SOLO, a change of OUT or CH, a panic (preset change), the main loop stalling (an update
 * session, which ends in a reset), and forgotten when USB goes away. Note-ons keep MO_RESERVE packets of
 * the queue free for note-offs and the clock.
 *
 * The FM-1 has no MIDI output jack (the 3.5 mm jack is an input) and SLOOP never switches the radio on:
 * USB is the only way out. A computer, a phone or a USB MIDI host box (to a synth's USB or DIN MIDI)
 * takes it to the instrument.
 *
 * Runs in the audio ISR (events_block, seq.c) like the rest of the sequencer; the TIMER5 ISR (usb_poll)
 * drains usb.c's ring, which mout_flush fills from this larger one. usb.c (shared with the update loader)
 * is not changed. The settings are a device setting (project.c persist_t.midi), not part of a project. */
enum { MO_INT, MO_EXT, MO_BOTH, MO_NMODE };
static const char *const N_MOUT[MO_NMODE] = {"INT", "MIDI", "BOTH"};
static const uint8_t MOUT_DEFCH[NTRK] = {0, 1, 2, 9};    /* tracks 1..3 on channels 1..3, the drums on 10 */
/* the page (params.c SC_MOUT): the selected track's OUT and CH, and CLK; the UI writes them */
static int16_t mout_mode[NTRK], mout_chan[NTRK] = {1, 2, 3, 10}, mout_clk;
static const param_desc_t MOUT_DESC[3] = {
    {"OUT", F_ENUM, 0, MO_NMODE - 1, 0, N_MOUT, 0},
    {"CH", F_INT, 1, 16, 1, 0, 0},
    {"CLK", F_ENUM, 0, 1, 0, N_ONOFF, 0},
};

static const param_desc_t *mout_desc(uint32_t slot, int16_t **valp)   /* params.c page_desc */
{
    *valp = slot == 0u ? &mout_mode[song.sel % NTRK] : slot == 1u ? &mout_chan[song.sel % NTRK] : &mout_clk;
    return &MOUT_DESC[slot < 3u ? slot : 2u];
}

/* the settings word: bits 0..7 OUT (2 per track), 8..23 CH xor the default (4 per track), 24 CLK.
 * 0 = every track INT on its default channel, no clock: what a settings object without it means */
static uint32_t mout_word(void)
{
    uint32_t w = (uint32_t)(mout_clk != 0) << 24, i;
    for (i = 0; i < NTRK; i++) {
        uint32_t m = (uint32_t)mout_mode[i] < MO_NMODE ? (uint32_t)mout_mode[i] : MO_INT;
        uint32_t c = (uint32_t)(mout_chan[i] - 1) & 15u;
        w |= m << (2u * i) | (c ^ MOUT_DEFCH[i]) << (8u + 4u * i);
    }
    return w;
}
static void mout_from_word(uint32_t w)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        uint32_t m = (w >> (2u * i)) & 3u;
        mout_mode[i] = (int16_t)(m < MO_NMODE ? m : MO_INT);
        mout_chan[i] = (int16_t)((((w >> (8u + 4u * i)) & 15u) ^ MOUT_DEFCH[i]) + 1u);
    }
    mout_clk = (int16_t)((w >> 24) & 1u);
}

/* ---- the queue: USB-MIDI event packets, cable 0 */
#define MO_N 256u                /* a power of two */
#define MO_RESERVE 48u           /* note-ons leave this much room: note-offs and the clock always fit */
#define MO_DGATE 4410u           /* the drum track's note-off: 100 ms after its hit (samples) */
#define MO_NDG 16u
#define MO_STALL_MS 250u         /* the main loop silent this long: release everything, send nothing new */
static uint32_t mo_q[MO_N];
static uint32_t mo_qw, mo_qr;    /* (one producer and one consumer: both in the audio ISR) */
static struct {
    uint32_t on[4];              /* notes sounding on the external gear (bit per note) */
    uint32_t offp[4];            /* their note-offs still to send (the queue was full) */
    uint8_t ch;                  /* the channel they were sent on */
    uint8_t mode, chan;          /* OUT and CH as last applied */
} mo_t[2 * NTRK];                /* 0..3 what the tracks play, 4..7 the keys' echo of tracks on OUT INT */
#define MO_ECHO NTRK
static struct { uint8_t note; uint16_t left; } mo_dg[MO_NDG];   /* the drum hits' note-offs to come */
static uint8_t mo_from_usb;      /* events_block: the note being handled came in over USB */
static uint8_t mo_playing;       /* the transport as the clock last saw it */
static uint32_t mo_clk_u;        /* units into the current clock pulse */
static volatile uint32_t mout_alive_ms;   /* the main loop's heartbeat (main.c); 0 = not running yet */
static uint8_t mo_alive_seen;
static uint32_t mo_sent, mo_dropped;      /* diagnostics: packets queued, note-ons dropped (full) */
static uint8_t mo_pend;          /* something may be sounding or waiting: mout_block has work */

static uint32_t mo_free(void) { return MO_N - (mo_qw - mo_qr); }
static int mo_push(uint32_t pkt)
{
    if (!usb.config || mo_qw - mo_qr >= MO_N)
        return 0;
    mo_q[mo_qw % MO_N] = pkt;
    mo_qw++;
    mo_sent++;
    return 1;
}
static uint32_t mo_ix(const track_t *t) { return (uint32_t)(t - trk) % NTRK; }
static int mo_has(const uint32_t *b, uint32_t n) { return (b[(n >> 5) & 3u] >> (n & 31u)) & 1u; }
static void mo_set(uint32_t *b, uint32_t n) { b[(n >> 5) & 3u] |= 1u << (n & 31u); }
static void mo_clr(uint32_t *b, uint32_t n) { b[(n >> 5) & 3u] &= ~(1u << (n & 31u)); }
static int mo_any(const uint32_t *b) { return (b[0] | b[1] | b[2] | b[3]) != 0; }
static int mo_held(void)         /* the main loop stalled (an update session): hold everything */
{
    return mo_alive_seen && (int32_t)(fm1_ms - mout_alive_ms) > (int32_t)MO_STALL_MS;   /* (signed: the
                                                    * heartbeat is fm1_ms | 1, a millisecond ahead at times) */
}

static int mout_ext(uint32_t i) { return mout_mode[i % NTRK] != MO_INT; }

static int mo_send_off(uint32_t i, uint32_t note)
{
    return mo_push(0x08u | (0x80u | mo_t[i].ch) << 8 | (note & 127u) << 16 | 64u << 24);
}
static __attribute__((noinline)) void mo_retry_offs(uint32_t i)
{
    uint32_t w, b;
    for (w = 0; w < 4u; w++)
        for (b = 0; mo_t[i].offp[w] && b < 32u; b++)
            if ((mo_t[i].offp[w] >> b) & 1u) {
                if (!mo_send_off(i, w * 32u + b))
                    return;
                mo_t[i].offp[w] &= ~(1u << b);
            }
}
static void mo_off(uint32_t i, uint32_t note)   /* note's note-off, now or as soon as there is room */
{
    if (!mo_has(mo_t[i].on, note))
        return;
    mo_clr(mo_t[i].on, note);
    mo_set(mo_t[i].offp, note);
    mo_retry_offs(i);
}

/* trk_note_on (voice.c), after MUTE / SOLO: 1 = MIDI only, the engine plays nothing */
static __attribute__((noinline)) int mout_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t i = mo_ix(t), k, free = MO_NDG, oldest = 0;
    if (mout_mode[i] == MO_INT)
        return 0;
    note &= 127u;
    if (mo_from_usb || !usb.config || mo_held() || mo_t[i].mode == MO_INT)
        return mout_mode[i] == MO_EXT;           /* (mode just set: mout_block applies it first) */
    if (mo_has(mo_t[i].on, note))
        mo_off(i, note);                         /* a retrigger: its old note ends first */
    if (mo_has(mo_t[i].offp, note) || mo_free() <= MO_RESERVE) {
        mo_dropped++;
        return mout_mode[i] == MO_EXT;
    }
    if (!mo_any(mo_t[i].on) && !mo_any(mo_t[i].offp))
        mo_t[i].ch = (uint8_t)((mout_chan[i] - 1) & 15);
    if (!mo_push(0x09u | (0x90u | mo_t[i].ch) << 8 | note << 16 | (vel < 1u ? 1u : vel > 127u ? 127u : vel) << 24))
        return mout_mode[i] == MO_EXT;
    mo_set(mo_t[i].on, note);
    mo_pend = 1;
    if (i == TRK_DRUM) {                         /* a drum hit: its note-off MO_DGATE later */
        for (k = 0; k < MO_NDG; k++) {
            if (mo_dg[k].left && mo_dg[k].note == note)
                break;
            if (!mo_dg[k].left && free == MO_NDG)
                free = k;
            if (mo_dg[k].left && mo_dg[k].left < mo_dg[oldest].left)
                oldest = k;
        }
        if (k == MO_NDG) {
            if (free == MO_NDG) {                /* all busy: the oldest hit ends now */
                mo_off(i, mo_dg[oldest].note);
                free = oldest;
            }
            k = free;
        }
        mo_dg[k].note = (uint8_t)note;
        mo_dg[k].left = MO_DGATE;
    }
    return mout_mode[i] == MO_EXT;
}

static __attribute__((noinline)) void mout_note_off(track_t *t, uint32_t note)
{
    uint32_t i = mo_ix(t);
    if (i != TRK_DRUM)                           /* (the drums: their gate ends them) */
        mo_off(i, note & 127u);
}

static __attribute__((noinline)) void mo_slot_off(uint32_t i)              /* every note of slot i */
{
    uint32_t w;
    for (w = 0; w < 4u; w++) {
        mo_t[i].offp[w] |= mo_t[i].on[w];
        mo_t[i].on[w] = 0;
    }
    mo_retry_offs(i);
}
static __attribute__((noinline)) void mout_track_off(track_t *t)           /* every note the track has sounding outside */
{
    uint32_t i = mo_ix(t), w;
    mo_slot_off(i);
    if (i == TRK_DRUM)
        for (w = 0; w < MO_NDG; w++)
            mo_dg[w].left = 0;
}

/* the keys of a track on OUT INT echo what they play on its channel (seq.c key_down / key_up, as 2.3);
 * kept like the tracks' notes, so a full queue cannot lose the note-off of an echoed note-on.
 * Returns 1 if the note-on went out (then key_up sends its note-off) */
static __attribute__((noinline)) int mout_echo_on(uint32_t i, uint32_t ch, uint32_t note, uint32_t vel)
{
    uint32_t e = MO_ECHO + i % NTRK;
    note &= 127u;
    if (!usb.config || mo_held() || mo_free() <= MO_RESERVE)
        return 0;
    if ((mo_any(mo_t[e].on) || mo_any(mo_t[e].offp)) && mo_t[e].ch != (ch & 15u))
        mo_slot_off(e);                          /* (GLO > DRUMS > CH changed while a key is held) */
    if (mo_any(mo_t[e].offp) || mo_has(mo_t[e].on, note))
        return 0;
    mo_t[e].ch = (uint8_t)(ch & 15u);
    if (!mo_push(0x09u | (0x90u | mo_t[e].ch) << 8 | note << 16 | (vel < 1u ? 1u : vel > 127u ? 127u : vel) << 24))
        return 0;
    mo_set(mo_t[e].on, note);
    mo_pend = 1;
    return 1;
}
static __attribute__((noinline)) void mout_echo_off(uint32_t i, uint32_t note)
{
    mo_off(MO_ECHO + i % NTRK, note & 127u);
}

static void mo_forget(void)                      /* USB gone: nothing to release any more */
{
    uint32_t i;
    for (i = 0; i < 2u * NTRK; i++) {
        mo_t[i].on[0] = mo_t[i].on[1] = mo_t[i].on[2] = mo_t[i].on[3] = 0;
        mo_t[i].offp[0] = mo_t[i].offp[1] = mo_t[i].offp[2] = mo_t[i].offp[3] = 0;
    }
    for (i = 0; i < MO_NDG; i++)
        mo_dg[i].left = 0;
    mo_qr = mo_qw;
    mo_playing = 0;
    mo_pend = 0;
}

/* once a block, before its events: the settings, the drum gates, what must be released */
static __attribute__((noinline)) void mout_block(uint32_t n)
{
    uint32_t i, busy = 0;
    if (mout_alive_ms)
        mo_alive_seen = 1;
    if (!mo_pend && !(mout_mode[0] | mout_mode[1] | mout_mode[2] | mout_mode[3]))
        return;                                  /* every track INT, nothing sounding outside: no work */
    if (!usb.config) {
        mo_forget();
        return;
    }
    for (i = 0; i < NTRK; i++) {
        uint32_t m = (uint32_t)mout_mode[i] < MO_NMODE ? (uint32_t)mout_mode[i] : MO_INT;
        uint32_t c = (uint32_t)(mout_chan[i] - 1) & 15u;
        if (m != mo_t[i].mode || c != mo_t[i].chan || trk_silent(&trk[i]) || mo_held()) {
            mout_track_off(&trk[i]);             /* OUT or CH changed, muted, or the main loop stalled */
            mo_t[i].mode = (uint8_t)m;
            mo_t[i].chan = (uint8_t)c;
        } else if (mo_any(mo_t[i].offp)) {
            mo_retry_offs(i);
        }
        if (mo_held())
            mo_slot_off(MO_ECHO + i);            /* (the echo: released on a stall, not on MUTE, as 2.3) */
        else if (mo_any(mo_t[MO_ECHO + i].offp))
            mo_retry_offs(MO_ECHO + i);
        busy |= (uint32_t)(mo_any(mo_t[i].on) | mo_any(mo_t[i].offp) | mo_any(mo_t[MO_ECHO + i].on) |
                           mo_any(mo_t[MO_ECHO + i].offp));
    }
    for (i = 0; i < MO_NDG; i++)
        if (mo_dg[i].left) {
            if (mo_dg[i].left > n) {
                mo_dg[i].left = (uint16_t)(mo_dg[i].left - n);
            } else {
                mo_dg[i].left = 0;
                mo_off(TRK_DRUM, mo_dg[i].note);
            }
            busy = 1;
        }
    mo_pend = (uint8_t)(busy != 0);
}

/* once a block, after its events: the clock (adv: the units the transport moves in this block) */
#define MO_PULSE_U (BEAT_U / 24u)
static __attribute__((noinline)) void mout_clock(uint32_t adv, uint32_t n)
{
    uint32_t k = 0;
    if (!mout_clk || song.g[G_SYNC] == 1 || !usb.config || mo_held()) {   /* (SYNC USB: back to the master) */
        mo_playing = song.playing;
        mo_clk_u = 0;
        return;
    }
    if (song.playing && !mo_playing) {           /* PLAY: START from the top (CONTINUE: on from elsewhere) */
        mo_push(clk_beat == 0u && clk_pos == 0u ? 0xFA0Fu : 0xFB0Fu);
        mo_push(0xF80Fu);                        /* the first pulse after START is the downbeat */
        mo_clk_u = 0;
    } else if (!song.playing && mo_playing) {
        mo_push(0xFC0Fu);                        /* STOP */
    }
    mo_playing = song.playing;
    mo_clk_u += song.playing ? adv : n * (uint32_t)song.g[G_BPM];   /* stopped: the tempo, for the followers */
    while (mo_clk_u >= MO_PULSE_U && k++ < 4u) {
        mo_clk_u -= MO_PULSE_U;
        mo_push(0xF80Fu);
    }
    if (mo_clk_u >= MO_PULSE_U)
        mo_clk_u %= MO_PULSE_U;                  /* (a tempo jump: no burst of pulses) */
}

static __attribute__((noinline)) void mout_flush(void)                     /* into usb.c's ring, as far as it has room */
{
    if (mo_qr == mo_qw)
        return;
    while (mo_qr != mo_qw && usb.config && mo_w - mo_r < MQ) {
        midi_out_event(mo_q[mo_qr % MO_N]);
        mo_qr++;
    }
}
