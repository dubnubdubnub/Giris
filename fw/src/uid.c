#include "uid.h"

static char     serial[25];
static uint16_t tag;
static uint8_t  ready;

static void uid_load(void)
{
  if (ready) return;

  static const char hexdig[] = "0123456789ABCDEF";
  const uint8_t *b = uid_bytes();

  for (int i = 0; i < 12; i++) {
    serial[2 * i]     = hexdig[b[i] >> 4];
    serial[2 * i + 1] = hexdig[b[i] & 0x0Fu];
  }
  serial[24] = '\0';

  uint32_t h = 2166136261u;                 /* FNV-1a, 32-bit */
  for (int i = 0; i < 12; i++) {
    h ^= b[i];
    h *= 16777619u;
  }
  tag = (uint16_t)(h ^ (h >> 16));
  if (tag == 0x0000u || tag == 0xFFFFu) tag = 0x0001u;

  ready = 1;
}

const uint32_t *uid_words(void)
{
  return (const uint32_t *)UID_BASE;
}

const uint8_t *uid_bytes(void)
{
  return (const uint8_t *)UID_BASE;
}

const char *uid_serial(void)
{
  uid_load();
  return serial;
}

uint16_t uid_tag(void)
{
  uid_load();
  return tag;
}
