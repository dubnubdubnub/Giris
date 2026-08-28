#ifndef GIRIS_ADC_H
#define GIRIS_ADC_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

typedef struct {
  uint32_t frame;                    /* monotonic frame index, 8 kHz */
  uint16_t slot[PROTO_NUM_SLOTS];    /* raw counts, hardware scan order */
} adc_frame_t;

void adc_scan_init(void);

/* Lock-free read of the most recent complete frame. False if the writer kept
 * winning the race (should never happen in practice). */
bool adc_read_frame(adc_frame_t *out);

const uint8_t *adc_slot_map(void);
uint32_t adc_phase_errors(void);

/* Times adc_read_frame() gave up, and the raw seqlock counter. An odd counter
 * means the writer is stuck mid-update; a climbing failure count with a static
 * frame index means the scan engine has stopped. */
uint32_t adc_read_failures(void);
uint32_t adc_seq_raw(void);

/* Just the published frame index — one volatile read, no seqlock retry and no
 * 10-slot copy. The split link paces itself off this, so it is called every
 * time round the main loop and adc_read_frame() would be far too heavy. */
uint32_t adc_frame_index(void);
bool adc_calibration_failed(void);

/* Realign the DMA ring with the mux SEL phase. Call after anything that blocks
 * interrupts for more than a scan period. */
void adc_resync(void);

/* Ordinary-sequence order, rank 1..4, as ADC channel numbers 0..3. */
const uint8_t *adc_sequence(void);
void adc_set_sequence(const uint8_t ch[5]);

#define ADC_SCAN_HZ  8000u

#endif
