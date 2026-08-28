/*
 * peer.h — link discovery, role arbitration and rate negotiation.
 *
 * One image runs on both halves, so this has to be completely symmetric: no
 * "master" build, no jumper, no strap. Everything is decided at run time from
 * facts the halves exchange.
 *
 * Three constraints shape the design, all measured on this hardware:
 *
 *  1. A straight-through USB-C cable joins D+ to D+ and D- to D-, so two
 *     identical halves land TX on TX. Exactly one must set CTRL2.TRPSWAP, and
 *     until they have talked, neither knows which. The only channel that exists
 *     before that agreement is SLBEN half-duplex on the D- wire, open-drain
 *     against the fitted 10k pull-ups — a wired-AND bus both halves can drive
 *     symmetrically.
 *
 *  2. That bus dies above 500 kbaud (1020 of 1024 bytes corrupt with FERR at
 *     1 Mbaud, unchanged between a 1.5 m and a 3 m cable, because the limit is
 *     the pull-up RC and not the cable). Discovery runs at 115200, with 4x
 *     margin. The run phase is push-pull full duplex at 12 Mbaud.
 *
 *  3. **This silicon does not echo its own half-duplex transmission**, so
 *     read-back collision detection is impossible. Two halves hailing at once
 *     simply do not hear each other and neither can tell. The protocol therefore
 *     never relies on detecting a collision: each half hails on a period derived
 *     from its own uid_tag, so two halves cannot stay in lockstep — their phases
 *     drift apart within a few periods and one hail lands in the other's listen
 *     window.
 *
 * Tie-break is uid_tag, FNV-1a over the whole 96-bit UID. Lower tag becomes A
 * and stays unswapped; higher becomes B and sets TRPSWAP. It is FNV rather than
 * a truncation because two boards off one reel differ in only a couple of UID
 * bytes and a truncation collides on exactly that pair.
 */
#ifndef GIRIS_PEER_H
#define GIRIS_PEER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  PEER_DISABLED  = 0,  /* the host has taken the link for a diagnostic */
  PEER_DISCOVER  = 1,  /* hailing and listening on the 115200 open-drain bus */
  PEER_ALONE     = 2,  /* hail went unanswered long enough; standalone keyboard */
  PEER_PAIRED    = 3,  /* peer known, roles assigned, still on the slow bus */
  PEER_SWITCHING = 4,  /* committing to the run rate */
  PEER_RUNNING   = 5,  /* full duplex at the run rate, liveness holding */
  PEER_INCOMPAT  = 6,  /* peer found, speaks a different link protocol; parked on
                        * the discovery bus rather than disconnected — see below */
} peer_state_t;

typedef enum {
  PEER_ROLE_UNKNOWN = 0,
  PEER_ROLE_A       = 1,   /* lower uid_tag — unswapped, initiates the switch */
  PEER_ROLE_B       = 2,   /* higher uid_tag — sets TRPSWAP */
} peer_role_t;

typedef struct {
  uint8_t  state;
  uint8_t  role;
  uint16_t peer_tag;
  uint16_t peer_fw;
  uint32_t baud;           /* rate currently in force */
  uint32_t hails_sent;
  uint32_t frames_rx;
  uint32_t crc_errors;
  uint32_t switches;       /* successful transitions into RUNNING */
  uint32_t drops;          /* restarts of discovery after a failed handshake or a
                            * liveness timeout — anything that was not a clean run */
  uint32_t last_rx_ms;     /* ms since a valid frame from the peer */
} peer_status_t;

void peer_init(void);

/* Non-blocking. Call from the main loop as often as convenient. */
void peer_task(void);

void peer_status(peer_status_t *out);

/* Diagnostics take the link away from the state machine; nothing else may drive
 * USART6 while it is running. peer_enable(true) restarts discovery from scratch. */
void peer_enable(bool on);

/* The rate the run phase aims for. 12 Mbaud measured clean over 3 m with the
 * receiver's noise flag never firing; 13.5 raises NERR on most runs. */
#define PEER_RUN_BAUD  12000000u

/* ------------------------------------------------------- protocol version */
/*
 * The version of the LINK WIRE FORMAT — not the firmware build. Two images with
 * different features but the same wire format must still pair, or every routine
 * firmware bump would split the keyboard in half.
 *
 * **Bump this whenever anything on the wire changes**: the discovery frame, the
 * run frame, the meaning of a field, the negotiated rate. Forgetting to is what
 * this whole mechanism exists to catch, and it has already happened once —
 * framing changed the run phase from 16-byte PING/PONG at 20 Hz to 48-byte data
 * frames at 8 kHz while the version stayed at 1. The two builds each claimed to
 * be version 1 and were not interoperable, and the symptom was a bare liveness
 * drop that named nothing.
 *
 * What a mismatch does NOT do is stop the halves talking. Refusing to pair would
 * remove the only channel that can repair the mismatch: in topology (a) the far
 * half has no host of its own, so its next image has to arrive over this very
 * link. On a mismatch the halves stay paired on the 115200 discovery bus, which
 * is where a firmware push would happen anyway, and simply never step up to the
 * run phase.
 *
 * That places one permanent obligation on the discovery frame: **the first eight
 * bytes of peer_frame_t are frozen.** sync, type, src, dst and this version field
 * must keep their offsets in every future version, or two halves lose the ability
 * to work out that they disagree. peer.c static-asserts those offsets.
 */
#define PEER_PROTO  2u

#endif
