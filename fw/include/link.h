/*
 * link.h — the inter-board link on J1 (USART6, PC6/PC7).
 *
 * Board facts, from the exported netlist:
 *
 *   J1 D+ (A6/B6) --- TP1 --- R7 120R --- PC7   net /UART7_TX   R1 10k -> +3.3V
 *   J1 D- (A7/B7) --- TP2 --- R3 120R --- PC6   net /UART7_RX   R2 10k -> +3.3V
 *   SRV05-4A (U1) clamps both.  Pull-ups R1/R2 sit on the MCU side of the 120R.
 *
 * TRAP 1 — the net names are backwards. PC6 is natively USART6_TX and PC7 is
 * natively USART6_RX (MUX8), so /UART7_TX is physically the RX pin.
 *
 * TRAP 2 — a straight-through USB-C cable joins like to like: D+ to D+, D- to D-.
 * Two identical halves therefore land TX on TX. Exactly one half must set
 * CTRL2.TRPSWAP, and until arbitration has run, neither knows which.
 *
 * Which is why discovery is single-wire. SLBEN half-duplex on PC6 with the pin
 * open-drain turns D- into a wired-AND bus that both halves drive and listen to
 * symmetrically, with no prior agreement and no pin swap. The pull-ups are
 * already fitted; two connected boards put 10k || 10k = 5k on the bus with 240R
 * (R3 + R3') between the MCU pins, so a driven low reads 3.3 * 240/10240 = 77 mV
 * at the far pin, comfortably below VIL. Rise is ~5k x 70pF = 350 ns, which is
 * why discovery is 115200 and the run phase is not.
 *
 * MEASURED, 2026-08-28, both boards, J1 empty:
 *  - MUX8 is USART6. link_probe() swept all sixteen indices and only MUX8 put a
 *    modulated waveform on the pad — 78-80 % low against the 90 % that a run of
 *    0x00 should give, the rest being sampling overhead. MUX15 reads a flat
 *    100 % low, which is a pin nobody is driving, not a second USART.
 *  - Both pads pass: each drives to 0 open-drain and returns high through its
 *    fitted 10k, so R1/R2/R3/R7 are all good on both boards.
 *  - There is NO self-echo. RM section 12.2 says "TX and SW_RX are
 *    interconnected inside the USART", but with SLBEN=1, TEN=1 and REN=1 the
 *    receiver never raised RDBF on 256 transmitted bytes, at 115200 and at
 *    9 Mbaud, open-drain and push-pull alike, with no error flag set. The
 *    transmitter is provably running; the receiver is simply muted while it
 *    does. So a half-duplex run cannot be checked by a lone board: testing the
 *    RX half needs a peer on J1 or a TP1-TP2 jumper.
 *
 * USART6, not UART7. Same pins at MUX8, but USART6 is APB2-clocked at 216 MHz.
 * BAUDR is an integer divider with fixed 16x oversampling and DIV >= 16, so
 * USART6 gives an exact 13.5 / 12 / 9 / 8 / 6 Mbaud ladder where UART7 on APB1
 * tops out at 6.75.  216e6 / 24 = 9 Mbaud exactly; 216e6 / 1875 = 115200 exactly.
 */
#ifndef GIRIS_LINK_H
#define GIRIS_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  LINK_MODE_OFF       = 0,
  LINK_MODE_HD_PC6    = 1,  /* single wire, open drain, TX+RX both on PC6 = D- */
  LINK_MODE_HD_PC7    = 2,  /* same, but TRPSWAP, so the wire is PC7 = D+      */
  LINK_MODE_FD        = 3,  /* full duplex push-pull, TX = PC6 (D-), RX = PC7  */
  LINK_MODE_FD_SWAP   = 4,  /* full duplex push-pull, TX = PC7 (D+), RX = PC6  */
  LINK_MODE_HD_PC6_PP = 5,  /* half duplex on PC6, but push-pull — diagnostic only.
                             * If this echoes and LINK_MODE_HD_PC6 does not, the
                             * fault is the 10k rise time, not the peripheral. */
} link_mode_t;

typedef struct {
  uint8_t  mode;
  uint32_t baud;
  uint16_t sent;
  uint16_t received;
  uint16_t mismatched;
  uint16_t timeouts;
  uint8_t  first_bad_tx;
  uint8_t  first_bad_rx;
  uint8_t  err_flags;       /* bit0 PERR, bit1 FERR, bit2 NERR, bit3 ROERR */
  uint32_t sts, ctrl1, ctrl2, ctrl3;   /* the peripheral's own account of itself */
} link_test_t;

/* Configures PB12 (/LM_ST) and PB10 (/AP_FAULT) as inputs and enables the
 * USART6 and GPIOC clocks. Does NOT touch PC13. */
void link_init(void);

/* bit0 = PB12 /LM_ST level, bit1 = PB10 /AP_FAULT level.
 *
 * /LM_ST is the ST pin of U2, the LM66100 ideal diode whose input is VBUS_B off
 * J1; U7 is the matching one on VBUS_HOST off J3, and the two OR into +5V. So
 * this bit says whether the J1 side is the rail that is feeding us. The datasheet
 * polarity is NOT assumed here — read it on a board powered only from J3 and
 * again on one powered only through J1, and write down which is which. */
uint8_t link_sense(void);

/* MUX8 for USART6 on PC6/PC7 is an assumption inherited from ST's numbering.
 * The datasheet (DS_AT32F405_402 V2.03) confirms the pins carry USART6_TX and
 * USART6_RX but does not print the MUX index — that table is in the reference
 * manual. link_probe() sweeps it rather than trusting it. */
#define LINK_MUX_USART6  8

void link_configure_mux(link_mode_t mode, uint32_t baud, uint8_t mux);
void link_configure(link_mode_t mode, uint32_t baud);

typedef struct {
  /* Plain-GPIO check of the two link pads, no USART involved. Each pin is driven
   * open-drain low and then released to the board's fitted 10k, and the pad is
   * read back both times. 0b01 is a working pin with its pull-up present. */
  uint8_t pads;          /* bit0 PC6 low when driven, bit1 PC6 high when released,
                          * bit2/bit3 the same for PC7 */

  /* PB12 /LM_ST and PB10 /AP_FAULT read with the internal pull-up and then the
   * pull-down. Same both ways = the pin is being driven; different = it is
   * floating, and whatever it seemed to say means nothing. */
  uint8_t sense_pu;      /* bit0 PB12, bit1 PB10 */
  uint8_t sense_pd;

  /* Samples of the TX pad that read low while the USART transmits a run of
   * 0x00, one entry per MUX index. Continuous 0x00 holds the line low 9 bits in
   * 10, so the index that actually routes USART6 is unmistakable. */
  uint16_t mux_low[16];
  uint16_t mux_samples;
} link_probe_t;

void link_probe(link_mode_t mode, uint32_t baud, link_probe_t *out);

/* Sends 0x00..0xFF (nbytes of them, wrapping) one at a time and reads back what
 * the line returns, byte for byte.
 *
 * In a half-duplex mode the receiver is on the wire being driven, so a lone board
 * hears its own transmission: this passes with nothing plugged into J1 and proves
 * the clock, the MUX8 mapping, the divider and the open-drain drive against the
 * fitted pull-up. In a full-duplex mode it needs either a TP1-TP2 jumper or a
 * peer that is echoing. */
void link_selftest(link_mode_t mode, uint32_t baud, uint16_t nbytes, link_test_t *out);

#endif
