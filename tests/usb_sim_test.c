/* SPDX-License-Identifier: GPL-3.0-only */
/* The update path through the real USB driver (firmware/src/usb.c) on a simulated USB device controller
 * (the MUSB-style SIE fm1_usb.h drives), against a simulated host that enumerates the FM-1 and serves the
 * update as the installers do (web/fm1ota.js, tools/fm1_install.py): the device asks, the host answers.
 *
 * Built three ways (tests/run_tests.sh):
 *   -DSIM_APP      the app: normal mode (TIMER5 polls USB, the audio ISR empties the MIDI in ring) and
 *                  the USB rescue (firmware/src/recovery.c: no TIMER5, no audio); the update entry
 *                  firmware/src/ota.c stages the package's loader in a simulated flash
 *   -DSIM_LOADER   the update loader (firmware/loader: usb.c as the loader builds it, ldr_core.c) writes
 *                  the app area from the package
 * Checked: enumeration as hosts do it (descriptors well formed, address, configuration), unknown and
 * abandoned requests, the M-UPGRADE handshake and a whole update session; with the host's other MIDI
 * traffic mixed in (notes and a DAW's clock: the update must get through it, in rescue too), with the
 * device's MIDI out busy (SysEx never interleaved with notes), with the audio dead (usb_guard.c), with
 * the host not reading for a while, and a bus reset (replug) in the middle of a session: nothing is
 * committed, and the next try succeeds.
 *   usb_sim_test PACKAGE.fwsc
 * Exit status: the number of failed checks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

/* ---- the real HAL header under other names; the simulated controller takes the real names ---- */
#define fm1_usb_reset real_fm1_usb_reset
#define fm1_usb_attach real_fm1_usb_attach
#define fm1_usb_off real_fm1_usb_off
#define fm1_usb_sie_on real_fm1_usb_sie_on
#define fm1_usb_sie_wr_start real_fm1_usb_sie_wr_start
#define fm1_usb_sie_rd_start real_fm1_usb_sie_rd_start
#define fm1_usb_sie_done real_fm1_usb_sie_done
#define fm1_usb_sie_data real_fm1_usb_sie_data
#define fm1_usb_ep0_buf real_fm1_usb_ep0_buf
#define fm1_usb_ep_txbuf real_fm1_usb_ep_txbuf
#define fm1_usb_ep_rxbuf real_fm1_usb_ep_rxbuf
#define fm1_usb_ep0_send real_fm1_usb_ep0_send
#define fm1_usb_ep_send real_fm1_usb_ep_send
#define fm1_usb_ep4_txbuf real_fm1_usb_ep4_txbuf
#define fm1_usb_ep4_send real_fm1_usb_ep4_send
#define fm1_usb_rx_sync real_fm1_usb_rx_sync
#define fm1_usb_ep_enable real_fm1_usb_ep_enable
#define fm1_usb_sof_take real_fm1_usb_sof_take
#include "../firmware/hal/fm1_usb.h"
#undef fm1_usb_reset
#undef fm1_usb_attach
#undef fm1_usb_off
#undef fm1_usb_sie_on
#undef fm1_usb_sie_wr_start
#undef fm1_usb_sie_rd_start
#undef fm1_usb_sie_done
#undef fm1_usb_sie_data
#undef fm1_usb_ep0_buf
#undef fm1_usb_ep_txbuf
#undef fm1_usb_ep_rxbuf
#undef fm1_usb_ep0_send
#undef fm1_usb_ep_send
#undef fm1_usb_ep4_txbuf
#undef fm1_usb_ep4_send
#undef fm1_usb_rx_sync
#undef fm1_usb_ep_enable
#undef fm1_usb_sof_take

/* ---- the simulated controller: SIE registers (usb.c's S_* map), EP0..4 DMA ---- */
static struct {
    int on;                          /* attached (CON0 USB_NRST) */
    uint8_t faddr, index, intrusb, intrtx, intrrx, csr0;
    uint16_t count0;
    uint8_t txcsr1[5], rxcsr1[5];
    uint16_t rxcount[5];
    uint8_t *ep0dma, *rxdma[5];
    const uint8_t *txp[5];
    uint32_t txn[5];
    uint32_t rd;                     /* the register a read started on */
    uint16_t frame;
    uint32_t attaches;
} sie;
static void fm1_usb_reset(void) { sie.on = 0; }
static void fm1_usb_attach(void *ep0)
{
    sie.faddr = sie.index = sie.intrusb = sie.intrtx = sie.intrrx = sie.csr0 = 0;
    memset(sie.txcsr1, 0, sizeof sie.txcsr1);
    memset(sie.rxcsr1, 0, sizeof sie.rxcsr1);
    sie.on = 1;
    sie.ep0dma = ep0;
    sie.attaches++;
}
static void fm1_usb_off(void) { sie.on = 0; }
static uint32_t fm1_usb_sie_on(void) { return sie.on ? 4u : 0u; }
static void fm1_usb_ep0_buf(void *p) { sie.ep0dma = p; }
static void fm1_usb_ep_txbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
static void fm1_usb_ep_rxbuf(uint32_t ep, void *p) { sie.rxdma[ep % 5u] = p; }
static void fm1_usb_ep0_send(void *p, uint32_t n) { sie.txp[0] = p; sie.txn[0] = n; }
static void fm1_usb_ep_send(uint32_t ep, void *p, uint32_t n) { sie.txp[ep % 5u] = p; sie.txn[ep % 5u] = n; }
static void fm1_usb_ep4_txbuf(void *p) { (void)p; }
static void fm1_usb_ep4_send(void *p, uint32_t n) { sie.txp[4] = p; sie.txn[4] = n; }
static void fm1_usb_rx_sync(void) {}
static void fm1_usb_ep_enable(uint32_t m) { (void)m; }
static uint32_t fm1_usb_sof_take(void) { return 1; }
static uint32_t sie_ep0_tx, sie_ep0_stall, sie_ep0_status;   /* the device's answers on EP0 */
static void fm1_usb_sie_wr_start(uint32_t r, uint32_t v)
{
    uint32_t ep = sie.index % 5u;
    v &= 0xFFu;
    switch (r) {
    case 0: sie.faddr = (uint8_t)v; break;
    case 14: sie.index = (uint8_t)v; break;
    case 17:
        if (ep == 0) {                                  /* CSR0 */
            if (v & 0x80u) sie.csr0 &= (uint8_t)~0x10u; /* ServicedSetupEnd */
            if (v & 0x40u) sie.csr0 &= (uint8_t)~0x01u; /* ServicedRxPktRdy */
            if (v & 0x20u) sie_ep0_stall = 1;           /* SendStall */
            if (v & 0x02u) { sie.csr0 |= 0x02u; sie_ep0_tx = 1; }   /* TxPktRdy: a data packet */
            if ((v & 0x08u) && !(v & 0x02u)) sie_ep0_status = 1;    /* DataEnd alone: status stage */
            if ((v & 0x0Au) == 0x0Au) sie_ep0_status = 1;           /* last IN packet */
            if (v == 0) sie.csr0 &= (uint8_t)~0x04u;    /* SentStall cleared */
        } else {
            if (v & 0x01u) sie.txcsr1[ep] |= 1u;        /* TxPktRdy */
            if (v & 0x08u) sie.txcsr1[ep] &= (uint8_t)~1u;   /* FlushFIFO */
            if (!(v & 0x80u)) sie.txcsr1[ep] &= (uint8_t)~0x80u;
        }
        break;
    case 20:                                            /* RXCSR1: the packet taken */
        if (v & 0x10u) sie.rxcsr1[ep] &= (uint8_t)~1u;
        if (v & 0x80u) sie.rxcsr1[ep] &= (uint8_t)~1u;  /* ClrDataTog (CLEAR_FEATURE) */
        break;
    default: break;
    }
}
static void fm1_usb_sie_rd_start(uint32_t r) { sie.rd = r; }
static uint32_t fm1_usb_sie_done(void) { return 0x8000u; }
static uint32_t fm1_usb_sie_data(void)
{
    uint32_t ep = sie.index % 5u, v = 0;
    switch (sie.rd) {
    case 0: v = sie.faddr; break;
    case 2: v = sie.intrtx; sie.intrtx = 0; break;
    case 4: v = sie.intrrx; sie.intrrx = 0; break;
    case 6: v = sie.intrusb; sie.intrusb = 0; break;
    case 12: v = sie.frame & 0xFFu; break;
    case 13: v = sie.frame >> 8; break;
    case 17: v = ep ? sie.txcsr1[ep] : sie.csr0; break;
    case 20: v = sie.rxcsr1[ep]; break;
    case 22: v = ep ? (sie.rxcount[ep] & 0xFFu) : sie.count0; break;
    case 23: v = ep ? (uint32_t)(sie.rxcount[ep] >> 8) : 0u; break;
    default: break;
    }
    return v;
}

/* ---- time, and what the firmware around usb.c supplies ---- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static uint64_t t_us;
static volatile uint32_t fm1_ms;
static void tick(void);
static void fm1_delay_ms(uint32_t ms) { uint64_t end = t_us + ms * 1000ull; while (t_us < end) tick(); }
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}

#ifdef SIM_LOADER
#define FELUCCA_LOADER 1
#define FELUCCA_CDC 0
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_USB_PID 0x0002
#define FELUCCA_ID "ota-FM-1_900"
#else
#define FELUCCA_CDC 1
#define FELUCCA_UAC 1
#define HALF_FRAMES 256
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_ID "FM-1_900"
static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#endif
#include "../firmware/src/usb.c"

/* ---- flash: 1 MiB NOR ---- */
static uint8_t nor[0x100000];
static uint32_t nor_bad_writes;
static int nor_erase(uint32_t off)
{
    if (off & 0xFFFu || off >= sizeof nor) return -8;
    memset(nor + off, 0xFF, 0x1000);
    return 0;
}
static int nor_prog(uint32_t off, const void *p, uint32_t n)
{
    uint32_t i;
    if (off + n > sizeof nor) return -8;
    for (i = 0; i < n; i++) nor[off + i] &= ((const uint8_t *)p)[i];
    return 0;
}
static int nor_read(uint32_t off, void *p, uint32_t n)
{
    if (off + n > sizeof nor) return -8;
    memcpy(p, nor + off, n);
    return 0;
}

static uint32_t ota_now_ms(void) { return fm1_ms; }
#ifdef SIM_LOADER
static void ldr_sim_poll(void)                           /* loader.c ldr_poll: USB at 2 kHz */
{
    static uint64_t last;
    tick();
    if (t_us - last >= 500) { last = t_us; usb_poll(); }
}
static void ota_idle(void) { ldr_sim_poll(); }
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { return nor_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../firmware/src/ota.c"
static uint32_t rec_cleared, records_left;
static int ldr_fread(uint32_t off, void *p, uint32_t n) { return nor_read(off, p, n); }
static int ldr_erase(uint32_t off)
{
    if (off < 0x4000u) nor_bad_writes++;                 /* the head: never */
    return nor_erase(off);
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    if (off < 0x4000u) nor_bad_writes++;
    return nor_prog(off, p, n);
}
static void ldr_record_clear(void) { rec_cleared++; }
static int ldr_flash_known(void) { return 1; }
static void ldr_progress(uint32_t done, uint32_t total) { (void)done; (void)total; ldr_sim_poll(); }
#include "../firmware/loader/ldr_core.c"
#else
#define OTA_AREA_SIM 0xE0000u
static int ota_in_area(uint32_t off, uint32_t n) { return off >= OTA_AREA_SIM && off + n <= OTA_AREA_SIM + 0x5000u; }
static int ota_erase(uint32_t off) { if (!ota_in_area(off, 0x1000)) { nor_bad_writes++; return -8; } return nor_erase(off); }
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n)) { nor_bad_writes++; return -8; }
    return nor_prog(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return nor_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static uint32_t committed;
static uint8_t committed_parm[112];
static void ota_commit(const uint8_t *parm) { committed++; memcpy(committed_parm, parm, 112); }
static void ota_idle(void);
#include "../firmware/src/ota.c"
#include "../firmware/src/usb_guard.c"
#ifdef NO_USB_GUARD                                     /* (to show what happens without it) */
#define usb_in_guard(ms) ((void)(ms))
#endif
/* recovery.c and what it needs */
#include "../firmware/src/bootguard.h"
static bootguard_t bootguard;
static uint8_t flash_ok = 1;
static struct { uint32_t buttons; } fm1_in;
static uint32_t uboot_entered, font_dummy;
#define FM1_TICKS_PER_US 24u
#define FL_FAR(x) x
#define C_BLACK 0
#define C_WHITE 65535
#define FONT_S font_dummy
static uint32_t fm1_ticks(void) { return (uint32_t)(t_us * FM1_TICKS_PER_US); }
static void fm1_wdt_feed(void) {}
static void fm1_enter_uboot(void) { uboot_entered++; }
static void fm1_input_init(void) {}
static void fm1_input_scan(void) {}
static uint32_t fl_jedec_ram(void) { return 0x856014u; }
static void lcd_init(void) {}
static void lcd_fill(int x, int y, int w, int h, int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void draw_text_box(int x, int y, int w, const void *f, const char *s, int c, int a)
{ (void)x; (void)y; (void)w; (void)f; (void)s; (void)c; (void)a; }
static void lcd_sync(void) {}
#ifdef RECOVERY_SRC
#include RECOVERY_SRC
#else
#include "../firmware/src/recovery.c"
#endif
static void ota_idle(void)                               /* as felucca.c */
{
    tick();
    if (recovery_active) recovery_poll();
    else usb_in_guard(fm1_ms);
}
#endif

/* ---- the simulated host ---- */
static uint8_t *image;                                   /* the package's logical image (served) */
static uint32_t image_len;
static int host_reads = 1;                               /* the host takes the device's IN packets */
static int audio_alive = 1;                              /* (app, normal mode) the audio ISR empties the ring */
static int dev_midi_out;                                 /* (app) the device's MIDI out busy (notes, clock) */
static uint32_t noise_every_us;                          /* the host's other MIDI traffic: one event every .. */
static uint64_t next_noise, next_out, last_poll, last_drain, next_devout;
static uint32_t noise_sent;
/* host -> device: USB-MIDI events waiting for EP1 OUT */
static uint32_t hq[200000];
static uint32_t hq_w, hq_r;
/* device -> host: SysEx assembly from the IN packets */
static uint8_t hx[4096];
static uint32_t hx_n;
static int hx_on;
static uint32_t interleaved, sysex_in, notes_in, requests, identities, success_asked;
static char identity[32];
static uint32_t serve_corrupt_at = 0xFFFFFFFFu;          /* a damaged byte served once at this address */
static uint32_t reset_after_requests = 0xFFFFFFFFu;      /* a bus reset (replug) after n requests */

static void hq_put(uint32_t ev) { if (hq_w - hq_r < sizeof hq / sizeof hq[0]) hq[hq_w++ % (sizeof hq / sizeof hq[0])] = ev; }
static void host_send_sysex(const uint8_t *m, uint32_t n)   /* F0..F7 -> CIN 4 / 5 / 6 / 7 events */
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, cin, ev;
        cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;
        ev = cin | (uint32_t)m[i] << 8 | (k > 1 ? (uint32_t)m[i + 1] << 16 : 0u) | (k > 2 ? (uint32_t)m[i + 2] << 24 : 0u);
        hq_put(ev);
        i += k;
        if (noise_every_us && (i % 30u) == 0u)
            hq_put(0x0Fu | 0xF8u << 8);                 /* realtime clock may come between SysEx packets */
    }
}
static uint32_t pk7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) { acc |= (uint32_t)*in++ << nb; nb += 8; while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; } }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}
static uint32_t up7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) { acc |= (uint32_t)(*in++ & 0x7F) << nb; nb += 7; if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; } }
    return o;
}
static void host_answer(uint32_t addr, const uint8_t *data, uint32_t len)
{
    static uint8_t body[600], wire[700];
    uint32_t s = 0, i, w;
    memcpy(body, "\x00\x59\x30", 3);
    body[3] = (uint8_t)(len + 8); body[4] = (uint8_t)((len + 8) >> 8); body[5] = 0; body[6] = 0;
    memcpy(body + 7, &addr, 4);
    body[11] = (uint8_t)len; body[12] = (uint8_t)(len >> 8); body[13] = 0;
    memcpy(body + 14, data, len);
    for (i = 6; i < 14u + len; i++) s += body[i];
    body[14 + len] = (uint8_t)~s;
    wire[0] = 0xF0;
    w = 1 + pk7(body, 15 + len, wire + 1);
    wire[w++] = 0xF7;
    host_send_sysex(wire, w);
}
static void host_message(const uint8_t *m, uint32_t n)  /* a complete F0..F7 from the device */
{
    uint8_t u[700];
    uint32_t d;
    sysex_in++;
    if (n < 3) return;
    d = up7(m + 1, n - 2, u);
    if (d == 34 && u[0] == 0 && u[1] == 0x59 && u[2] == 0x11) {   /* identity */
        identities++;
        memcpy(identity, u + 6, 27);
        identity[27] = 0;
        return;
    }
    if (d == 15 && u[0] == 0 && u[1] == 0x59 && u[2] == 0x30) {   /* a read request */
        uint32_t addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24, len = u[11] | u[12] << 8, s = 0, i;
        for (i = 6; i < 14; i++) s += u[i];
        if ((uint8_t)~s != u[14]) return;
        requests++;
        if (requests == reset_after_requests) {         /* replugged: the host side starts again */
            sie.rxcsr1[1] = sie.txcsr1[1] = 0;
            sie.intrusb |= 4u;
            hq_r = hq_w;
            return;
        }
        if (addr >= 0xE0000000u) {
            success_asked++;
            host_answer(addr, (const uint8_t *)"success", 8);
            return;
        }
        if (len > 512 || addr + len > image_len) return;
        {
            static uint8_t b[512];
            memcpy(b, image + addr, len);
            if (serve_corrupt_at >= addr && serve_corrupt_at < addr + len) {
                b[serve_corrupt_at - addr] ^= 0x40;
                serve_corrupt_at = 0xFFFFFFFFu;         /* once */
            }
            host_answer(addr, b, len);
        }
    }
}
static void host_take_in(void)                          /* EP1 IN: one packet of events */
{
    uint32_t i;
    if (!(sie.txcsr1[1] & 1u) || !host_reads) return;
    for (i = 0; i + 3 < sie.txn[1]; i += 4) {
        const uint8_t *e = sie.txp[1] + i;
        uint32_t cin = e[0] & 15u, k, nb;
        if (cin >= 4 && cin <= 7) {
            nb = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : 1;
            for (k = 0; k < nb; k++) {
                uint8_t b = e[1 + k];
                if (b == 0xF0) { hx_on = 1; hx_n = 0; }
                if (hx_on && hx_n < sizeof hx) hx[hx_n++] = b;
                if (b == 0xF7 && hx_on) { hx_on = 0; host_message(hx, hx_n); }
            }
        } else if (cin >= 8 && cin <= 14) {
            notes_in++;
            if (hx_on) interleaved++;                    /* a note inside a SysEx message */
        } else if (cin == 15) {
            notes_in++;                                  /* realtime: allowed anywhere */
        }
    }
    sie.txcsr1[1] &= (uint8_t)~1u;
}
static void host_put_out(void)                          /* EP1 OUT: up to 16 events, if the last one was taken */
{
    uint32_t n = 0;
    uint8_t *d = sie.rxdma[1];
    if (!d || (sie.rxcsr1[1] & 1u) || hq_r == hq_w) return;
    while (hq_r != hq_w && n < 64u) {
        uint32_t ev = hq[hq_r++ % (sizeof hq / sizeof hq[0])];
        d[n] = (uint8_t)ev; d[n + 1] = (uint8_t)(ev >> 8); d[n + 2] = (uint8_t)(ev >> 16); d[n + 3] = (uint8_t)(ev >> 24);
        n += 4;
    }
    sie.rxcount[1] = (uint16_t)n;
    sie.rxcsr1[1] |= 1u;
    sie.intrrx |= 2u;
}

/* one step of 50 us: the host's traffic, then (app, normal mode) TIMER5 and the audio ISR */
static void tick(void)
{
    t_us += 50;
#ifndef SIM_LOADER
    if (!recovery_active)
#endif
        fm1_ms = (uint32_t)(t_us / 1000u);
    if (t_us % 1000u == 0) {
        sie.frame = (uint16_t)((sie.frame + 1u) & 0x7FFu);
        host_take_in();
        if (noise_every_us && t_us >= next_noise) {     /* the host's other MIDI: notes and clock */
            static uint32_t k;
            next_noise = t_us + noise_every_us;
            hq_put(k & 1u ? (0x08u | 0x80u << 8 | 60u << 16) : (0x09u | 0x90u << 8 | 60u << 16 | 100u << 24));
            hq_put(0x0Fu | 0xF8u << 8);
            k++;
            noise_sent += 2;
        }
        host_put_out();
    }
#ifdef SIM_LOADER
    if (t_us - last_poll >= 500) { last_poll = t_us; }  /* (the loader polls in ldr_poll / ota_idle) */
#else
    if (!recovery_active && sie.on && t_us - last_poll >= 500) {   /* TIMER5, 2 kHz */
        last_poll = t_us;
        usb_poll();
    }
    if (!recovery_active && audio_alive && t_us - last_drain >= 2900) {   /* the audio ISR: events_block */
        last_drain = t_us;
        mi_r = mi_w;
    }
    if (!recovery_active && dev_midi_out && t_us >= next_devout) {   /* the device's MIDI out (midi_out.c) */
        next_devout = t_us + 700;
        midi_out_event(0x09u | 0x90u << 8 | 64u << 16 | 90u << 24);
        midi_out_event(0x0Fu | 0xF8u << 8);
        midi_out_event(0x08u | 0x80u << 8 | 64u << 16);
    }
#endif
}

#ifdef SIM_LOADER
static void dev_poll(void) { ldr_sim_poll(); }
#else
static void dev_poll(void)
{
    tick();
    if (recovery_active) recovery_poll();
}
#endif

/* ---- host-side control transfers on EP0 ---- */
static void settle(void) { uint32_t i; for (i = 0; i < 40; i++) dev_poll(); }   /* 2 ms: a few device polls */
static int ctrl(uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, uint16_t len, uint8_t *in, uint32_t *got)
{
    uint8_t setup[8] = {rt, rq, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
                        (uint8_t)len, (uint8_t)(len >> 8)};
    uint32_t n = 0, guard;
    sie_ep0_tx = sie_ep0_stall = sie_ep0_status = 0;
    memcpy(sie.ep0dma, setup, 8);
    sie.count0 = 8;
    sie.csr0 |= 1u;
    sie.intrtx |= 1u;
    for (guard = 0; guard < 400; guard++) {
        dev_poll();
        if (sie_ep0_stall) { sie.csr0 |= 0x04u; sie.intrtx |= 1u; settle(); return -1; }
        if (sie_ep0_tx) {                               /* an IN data packet */
            uint32_t k = sie.txn[0] > 64 ? 64 : sie.txn[0];
            if (in && n + k <= 512) memcpy(in + n, sie.txp[0], k);
            n += k;
            sie_ep0_tx = 0;
            sie.csr0 &= (uint8_t)~0x02u;
            sie.intrtx |= 1u;
            if (sie_ep0_status || k < 64 || n >= len) {   /* the last one: the host's status OUT */
                settle();
                sie.intrtx |= 1u;
                settle();
                if (got) *got = n;
                return 0;
            }
            continue;
        }
        if (sie_ep0_status) {                           /* no data stage: status IN done */
            sie.intrtx |= 1u;                           /* (SET_ADDRESS takes effect here) */
            settle();
            if (got) *got = 0;
            return 0;
        }
    }
    return -2;                                          /* no answer */
}

static int fails;
static void check(int ok, const char *what)
{
    printf("usb sim: %-86s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static void bus_reset(void)
{
    sie.rxcsr1[1] = sie.txcsr1[1] = 0;                  /* (a reset drops what the endpoints held) */
    sie.intrusb |= 4u;
    settle();
}

/* enumerate as Windows does (64-byte device descriptor first, reset, address, then the rest);
 * checks the descriptors; returns 1 when configured */
static int enumerate(uint32_t want_pid, int quiet)
{
    static uint8_t d[512];
    uint32_t got = 0, pos, nif = 0, neps = 0, cfg_len;
    int ok = 1, midi_out_ep = 0, midi_in_ep = 0;
    bus_reset();
    ok &= ctrl(0x80, 6, 0x0100, 0, 64, d, &got) == 0 && got == 18 && d[0] == 18 && d[1] == 1;
    bus_reset();
    ok &= ctrl(0x00, 5, 7, 0, 0, 0, 0) == 0 && sie.faddr == 7;
    ok &= ctrl(0x80, 6, 0x0100, 0, 18, d, &got) == 0 && got == 18;
    ok &= (uint32_t)(d[10] | d[11] << 8) == want_pid && d[8] == 0x09 && d[9] == 0x12 && d[17] == 1;
    ok &= ctrl(0x80, 6, 0x0200, 0, 9, d, &got) == 0 && got == 9 && d[1] == 2;
    cfg_len = d[2] | d[3] << 8;
    ok &= ctrl(0x80, 6, 0x0200, 0, 255, d, &got) == 0 && got == cfg_len && cfg_len < 256;
    for (pos = 0; ok && pos < got;) {                   /* every descriptor's length adds up */
        uint32_t l = d[pos];
        if (l < 2 || pos + l > got) { ok = 0; break; }
        if (d[pos + 1] == 4 && d[pos + 3] == 0) nif++;  /* interfaces (alt 0) */
        if (d[pos + 1] == 5) {
            neps++;
            if (d[pos + 2] == 0x01 && d[pos + 3] == 2 && d[pos + 4] == 64) midi_out_ep = 1;
            if (d[pos + 2] == 0x81 && d[pos + 3] == 2 && d[pos + 4] == 64) midi_in_ep = 1;
        }
        pos += l;
    }
    ok &= pos == got && nif == d[4] && midi_out_ep && midi_in_ep;
    ok &= ctrl(0x80, 6, 0x0300, 0, 255, d, &got) == 0 && got == 4;   /* languages */
    ok &= ctrl(0x80, 6, 0x0302, 0x409, 255, d, &got) == 0 && got == d[0] && d[1] == 3;   /* the product */
    ok &= ctrl(0x80, 6, 0x03EE, 0, 18, d, &got) == -1;   /* MS OS string: stalled, as it should */
    ok &= ctrl(0x80, 6, 0x0600, 0, 10, d, &got) == -1;   /* device qualifier: full speed only */
    ok &= ctrl(0x00, 9, 1, 0, 0, 0, 0) == 0 && usb.config == 1;
    ok &= ctrl(0x01, 11, 0, 1, 0, 0, 0) == 0;            /* SET_INTERFACE MIDI streaming alt 0 (Linux) */
    if (!quiet)
        printf("usb sim: enumerated: pid %04x, configuration %u B, %u interfaces, %u endpoints\n",
               want_pid, cfg_len, nif, neps);
    return ok;
}

static void run_ms(uint32_t ms)
{
    uint64_t end = t_us + ms * 1000ull;
    while (t_us < end) dev_poll();
}

static void handshake(void)
{
    static const uint8_t HS[] = {0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
    identities = 0;
    host_send_sysex(HS, sizeof HS);
}
static void upgrade_key(void)
{
    static const uint8_t UP[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};
    host_send_sysex(UP, sizeof UP);
}

static uint8_t *read_file(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long l;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)l);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return 0; }
    fclose(f);
    *n = (uint32_t)l;
    return b;
}
static void logical_of(const uint8_t *raw, uint32_t n)   /* drop the 20 identity marker bytes */
{
    uint32_t i, o = 0;
    image = malloc(n);
    for (i = 0; i < n; i++)
        if (!(i < 20u * 0x30u && i % 0x30u == 0x2Fu)) image[o++] = raw[i];
    image_len = o;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void ufw_file(uint32_t type, uint32_t *off, uint32_t *len)   /* an entry of the UFW list (decoded) */
{
    uint32_t i, nent;
    uint8_t h[0x40], e[0x50];
    memcpy(h, image, 0x40);
    ota_jl_enc(h, 0x40);
    nent = rd16(h + 8);
    *off = *len = 0;
    for (i = 0; i < nent; i++) {
        memcpy(e, image + 0x40 + i * 0x50, 0x50);
        ota_jl_enc(e, 0x50);
        if (rd16(e) == type) { *off = rd32(e + 8); *len = rd32(e + 12); }
    }
}

#ifndef SIM_LOADER
/* the app: the main loop as main.c / recovery.c run it, until a session ends (or ms pass) */
static int app_session(uint32_t ms)
{
    uint64_t end = t_us + ms * 1000ull;
    while (t_us < end) {
        if (recovery_active) {
            tick();
            recovery_step();                             /* (polls, ota_service, the session) */
            if (committed) return 1;
            continue;
        }
        dev_poll();
        usb_in_guard(fm1_ms);
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            ota_session();
            return committed != 0;
        }
    }
    return 0;
}

static void reset_world(int rescue)
{
    memset(nor, 0xFF, sizeof nor);
    committed = 0;
    hq_r = hq_w = 0;
    mi_r = mi_w = 0;
    mo_r = mo_w = 0;
    so_r = so_w = 0;
    sx_ready = 0;
    usb.ota_req = usb.uboot_req = 0;
    interleaved = sysex_in = notes_in = requests = success_asked = 0;
    noise_every_us = 0;
    dev_midi_out = 0;
    audio_alive = 1;
    host_reads = 1;
    reset_after_requests = 0xFFFFFFFFu;
    recovery_active = (uint8_t)rescue;
    ota_deadline = 0;
    usb.detached = 0;
    usb_start();
    if (rescue) {
        recovery_last = recovery_usb_last = fm1_ticks();
        recovery_fraction = 0;
    }
}

static int staged_ok(void)                               /* the package's loader in the staging area */
{
    uint32_t off, len;
    ufw_file(100, &off, &len);
    return off && !memcmp(nor + 0xE0000u + 32u, image + off + 32u, len - 32u) && committed_parm[6] == 0x41;
}

int main(int argc, char **argv)
{
    uint8_t *raw;
    uint32_t n;
    if (argc < 2 || !(raw = read_file(argv[1], &n))) { fprintf(stderr, "usb_sim_test PACKAGE.fwsc\n"); return 1; }
    logical_of(raw, n);

    reset_world(0);
    check(enumerate(0x0001, 0), "app: enumerates (device, configuration with MIDI + audio + CDC, strings, address)");
    handshake();
    run_ms(100);
    for (n = 0; n < 50 && !identities; n++) { ota_service(); run_ms(5); }
    check(identities == 1 && !strcmp(identity, "FM-1_900"), "app: the M-UPGRADE handshake answers FM-1_900");

    reset_world(0);
    enumerate(0x0001, 1);
    noise_every_us = 3000;
    dev_midi_out = 1;
    upgrade_key();
    check(app_session(60000) && staged_ok() && !nor_bad_writes && !interleaved,
          "app: a whole update session over USB, host notes + clock in, device MIDI out busy: staged, committed");
    printf("usb sim:   %u read requests, %u host events mixed in, %u device events out, 0 notes inside SysEx\n",
           requests, noise_sent, notes_in);

    reset_world(0);
    enumerate(0x0001, 1);
    noise_every_us = 2000;
    audio_alive = 0;                                     /* nothing empties the MIDI in ring */
    run_ms(500);                                         /* (the host's notes fill it meanwhile) */
    upgrade_key();
    check(app_session(60000) && staged_ok(), "app: the audio dead, MIDI in ring full: the update still gets through (usb_guard.c)");

    reset_world(0);
    enumerate(0x0001, 1);
    reset_after_requests = 4;                            /* replugged in the middle */
    upgrade_key();
    check(!app_session(40000) && !committed, "app: a bus reset in the middle of a session: nothing committed");
    reset_after_requests = 0xFFFFFFFFu;
    enumerate(0x0001, 1);
    hq_r = hq_w;
    upgrade_key();
    check(app_session(60000) && staged_ok(), "app: ... the next try (enumerated again) succeeds");

    reset_world(0);
    enumerate(0x0001, 1);
    upgrade_key();
    host_reads = 0;                                      /* the host stops reading for 3 s, then reads again */
    {
        uint64_t end = t_us + 3000000ull;
        while (t_us < end) { dev_poll(); ota_service(); if (usb.ota_req) break; }
    }
    host_reads = 1;
    if (usb.ota_req) { usb.ota_req = 0; ota_session(); }
    check(committed && staged_ok(), "app: the host not reading for 3 s: the device retries, the session completes");

    /* the USB rescue: no TIMER5, no audio; the host keeps sending notes and clock (a DAW) */
    reset_world(1);
    check(enumerate(0x0001, 1), "rescue: enumerates");
    noise_every_us = 2000;
    run_ms(1000);                                        /* ~1000 events: far more than the ring holds */
    handshake();
    {
        uint64_t end = t_us + 2000000ull;
        while (t_us < end && !identities) { tick(); recovery_step(); }
    }
    check(identities >= 1 && !strcmp(identity, "FM-1_000"), "rescue: with a DAW's notes and clock coming in, the handshake answers FM-1_000");
    upgrade_key();
    check(app_session(90000) && staged_ok() && !interleaved,
          "rescue: with the notes and clock going on, the whole update session completes");

    {   /* unknown and abandoned requests leave EP0 working */
        uint8_t d[64];
        uint32_t got;
        int ok = 1;
        reset_world(0);
        enumerate(0x0001, 1);
        ok &= ctrl(0x80, 0x33, 0, 0, 8, d, &got) == -1;              /* an unknown request: stalled */
        ok &= ctrl(0x00, 3, 1, 0, 0, 0, 0) == -1;                    /* SET_FEATURE remote wakeup: stalled */
        ok &= ctrl(0x80, 6, 0x0100, 0, 0, d, &got) == 0;             /* wLength 0 */
        ok &= ctrl(0x80, 0, 0, 0, 2, d, &got) == 0 && got == 2;      /* GET_STATUS */
        ok &= ctrl(0x80, 8, 0, 0, 1, d, &got) == 0 && got == 1 && d[0] == 1;   /* GET_CONFIGURATION */
        ok &= ctrl(0x02, 1, 0, 0x81, 0, 0, 0) == 0;                  /* CLEAR_FEATURE halt EP1 IN */
        ok &= ctrl(0x02, 1, 0, 0x85, 0, 0, 0) == -1;                 /* .. an endpoint we have not */
        sie.csr0 |= 0x10u;                                           /* SetupEnd: the host gave up a transfer */
        sie.intrtx |= 1u;
        settle();
        ok &= ctrl(0x80, 6, 0x0100, 0, 18, d, &got) == 0 && got == 18;
        ok &= ctrl(0x00, 9, 1, 0, 0, 0, 0) == 0 && ctrl(0x00, 9, 1, 0, 0, 0, 0) == 0;   /* configured twice */
        handshake();
        identities = 0;
        for (n = 0; n < 50 && !identities; n++) { run_ms(5); ota_service(); }
        check(ok && identities, "app: unknown, stalled and abandoned requests, a second SET_CONFIGURATION: still updatable");
    }
    printf(fails ? "usb sim (app): %d FAILED\n" : "usb sim (app) passed\n", fails);
    return fails;
}

#else /* SIM_LOADER */

int main(int argc, char **argv)
{
    uint8_t *raw;
    uint32_t n, fl_off, fl_len, s;
    static uint8_t want[0x93000];
    int rc;
    if (argc < 2 || !(raw = read_file(argv[1], &n))) { fprintf(stderr, "usb_sim_test PACKAGE.fwsc\n"); return 1; }
    logical_of(raw, n);
    ufw_file(0, &fl_off, &fl_len);
    memcpy(want, image + fl_off, sizeof want);
    /* the device: the package's head (chip key) and an app area that differs (another firmware) */
    memset(nor, 0xFF, sizeof nor);
    memcpy(nor, want, 0x4000);
    for (s = 0x4000; s < 0x93000u; s++) nor[s] = (uint8_t)(s * 7u);
    {   /* the update record the app left (0xE4F00) */
        uint8_t r[112] = {0};
        r[2] = 0x0D; r[3] = 0x5A; r[4] = 0x01; r[5] = 0x5A; r[6] = 0x41; r[7] = 0x54;
        n = ota_crc16(r + 2, 78, 0);
        r[0] = (uint8_t)n; r[1] = (uint8_t)(n >> 8);
        memcpy(nor + 0xE4F00u, r, 112);
    }
    usb_start();
    check(enumerate(0x0002, 0), "loader: enumerates (Felucca Update, MIDI only)");
    handshake();
    for (n = 0; n < 200 && !identities; n++) { dev_poll(); ota_service(); }
    check(identities == 1 && !strcmp(identity, "ota-FM-1_900"), "loader: the handshake answers ota-FM-1_900 (the installers' resume)");

    noise_every_us = 2000;                               /* notes and clock all along */
    serve_corrupt_at = fl_off + 0x30000u;                /* one byte damaged on the way, once */
    upgrade_key();
    rc = 1;
    for (n = 0; n < 4000000 && rc; n++) {
        dev_poll();
        ota_service();
        if (usb.ota_req) { usb.ota_req = 0; rc = ldr_session(); break; }
    }
    check(rc == 0, "loader: a whole session over USB with notes and clock coming in, one damaged byte (pass 2 fixes it)");
    check(!memcmp(nor + 0x4000, want + 0x4000, 0x93000u - 0x4000u) && !nor_bad_writes,
          "loader: the app area is the package's, byte for byte; the head never written");
    check(nor[0xE4F00] == 0xFF && rec_cleared, "loader: the update records dropped: the new app boots");
    printf("usb sim:   %u read requests, %u host events mixed in\n", requests, noise_sent);
    printf(fails ? "usb sim (loader): %d FAILED\n" : "usb sim (loader) passed\n", fails);
    return fails;
}
#endif
