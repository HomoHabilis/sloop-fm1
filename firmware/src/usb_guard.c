/* SPDX-License-Identifier: GPL-3.0-only */
/* The update must stay possible whatever the rest of the app does. usb.c holds a USB-MIDI packet back
 * (NAK) while the MIDI in ring is too full for it (ep1_take), so a burst never loses a note-off; the audio
 * ISR (seq.c events_block) empties the ring. If nothing empties it (the audio stopped, or stuck), every
 * later packet stays held back, the installer's SysEx too, and the FM-1 could no longer be updated over
 * USB. Called from the main loop and while an update session waits (felucca.c ota_idle): a ring that has
 * been too full for a packet and not emptied for USB_GUARD_MS is emptied here (those notes are lost; the
 * update gets through). App only: the update loader never holds packets back, the USB rescue empties the
 * ring itself (recovery.c). */
#define USB_GUARD_MS 50u                  /* the audio ISR empties it every <= 6 ms: 50 ms unmoved = dead */
static uint32_t usb_guard_r, usb_guard_ms, usb_guard_drops;
static uint8_t usb_guard_stuck;
static void usb_in_guard(uint32_t now_ms)
{
    if (mi_r != usb_guard_r || MQ - (mi_w - mi_r) >= EP1_ROOM) {
        usb_guard_r = mi_r;                      /* emptied, or room for a packet: all is well */
        usb_guard_stuck = 0;
    } else if (!usb_guard_stuck) {
        usb_guard_stuck = 1;                     /* the clock starts at the first look that finds it stuck: */
        usb_guard_ms = now_ms;                   /* fm1_ms jumps after an IRQ-off flash erase, and the audio */
    } else if (now_ms - usb_guard_ms > USB_GUARD_MS) {   /* has not run yet then; it has by the next look */
        fm1_irq_off();                           /* (TIMER5 writes mi_w, the audio ISR mi_r) */
        mi_r = mi_w;
        fm1_irq_on();
        usb_guard_r = mi_r;
        usb_guard_stuck = 0;
        usb_guard_drops++;
    }
}
