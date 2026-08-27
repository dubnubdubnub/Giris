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
bool adc_calibration_failed(void);

#define ADC_SCAN_HZ  8000u

#endif
