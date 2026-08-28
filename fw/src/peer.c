#include <string.h>

#include "board.h"
#include "clock.h"
#include "link.h"
#include "uid.h"
#include "peer.h"

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
#define RX_RING_LEN   256          /* power of two: the DMA counter maps directly */
static uint8_t  rx_ring[RX_RING_LEN];
static uint16_t rx_read;
static uint8_t  asm_buf[PEER_FRAME_LEN];
static uint8_t  asm_len;

static uint8_t  tx_buf[PEER_FRAME_LEN];
static bool     tx_busy;

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

#define FW_VERSION      1u
#define ALONE_AFTER_MS  1500u      /* hail unanswered this long -> standalone */
#define ALONE_HAIL_MS   500u       /* keep hailing, so a late peer can still join */
#define PING_MS         50u
#define LIVENESS_MS     250u
#define SWITCH_TMO_MS   200u

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
   * to 0..255, can never equal. The scan loop then never terminates and the
   * whole main loop stops, taking tud_task() with it: the board simply never
   * enumerates. Cost one hung board to find. */
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

static void tx_frame(uint8_t type, uint16_t dst, uint32_t arg)
{
  peer_frame_t f;
  memset(&f, 0, sizeof(f));
  f.sync = PEER_SYNC;
  f.type = type;
  f.src  = my_tag;
  f.dst  = dst;
  f.fw   = FW_VERSION;
  f.arg  = arg;
  memcpy(tx_buf, &f, PEER_FRAME_LEN);
  const uint16_t c = crc16(tx_buf, PEER_FRAME_LEN - 2);
  tx_buf[PEER_FRAME_LEN - 2] = (uint8_t)(c & 0xFFu);
  tx_buf[PEER_FRAME_LEN - 1] = (uint8_t)(c >> 8);

  usart_flag_clear(USART6, USART_TDC_FLAG);
  dma_reset(DMA1_CHANNEL3);
  dma_init_type d;
  dma_default_para_init(&d);
  d.buffer_size           = PEER_FRAME_LEN;
  d.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
  d.memory_base_addr      = (uint32_t)tx_buf;
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

static bool tx_done(void)
{
  if (!tx_busy) return true;
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
  switch (f->type) {
    case PF_HAIL:
      if (state == PEER_DISCOVER || state == PEER_ALONE) {
        peer_fw = f->fw;
        assign_roles(f->src);
        tx_frame(PF_HAIL_ACK, f->src, 0);
        go(PEER_PAIRED);
      }
      break;

    case PF_HAIL_ACK:
      if (f->dst == my_tag && (state == PEER_DISCOVER || state == PEER_ALONE)) {
        peer_fw = f->fw;
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
    state = PEER_DISABLED;
    role  = PEER_ROLE_UNKNOWN;
    peer_tag = peer_fw = 0;
    return;
  }
  role = PEER_ROLE_UNKNOWN;
  peer_tag = peer_fw = 0;
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
  rx_scan();

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
      if (since(t_rx) > ALONE_AFTER_MS) peer_enable(true);
      break;

    case PEER_SWITCHING:
      if (role == PEER_ROLE_A) {
        /* A reconfigures only once COMMIT has fully left the shift register —
         * changing the pin mode mid-byte would truncate the very frame that
         * tells the peer to follow. */
        if (tx_buf[1] == PF_COMMIT && tx_done()) {
          mode_for(run_baud, true);
          st.switches++;
          go(PEER_RUNNING);
        }
      }
      if (since(t_state) > SWITCH_TMO_MS) { st.drops++; peer_enable(true); }
      break;

    case PEER_RUNNING:
      if (role == PEER_ROLE_A && !tx_busy && since(t_hail) >= PING_MS) {
        t_hail = ms_now;
        tx_frame(PF_PING, peer_tag, ms_now);
      }
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
