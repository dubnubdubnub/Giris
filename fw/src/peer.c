#include <stddef.h>
#include <string.h>

#include "board.h"
#include "clock.h"
#include "link.h"
#include "uid.h"
#include "peer.h"
#include "split.h"

/* ------------------------------------------------------------ wire format */
/* Fixed 16 bytes so the receiver never has to parse a length it has not
 * validated. CRC-16/CCITT-FALSE over the first 14. */
#define PEER_SYNC      0xA5u
#define PEER_FRAME_LEN 16

enum {
  PF_HAIL       = 1,   /* broadcast: here I am, this is my tag and firmware */
  PF_HAIL_ACK   = 2,   /* directed: I heard you, here is mine */
  PF_SWITCH     = 3,   /* A -> B: let us move to arg baud */
  PF_SWITCH_ACK = 4,   /* B -> A: agreed */
  PF_COMMIT     = 5,   /* A -> B: switching now, on my TDC */
  PF_PING       = 6,   /* run phase liveness */
  PF_PONG       = 7,
};

typedef struct {
  uint8_t  sync;
  uint8_t  type;
  uint16_t src;
  uint16_t dst;        /* 0 = broadcast */
  uint16_t fw;
  uint32_t arg;
  uint8_t  flags;
  uint8_t  rsv;
  uint16_t crc;
} peer_frame_t;

_Static_assert(sizeof(peer_frame_t) == PEER_FRAME_LEN, "peer frame must be 16 bytes");

/* Frozen, forever, across every protocol version. If a future version moves the
 * field carrying the version number, two halves lose the ability to discover
 * that they disagree — and the failure reappears as the unexplained liveness
 * drop this whole mechanism exists to replace. See peer.h. */
_Static_assert(offsetof(peer_frame_t, sync) == 0, "discovery frame header is frozen");
_Static_assert(offsetof(peer_frame_t, type) == 1, "discovery frame header is frozen");
_Static_assert(offsetof(peer_frame_t, src)  == 2, "discovery frame header is frozen");
_Static_assert(offsetof(peer_frame_t, dst)  == 4, "discovery frame header is frozen");
_Static_assert(offsetof(peer_frame_t, fw)   == 6, "the version field may never move");

static uint16_t crc16(const uint8_t *p, uint32_t n)
{
  uint16_t c = 0xFFFFu;                       /* CCITT-FALSE */
  while (n--) {
    c ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
  }
  return c;
}

/* ------------------------------------------------------------------ time */
/* DWT wraps every 2^32 / 216e6 = 19.9 s, so keep a wrap-safe millisecond count
 * rather than comparing raw cycle stamps across long timeouts. */
static uint32_t ms_now, ms_acc, ms_last_cyc;

static void ms_tick(void)
{
  const uint32_t per_ms = system_core_clock / 1000u;
  if (per_ms == 0u) return;              /* never divide the main loop by zero */

  const uint32_t now = DWT->CYCCNT;
  ms_acc += now - ms_last_cyc;
  ms_last_cyc = now;
  /* Bounded: a stale ms_last_cyc can make the first delta a full DWT wrap, and
   * an unbounded catch-up loop there is 20 s of frozen main loop. */
  uint32_t guard = 4000u;
  while (ms_acc >= per_ms && guard--) { ms_acc -= per_ms; ms_now++; }
}

static uint32_t since(uint32_t stamp) { return ms_now - stamp; }

/* ------------------------------------------------------------------ state */
/* Power of two: the DMA counter maps directly onto the index. 512 bytes is
 * 10.6 run-phase frames, i.e. 1.3 ms of slack before the DMA laps the reader —
 * the main loop would have to stall for longer than USB's own bus-turnaround
 * budget before anything is lost. */
#define RX_RING_LEN   512
static uint8_t  rx_ring[RX_RING_LEN];
static uint16_t rx_read;
static uint8_t  asm_buf[PEER_FRAME_LEN];
static uint8_t  asm_len;

static uint8_t  tx_buf[PEER_FRAME_LEN];
static bool     tx_busy;

/* The CRC unit consumes words, so the frame the scanner hands to split_rx()
 * has to be word-aligned. It is copied out of the ring rather than parsed in
 * place because a frame can straddle the wrap. */
static union {
  uint32_t w[SPLIT_FRAME_LEN / 4];
  uint8_t  b[SPLIT_FRAME_LEN];
} rx_frame;

static peer_state_t state = PEER_DISCOVER;
static peer_role_t  role  = PEER_ROLE_UNKNOWN;
static uint16_t     my_tag, peer_tag, peer_fw;
/* Two different numbers, and conflating them cost a link that paired perfectly
 * and then ran at the discovery rate. run_baud is the NEGOTIATED target; cur_baud
 * is whatever the USART is actually configured for at this instant, including
 * 115200 while discovery is in progress. */
static uint32_t     run_baud = PEER_RUN_BAUD;
static uint32_t     cur_baud;
static uint32_t     t_hail, t_rx, t_state;
static uint32_t     hail_period;
static peer_status_t st;

#define ALONE_AFTER_MS  1500u      /* hail unanswered this long -> standalone */
#define ALONE_HAIL_MS   500u       /* keep hailing, so a late peer can still join */
#define PING_MS         50u
#define LIVENESS_MS     250u
#define SWITCH_TMO_MS   200u
#define PAIRED_TMO_MS   300u       /* generous: A acts within 5 ms of pairing */

/* ------------------------------------------------------------- transport */

static void rx_dma_start(void)
{
  dma_reset(DMA1_CHANNEL2);
  dma_init_type d;
  dma_default_para_init(&d);
  d.buffer_size           = RX_RING_LEN;
  d.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
  d.memory_base_addr      = (uint32_t)rx_ring;
  d.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  d.memory_inc_enable     = TRUE;
  d.peripheral_base_addr  = (uint32_t)&USART6->dt;
  d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  d.peripheral_inc_enable = FALSE;
  d.priority              = DMA_PRIORITY_HIGH;
  d.loop_mode_enable      = TRUE;            /* circular: never stops, never overruns */
  dma_init(DMA1_CHANNEL2, &d);
  dmamux_enable(DMA1, TRUE);
  dmamux_init(DMA1MUX_CHANNEL2, DMAMUX_DMAREQ_ID_USART6_RX);
  dma_channel_enable(DMA1_CHANNEL2, TRUE);
  usart_dma_receiver_enable(USART6, TRUE);
  rx_read = 0;
  asm_len = 0;
}

/* Circular DMA: the remaining-count register IS the write pointer. Polling it
 * costs one read and cannot overrun, which a byte-at-a-time RDBF loop would do
 * the moment the main loop stalls — and at 12 Mbaud a byte lands every 160
 * core cycles. */
static uint16_t rx_write_index(void)
{
  /* Mask. In circular mode the counter reloads to RX_RING_LEN rather than
   * resting at 0, so the raw subtraction yields 0 for "empty" but ALSO yields
   * RX_RING_LEN itself at the reload instant — a value rx_read, which is masked
   * to 0..RX_RING_LEN-1, can never equal. The scan loop then never terminates
   * and the whole main loop stops, taking tud_task() with it: the board simply
   * never enumerates. Cost one hung board to find. */
  return (uint16_t)((RX_RING_LEN - dma_data_number_get(DMA1_CHANNEL2)) & (RX_RING_LEN - 1u));
}

static void mode_for(uint32_t baud, bool run)
{
  usart_dma_receiver_enable(USART6, FALSE);
  dma_channel_enable(DMA1_CHANNEL2, FALSE);

  if (!run) {
    link_configure(LINK_MODE_HD_PC6, baud);          /* open drain, wired-AND */
  } else {
    link_configure(role == PEER_ROLE_B ? LINK_MODE_FD_SWAP : LINK_MODE_FD, baud);
  }
  rx_dma_start();
  cur_baud = baud;
}

static void tx_raw(const void *buf, uint32_t len)
{
  usart_flag_clear(USART6, USART_TDC_FLAG);
  dma_reset(DMA1_CHANNEL3);
  dma_init_type d;
  dma_default_para_init(&d);
  d.buffer_size           = (uint16_t)len;
  d.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
  d.memory_base_addr      = (uint32_t)buf;
  d.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  d.memory_inc_enable     = TRUE;
  d.peripheral_base_addr  = (uint32_t)&USART6->dt;
  d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  d.peripheral_inc_enable = FALSE;
  d.priority              = DMA_PRIORITY_MEDIUM;
  d.loop_mode_enable      = FALSE;
  dma_init(DMA1_CHANNEL3, &d);
  dmamux_init(DMA1MUX_CHANNEL3, DMAMUX_DMAREQ_ID_USART6_TX);
  dma_channel_enable(DMA1_CHANNEL3, TRUE);
  usart_dma_transmitter_enable(USART6, TRUE);
  tx_busy = true;
}

static void tx_frame(uint8_t type, uint16_t dst, uint32_t arg)
{
  peer_frame_t f;
  memset(&f, 0, sizeof(f));
  f.sync = PEER_SYNC;
  f.type = type;
  f.src  = my_tag;
  f.dst  = dst;
  f.fw   = PEER_PROTO;
  f.arg  = arg;
  memcpy(tx_buf, &f, PEER_FRAME_LEN);
  const uint16_t c = crc16(tx_buf, PEER_FRAME_LEN - 2);
  tx_buf[PEER_FRAME_LEN - 2] = (uint8_t)(c & 0xFFu);
  tx_buf[PEER_FRAME_LEN - 1] = (uint8_t)(c >> 8);
  tx_raw(tx_buf, PEER_FRAME_LEN);
}

static bool tx_done(void)
{
  if (!tx_busy) return true;

  /* Both conditions, and the DMA one is NOT redundant with TDC.
   *
   * TDC says only that the shift register drained. It drains just the same in
   * the gap left when a starved DMA fails to deliver the next byte in time —
   * so on TDC alone this returns "sent" while the DMA still has bytes to fetch,
   * the main loop rebuilds the buffer underneath it, and the tail of the frame
   * goes out as the head of the next one. The receiver sees a bad CRC and then
   * slides a byte at a time hunting for the sync it just lost.
   *
   * Measured, 300 s each way: an idle half corrupts nothing, while a half also
   * servicing 8 kHz USB — where the OTG core's DMA contends for the bus —
   * produced 1,661 corrupt frames and 52,594 resync slides at the far end while
   * its OWN reception stayed spotless. That asymmetry is what named the bug:
   * the fault was always in what the busy half transmitted. */
  if (dma_data_number_get(DMA1_CHANNEL3) != 0u) return false;
  if (usart_flag_get(USART6, USART_TDC_FLAG) != SET) return false;
  usart_dma_transmitter_enable(USART6, FALSE);
  dma_channel_enable(DMA1_CHANNEL3, FALSE);
  tx_busy = false;
  return true;
}

/* --------------------------------------------------------------- receive */

/* Returns true if it reconfigured the USART, which invalidates the caller's
 * view of the ring. */
static bool on_frame(const peer_frame_t *f);

static void rx_scan(void)
{
  const uint16_t w = rx_write_index();
  /* Hard bound. Nothing inside this loop should be able to stop it terminating,
   * but "should" is how the board that never enumerated got built: peer_task()
   * runs in the same loop as tud_task(), so any spin here takes USB with it and
   * the only way back in is BOOT0 + NRST. One bounded counter buys the ability
   * to always reflash over HID instead. */
  uint32_t guard = RX_RING_LEN;
  while (rx_read != w && guard--) {
    const uint8_t b = rx_ring[rx_read];
    rx_read = (uint16_t)((rx_read + 1u) & (RX_RING_LEN - 1u));

    /* Resynchronise on the sync byte rather than trusting alignment: on the
     * open-drain bus we may well start listening mid-frame, and a half-duplex
     * transmitter's own frame is not echoed back to filter out. */
    if (asm_len == 0 && b != PEER_SYNC) continue;
    asm_buf[asm_len++] = b;
    if (asm_len < PEER_FRAME_LEN) continue;
    asm_len = 0;

    const uint16_t got = (uint16_t)(asm_buf[PEER_FRAME_LEN - 2] |
                                   ((uint16_t)asm_buf[PEER_FRAME_LEN - 1] << 8));
    if (got != crc16(asm_buf, PEER_FRAME_LEN - 2)) { st.crc_errors++; continue; }

    peer_frame_t f;
    memcpy(&f, asm_buf, PEER_FRAME_LEN);
    if (f.src == my_tag) continue;             /* never pair with ourselves */
    st.frames_rx++;
    t_rx = ms_now;
    /* If that frame reconfigured the link, rx_read has been reset to 0 and the
     * ring now holds bytes from the old baud rate. Carrying on would re-parse
     * frames already acted on — which is exactly why B counted five switches
     * for one COMMIT. */
    if (on_frame(&f)) return;
  }
}

/* The run phase has one frame shape and no control traffic, so this is a
 * different scanner from the discovery one above, in one way that matters:
 * **on a rejected frame it slides forward by a single byte, not by 48.**
 *
 * Discovery frames are rare and separated by milliseconds of silence, so the
 * ring always starts at a real sync byte and discarding a whole failed frame
 * costs nothing. Data frames are back to back at 8 kHz with 9-bit-packed
 * payloads, and 0x5A turns up inside a payload roughly one frame in five.
 * Consuming 48 bytes from a false sync would swallow the genuine sync behind
 * it — and then the next one, and the next: a link that mis-frames once stays
 * mis-framed. Sliding by one costs a CRC per false sync (eleven register
 * writes, tens of nanoseconds) and re-locks inside a single frame period. */
static void rx_scan_run(void)
{
  const uint16_t w = rx_write_index();
  /* Normal service accepts one frame per pass; 64 bounds the pathological case
   * to roughly 60 us of main loop rather than the 475 us a full-ring guard
   * would allow. Anything left over is picked up next time round. */
  uint32_t guard = 64u;
  while (guard--) {
    if ((uint16_t)((w - rx_read) & (RX_RING_LEN - 1u)) < SPLIT_FRAME_LEN) break;

    if (rx_ring[rx_read] != SPLIT_SYNC) {
      rx_read = (uint16_t)((rx_read + 1u) & (RX_RING_LEN - 1u));
      split_note_resync();
      continue;
    }

    for (uint32_t i = 0; i < SPLIT_FRAME_LEN; i++)
      rx_frame.b[i] = rx_ring[(rx_read + i) & (RX_RING_LEN - 1u)];

    if (!split_rx(rx_frame.b)) {
      rx_read = (uint16_t)((rx_read + 1u) & (RX_RING_LEN - 1u));
      split_note_resync();
      continue;
    }

    rx_read = (uint16_t)((rx_read + SPLIT_FRAME_LEN) & (RX_RING_LEN - 1u));
    st.frames_rx++;
    t_rx = ms_now;
  }
}

static void assign_roles(uint16_t other)
{
  peer_tag = other;
  /* Deterministic and symmetric: both halves compute the same answer from the
   * same two numbers, with no negotiation and no round trip to get wrong. */
  role = (my_tag < other) ? PEER_ROLE_A : PEER_ROLE_B;
}

static void go(peer_state_t s) { state = s; t_state = ms_now; }

static bool on_frame(const peer_frame_t *f)
{
  peer_fw = f->fw;

  /* Version gate, applied to EVERY frame rather than only to the hail, so no
   * path into the run phase can skip it.
   *
   * A mismatch parks us on the discovery bus instead of disconnecting. That is
   * deliberate: in topology (a) the far half has no host, so the image that
   * would fix the mismatch has to arrive over this same link — refusing to talk
   * would remove the only channel that can repair it. We keep answering hails,
   * keep reporting what we heard, and simply never step up to the run phase. */
  if (f->fw != PEER_PROTO) {
    if (state != PEER_INCOMPAT) {
      assign_roles(f->src);              /* deterministic; useful to report */
      go(PEER_INCOMPAT);
    }
    if (f->type == PF_HAIL) tx_frame(PF_HAIL_ACK, f->src, 0);
    return false;
  }

  /* A matching frame while parked means the peer has been updated under us.
   * Restart discovery cleanly rather than trying to resume mid-handshake. */
  if (state == PEER_INCOMPAT) { peer_enable(true); return true; }

  switch (f->type) {
    case PF_HAIL:
      /* Answered from PAIRED and SWITCHING too, not only from DISCOVER/ALONE.
       * A hail means the peer has forgotten the pairing — it rebooted, or it
       * missed our HAIL_ACK — so holding the old pairing is waiting for a
       * handshake that is never coming. assign_roles() is deterministic, so
       * re-pairing is idempotent and costs one frame.
       *
       * Ignoring hails here was a deadlock, seen on the bench: A missed a
       * HAIL_ACK and fell back to ALONE, hailing twice a second; B stayed in
       * PAIRED as role B waiting for a switch A no longer knew to send; and
       * every one of those hails refreshed B's t_rx, so the liveness escape
       * that should have freed it never fired. B received 24 hails in 12 s and
       * answered none of them. */
      if (state != PEER_DISABLED) {
        assign_roles(f->src);
        tx_frame(PF_HAIL_ACK, f->src, 0);
        go(PEER_PAIRED);
      }
      break;

    case PF_HAIL_ACK:
      if (f->dst == my_tag && (state == PEER_DISCOVER || state == PEER_ALONE)) {
        assign_roles(f->src);
        go(PEER_PAIRED);
      }
      break;

    case PF_SWITCH:
      if (role == PEER_ROLE_B && f->dst == my_tag) {
        run_baud = f->arg;
        tx_frame(PF_SWITCH_ACK, f->src, f->arg);
        go(PEER_SWITCHING);
      }
      break;

    case PF_SWITCH_ACK:
      if (role == PEER_ROLE_A && f->dst == my_tag && state == PEER_SWITCHING) {
        tx_frame(PF_COMMIT, f->src, run_baud);
      }
      break;

    case PF_COMMIT:
      if (role == PEER_ROLE_B && f->dst == my_tag) {
        run_baud = f->arg;
        mode_for(run_baud, true);
        split_start();
        st.switches++;
        go(PEER_RUNNING);
        return true;                            /* ring is stale from here on */
      }
      break;

    case PF_PING:
      if (f->dst == my_tag) tx_frame(PF_PONG, f->src, f->arg);
      break;

    case PF_PONG:
      break;                                    /* t_rx already refreshed */

    default:
      break;
  }
  return false;
}

/* ------------------------------------------------------------------- api */

void peer_init(void)
{
  link_init();
  split_init();
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
  ms_last_cyc = DWT->CYCCNT;
  my_tag = uid_tag();
  /* Period differs per board, so two halves cannot stay in lockstep — which
   * matters because a collision on this bus is undetectable (no self-echo). */
  hail_period = 20u + (uint32_t)(my_tag & 0x0Fu);
  memset(&st, 0, sizeof(st));
  peer_enable(true);
}

void peer_enable(bool on)
{
  if (!on) {
    usart_dma_receiver_enable(USART6, FALSE);
    dma_channel_enable(DMA1_CHANNEL2, FALSE);
    dma_channel_enable(DMA1_CHANNEL3, FALSE);
    tx_busy = false;
    split_stop();
    state = PEER_DISABLED;
    role  = PEER_ROLE_UNKNOWN;
    peer_tag = peer_fw = 0;
    return;
  }
  role = PEER_ROLE_UNKNOWN;
  peer_tag = peer_fw = 0;
  split_stop();
  mode_for(LINK_DISCOVERY_BAUD, false);
  t_hail = t_rx = ms_now;
  go(PEER_DISCOVER);
}

void peer_task(void)
{
  ms_tick();
  if (state == PEER_DISABLED) return;

  /* Let USB come up first. If anything below ever wedges, enumeration has
   * already happened and CMD_BOOTLOADER is still reachable — the difference
   * between a two-minute reflash and asking someone to hold two buttons. */
  if (ms_now < 1500u) return;

  tx_done();
  if (state == PEER_RUNNING) rx_scan_run(); else rx_scan();

  switch (state) {
    case PEER_DISCOVER:
    case PEER_ALONE: {
      const uint32_t period = (state == PEER_ALONE) ? ALONE_HAIL_MS : hail_period;
      if (!tx_busy && since(t_hail) >= period) {
        t_hail = ms_now;
        st.hails_sent++;
        tx_frame(PF_HAIL, 0, 0);
      }
      if (state == PEER_DISCOVER && since(t_state) > ALONE_AFTER_MS) go(PEER_ALONE);
      break;
    }

    case PEER_PAIRED:
      /* Only A drives the switch, so there is no race over who speaks first.
       * Wait for our own HAIL_ACK to finish clearing the wire before pushing
       * the next frame onto a bus that cannot detect a collision. */
      if (role == PEER_ROLE_A && tx_done() && since(t_state) >= 5u) {
        run_baud = PEER_RUN_BAUD;               /* the target, not the current rate */
        tx_frame(PF_SWITCH, peer_tag, run_baud);
        go(PEER_SWITCHING);
      }
      /* Bounded by time IN THIS STATE, not by liveness. Role B has no action
       * of its own here — it waits for A's SWITCH — and t_rx is refreshed by
       * every valid frame including ones we do not act on, so a liveness timer
       * cannot bound a wait for a specific frame. This is the backstop for the
       * deadlock described above; the hail handler is the fast path out. */
      if (since(t_state) > PAIRED_TMO_MS) { st.drops++; peer_enable(true); }
      break;

    case PEER_SWITCHING:
      if (role == PEER_ROLE_A) {
        /* A reconfigures only once COMMIT has fully left the shift register —
         * changing the pin mode mid-byte would truncate the very frame that
         * tells the peer to follow. */
        if (tx_buf[1] == PF_COMMIT && tx_done()) {
          mode_for(run_baud, true);
          split_start();
          st.switches++;
          go(PEER_RUNNING);
        }
      }
      if (since(t_state) > SWITCH_TMO_MS) { st.drops++; peer_enable(true); }
      break;

    case PEER_INCOMPAT:
      /* Keep hailing at the standalone rate. This is both how the host sees the
       * mismatch persist and how we notice the moment the peer is updated. */
      if (!tx_busy && since(t_hail) >= ALONE_HAIL_MS) {
        t_hail = ms_now;
        st.hails_sent++;
        tx_frame(PF_HAIL, 0, 0);
      }
      break;

    case PEER_RUNNING:
      /* Both halves transmit unprompted on their own scan tick, so there is no
       * initiator here and no request/response round trip — which is also what
       * makes the run phase completely symmetric between A and B. The data
       * frame IS the liveness beacon; PF_PING exists only for the discovery
       * bus and is unused from here on. */
      if (!tx_busy && split_tx_due()) tx_raw(split_build_tx(), SPLIT_FRAME_LEN);
      if (since(t_rx) > LIVENESS_MS) { st.drops++; peer_enable(true); }
      break;

    default:
      break;
  }
}

void peer_status(peer_status_t *out)
{
  ms_tick();
  *out = st;
  out->state      = (uint8_t)state;
  out->role       = (uint8_t)role;
  out->peer_tag   = peer_tag;
  out->peer_fw    = peer_fw;
  out->baud       = cur_baud;
  out->last_rx_ms = since(t_rx);
}
