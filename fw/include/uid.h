/*
 * uid.h — per-board identity from the 96-bit factory unique ID.
 *
 * Two boards running one image are indistinguishable to the host without this:
 * same VID, same PID, same descriptor strings, and even the ROM DFU reports the
 * serial "AT32" on every part. Everything about split bring-up — which half
 * logged that frame, which half to reflash, which half won arbitration — needs a
 * name that the board itself supplies.
 *
 * 0x1FFFF7E8 is Artery's device electronic signature, the same address their own
 * USB middleware reads (vendor/at32f402_405/middlewares/usbd_class (their _desc.h files)).
 */
#ifndef GIRIS_UID_H
#define GIRIS_UID_H

#include <stdint.h>

#define UID_BASE  0x1FFFF7E8u

/* The raw 96 bits, little-endian words, as read from flash. */
const uint32_t *uid_words(void);

/* The same 12 bytes, for the wire. */
const uint8_t *uid_bytes(void);

/* 24 uppercase hex characters, NUL-terminated. This is the USB serial string. */
const char *uid_serial(void);

/* A 16-bit condensation of the UID.
 *
 * Used for two things: a stable per-board colour, and the tie-break in link
 * arbitration. FNV-1a over all 12 bytes, so it does not care that Artery's UID
 * words share a lot/wafer prefix between two boards off the same reel — a plain
 * truncation would collide there. Never 0x0000 or 0xFFFF: those are reserved as
 * "no peer" and "line stuck" on the link. */
uint16_t uid_tag(void);

#endif
