/*
 * main.c — Giris firmware, first light.
 *
 * Today: clock to 216 MHz, run the ADC/mux scan engine at 8 kHz, and expose the
 * readings over a raw-HID interface on the USB HS port (J3) so the browser viewer
 * can plot and log them. The SK6812 chain doubles as a status indicator.
 *
 * Still no keyboard interface, deliberately — with no keyboard usages in the
 * descriptor, macOS does not demand Input Monitoring to open the device.
 *
 * See ../../hw/TMR2615F_osu_pad/firmware_architecture.md for what comes next.
 */
#include "board.h"
#include "clock.h"
#include "sk6812.h"
#include "adc.h"
#include "uid.h"
#include "reset.h"
#include "power.h"
#include "link.h"
#include "peer.h"
#include "split.h"
#include "usb.h"
#include "console.h"
#include "tusb.h"

static uint32_t leds[LED_COUNT];

/* 8-bit hue -> 0x00GGRRBB, dim enough that the whole chain is safe on any port. */
static uint32_t hue(uint8_t h, uint8_t v)
{
  uint8_t r, g, b;
  const uint8_t seg = h / 43u;
  const uint8_t f   = (uint8_t)((h - (seg * 43u)) * 6u);

  switch (seg) {
    case 0:  r = 255;            g = f;              b = 0;   break;
    case 1:  r = (uint8_t)(255 - f); g = 255;        b = 0;   break;
    case 2:  r = 0;              g = 255;            b = f;   break;
    case 3:  r = 0;              g = (uint8_t)(255 - f); b = 255; break;
    case 4:  r = f;              g = 0;              b = 255; break;
    default: r = 255;            g = 0;              b = (uint8_t)(255 - f); break;
  }

  r = (uint8_t)((r * v) / 255u);
  g = (uint8_t)((g * v) / 255u);
  b = (uint8_t)((b * v) / 255u);

  return ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
}

static void scope_pin_init(void)
{
  crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);

  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins           = SCOPE_PIN;
  gi.gpio_mode           = GPIO_MODE_OUTPUT;
  gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull           = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(SCOPE_PORT, &gi);
}

int main(void)
{
  /* Boot on the reset-default internal oscillator so the console exists BEFORE
   * anything that can hang. The previous build initialised the PLL first and
   * spun forever waiting on HEXT — from outside that is indistinguishable from
   * a dead board. */
  /* Before anything else can reset the part, and before the DFU jump, so the
   * flags describe the reset that actually got us here. */
  reset_capture();

  usb_bootloader_check();      /* honours a pending reboot-to-DFU request */

  system_core_clock_update();
  board_delay_cycles_init();
  console_init();

  console_puts("\n\n=== Giris fw (safe boot) ===\nboot sclk ");
  console_dec(system_core_clock);
  console_puts(" Hz (HICK)\n");

  console_stage("probing HEXT - the 12 MHz crystal");
  const bool hext_ok = board_clock_try_hext();
  if (!hext_ok) {
    console_puts("!! HEXT NEVER STABILISED.\n");
    console_puts("!! USB HS cannot work: that crystal is the HS PHY's only legal reference.\n");
    console_puts("!! Check Y1, its load caps, and that OTGHS1_R has its 12k 1%.\n");
  } else {
    console_stage("HEXT stable - switching to 216 MHz PLL");
    if (board_clock_init()) {
      console_init();                 /* re-init: the baud divisor changed */
      console_puts("sclk now ");
      console_dec(system_core_clock);
      console_puts(" Hz\n");
    } else {
      console_puts("!! PLL or sysclk switch timed out; still on HICK\n");
    }
  }

  /* PC13 (AP22653 enable) is deliberately left untouched. It is Hi-Z at reset and
   * R27's 10k pulldown holds the 5 V source OFF. */

  /* Identity before anything else that talks. With two halves running one
   * image, an unlabelled log line is worthless. */
  console_stage("board identity");
  console_puts("   uid    ");
  console_puts(uid_serial());
  console_puts("\n   tag    0x");
  console_hex(uid_tag());
  {
    const uint8_t rf = reset_flags();
    console_puts("\n   reset  ");
    if (rf & RESET_POR)    console_puts("power-on/brown-out ");
    if (rf & RESET_NRST)   console_puts("NRST-pin ");
    if (rf & RESET_SW)     console_puts("software ");
    if (rf & RESET_WDT)    console_puts("watchdog ");
    if (rf & RESET_WWDT)   console_puts("window-watchdog ");
    if (rf & RESET_LOWPWR) console_puts("low-power ");
    if (rf == 0)           console_puts("(no flag set)");
  }
  console_puts("\n");

  console_stage("link pins (J1 / USART6)");
  link_init();
  {
    const uint8_t s = link_sense();
    console_puts("   PB12 /LM_ST   ");
    console_puts((s & 1u) ? "high" : "low");
    console_puts("   (U2 LM66100 ST, the VBUS_B ideal diode)\n   PB10 /AP_FAULT ");
    console_puts((s & 2u) ? "high" : "low");
    console_puts("   (AP22653 overcurrent, active low)\n");
  }

  console_stage("scope pin");
  scope_pin_init();

  console_stage("sk6812");
  sk6812_init();

  console_stage("usb (OTG_HS on J3)");
  if (hext_ok) {
    usb_init();
  } else {
    console_puts("   skipped - no crystal\n");
  }

  console_stage("adc scan engine");
  adc_scan_init();
  if (adc_calibration_failed()) console_puts("!! ADC calibration timed out\n");

  console_stage("power policy");
  power_init();

  console_stage("link arbitration");
  console_puts("   link protocol v");
  console_dec(PEER_PROTO);
  console_puts("  (both halves must match to reach the run phase)\n");
  peer_init();

  console_stage("running");

  /* Set the status pixels ONCE and leave them alone. The bit-bang blocks
   * interrupts for ~210 us, which is 3.4 TMR2 periods: conversions keep firing
   * while the SEL flip cannot, so the DMA ring slips a bank. Resyncing after
   * each write costs a glitched frame instead, which is no better while we are
   * measuring. The real fix is SPI + DMA (see the architecture doc); until then
   * the LEDs simply do not run during acquisition. */
  leds[0] = 0;
  for (uint32_t i = LED_FIRST_KEY_PIXEL; i < LED_COUNT; i++) leds[i] = hue((uint8_t)(i * 36u), 10);
  sk6812_write(leds, LED_COUNT);
  adc_resync();

  /* No periodic console heartbeat. It was a first-light aid and USB telemetry
   * replaced it long ago, but it survived into the run phase and cost dearly:
   * console_puts() is a blocking polled write at 115200, so a ~100-character
   * status line stops the main loop for ~9 ms. Measured against the split link
   * that was 96 lost frames per second — 1.2 % of an 8 kHz stream, and a
   * worst-case inter-arrival of 8.6 ms — from a debug print nobody was reading.
   * Everything it printed is available over USB: CMD_SNAPSHOT for the frame
   * index and slots, RSP_INFO for the phase error count. */
  for (;;) {
    usb_task();
    /* Rides in every link frame. Both halves setting it is topology (b) — the
     * dual-host case — and it is the one input topology detection needs that
     * cannot be worked out locally. */
    split_set_host(tud_mounted());
    peer_task();
    power_task();

    /* Only when the host is asleep AND no peer is relying on us. In SERVING the
     * link still has a frame due every 125 us and sleeping would wreck it. */
    if (power_may_idle()) __WFI();
  }
}
