/*
 * protocol.h — the Giris raw-HID wire format.
 *
 * Transport: one HID interface, vendor usage page 0xFF60 / usage 0x61 (the QMK
 * raw-HID convention), 64-byte IN and OUT interrupt endpoints at bInterval = 1,
 * and NO report IDs.
 *
 * Why unnumbered reports: TinyUSB calls tud_hid_set_report_cb() with report_id
 * hardcoded to 0 for anything arriving on the interrupt OUT endpoint, so a real
 * report ID would silently end up in buf[0] and break dispatch. Unnumbered also
 * means WebHID uses reportId 0 in both directions, which is the simplest host
 * contract. The cost is that every report is a full 64 bytes.
 *
 * Why this usage page: Chromium protects usage page 0x07 and the GenericDesktop
 * pointer/mouse/keyboard/keypad usages per top-level collection, and its static
 * blocklist names vendor page 0xFF00 — so 0xFF60/0x61 is claimable by a page.
 * This firmware also exposes NO keyboard usages, which is what keeps macOS from
 * demanding Input Monitoring permission.
 *
 * This header is the single source of truth: the web viewer mirrors it.
 */
#ifndef GIRIS_PROTOCOL_H
#define GIRIS_PROTOCOL_H

#include <stdint.h>

#define PROTO_REPORT_SIZE     64
#define PROTO_VERSION         1

/* ----- host -> device ------------------------------------------------- */
enum {
  CMD_INFO         = 0x01,  /* -> RSP_INFO */
  CMD_STREAM_SET   = 0x02,  /* args: [2]=on/off, [3]=decimation (1..255) */
  CMD_SNAPSHOT     = 0x03,  /* -> RSP_SNAPSHOT, one frame right now */
  CMD_BURST_START  = 0x04,  /* args: [2]=slot 0..7, [3..4]=count LE (<= BURST_MAX) */
  CMD_BURST_STATUS = 0x05,  /* -> RSP_BURST_STATUS */
  CMD_BURST_READ   = 0x06,  /* args: [2..3]=sample offset LE -> RSP_BURST_DATA */
  CMD_BURST_ABORT  = 0x07,
  CMD_BOOTLOADER   = 0x7E,  /* jump to the ROM DFU — no buttons needed */
};

/* ----- device -> host ------------------------------------------------- */
enum {
  RSP_INFO         = 0x81,
  RSP_STREAM       = 0x82,  /* unsolicited */
  RSP_SNAPSHOT     = 0x83,
  RSP_BURST_STATUS = 0x85,
  RSP_BURST_DATA   = 0x86,
  RSP_ERROR        = 0xEE,
};

/* Every IN report starts: [0]=msg, [1]=tag (echoes the request; 0 if unsolicited),
 * [2..3]=seq, a single u16 LE counter bumped on EVERY IN report of every type, so
 * one gap in seq detects a lost stream frame, burst chunk or response alike. */
#define PROTO_HDR_LEN         4

#define PROTO_NUM_SLOTS       8    /* 2 mux states x 4 ADC channels */
#define PROTO_NUM_KEYS        6

/* slot_map[] in RSP_INFO labels each hardware scan slot. Sent over the wire so a
 * respin or a mux rewiring does not need a new viewer build. */
#define SLOT_VBUS_DIV         0xFE
#define SLOT_UNUSED           0xFF

/* RSP_INFO body (offset 4):
 *   [4]      protocol version
 *   [5]      num_keys
 *   [6]      num_slots
 *   [7]      adc_bits (12)
 *   [8..9]   scan_hz LE          (full frames per second)
 *   [10..17] slot_map[8]
 *   [18..21] counts_per_gauss_q8 LE   (6.144 * 256 = 1573)
 *   [22..25] firmware build id LE
 */

/* RSP_STREAM body (offset 4): a run of frames, oldest first.
 *   [4]      frame_count in this report
 *   [5]      flags: bit0 = frames were dropped before this one
 *   [6..7]   decimation currently in force
 *   then frame_count x { u32 frame_index LE, u16 key[6] LE }  = 16 bytes each
 * 3 frames fit in a 64-byte report (4 + 4 + 3*16 = 56). */
#define PROTO_STREAM_HDR      8
#define PROTO_FRAME_BYTES     16
#define PROTO_FRAMES_PER_RPT  3

/* RSP_SNAPSHOT body (offset 4): u32 frame_index, then u16 slot[8] raw, in
 * hardware scan order — deliberately raw and unmapped, for debugging the mux. */

/* Burst capture: one slot, sampled as fast as the ADC allows, into SRAM. */
#define PROTO_BURST_MAX       8192
/* RSP_BURST_STATUS body: [4]=state (0 idle, 1 running, 2 done), [5]=slot,
 *   [6..7]=captured LE, [8..11]=sample_period_ns LE
 * RSP_BURST_DATA body:   [4..5]=offset LE, [6]=count, then count x u16 LE
 *   (28 samples per report) */
#define PROTO_BURST_PER_RPT   28

#endif
