/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * board.h — Giris TMR2615F_osu_pad (6-key analog dev pad) pin map.
 *
 * Every assignment here was read out of the exported netlist, not the schematic
 * source:  kicad-cli sch export netlist --format kicadsexpr
 * See ../../hw/TMR2615F_osu_pad/firmware_architecture.md §11b.
 */
#ifndef GIRIS_BOARD_H
#define GIRIS_BOARD_H

#include "at32f402_405.h"

/* ---------------------------------------------------------------- clocks */
#define BOARD_HEXT_HZ        12000000U   /* mandatory: the OTG_HS PHY's only legal reference */
#define BOARD_SCLK_HZ       216000000U

/* ------------------------------------------------------------- analog in */
/* TMUX1574 (U11) 4x SPDT.  SEL low  -> A set: sensors U13/U17/U21 + VBUS_B sense
 *                          SEL high -> B set: sensors U15/U19/U23                */
#define MUX_SEL_PORT         GPIOB
#define MUX_SEL_PIN          GPIO_PINS_2      /* PB2 */

/* Mux outputs D1..D4 land on ADC1_IN3..IN0 (note the reversed order). */
#define ADC_CH_D1            3                /* PA3 — S1A/S1B */
#define ADC_CH_D2            2                /* PA2 — S2A/S2B */
#define ADC_CH_D3            1                /* PA1 — S3A/S3B */
#define ADC_CH_D4            0                /* PA0 — S4A = VBUS_B/2, S4B = J5.10 */

/* Six TMR2615F-AAC-1.500-500 sensors, all on the +3.3VA rail (TPS7A4700, U4).
 * 1.500 mV/V/Gs, offset 500 mV/V, +-500 Gs magnetic, electrical rails at +-300 Gs.
 * => 6.144 ADC counts per Gauss, and the ratiometric output tracks VDDA exactly.
 * NOTE: the F variant's output is INVERTED — counts fall as the key travels.     */
#define SENSOR_COUNT         6

/* Per-key chain as built: VOUT -> 30 pF -> 5.1 k -> 3.3 nF -> mux.
 * Respin target is 2.2 k + 47 nF (+ 100 R in each hotswap socket branch).        */

/* Kailh hotswap sockets SW3..SW8 sit on the SAME analog node with their other
 * side on GND, so a *mechanical* switch shorts the sensor node. A reading pinned
 * near the bottom rail means "mechanical key closed", not a fault.               */

/* ------------------------------------------------------------------ leds */
/* 7-pixel SK6812MINI-E chain. Pixel 0 (U12) is the sacrificial level-shifter on
 * the 1N4148-dropped ~4.3 V rail; pixels 1..6 are the per-key LEDs on +5 V.      */
#define LED_DATA_PORT        GPIOB
#define LED_DATA_PIN         GPIO_PINS_9      /* PB9 -> R27 120R -> U12.DIN */
#define LED_COUNT            7
#define LED_FIRST_KEY_PIXEL  1

/* ------------------------------------------------------------------ link */
/* Inter-board USB-C J1 carries a UART, not USB.
 *   J1 D+ (A6/B6) -> R7 120R -> PC7   (net /UART7_TX)
 *   J1 D- (A7/B7) -> R3 120R -> PC6   (net /UART7_RX)
 * Both have 10k pull-ups (R1/R2) and SRV05-4A (U1) clamps.
 *
 * CAREFUL: the net names are inverted relative to the silicon. PC6 is natively
 * USART6_TX/UART7_TX and PC7 is natively USART6_RX/UART7_RX, so this board's
 * "default" half is the one that must set CTRL2.TRPSWAP.
 *
 * Prefer USART6 (MUX8) over UART7 (MUX9): same pins, but APB2-clocked at 216 MHz,
 * giving an exact integer ladder of 13.5 / 12 / 9 / 8 / 6 Mbaud instead of a
 * 6.75 Mbaud ceiling.  Target 9 Mbaud.                                           */
#define LINK_PORT            GPIOC
#define LINK_PIN_PC6         GPIO_PINS_6
#define LINK_PIN_PC7         GPIO_PINS_7
#define LINK_DISCOVERY_BAUD  115200U    /* open-drain phase; ~500 kbaud is the ceiling */
#define LINK_RUN_BAUD        9000000U   /* push-pull phase on USART6 */

/* ----------------------------------------------------------------- power */
/* PC13 drives the AP22653 (U3) enable, ACTIVE HIGH, and sources 5 V out J1.
 * It is Hi-Z at reset and R27's 10k pulldown holds the switch OFF.
 *
 * DO NOT DRIVE THIS PIN until link arbitration has completed and the VBUS_B
 * divider (mux S4A -> ADC1_IN0) reads cold. If a peer answers the hail, that is
 * dual-host mode and NOBODY sources VBUS.                                        */
#define PWR_SOURCE_EN_PORT   GPIOC
#define PWR_SOURCE_EN_PIN    GPIO_PINS_13     /* PC13, net /PW_PSTRH */

#define LINK_POWERED_PORT    GPIOB
#define LINK_POWERED_PIN     GPIO_PINS_12     /* PB12, net /LM_ST — high = we are the slave */

#define PWR_FAULT_PORT       GPIOB
#define PWR_FAULT_PIN        GPIO_PINS_10     /* PB10, net /AP_FAULT — AP22653 overcurrent */

#define PWR_DEBOUNCE_MS      10               /* AP22653 FAULT# has a 6 ms deglitch */

/* PB12 is also OTGHS_ID in AF 0xA and PB13 is OTGHS_VBUS — never mux PB12 to
 * AF 0xA. This design is device-only; force device mode.                         */

/* ------------------------------------------------------------------- usb */
/* J3 = USB HS host port (dedicated OTGHS1_D+/D-/R pins; OTGHS1_R needs its
 *      external 12k 1% to ground).  This is the 8 kHz path.
 * J2 = USB FS, DATA ONLY — PA11/PA12, no VBUS net at all. This is the only port
 *      that can reach the factory ROM DFU (2e3c:df11).                           */

/* ------------------------------------------------------------ debug / io */
#define SCOPE_PORT           GPIOD
#define SCOPE_PIN            GPIO_PINS_2      /* PD2, net /IO1, on J6 pin 17 */

/* J9 = SWD: PA13 SWDIO, PA14 SWCLK, PB3 SWO, plus USART1 on PA9/PA10.
 * SW1 = BOOT0 (PF11), SW2 = NRST.                                                */

#endif /* GIRIS_BOARD_H */
