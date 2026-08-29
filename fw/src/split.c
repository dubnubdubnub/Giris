#include <string.h>

#include "board.h"
#include "adc.h"
#include "split.h"

typedef union {
  uint32_t w[SPLIT_FRAME_LEN / 4];
  uint8_t  b[SPLIT_FRAME_LEN];
} split_buf_t;

/* Byte offsets, spelled out rather than left to a packed struct: this layout is
 * a wire format two independently-built images have to agree on. */
enum {
  F_SYNC = 0, F_FLAGS = 1, F_SEQ = 2, F_CTRL = 4,
  F_CTRL_ARG = 5, F_CTRL_DATA = 6, F_KEYS = 8, F_CRC = 44,
};

static split_buf_t   txb;
static split_stats_t st;
static uint16_t      tx_seq;
static uint16_t      rx_seq;
static bool          rx_seq_valid;
static uint32_t      last_tx_frame;
static uint32_t      t_rx_cyc;
static bool          rx_any;
static bool          test_mode;
static bool          host_present;
static bool          host_awake = true;
static uint16_t      peer_keys[SPLIT_KEYS];

/* --------------------------------------------------------------------- crc */
/* Hardware unit, reset default polynomial (0x04C11DB7, init 0xFFFFFFFF). Eleven
 * word writes and a read; see the header for why this is CRC-32 and not the
 * CRC-16 the architecture doc costed. */
static uint32_t frame_crc(const uint32_t *w)
{
  crc_data_reset();
  return crc_block_calculate((uint32_t *)(uintptr_t)w, SPLIT_CRC_WORDS);
}

/* --------------------------------------------------------------- bit packing */
/* 9 bits never spans more than two bytes (max shift 7, 7 + 9 = 16), so a
 * two-byte window is sufficient and the last key, at bit 279, lands in bytes
 * 34..35 — exactly filling the 36. */
static void pack9(uint8_t *dst, const uint16_t *v)
{
  memset(dst, 0, SPLIT_KEY_BYTES);
  for (uint32_t i = 0; i < SPLIT_KEYS; i++) {
    const uint32_t bit = i * 9u;
    const uint32_t by  = bit >> 3;
    const uint32_t x   = (uint32_t)(v[i] & 0x1FFu) << (bit & 7u);
    dst[by]     |= (uint8_t)(x & 0xFFu);
    dst[by + 1] |= (uint8_t)((x >> 8) & 0xFFu);
  }
}

static void unpack9(uint16_t *v, const uint8_t *src)
{
  for (uint32_t i = 0; i < SPLIT_KEYS; i++) {
    const uint32_t bit = i * 9u;
    const uint32_t by  = bit >> 3;
    const uint32_t x   = (uint32_t)src[by] | ((uint32_t)src[by + 1] << 8);
    v[i] = (uint16_t)((x >> (bit & 7u)) & 0x1FFu);
  }
}

/* Deterministic from the sequence number alone, so the receiver regenerates it
 * without any shared state. The CRC already catches corruption; this catches
 * the subtler failure of accepting a frame that is not the frame that was sent
 * — a stale ring, a wrapped DMA, a coincidental CRC pass. */
static void pattern(uint16_t *v, uint16_t seq)
{
  for (uint32_t i = 0; i < SPLIT_KEYS; i++)
    v[i] = (uint16_t)((seq * 31u + i * 17u + 5u) & 0x1FFu);
}

/* ------------------------------------------------------------------- timing */
static uint32_t us_since(uint32_t cyc)
{
  const uint32_t per_us = system_core_clock / 1000000u;
  if (per_us == 0u) return 0u;
  const uint32_t d = (DWT->CYCCNT - cyc) / per_us;
  /* DWT wraps every 19.9 s. Past a second the exact figure is worthless and the
   * link is long gone — peer.c tears it down at 250 ms — so saturate rather
   * than let a wrap report a plausible-looking small number. */
  return (d > 1000000u) ? 1000000u : d;
}

/* ---------------------------------------------------------------------- api */

void split_init(void)
{
  crm_periph_clock_enable(CRM_CRC_PERIPH_CLOCK, TRUE);
  split_stop();
}

void split_start(void)
{
  memset(&st, 0, sizeof(st));
  memset(peer_keys, 0, sizeof(peer_keys));
  tx_seq = 0;
  rx_seq_valid = false;
  rx_any = false;
  st.stale = 1u;
  st.period_min_us = 0xFFFFFFFFu;
  last_tx_frame = adc_frame_index();
  t_rx_cyc = DWT->CYCCNT;
}

void split_stop(void)
{
  rx_seq_valid = false;
  rx_any = false;
  st.stale = 1u;
  memset(peer_keys, 0, sizeof(peer_keys));
}

bool split_tx_due(void)
{
  const uint32_t f = adc_frame_index();
  if (f == last_tx_frame) return false;
  last_tx_frame = f;
  return true;
}

void split_set_test(bool on)  { test_mode = on; }
bool split_test_enabled(void) { return test_mode; }
void split_set_host(bool p)   { host_present = p; }
void split_set_awake(bool a)  { host_awake = a; }

const uint16_t *split_peer_keys(void) { return peer_keys; }

const void *split_build_tx(void)
{
  uint16_t keys[SPLIT_KEYS];
  uint8_t  flags = 0u;
  if (host_present) flags |= SPLIT_F_HOST;
  if (host_present && host_awake) flags |= SPLIT_F_AWAKE;

  if (test_mode) {
    pattern(keys, tx_seq);
    flags |= SPLIT_F_TEST;
  } else {
    adc_frame_t f;
    memset(keys, 0, sizeof(keys));
    if (adc_read_frame(&f)) {
      /* Provisional unit. The real payload is centi-mm of travel, 0..400, which
       * is what makes 9 bits the right width — but there is no calibration or
       * travel curve yet, so what goes out is the raw 12-bit count shifted to
       * 9. Key INDEX is hardware scan order for the same reason: the keymap
       * layer that would give these positions names does not exist either. */
      for (uint32_t i = 0; i < PROTO_NUM_SLOTS && i < SPLIT_KEYS; i++)
        keys[i] = (uint16_t)(f.slot[i] >> 3);
      flags |= SPLIT_F_KEYS;
    }
  }

  memset(txb.b, 0, sizeof(txb.b));
  txb.b[F_SYNC]  = SPLIT_SYNC;
  txb.b[F_FLAGS] = flags;
  txb.b[F_SEQ]     = (uint8_t)(tx_seq & 0xFFu);
  txb.b[F_SEQ + 1] = (uint8_t)(tx_seq >> 8);
  pack9(&txb.b[F_KEYS], keys);

  const uint32_t c = frame_crc(txb.w);
  txb.b[F_CRC]     = (uint8_t)(c & 0xFFu);
  txb.b[F_CRC + 1] = (uint8_t)((c >> 8) & 0xFFu);
  txb.b[F_CRC + 2] = (uint8_t)((c >> 16) & 0xFFu);
  txb.b[F_CRC + 3] = (uint8_t)((c >> 24) & 0xFFu);

  tx_seq++;
  st.tx_frames++;
  return txb.b;
}

bool split_rx(const void *frame)
{
  const split_buf_t *f = (const split_buf_t *)frame;

  if (f->b[F_SYNC] != SPLIT_SYNC) return false;

  const uint32_t got = (uint32_t)f->b[F_CRC]
                     | ((uint32_t)f->b[F_CRC + 1] << 8)
                     | ((uint32_t)f->b[F_CRC + 2] << 16)
                     | ((uint32_t)f->b[F_CRC + 3] << 24);
  if (got != frame_crc(f->w)) { st.crc_errors++; return false; }

  const uint16_t seq = (uint16_t)(f->b[F_SEQ] | ((uint16_t)f->b[F_SEQ + 1] << 8));

  unpack9(peer_keys, &f->b[F_KEYS]);

  if (f->b[F_FLAGS] & SPLIT_F_TEST) {
    uint16_t want[SPLIT_KEYS];
    pattern(want, seq);
    if (memcmp(want, peer_keys, sizeof(want)) != 0) st.payload_errors++;
  }

  if (rx_seq_valid) {
    const uint16_t gap = (uint16_t)(seq - rx_seq - 1u);
    if (gap) st.seq_gaps += gap;
  }
  rx_seq = seq;
  rx_seq_valid = true;

  if (rx_any) {
    const uint32_t p = us_since(t_rx_cyc);
    st.period_us = p;
    if (p < st.period_min_us) st.period_min_us = p;
    if (p > st.period_max_us) st.period_max_us = p;
  }
  rx_any   = true;
  t_rx_cyc = DWT->CYCCNT;

  st.peer_seq   = seq;
  st.peer_flags = f->b[F_FLAGS];
  st.rx_frames++;
  return true;
}

void split_stats(split_stats_t *out)
{
  *out = st;
  out->age_us = rx_any ? us_since(t_rx_cyc) : 1000000u;
  out->stale  = (!rx_any || out->age_us > SPLIT_STALE_US) ? 1u : 0u;
  if (out->period_min_us == 0xFFFFFFFFu) out->period_min_us = 0u;
}

void split_note_resync(void) { st.resyncs++; }
