/*
 * split.h — the run-phase data plane: what actually crosses J1 once the two
 * halves have agreed on a rate.
 *
 * peer.c gets the halves talking. This is what they say afterwards, 8000 times
 * a second, forever.
 *
 * Shape, and why
 * --------------
 * **Fixed size, sent unconditionally, both directions, every scan period.** Not
 * event-driven. An 8 kHz keyboard exists to make latency *bounded*, and a frame
 * that is only sent when something changed makes the interesting case — the
 * frame right after you press a key — the one that queues behind everything
 * else. Constant traffic also means the receiver's clock recovery never has to
 * re-acquire, and a lost frame is visible as a sequence gap rather than as
 * silence indistinguishable from "nothing happened".
 *
 * 48 bytes, word-aligned, at 12 Mbaud 8N1 = 480 bit-times = **40 us**, against
 * a 125 us period. 32 % duty each way on a full-duplex pair, so the wire is
 * idle for 85 us between frames and the two halves' independent 8 kHz domains
 * can drift a long way before anything overlaps.
 *
 *   0    sync      u8    0x5A — deliberately NOT peer.c's 0xA5, so a control
 *                        frame left in the ring across a rate change can never
 *                        be mistaken for data
 *   1    flags     u8    SPLIT_F_*
 *   2    seq       u16   per-frame, wraps; the receiver's only drop detector
 *   4    ctrl      u8    piggybacked control channel — token transfer, layer
 *   5    ctrl_arg  u8    state and lock LEDs ride here rather than on frames of
 *   6    ctrl_data u16   their own, so the wire has exactly one frame shape
 *   8..43  keys    36 B  32 keys x 9 bits, LSB-first
 *   44..47 crc     u32
 *
 * Why CRC-32 and not the CRC-16 the architecture doc planned
 * ----------------------------------------------------------
 * The doc costed CRC-16/CCITT on the hardware CRC unit. That unit turns out to
 * have a programmable polynomial (CRC_POLY, POLY_SIZE = 16b), so CRC-16 was
 * available — but its RESET DEFAULT is already the 32-bit Ethernet polynomial,
 * it consumes a word per write, and the header+payload is 44 bytes = 11 words
 * exactly. CRC-32 therefore costs the same eleven register writes as CRC-16
 * would, needs no configuration at all, and detects every burst up to 32 bits
 * instead of 16. The 2 extra bytes are what makes the frame word-aligned.
 *
 * The unit is a single global with internal state and no lock. Everything here
 * runs from the main loop; **nothing in an ISR may touch the CRC unit** without
 * adding one.
 *
 * Nine-bit keys
 * -------------
 * 32 x 9 = 288 bits = 36 bytes exactly, which is the whole reason the doc chose
 * centi-mm (400 steps) as the pipeline's unit. There is no travel pipeline yet,
 * so what actually ships today is the raw ADC count shifted down to 9 bits —
 * the transport is final, the unit is provisional. See split_pack_keys().
 */
#ifndef GIRIS_SPLIT_H
#define GIRIS_SPLIT_H

#include <stdint.h>
#include <stdbool.h>

#define SPLIT_SYNC        0x5Au
#define SPLIT_KEYS        32
#define SPLIT_KEY_BYTES   36        /* 32 x 9 bits, exactly */
#define SPLIT_HDR_BYTES   8
#define SPLIT_CRC_WORDS   11        /* header + keys = 44 B = 11 words */
#define SPLIT_FRAME_LEN   48

/* Frame flags. */
#define SPLIT_F_KEYS      0x01u     /* payload is live key data */
#define SPLIT_F_TEST      0x02u     /* payload is the deterministic test pattern */
#define SPLIT_F_HOST      0x04u     /* this half has an enumerated USB host.
                                     * Both halves setting it is topology (b),
                                     * the dual-host KVM case. */

/* Two tiers, because they answer different questions.
 *
 * STALE_US is "should I still be holding the peer's keys down?" — 5 ms is 40
 * consecutive lost frames, far beyond any plausible hiccup and still well under
 * human perception. Past it, the peer's keys are released; the link stays up.
 *
 * peer.c's own LIVENESS_MS is the separate question of whether the link is
 * worth keeping at all. A brief stall must not tear down and renegotiate a
 * working 12 Mbaud link. */
#define SPLIT_STALE_US    5000u

typedef struct {
  uint32_t tx_frames;
  uint32_t rx_frames;
  uint32_t crc_errors;     /* 48 bytes starting on a 0x5A that failed the CRC.
                            * Includes false syncs inside a payload, so a small
                            * nonzero count while resyncing is normal; a climbing
                            * count while aligned is not. */
  uint32_t seq_gaps;       /* frames the sequence number says never arrived */
  uint32_t resyncs;        /* bytes discarded hunting for frame alignment */
  uint32_t payload_errors; /* test pattern wrong despite a GOOD CRC. Must be 0.
                            * Anything else means a frame was accepted that was
                            * not the frame that was sent. */
  uint32_t age_us;         /* since the last accepted frame */
  uint32_t period_us;      /* most recent inter-arrival */
  uint32_t period_min_us;
  uint32_t period_max_us;  /* the jitter number. 125 us nominal. */
  uint16_t peer_seq;
  uint8_t  peer_flags;
  uint8_t  stale;          /* peer key data is older than SPLIT_STALE_US */
} split_stats_t;

void split_init(void);

/* True once the scan engine has produced a frame we have not sent yet. This is
 * what paces the link: the period comes from the ADC's own 8 kHz tick, so the
 * frame carries data that is at most one scan old and the two rates cannot
 * drift apart. */
bool split_tx_due(void);

/* Builds the next outbound frame, CRC included, and returns it. 4-byte aligned
 * and SPLIT_FRAME_LEN long. Must not be called while a previous frame is still
 * being DMA'd out of it. */
const void *split_build_tx(void);

/* Validates and consumes one candidate frame. The pointer must be 4-byte
 * aligned. False means "not a frame" — the caller should slide one byte and try
 * again rather than discarding all 48, because a payload byte that happens to
 * be 0x5A is a perfectly ordinary event and eating the real sync behind it is
 * how a link stays mis-framed forever. */
bool split_rx(const void *frame);

/* Called on entry to and exit from the run phase. Resets sequence tracking so a
 * renegotiated link does not report a 60000-frame gap. */
void split_start(void);
void split_stop(void);

void split_stats(split_stats_t *out);

/* The receiver slides a byte at a time to find frame alignment; that hunting
 * belongs in the same stats block as everything else. */
void split_note_resync(void);

/* The peer's most recent key values, 9-bit. Meaningless while stats.stale. */
const uint16_t *split_peer_keys(void);

/* Send the deterministic pattern instead of key data, so the frame error rate
 * can be measured without the ADC in the loop. */
void split_set_test(bool on);
bool split_test_enabled(void);

/* Feeds SPLIT_F_HOST. Topology detection reads it off the peer's frames. */
void split_set_host(bool present);

#endif
