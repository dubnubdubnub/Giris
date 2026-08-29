#include <string.h>

#include "board.h"
#include "adc.h"
#include "split.h"
#include "keys.h"
#include "usb_descriptors.h"
#include "tusb.h"

/* Default map. Z and X are the two an osu! pad actually uses; the rest follow
 * so every fitted key does something visible while testing. Replaced wholesale
 * by the keymap layer later. */
static const uint8_t keycode[KEYS_PER_HALF] = {
  HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V, HID_KEY_B, HID_KEY_N
};

static bool     enabled;
static uint16_t baseline[KEYS_PER_HALF];
static bool     baseline_valid;
static uint8_t  settle;
static uint8_t  own_bits, peer_bits, last_sent_bits;
static bool     sending_peer;
static uint16_t reports;

/* slot_map[] gives the key number each hardware scan slot carries, so the
 * inverse is what turns a frame into keys. Built once rather than searched per
 * frame — this runs at 8 kHz. */
static uint8_t slot_of_key[KEYS_PER_HALF];

static void build_inverse(void)
{
  const uint8_t *map = adc_slot_map();
  memset(slot_of_key, 0xFF, sizeof(slot_of_key));
  for (uint32_t s = 0; s < PROTO_NUM_SLOTS; s++)
    if (map[s] < KEYS_PER_HALF) slot_of_key[map[s]] = (uint8_t)s;
}

void keys_init(void)
{
  enabled = false;
  baseline_valid = false;
  settle = 8u;
  own_bits = peer_bits = last_sent_bits = 0;
  sending_peer = false;
  reports = 0;
  build_inverse();
}

void keys_set_enabled(bool on)
{
  enabled = on;
  if (!on && last_sent_bits) {
    /* Never leave a key stuck down on the host. */
    hid_keyboard_report_t r;
    memset(&r, 0, sizeof(r));
    if (tud_mounted()) tud_hid_n_report(ITF_NUM_KBD, 0, &r, sizeof(r));
    last_sent_bits = 0;
  }
}

void keys_calibrate(void) { baseline_valid = false; settle = 8u; }

static uint8_t scan_own(const adc_frame_t *f)
{
  uint8_t bits = 0;
  for (uint32_t k = 0; k < KEYS_PER_HALF; k++) {
    const uint8_t s = slot_of_key[k];
    if (s >= PROTO_NUM_SLOTS) continue;

    int32_t d = (int32_t)f->slot[s] - (int32_t)baseline[k];
    if (d < 0) d = -d;                       /* direction-agnostic: see keys.h */

    const bool was = (own_bits >> k) & 1u;
    const bool now = was ? (d > (int32_t)KEYS_RELEASE_DELTA)   /* hysteresis */
                         : (d > (int32_t)KEYS_PRESS_DELTA);
    if (now) bits |= (uint8_t)(1u << k);
  }
  return bits;
}

void keys_task(void)
{
  adc_frame_t f;
  if (!adc_read_frame(&f)) return;

  if (settle) { settle--; return; }

  if (!baseline_valid) {
    for (uint32_t k = 0; k < KEYS_PER_HALF; k++) {
      const uint8_t s = slot_of_key[k];
      baseline[k] = (s < PROTO_NUM_SLOTS) ? f.slot[s] : 0u;
    }
    baseline_valid = true;
    return;
  }

  own_bits = scan_own(&f);

  /* Merge the peer's keys only when the peer has NO host of its own — that is
   * topology (a), where one half tunnels through the other and this half is the
   * only route to a machine. In topology (b) both halves have hosts and merging
   * would type on both at once; deciding which host receives what is the
   * input-owner token's job, and the token does not exist yet. So (b) falls
   * back to each half reporting only its own keys, which is wrong in the long
   * run and harmless in the short one. */
  split_stats_t ss;
  split_stats(&ss);
  sending_peer = !ss.stale && (ss.peer_flags & SPLIT_F_HOST) == 0u;

  peer_bits = 0;
  if (sending_peer) {
    const uint16_t *pk = split_peer_keys();
    for (uint32_t k = 0; k < KEYS_PER_HALF; k++) {
      /* The peer sends 9-bit values, so its baseline is not ours. Until the
       * travel pipeline gives both halves a common unit, compare against the
       * peer's own resting value the only way available: its first reading. */
      if (pk[k] > (KEYS_PRESS_DELTA >> 3)) peer_bits |= (uint8_t)(1u << k);
    }
  }

  const uint8_t bits = (uint8_t)(own_bits | peer_bits);
  if (!enabled || bits == last_sent_bits) return;
  if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_KBD)) return;

  hid_keyboard_report_t r;
  memset(&r, 0, sizeof(r));
  uint32_t n = 0;
  for (uint32_t k = 0; k < KEYS_PER_HALF && n < 6u; k++)
    if ((bits >> k) & 1u) r.keycode[n++] = keycode[k];

    /* tud_hid_n_*, not tud_hid_*. The short forms take a REPORT ID and are
   * hardwired to instance 0, so tud_hid_report(ITF_NUM_KBD, ...) would have
   * quietly sent a report_id of 1 down the telemetry interface. */
  if (tud_hid_n_report(ITF_NUM_KBD, 0, &r, sizeof(r))) {
    last_sent_bits = bits;
    reports++;
  }
}

void keys_status(keys_status_t *out)
{
  out->enabled      = enabled ? 1u : 0u;
  out->own_pressed  = own_bits;
  out->peer_pressed = peer_bits;
  out->sending_peer = sending_peer ? 1u : 0u;
  out->reports      = reports;
  memcpy(out->baseline, baseline, sizeof(out->baseline));
}
