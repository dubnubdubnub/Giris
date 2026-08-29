/*
 * usb.c — OTG_HS device bring-up and the raw-HID telemetry protocol.
 *
 * The two things TinyUSB's AT32 port does NOT do for you, and which look like
 * dead silicon if you miss them:
 *
 *  1. CRM gating. dwc2_clock_init() is an empty stub, so board code must enable
 *     CRM_OTGHS_PERIPH_CLOCK and CRM_OTGHSPHY_PERIPH_CLOCK and select the PHY's
 *     12 MHz reference.
 *  2. GCCFG.VBUSIG. This board does not wire PB13 as a VBUS sense, so with VBUS
 *     sensing left enabled the core decides VBUS is absent and never pulls up
 *     D+. The device simply never appears, with no error anywhere.
 */
#include <string.h>

#include "tusb.h"
#include "adc.h"
#include "uid.h"
#include "link.h"
#include "peer.h"
#include "split.h"
#include "reset.h"
#include "power.h"
#include "keys.h"
#include "usb_descriptors.h"
#include "board.h"
#include "clock.h"
#include "protocol.h"
#include "usb.h"

/* OTG_HS global registers. GCCFG lives at OTGHS_BASE + 0x38. */
#define OTGHS_BASE_ADDR    0x40040000UL
#define OTGHS_GCCFG        (*(volatile uint32_t *)(OTGHS_BASE_ADDR + 0x38UL))
#define GCCFG_PWRDOWN      (1u << 16)
#define GCCFG_VBUSIG       (1u << 21)

#define BUILD_ID           0x00000101u

/* Artery's ROM bootloader. Jumping here from the application is what makes this
 * the LAST time anyone has to hold BOOT0 and tap NRST to reflash. */
#define ROM_BOOTLOADER_ADDR  0x1FFFA400UL

/* ------------------------------------------------------------- tx ring */
#define TX_RING_LEN  8

static uint8_t  tx_ring[TX_RING_LEN][PROTO_REPORT_SIZE];
static volatile uint8_t tx_head, tx_tail;
static uint16_t in_seq;
static uint32_t tx_dropped;

static uint8_t tx_ring_used(void) { return (uint8_t)(tx_head - tx_tail); }
static bool tx_ring_full(void) { return tx_ring_used() >= TX_RING_LEN; }

/* Unsolicited telemetry stops well short of full so a command response always
 * has somewhere to go.
 *
 * tx_begin() evicts the OLDEST entry when the ring is full, and at 8 kHz the
 * stream can refill all eight slots between two host polls. So a reply to an
 * explicit command could be queued and then silently thrown away before it was
 * ever sent — the host sees a command that produced no answer, which is
 * indistinguishable from a dead peripheral and is exactly how the board on the
 * Windows host looked. Stale telemetry is worthless; a dropped answer is a lie. */
#define TX_RING_STREAM_LIMIT  (TX_RING_LEN / 2)

/* Claim the next slot, pre-filled with the common header. */
static uint8_t *tx_begin(uint8_t msg, uint8_t tag)
{
  if (tx_ring_full()) {
    tx_tail++;            /* drop the OLDEST — stale telemetry is worthless */
    tx_dropped++;
  }
  uint8_t *p = tx_ring[tx_head % TX_RING_LEN];
  memset(p, 0, PROTO_REPORT_SIZE);
  p[0] = msg;
  p[1] = tag;
  p[2] = (uint8_t)(in_seq & 0xFF);
  p[3] = (uint8_t)(in_seq >> 8);
  in_seq++;
  return p;
}

static void tx_commit(void) { tx_head++; }

/* Hand the endpoint the next queued report if it is free. tud_hid_n_report()
 * does not queue — it returns false when the endpoint is still busy — so the
 * ring plus this pump is what keeps a skipped microframe from losing data. */
static void tx_pump(void)
{
  if (tx_head == tx_tail) return;
  if (!tud_hid_ready()) return;

  const uint8_t *p = tx_ring[tx_tail % TX_RING_LEN];
  if (tud_hid_report(0, p, PROTO_REPORT_SIZE)) tx_tail++;
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
  (void)instance; (void)report; (void)len;
  tx_pump();
}

/* ------------------------------------------------------------ streaming */
static bool     stream_on;
static uint8_t  stream_decim = 8;      /* 8 kHz / 8 = 1 kHz frames by default */
static uint32_t last_streamed;
static bool     stream_gap;

static void stream_service(void)
{
  if (!stream_on) return;
  if (tx_ring_used() >= TX_RING_STREAM_LIMIT) return;

  uint8_t *p = NULL;
  uint8_t  n = 0;

  for (n = 0; n < PROTO_FRAMES_PER_RPT; n++) {
    adc_frame_t f;
    if (!adc_read_frame(&f)) break;
    if (f.frame == last_streamed) break;
    if (last_streamed && (f.frame - last_streamed) < stream_decim) break;
    if (last_streamed && (f.frame - last_streamed) > stream_decim) stream_gap = true;
    last_streamed = f.frame;

    if (!p) p = tx_begin(RSP_STREAM, 0);

    uint8_t *e = p + PROTO_STREAM_HDR + n * PROTO_FRAME_BYTES;
    memcpy(e, &f.frame, 4);
    const uint8_t *map = adc_slot_map();
    for (int s = 0; s < PROTO_NUM_SLOTS; s++) {
      if (map[s] < PROTO_NUM_KEYS) memcpy(e + 4 + 2 * map[s], &f.slot[s], 2);
    }
  }

  if (!p) return;

  p[4] = n;
  p[5] = (uint8_t)(stream_gap ? 1 : 0);
  p[6] = stream_decim;
  stream_gap = false;
  tx_commit();
  tx_pump();
}

/* -------------------------------------------------------------- burst */
static uint16_t burst_buf[PROTO_BURST_MAX];
static volatile uint8_t  burst_state;      /* 0 idle, 1 running, 2 done */
static uint16_t burst_count, burst_want;
static uint8_t  burst_slot;

/* v1 captures by polling frames rather than reconfiguring the ADC — it costs
 * scan-rate resolution but cannot disturb the live scan engine, which matters
 * while this is the only way to see the board. */
static void burst_service(void)
{
  if (burst_state != 1) return;

  adc_frame_t f;
  if (!adc_read_frame(&f)) return;
  static uint32_t last;
  if (f.frame == last) return;
  last = f.frame;

  burst_buf[burst_count++] = f.slot[burst_slot];
  if (burst_count >= burst_want) burst_state = 2;
}

/* ------------------------------------------------------------ dispatch */
static void send_info(uint8_t tag)
{
  uint8_t *p = tx_begin(RSP_INFO, tag);
  p[4] = PROTO_VERSION;
  p[5] = PROTO_NUM_KEYS;
  p[6] = PROTO_NUM_SLOTS;
  p[7] = 12;
  const uint16_t hz = ADC_SCAN_HZ;
  memcpy(&p[8], &hz, 2);
  memcpy(&p[10], adc_slot_map(), PROTO_NUM_SLOTS);   /* [10..19] */
  const uint32_t cpg_q8 = 1573;                      /* 6.144 counts/Gs * 256 */
  memcpy(&p[20], &cpg_q8, 4);
  const uint32_t build = BUILD_ID;
  memcpy(&p[24], &build, 4);
  memcpy(&p[28], adc_sequence(), PROTO_SEQ_LEN);     /* [28..32] */
  memcpy(&p[PROTO_INFO_UID], uid_bytes(), 12);       /* [33..44] */
  const uint16_t tag16 = uid_tag();
  memcpy(&p[PROTO_INFO_UID_TAG], &tag16, 2);         /* [45..46] */
  p[PROTO_INFO_SENSE] = link_sense();
  p[PROTO_INFO_RESET] = reset_flags();
  {
    const uint16_t su = reset_suspends(), re = reset_resumes();
    memcpy(&p[PROTO_INFO_SUSPENDS], &su, 2);
    memcpy(&p[PROTO_INFO_RESUMES],  &re, 2);
  }
  {
    power_status_t ps;
    power_status(&ps);
    p[PROTO_INFO_POWER]  = ps.state;
    p[PROTO_INFO_RWU_EN] = ps.remote_wakeup_en;
    memcpy(&p[PROTO_INFO_WAKE_TRY],   &ps.wake_attempts, 2);
    memcpy(&p[PROTO_INFO_WAKE_GRANT], &ps.wake_grants, 2);
  }
  {
    keys_status_t ks;
    keys_status(&ks);
    p[PROTO_INFO_KEYS_EN]   = ks.enabled;
    p[PROTO_INFO_KEYS_OWN]  = ks.own_pressed;
    p[PROTO_INFO_KEYS_PEER] = ks.peer_pressed;
    p[PROTO_INFO_KEYS_MERGE]= ks.sending_peer;
  }                /* [47]     */
  tx_commit();
}

static void send_link(uint8_t tag, const link_test_t *t)
{
  uint8_t *p = tx_begin(RSP_LINK, tag);
  p[4] = t->mode;
  memcpy(&p[5],  &t->baud, 4);
  memcpy(&p[9],  &t->sent, 2);
  memcpy(&p[11], &t->received, 2);
  memcpy(&p[13], &t->mismatched, 2);
  memcpy(&p[15], &t->timeouts, 2);
  p[17] = t->first_bad_tx;
  p[18] = t->first_bad_rx;
  p[19] = t->err_flags;
  p[20] = link_sense();
  memcpy(&p[21], &t->sts,   4);
  memcpy(&p[25], &t->ctrl1, 4);
  memcpy(&p[29], &t->ctrl2, 4);
  memcpy(&p[33], &t->ctrl3, 4);
  memcpy(&p[37], &t->overruns, 2);
  memcpy(&p[39], &t->cycles, 4);
  tx_commit();
}

static void send_snapshot(uint8_t tag)
{
  /* ALWAYS answer. Returning silently when the frame read fails turns a
   * diagnosable fault into a host that hangs for its whole timeout with nothing
   * to go on — which is exactly how this looked from the outside. */
  adc_frame_t f;
  const bool ok = adc_read_frame(&f);
  if (!ok) memset(&f, 0, sizeof(f));

  uint8_t *p = tx_begin(RSP_SNAPSHOT, tag);
  memcpy(&p[4], &f.frame, 4);
  memcpy(&p[8], f.slot, 2 * PROTO_NUM_SLOTS);
  const uint32_t pe = adc_phase_errors();
  memcpy(&p[48], &pe, 4);
  memcpy(&p[52], &tx_dropped, 4);
  p[PROTO_SNAP_FLAGS] = ok ? 0u : 1u;
  const uint32_t rf = adc_read_failures();
  memcpy(&p[PROTO_SNAP_READ_FAIL], &rf, 4);
  p[PROTO_SNAP_SEQ_LSB] = (uint8_t)(adc_seq_raw() & 0xFFu);
  tx_commit();
}

static void send_burst_status(uint8_t tag)
{
  uint8_t *p = tx_begin(RSP_BURST_STATUS, tag);
  p[4] = burst_state;
  p[5] = burst_slot;
  memcpy(&p[6], &burst_count, 2);
  const uint32_t period_ns = 1000000000u / ADC_SCAN_HZ;
  memcpy(&p[8], &period_ns, 4);
  tx_commit();
}

static void send_burst_data(uint8_t tag, uint16_t off)
{
  uint8_t *p = tx_begin(RSP_BURST_DATA, tag);
  uint16_t n = 0;
  if (off < burst_count) {
    n = burst_count - off;
    if (n > PROTO_BURST_PER_RPT) n = PROTO_BURST_PER_RPT;
    memcpy(&p[7], &burst_buf[off], 2u * n);
  }
  memcpy(&p[4], &off, 2);
  p[6] = (uint8_t)n;
  tx_commit();
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize)
{
  (void)report_id; (void)report_type;
  /* Interface 1 is the keyboard, and its OUT reports are lock-LED state, not
   * protocol commands. Parsing them as commands would mean a host toggling caps
   * lock could execute whatever opcode the LED bitmap happened to spell. */
  if (instance != ITF_NUM_HID) return;
  if (bufsize < 2) return;

  const uint8_t cmd = buffer[0];
  const uint8_t tag = buffer[1];

  switch (cmd) {
    case CMD_INFO:
      send_info(tag);
      break;

    case CMD_STREAM_SET:
      stream_on    = buffer[2] != 0;
      stream_decim = buffer[3] ? buffer[3] : 1;
      last_streamed = 0;
      stream_gap    = false;
      send_info(tag);
      break;

    case CMD_SNAPSHOT:
      send_snapshot(tag);
      break;

    case CMD_BURST_START: {
      burst_slot  = buffer[2] < PROTO_NUM_SLOTS ? buffer[2] : 0;
      uint16_t want;
      memcpy(&want, &buffer[3], 2);
      if (want == 0 || want > PROTO_BURST_MAX) want = PROTO_BURST_MAX;
      burst_want  = want;
      burst_count = 0;
      burst_state = 1;
      send_burst_status(tag);
      break;
    }

    case CMD_SEQ_SET: {
      bool ok = true;
      for (int i = 0; i < PROTO_SEQ_LEN; i++) if (buffer[2 + i] > 3) ok = false;
      if (ok) adc_set_sequence(&buffer[2]);
      uint8_t *p = tx_begin(RSP_SEQ, tag);
      memcpy(&p[4], adc_sequence(), PROTO_SEQ_LEN);
      tx_commit();
      break;
    }

    case CMD_LINK_TEST: {
      uint32_t baud;
      uint16_t nbytes;
      memcpy(&baud,   &buffer[3], 4);
      memcpy(&nbytes, &buffer[7], 2);
      if (baud < 9600u || baud > 13500000u) baud = LINK_DISCOVERY_BAUD;
      if (nbytes == 0 || nbytes > 4096u)    nbytes = 256u;   /* == LINK_RX_BUF_MAX */

      const link_role_t role = (link_role_t)buffer[9];
      uint16_t tmo;
      memcpy(&tmo, &buffer[10], 2);
      if (tmo == 0 || tmo > 5000u) tmo = 750u;

      peer_enable(false);           /* nothing else may drive USART6 meanwhile */
      link_test_t t;
      link_selftest((link_mode_t)buffer[2], baud, nbytes, role, tmo, &t);

      /* Park the pins again. Leaving PC6/PC7 driven push-pull between tests is
       * how you find out the hard way that the peer was mid-transmission. */
      link_configure(LINK_MODE_OFF, 0);
      send_link(tag, &t);
      break;
    }

    case CMD_PEER: {
      if (buffer[2] == 1) peer_enable(true);
      else if (buffer[2] == 2) peer_enable(false);

      peer_status_t ps;
      peer_status(&ps);
      uint8_t *p = tx_begin(RSP_PEER, tag);
      p[4] = ps.state;
      p[5] = ps.role;
      const uint16_t mine = uid_tag();
      memcpy(&p[6],  &mine, 2);
      memcpy(&p[8],  &ps.peer_tag, 2);
      memcpy(&p[10], &ps.peer_fw, 2);
      memcpy(&p[12], &ps.baud, 4);
      memcpy(&p[16], &ps.hails_sent, 4);
      memcpy(&p[20], &ps.frames_rx, 4);
      memcpy(&p[24], &ps.crc_errors, 4);
      memcpy(&p[28], &ps.switches, 4);
      memcpy(&p[32], &ps.drops, 4);
      memcpy(&p[36], &ps.last_rx_ms, 4);
      tx_commit();
      break;
    }

    case CMD_SPLIT: {
      if (buffer[2] == 1) split_set_test(true);
      else if (buffer[2] == 2) split_set_test(false);

      split_stats_t ss;
      split_stats(&ss);
      uint8_t *p = tx_begin(RSP_SPLIT, tag);
      memcpy(&p[4],  &ss.tx_frames, 4);
      memcpy(&p[8],  &ss.rx_frames, 4);
      memcpy(&p[12], &ss.crc_errors, 4);
      memcpy(&p[16], &ss.seq_gaps, 4);
      memcpy(&p[20], &ss.resyncs, 4);
      memcpy(&p[24], &ss.payload_errors, 4);
      memcpy(&p[28], &ss.age_us, 4);
      memcpy(&p[32], &ss.period_us, 4);
      memcpy(&p[36], &ss.period_max_us, 4);
      memcpy(&p[40], &ss.period_min_us, 4);
      memcpy(&p[44], &ss.peer_seq, 2);
      p[46] = ss.peer_flags;
      p[47] = ss.stale;
      p[48] = split_test_enabled() ? 1u : 0u;

      const uint32_t room = (PROTO_REPORT_SIZE - PROTO_SPLIT_KEYS) / 2u;
      const uint32_t n    = (room < SPLIT_KEYS) ? room : SPLIT_KEYS;
      p[49] = (uint8_t)n;
      memcpy(&p[PROTO_SPLIT_KEYS], split_peer_keys(), n * 2u);
      tx_commit();
      break;
    }

    case CMD_POWER:
      /* Drives the real code path, not a mock: same power_on_suspend() the USB
       * stack calls. What it cannot reproduce is the bus going idle, so the
       * remote-wakeup handshake itself still needs a host that actually sleeps.
       * Pretend the host granted remote wakeup, so the press detector's call to
       * tud_remote_wakeup() is exercised too — it will simply be refused by the
       * stack while the bus is awake, which is the correct outcome and shows up
       * as attempts climbing with grants at zero. */
      if (buffer[2] == 1) power_on_suspend(true);
      else if (buffer[2] == 2) power_on_resume();
      send_info(tag);
      break;

    case CMD_KEYS:
      /* Output is off at boot and stays off until someone asks for it. The
       * thresholds are guesses until the travel pipeline calibrates them, and
       * this interface types into whatever it is plugged into. */
      if (buffer[2] == 1) keys_set_enabled(true);
      else if (buffer[2] == 2) keys_set_enabled(false);
      else if (buffer[2] == 3) keys_calibrate();
      send_info(tag);
      break;

    case CMD_LINK_HOLD:
      peer_enable(false);           /* nothing else may drive USART6 meanwhile */
      link_hold(buffer[2], buffer[3] != 0);
      send_info(tag);
      break;

    case CMD_LINK_PROBE: {
      uint32_t baud;
      memcpy(&baud, &buffer[3], 4);
      if (baud < 9600u || baud > 13500000u) baud = LINK_DISCOVERY_BAUD;

      peer_enable(false);           /* nothing else may drive USART6 meanwhile */
      link_probe_t pr;
      link_probe((link_mode_t)buffer[2], baud, &pr);

      uint8_t *p = tx_begin(RSP_PROBE, tag);
      p[PROTO_PROBE_PADS]     = pr.pads;
      p[PROTO_PROBE_SENSE_PU] = pr.sense_pu;
      p[PROTO_PROBE_SENSE_PD] = pr.sense_pd;
      memcpy(&p[PROTO_PROBE_SAMPLES], &pr.mux_samples, 2);
      memcpy(&p[PROTO_PROBE_MUX], pr.mux_low, sizeof(pr.mux_low));
      tx_commit();
      break;
    }

    case CMD_BURST_STATUS:
      send_burst_status(tag);
      break;

    case CMD_BURST_READ: {
      uint16_t off;
      memcpy(&off, &buffer[2], 2);
      send_burst_data(tag, off);
      break;
    }

    case CMD_BOOTLOADER: {
      /* Acknowledge first — the host will never see anything after the jump. */
      uint8_t *p = tx_begin(RSP_INFO, tag);
      p[4] = PROTO_VERSION;
      tx_commit();
      tx_pump();
      for (volatile int i = 0; i < 2000000; i++) {
      }
      usb_jump_to_bootloader();      /* resets; nothing after this runs */
      break;
    }

    case CMD_BURST_ABORT:
      burst_state = 0;
      burst_count = 0;
      send_burst_status(tag);
      break;

    default: {
      uint8_t *p = tx_begin(RSP_ERROR, tag);
      p[4] = cmd;
      tx_commit();
      break;
    }
  }
  tx_pump();
}

/* ------------------------------------------------- connection lifecycle */
/* Telemetry must not outlive the host that asked for it.
 *
 * Without these, stream_on survives a disconnect, a re-enumeration and the host
 * closing its handle — so an aborted capture leaves the device blasting 64-byte
 * reports at 8 kHz forever, with nobody reading. On macOS that is merely untidy.
 * On Windows the HID class driver's input ring overflows, ReadFile fails, the
 * port gets reset, the device re-enumerates into the same flood, and the next
 * open walks straight back into it. Observed here as a device that "randomly"
 * dropped off and could not be reopened, and very likely as the AMD xHCI
 * controller that went to CM_PROB_FAILED_POST_START (problem 43) under a flood
 * with no reader.
 *
 * A host that wants telemetry asks for it again after it reconnects. */
static void telemetry_quiesce(void)
{
  stream_on     = false;
  stream_decim  = 8;
  last_streamed = 0;
  stream_gap    = false;
  burst_state   = 0;
  burst_count   = 0;
  tx_head = tx_tail = 0;      /* drop anything queued for a host that is gone */
}

void tud_mount_cb(void)   { telemetry_quiesce(); }
void tud_umount_cb(void)  { telemetry_quiesce(); }

/* Suspend is not disconnect: the host still owns us, it has just stopped
 * polling. Stop producing either way — bus-powered devices must drop to the
 * suspend current budget, and anything queued now is stale by resume. */
void tud_suspend_cb(bool remote_wakeup_en)
{
  (void)remote_wakeup_en;
  telemetry_quiesce();
  /* A host that suspends us and a hub that cuts our power look identical from
   * the outside. This counts the first; RESET_POR on the next boot reports the
   * second. */
  reset_note_suspend();
  power_on_suspend(remote_wakeup_en);
}
void tud_resume_cb(void) { reset_note_resume(); power_on_resume(); }

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
  (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
  return 0;
}

/* Survives reset (see ld/at32f405rbt7.ld). Jumping into the ROM bootloader from
 * a live application does not work on this part — the first attempt left the
 * core hung with nothing on either USB port. Instead: stash a magic, reset, and
 * let early startup do the jump from a clean state. */
__attribute__((section(".noinit"))) volatile uint32_t giris_boot_magic;

void usb_jump_to_bootloader(void)
{
  giris_boot_magic = GIRIS_BOOT_MAGIC;
  __DSB();
  NVIC_SystemReset();
}

void usb_bootloader_check(void)
{
  if (giris_boot_magic != GIRIS_BOOT_MAGIC) return;
  giris_boot_magic = 0;
  __DSB();

  __disable_irq();

  for (uint32_t i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }
  SysTick->CTRL = 0;

  const uint32_t sp = *(volatile uint32_t *)ROM_BOOTLOADER_ADDR;
  const uint32_t pc = *(volatile uint32_t *)(ROM_BOOTLOADER_ADDR + 4u);

  __set_MSP(sp);
  ((void (*)(void))pc)();

  for (;;) {
  }
}

/* ----------------------------------------------------------------- init */
void usb_init(void)
{
  crm_periph_clock_enable(CRM_OTGHS_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_OTGHSPHY_PERIPH_CLOCK, TRUE);

  /* The PHY's 12 MHz reference can only come from HEXT — the enum has exactly
   * one legal value, which is why the crystal must be 12 MHz +-50 ppm. */
  crm_usb_phy12_clock_select(CRM_USB_PHY12_CLOCK_HEXT_DIV_1);

  /* Leave power-down, and ignore VBUS. The RM wants ~1 ms after the PHY clock
   * is stable before going further. */
  OTGHS_GCCFG |= GCCFG_PWRDOWN | GCCFG_VBUSIG;
  board_delay_us(1000);

  nvic_irq_enable(OTGHS_IRQn, 2, 0);

  tud_init(BOARD_TUD_RHPORT);
}

void usb_task(void)
{
  tud_task();
  burst_service();
  stream_service();
  tx_pump();
}

void OTGHS_IRQHandler(void)
{
  tud_int_handler(BOARD_TUD_RHPORT);
}
