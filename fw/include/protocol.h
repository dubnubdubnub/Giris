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
#define PROTO_VERSION         3

/* ----- host -> device ------------------------------------------------- */
enum {
  CMD_INFO         = 0x01,  /* -> RSP_INFO */
  CMD_STREAM_SET   = 0x02,  /* args: [2]=on/off, [3]=decimation (1..255) */
  CMD_SNAPSHOT     = 0x03,  /* -> RSP_SNAPSHOT, one frame right now */
  CMD_BURST_START  = 0x04,  /* args: [2]=slot 0..7, [3..4]=count LE (<= BURST_MAX) */
  CMD_BURST_STATUS = 0x05,  /* -> RSP_BURST_STATUS */
  CMD_BURST_READ   = 0x06,  /* args: [2..3]=sample offset LE -> RSP_BURST_DATA */
  CMD_BURST_ABORT  = 0x07,
  CMD_SEQ_SET      = 0x08,  /* args [2..5] = ADC channel for rank 1..4 */
  CMD_LINK_TEST    = 0x09,  /* args: [2]=link_mode_t, [3..6]=baud LE, [7..8]=nbytes LE,
                             *       [9]=link_role_t (0 loop, 1 tx, 2 rx), [10..11]=rx timeout ms */
  CMD_LINK_HOLD    = 0x0B,  /* args: [2]=pin (0 PC6/D-, 1 PC7/D+), [3]=1 hold low, 0 release */
  CMD_PEER         = 0x0C,  /* args: [2]=0 query, 1 enable+restart, 2 disable -> RSP_PEER */
  CMD_SPLIT        = 0x0D,  /* args: [2]=0 query, 1 test pattern on, 2 off -> RSP_SPLIT */
  CMD_LINK_PROBE   = 0x0A,  /* args: [2]=link_mode_t, [3..6]=baud LE -> RSP_PROBE */
  CMD_BOOTLOADER   = 0x7E,  /* jump to the ROM DFU — no buttons needed */
};

/* ----- device -> host ------------------------------------------------- */
enum {
  RSP_INFO         = 0x81,
  RSP_STREAM       = 0x82,  /* unsolicited */
  RSP_SNAPSHOT     = 0x83,
  RSP_BURST_STATUS = 0x85,
  RSP_BURST_DATA   = 0x86,
  RSP_SEQ          = 0x88,
  RSP_LINK         = 0x89,
  RSP_PROBE        = 0x8A,
  RSP_PEER         = 0x8B,
  RSP_SPLIT        = 0x8C,
  RSP_ERROR        = 0xEE,
};

/* Every IN report starts: [0]=msg, [1]=tag (echoes the request; 0 if unsolicited),
 * [2..3]=seq, a single u16 LE counter bumped on EVERY IN report of every type, so
 * one gap in seq detects a lost stream frame, burst chunk or response alike. */
#define PROTO_HDR_LEN         4

#define PROTO_NUM_SLOTS       10   /* 2 mux states x 5 conversions (4 real + 1 dummy) */
#define PROTO_SEQ_LEN         5
#define PROTO_NUM_KEYS        6

/* slot_map[] in RSP_INFO labels each hardware scan slot. Sent over the wire so a
 * respin or a mux rewiring does not need a new viewer build. */
#define SLOT_VBUS_DIV         0xFE
#define SLOT_UNUSED           0xFF
#define SLOT_DUMMY            0xFD   /* discarded: absorbs the VBUS charge step */

/* RSP_INFO body (offset 4):
 *   [4]      protocol version
 *   [5]      num_keys
 *   [6]      num_slots
 *   [7]      adc_bits (12)
 *   [8..9]   scan_hz LE          (full frames per second)
 *   [10..19] slot_map[PROTO_NUM_SLOTS]
 *   [20..23] counts_per_gauss_q8 LE   (6.144 * 256 = 1573)
 *   [24..27] firmware build id LE
 *   [28..32] ordinary-sequence channel order, rank 1..5
 *   [33..44] the 96-bit factory UID, low word first
 *   [45..46] uid_tag LE — the 16-bit condensation used for link arbitration
 *   [47]     link sense: bit0 = PB12 /LM_ST, bit1 = PB10 /AP_FAULT,
 *            bit2 = PC13 AP22653 EN, bit3 = PC6 pad, bit4 = PC7 pad
 *
 * Two halves run one image, so every field above that is not identical between
 * them is the interesting one. The UID is what makes a report attributable.
 */
#define PROTO_INFO_UID        33
#define PROTO_INFO_UID_TAG    45
#define PROTO_INFO_SENSE      47

/* RSP_STREAM body (offset 4): a run of frames, oldest first.
 *   [4]      frame_count in this report
 *   [5]      flags: bit0 = frames were dropped before this one
 *   [6..7]   decimation currently in force
 *   then frame_count x { u32 frame_index LE, u16 key[6] LE }  = 16 bytes each
 * 3 frames fit in a 64-byte report (4 + 4 + 3*16 = 56). */
#define PROTO_STREAM_HDR      8
#define PROTO_FRAME_BYTES     16
#define PROTO_FRAMES_PER_RPT  3

/* RSP_SNAPSHOT body: [4..7] u32 frame_index, [8..] u16 slot[PROTO_NUM_SLOTS] raw
 * in hardware scan order, then at fixed offsets [48] phase_errors u32 and
 * [52] tx_dropped u32. Deliberately raw and unmapped, for debugging the mux.
 *
 * A snapshot is ALWAYS answered, even when the scan engine cannot produce a
 * frame — silence is not a diagnosis:
 *   [56]     flags: bit0 = the frame read failed, so [4..47] are zeros
 *   [57..60] adc_read_failures LE
 *   [61]     low byte of the raw seqlock counter; odd means the writer is stuck
 *            mid-update, which is the only way the read fails four times running */
#define PROTO_SNAP_FLAGS      56
#define PROTO_SNAP_READ_FAIL  57
#define PROTO_SNAP_SEQ_LSB    61

/* RSP_PEER body — where link arbitration has got to. Both halves run one image,
 * so the interesting check is that their answers are complementary: one A and
 * one B, each naming the other's tag, both RUNNING at the same baud.
 *   [4]      state   0 disabled, 1 discover, 2 alone, 3 paired, 4 switching,
 *                    5 running, 6 incompatible (peer speaks a different link
 *                    protocol; parked on the discovery bus, never framing)
 *   [5]      role    0 unknown, 1 A (lower tag, unswapped), 2 B (TRPSWAP)
 *   [6..7]   our uid_tag LE
 *   [8..9]   peer uid_tag LE
 *   [10..11] peer LINK PROTOCOL version LE — not its firmware build. Compare
 *            against our own PEER_PROTO; unequal is why state would be 6
 *   [12..15] baud currently in force LE
 *   [16..19] hails sent LE
 *   [20..23] valid frames received LE
 *   [24..27] CRC errors LE
 *   [28..31] successful switches into RUNNING LE
 *   [32..35] drops back to discovery LE
 *   [36..39] ms since the last valid frame from the peer LE */
#define PROTO_PEER_STATE   4

/* RSP_SPLIT body — the run-phase data plane. Only meaningful while RSP_PEER
 * says state 5. The two numbers that decide whether the split is sound are
 * [36..39] period_max_us, which should sit near 125, and [24..27]
 * payload_errors, which must be exactly zero.
 *   [4..7]   frames transmitted LE
 *   [8..11]  frames received and accepted LE
 *   [12..15] CRC rejects LE  (a small count while re-locking is normal)
 *   [16..19] sequence gaps — frames the peer sent that never arrived LE
 *   [20..23] resync slides, in bytes LE
 *   [24..27] payload errors: good CRC, wrong contents. MUST be 0 LE
 *   [28..31] us since the last accepted frame LE
 *   [32..35] most recent inter-arrival, us LE
 *   [36..39] worst inter-arrival, us LE   <- the jitter figure
 *   [40..43] best inter-arrival, us LE
 *   [44..45] peer sequence number LE
 *   [46]     peer frame flags: bit0 keys, bit1 test pattern, bit2 peer has a host
 *   [47]     peer key data is stale (older than 5 ms)
 *   [48]     1 if we are sending the test pattern
 *   [49]     how many peer key values follow
 *   [50..63] peer key values, u16 LE — 7 of them, which is all a 64-byte
 *            report has room for. Enough for the 6-key dev board; the 32-key
 *            build will need a paged read. */
#define PROTO_SPLIT_TX     4
#define PROTO_SPLIT_KEYS   50

/* Burst capture: one slot, sampled as fast as the ADC allows, into SRAM. */
#define PROTO_BURST_MAX       8192
/* RSP_BURST_STATUS body: [4]=state (0 idle, 1 running, 2 done), [5]=slot,
 *   [6..7]=captured LE, [8..11]=sample_period_ns LE
 * RSP_BURST_DATA body:   [4..5]=offset LE, [6]=count, then count x u16 LE
 *   (28 samples per report) */
#define PROTO_BURST_PER_RPT   28

/* RSP_LINK body — the result of one CMD_LINK_TEST run:
 *   [4]      mode echoed
 *   [5..8]   baud LE
 *   [9..10]  sent LE
 *   [11..12] received LE
 *   [13..14] mismatched LE
 *   [15..16] timeouts LE
 *   [17]     first mismatching byte sent
 *   [18]     what came back instead
 *   [19]     USART error flags: bit0 PERR, bit1 FERR, bit2 NERR, bit3 ROERR
 *   [20]     link sense (same encoding as RSP_INFO[47])
 *   [21..36] USART6 STS, CTRL1, CTRL2, CTRL3 as the run left them
 *   [37..38] overruns LE — bytes the receive loop was too slow to collect
 *   [39..42] cycles LE — core cycles the payload occupied the wire (216 MHz)
 *
 * Modes mirror link_mode_t in include/link.h. Every one of them needs the loop
 * closed outside the chip — a peer on J1, or a TP1-TP2 jumper — because this
 * silicon does not echo its own half-duplex transmission back into RDBF:
 *   1 = half duplex open drain on PC6 (J1 D-)   — the discovery bus
 *   2 = half duplex open drain on PC7 (J1 D+)   — same, TRPSWAP
 *   3 = full duplex push-pull, TX PC6 / RX PC7
 *   4 = full duplex push-pull, TX PC7 / RX PC6  — the TRPSWAP half
 *   5 = half duplex on PC6 push-pull            — diagnostic only
 *
 * CMD_LINK_PROBE is the one that works alone: it checks the pads and sweeps the
 * MUX index electrically, without needing anything to answer. */
#define PROTO_LINK_MODE_HD_PC6   1
#define PROTO_LINK_MODE_HD_PC7   2
#define PROTO_LINK_MODE_FD       3
#define PROTO_LINK_MODE_FD_SWAP  4
#define PROTO_LINK_MODE_HD_PP    5   /* half duplex, push-pull — diagnostic */

/* RSP_PROBE body — electrical truth about the link pins, no peer required:
 *   [4]      pads: bit0 PC6 reads low when driven low, bit1 PC6 reads high when
 *            released to its 10k, bit2/3 the same for PC7. 0xF is a healthy pair.
 *   [5]      /LM_ST and /AP_FAULT read with the internal pull-up  (bit0 PB12, bit1 PB10)
 *   [6]      the same two read with the internal pull-down
 *            -> equal means driven, different means floating
 *   [7..8]   samples per MUX index LE
 *   [9..40]  16 x u16 LE: how many of those samples caught the TX pad low while
 *            the USART shifted out 0x00. The MUX index that routes USART6 shows
 *            ~90%; every other index shows ~0. */
#define PROTO_PROBE_PADS      4
#define PROTO_PROBE_SENSE_PU  5
#define PROTO_PROBE_SENSE_PD  6
#define PROTO_PROBE_SAMPLES   7
#define PROTO_PROBE_MUX       9

#endif
