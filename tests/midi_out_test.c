/* SPDX-License-Identifier: GPL-3.0-only */
/* MIDI OUT (firmware/src/midi_out.c): the tracks play external gear over USB MIDI.
 *   default    every track OUT INT: the sequencer sends nothing (as 2.3); the keys echo as before
 *   MIDI       a pattern's notes go out on the track's channel, every note-on with its note-off; the engine
 *              stays silent; BOTH: the engine plays too
 *   releases   STOP with a tie held, MUTE, SOLO, a change of CH or OUT, a panic: the note-offs go out;
 *              USB gone: forgotten; the main loop stalled (an update session): released, nothing new
 *   drums      each hit a note-on and its note-off ~100 ms later, on channel 10
 *   clock      CLK: START + 24 pulses a beat + STOP; none with SYNC = USB
 *   loops      a note in over USB is not sent back out; one from the TRS jack is
 *   queue      the host not reading (queue full): note-offs still go out first once it reads again
 *   fuzz       random settings, transport, mutes, keys and host stalls: no note left hanging
 *   settings   the settings word: round trip, 0 = the defaults
 * Exit status: the number of failed checks. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int fails;
static void check(int ok, const char *what)
{
    printf("midi out: %-74s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* ---- the host side: drains usb.c's ring as usb_poll would, and keeps score */
static uint8_t sounding[16][128];        /* note-on seen, note-off not yet */
static int16_t ons[16][128];             /* note-ons - note-offs (two sources on one channel may overlap) */
static uint32_t n_on, n_off, n_double, n_stray, n_start, n_stop, n_cont, n_pulse;
static uint32_t on_ch[16];
static int host_reads = 1;
static int main_loop;                    /* the main loop runs (its heartbeat, as main.c) */
static uint64_t blk;
static void host_reset(void)
{
    memset(sounding, 0, sizeof sounding);
    memset(ons, 0, sizeof ons);
    memset(on_ch, 0, sizeof on_ch);
    n_on = n_off = n_double = n_stray = n_start = n_stop = n_cont = n_pulse = 0;
}
static void host_take(uint32_t pkt)
{
    uint32_t st = (pkt >> 8) & 0xFFu, ch = st & 15u, d1 = (pkt >> 16) & 127u, d2 = (pkt >> 24) & 127u;
    if ((pkt & 15u) == 0xFu) {
        n_start += st == 0xFAu;
        n_cont += st == 0xFBu;
        n_stop += st == 0xFCu;
        n_pulse += st == 0xF8u;
        return;
    }
    if ((st & 0xF0u) == 0x90u && d2) {
        n_double += sounding[ch][d1];
        sounding[ch][d1] = 1;
        ons[ch][d1]++;
        on_ch[ch]++;
        n_on++;
    } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
        n_stray += !sounding[ch][d1];
        sounding[ch][d1] = 0;
        if (ons[ch][d1] > 0)
            ons[ch][d1]--;
        n_off++;
    }
}
static void host_drain(void)
{
    while (host_reads && mo_r != mo_w)
        host_take(midi_out_q[mo_r++ % MQ]);
}
static uint32_t hanging(void)
{
    uint32_t c, n, h = 0;
    for (c = 0; c < 16u; c++)
        for (n = 0; n < 128u; n++)
            h += sounding[c][n];
    return h;
}
static uint32_t unbalanced(void)          /* notes with more note-ons than note-offs */
{
    uint32_t c, n, h = 0;
    for (c = 0; c < 16u; c++)
        for (n = 0; n < 128u; n++)
            h += ons[c][n] > 0;
    return h;
}
static uint32_t engine_voices(const track_t *t)
{
    uint32_t i, a = 0;
    for (i = 0; i < NVOICE; i++)
        a += t->v[i].active && t->v[i].gate;
    return a;
}

static void run_block(void)
{
    int32_t out[CTL * 2];
    if (main_loop)
        mout_alive_ms = fm1_ms | 1u;
    mix_block(out, CTL);
    host_drain();
    blk++;
    fm1_ms = (uint32_t)(blk * CTL * 1000u / FS);
}
static void run_ms(uint32_t ms)
{
    uint64_t end = blk + (uint64_t)ms * FS / 1000u / CTL;
    while (blk < end)
        run_block();
}

static void reset(void)
{
    uint32_t i;
    host_tracks_init();
    for (i = 0; i < NTRK; i++) {
        steps_clear(&trk[i]);
        mout_mode[i] = MO_INT;
        mout_chan[i] = (int16_t)(MOUT_DEFCH[i] + 1u);
        trk_all_off(&trk[i]);
    }
    mout_clk = 0;
    host_preset(&trk[0], 0, 0);
    host_preset(&trk[1], 0, 0);
    memset(&drums, 0, sizeof drums);
    drums.set = -2;
    song.playing = 0;
    song.solo = 0;
    song.rec = 0;
    song.sel = 0;
    rec_wait = 0;
    transport_req = 0;
    fm1_in.notes = fm1_in.buttons = 0;
    kb_prev = 0;
    usb.config = 1;
    host_reads = 1;
    mout_alive_ms = 0;
    mo_alive_seen = 0;
    main_loop = 0;
    run_ms(20);                                   /* (settles: the settings applied) */
    mo_w = mo_r = 0;
    host_reset();
}

static void pattern_bass(track_t *t)              /* 16 steps: notes, a rest, a tie, a chord */
{
    static const uint8_t A[1] = {36}, B[1] = {43}, C[3] = {48, 52, 55};
    uint32_t i;
    t->p[P_SLEN] = 16;
    t->p[P_SDIV] = 2;                             /* 1/16: 8 steps a second at 120 BPM */
    for (i = 0; i < 16u; i++)
        put_step(t, i, 1, i % 4u == 2u ? B : A, i % 8u == 3u ? ST_REST : ST_NOTE, 0);
    put_step(t, 5, 0, A, ST_TIE, 0);
    put_step(t, 6, 0, A, ST_TIE, 0);
    put_step(t, 12, 3, C, ST_NOTE, 0);
}

static void t_default(void)
{
    reset();
    pattern_bass(&trk[0]);
    transport_req = 1;
    run_ms(2000);
    check(n_on == 0 && engine_voices(&trk[0]) + 1u > 0u, "default OUT INT: the sequencer sends nothing (as 2.3)");
    transport_req = 2;
    run_ms(200);
    fm1_in.notes = 1u << 3;                       /* a key on track 1: echoed, as before */
    run_ms(50);
    fm1_in.notes = 0;
    run_ms(50);
    check(n_on == 1 && n_off == 1 && hanging() == 0, "default OUT INT: a key still echoes its note-on and note-off");
}

static void t_midi(void)
{
    uint32_t heard = 0;
    uint64_t b0;
    reset();
    pattern_bass(&trk[0]);
    mout_mode[0] = MO_EXT;
    mout_chan[0] = 5;
    transport_req = 1;
    b0 = blk;
    while (blk - b0 < 2000u * FS / 1000u / CTL) {
        run_block();
        heard |= engine_voices(&trk[0]);
    }
    check(n_on >= 12u && on_ch[4] == n_on, "OUT MIDI: the pattern goes out on CH 5");
    check(heard == 0, "OUT MIDI: the engine stays silent");
    check(n_double == 0 && n_stray == 0, "OUT MIDI: every note-on ends before it plays again");
    transport_req = 2;
    run_ms(100);
    check(hanging() == 0, "OUT MIDI: STOP releases every note");

    reset();
    pattern_bass(&trk[0]);
    mout_mode[0] = MO_BOTH;
    transport_req = 1;
    heard = 0;
    b0 = blk;
    while (blk - b0 < 1000u * FS / 1000u / CTL) {
        run_block();
        heard |= engine_voices(&trk[0]);
    }
    check(n_on >= 5u && on_ch[0] == n_on && heard, "OUT BOTH: MIDI on CH 1 and the engine plays");
    transport_req = 2;
    run_ms(100);
    check(hanging() == 0, "OUT BOTH: STOP releases every note");
}

static void t_releases(void)
{
    static const uint8_t N[1] = {60};
    uint32_t i;
    /* a note tied over the whole pattern: only the releases can end it */
    reset();
    trk[0].p[P_SLEN] = 16;
    put_step(&trk[0], 0, 1, N, ST_NOTE, 0);
    for (i = 1; i < 16u; i++)
        put_step(&trk[0], i, 0, N, ST_TIE, 0);
    mout_mode[0] = MO_EXT;
    transport_req = 1;
    run_ms(300);
    check(hanging() == 1, "a tied note sounds");
    transport_req = 2;
    run_ms(20);
    check(hanging() == 0, "STOP: its note-off");

    transport_req = 1;
    run_ms(300);
    trk[0].p[P_MUTE] = 1;
    run_ms(20);
    check(hanging() == 0, "MUTE: its note-off");
    run_ms(2500);
    check(hanging() == 0, "MUTE: no new notes");
    trk[0].p[P_MUTE] = 0;
    run_ms(2100);
    song.solo = 1u << 1;                          /* another track soloed */
    run_ms(20);
    check(hanging() == 0, "SOLO of another track: its note-off");
    song.solo = 0;
    run_ms(2100);
    check(hanging() == 1, "a tied note sounds again");
    mout_chan[0] = 7;
    run_ms(20);
    check(hanging() == 0, "CH changed: its note-off (on the old channel)");
    run_ms(2100);
    check(on_ch[6] >= 1u && sounding[6][60], "CH changed: the next notes on the new channel");
    mout_mode[0] = MO_INT;
    run_ms(20);
    check(hanging() == 0, "OUT back to INT: its note-off");
    mout_mode[0] = MO_EXT;
    run_ms(2100);
    panic_req = 1u;                               /* a preset change */
    run_ms(20);
    check(hanging() == 0, "panic (preset change): its note-off");

    run_ms(2100);
    main_loop = 1;                                /* the main loop runs, then stops (an update session) */
    run_ms(100);
    check(hanging() == 1, "main loop alive: the note sounds");
    i = n_on;
    main_loop = 0;
    run_ms(MO_STALL_MS + 50u);
    check(hanging() == 0, "main loop stalled (update session): its note-off");
    run_ms(3000);
    check(n_on == i, "main loop stalled: no new note-on");
    main_loop = 1;
    run_ms(2100);
    check(n_on > i, "main loop back: notes again");
    transport_req = 2;
    run_ms(20);

    usb.config = 0;                               /* unplugged: forgotten, nothing queued */
    run_ms(20);
    check(mo_qr == mo_qw && !mo_any(mo_t[0].on), "USB gone: nothing kept, nothing queued");
}

static void t_drums(void)
{
    static const uint8_t K[1] = {36}, H[1] = {42};
    uint32_t i, ok_gate = 1;
    reset();
    TDRUM->p[P_SLEN] = 16;
    TDRUM->p[P_SDIV] = 2;                         /* 1/16 */
    for (i = 0; i < 16u; i++)
        put_step(TDRUM, i, 1, i % 4u ? H : K, ST_NOTE, 0);
    mout_mode[TRK_DRUM] = MO_EXT;
    transport_req = 1;
    for (i = 0; i < 2000u * FS / 1000u / CTL; i++) {
        run_block();
        if (sounding[9][36] && drums.v[0].active)
            ok_gate = 0;
    }
    check(on_ch[9] >= 15u && on_ch[9] == n_on, "drums OUT MIDI: every hit on CH 10");
    {
        uint32_t k, act = 0;
        for (k = 0; k < NDRUM; k++)
            act += drums.v[k].active;
        check(act == 0 && ok_gate, "drums OUT MIDI: the drum engine stays silent");
    }
    transport_req = 2;
    run_ms(150);
    check(hanging() == 0 && n_off == n_on, "drums: each hit has its note-off (100 ms)");
}

static void t_clock(void)
{
    uint32_t p;
    reset();
    mout_clk = 1;
    run_ms(1000);                                 /* stopped: the tempo for the followers */
    check(n_pulse >= 46u && n_pulse <= 50u && n_start == 0, "CLK stopped: 24 pulses a beat at 120 BPM, no START");
    n_pulse = 0;
    transport_req = 1;
    run_block();
    check(n_start == 1, "CLK: PLAY sends START");
    run_ms(4000 - 1);
    p = n_pulse;
    check(p >= 191u && p <= 194u, "CLK: 8 beats = 192 pulses (the downbeat counted)");
    transport_req = 2;
    run_block();
    check(n_stop == 1, "CLK: STOP sends STOP");
    reset();
    mout_clk = 1;
    song.g[G_SYNC] = 1;                           /* following USB: nothing back to it */
    transport_req = 1;
    run_ms(1000);
    check(n_pulse == 0 && n_start == 0, "CLK with SYNC = USB: no clock out");
    song.g[G_SYNC] = 0;
}

static void push_in(uint32_t cable, uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_in_q[mi_w % MQ] = (cable << 4) | (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
    mi_w++;
}
static void t_loops(void)
{
    reset();
    mout_mode[0] = MO_EXT;
    push_in(0, 0x90, 60, 100);                    /* USB, channel 1 */
    run_ms(20);
    check(n_on == 0, "a note in over USB is not sent back out over USB");
    push_in(0, 0x80, 60, 0);
    push_in(1, 0x90, 62, 100);                    /* the TRS jack */
    run_ms(20);
    check(n_on == 1 && sounding[0][62], "a note from the TRS jack plays out of the track");
    push_in(1, 0x80, 62, 0);
    run_ms(20);
    check(hanging() == 0, "... and its note-off");
}

static void t_queue(void)
{
    static const uint8_t C[4] = {48, 52, 55, 59};
    uint32_t i;
    reset();
    for (i = 0; i < NPART; i++) {
        uint32_t s;
        trk[i].p[P_SLEN] = 16;
        trk[i].p[P_SDIV] = 3;                    /* 1/32 */
        for (s = 0; s < 16u; s++)
            put_step(&trk[i], s, 4, C, ST_NOTE, 0);
        mout_mode[i] = MO_EXT;
    }
    transport_req = 1;
    run_ms(500);
    host_reads = 0;                               /* the host stops reading: the queues fill */
    run_ms(1500);
    check(mo_free() <= MO_RESERVE + 8u, "host not reading: the queue fills");
    host_reads = 1;
    run_ms(500);
    transport_req = 2;
    run_ms(50);
    check(hanging() == 0 && n_double == 0, "host reads again: every note-off went out, none doubled");
}

static void t_keys(void)
{
    reset();
    fm1_in.notes = 1u << 5;                       /* pressed on OUT INT: echoed */
    run_ms(30);
    mout_mode[0] = MO_EXT;                        /* OUT changed while held */
    run_ms(30);
    fm1_in.notes = 0;
    run_ms(30);
    check(hanging() == 0, "a key pressed on INT, released on MIDI: its note-off");
    fm1_in.notes = 1u << 5;
    run_ms(30);
    check(n_on == 2 && engine_voices(&trk[0]) == 0, "OUT MIDI: a key plays out (once), not the engine");
    fm1_in.notes = 0;
    run_ms(30);
    check(hanging() == 0, "OUT MIDI: the key's note-off");
}

static uint32_t rnd = 12345;
static uint32_t rn(uint32_t n) { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) % n; }
static void t_fuzz(void)
{
    static const uint8_t N[4] = {40, 47, 52, 59};
    uint32_t i, s, ev = 0;
    reset();
    for (i = 0; i < NTRK; i++) {
        trk[i].p[P_SLEN] = (int16_t)(4 + rn(13));
        for (s = 0; s < 16u; s++) {
            uint32_t r = rn(4);
            put_step(&trk[i], s, 1 + rn(3), N + rn(2), r == 0 ? ST_REST : r == 1 ? ST_TIE : ST_NOTE, rn(2) ? SF_SLIDE : 0);
        }
    }
    trk[1].p[P_AMODE] = 1;                        /* an arp on track 2 */
    for (i = 0; i < 20000u; i++) {
        switch (rn(40)) {
        case 0: mout_mode[rn(NTRK)] = (int16_t)rn(MO_NMODE); ev++; break;
        case 1: {                                 /* (each track its own channels: no two share one) */
            static const int16_t CH[NTRK][3] = {{1, 4, 5}, {2, 6, 7}, {3, 8, 9}, {10, 11, 12}};
            uint32_t k = rn(NTRK);
            mout_chan[k] = CH[k][rn(3)];
            ev++;
            break;
        }
        case 2: transport_req = (uint8_t)(1 + rn(2)); ev++; break;
        case 3: trk[rn(NTRK)].p[P_MUTE] ^= 1; ev++; break;
        case 4: song.solo = (uint8_t)(rn(3) ? 0u : 1u << rn(NTRK)); ev++; break;
        case 5: fm1_in.notes ^= 1u << rn(27); ev++; break;
        case 6: song.sel = (uint8_t)rn(NTRK); ev++; break;
        case 7: host_reads = rn(4) != 0; ev++; break;
        case 8: panic_req = (uint8_t)(1u << rn(NTRK)); ev++; break;
        case 9: push_in(rn(2), rn(2) ? 0x90 : 0x80, N[rn(4)], 90); ev++; break;
        case 10: mout_clk = (int16_t)rn(2); ev++; break;
        case 11: main_loop = rn(3) != 0; ev++; break;
        default: break;
        }
        run_ms(5 + rn(40));
    }
    fm1_in.notes = 0;
    host_reads = 1;
    main_loop = 0;
    for (i = 0; i < 4u; i++) {                    /* (MIDI in left held: let go) */
        push_in(1, 0x80, N[i], 0);
        push_in(0, 0x80, N[i], 0);
    }
    transport_req = 2;
    run_ms(300);
    printf("midi out: fuzz: %u events, %u note-ons, %u note-offs, %u dropped (queue full)\n", ev, n_on, n_off, mo_dropped);
    check(n_on > 1000u && unbalanced() == 0 && hanging() == 0, "fuzz: random settings, transport, mutes, keys, host stalls: nothing hangs");
}

static void t_settings(void)
{
    uint32_t i, ok = 1;
    reset();
    check(mout_word() == 0u, "settings: the defaults are word 0 (a 2.3 settings object)");
    for (i = 0; i < 2000u && ok; i++) {
        uint32_t k, w;
        int16_t m[NTRK], c[NTRK], clk = (int16_t)rn(2);
        for (k = 0; k < NTRK; k++) {
            m[k] = mout_mode[k] = (int16_t)rn(MO_NMODE);
            c[k] = mout_chan[k] = (int16_t)(1 + rn(16));
        }
        mout_clk = clk;
        w = mout_word();
        mout_from_word(0);
        mout_from_word(w);
        for (k = 0; k < NTRK; k++)
            ok &= mout_mode[k] == m[k] && mout_chan[k] == c[k];
        ok &= mout_clk == clk;
    }
    check(ok, "settings: word round trip");
    mout_from_word(0xFFFFFFFFu);
    ok = mout_clk == 1;
    for (i = 0; i < NTRK; i++)
        ok &= mout_mode[i] == MO_INT && mout_chan[i] >= 1 && mout_chan[i] <= 16;
    check(ok, "settings: a damaged word stays in range (OUT 3 -> INT)");
    mout_from_word(0);
}

int main(void)
{
    t_default();
    t_midi();
    t_releases();
    t_drums();
    t_clock();
    t_loops();
    t_queue();
    t_keys();
    t_fuzz();
    t_settings();
    printf(fails ? "midi out: %d FAILED\n" : "midi out test passed\n", fails);
    return fails;
}
