/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
#ifndef GIRIS_CONSOLE_H
#define GIRIS_CONSOLE_H

#include <stdint.h>

/* USART1 at 115200 8N1 on PA9 (TX) / PA10 (RX) — J6 pin 8 / pin 7, GND on J6
 * pin 1, 2 or 16. Wire a Raspberry Pi Debug Probe's UART side to those three and
 * you have a console without needing the Tag-Connect cable for SWD. */
void console_init(void);
void console_puts(const char *s);
void console_hex(uint32_t v);
void console_dec(uint32_t v);

/* Boot tracing: prints the stage and leaves a breadcrumb readable after a hang. */
void console_stage(const char *name);
uint32_t console_last_stage(void);

#endif
