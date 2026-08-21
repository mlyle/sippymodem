// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_S_TONE_DETECTOR_H
#define V34_S_TONE_DETECTOR_H

#include "v34_phase3_profile.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_s_tone_detector_result {
	V34_S_TONE_DETECTOR_OK,
	V34_S_TONE_DETECTOR_DETECTED,
	V34_S_TONE_DETECTOR_INVALID_ARGUMENT,
	V34_S_TONE_DETECTOR_INVALID_STATE,
};

enum v34_s_tone_carrier {
	V34_S_TONE_CARRIER_1600,
	V34_S_TONE_CARRIER_1680,
	V34_S_TONE_CARRIER_1800,
	V34_S_TONE_CARRIER_1829,
	V34_S_TONE_CARRIER_1867,
	V34_S_TONE_CARRIER_1920,
	V34_S_TONE_CARRIER_1959,
	V34_S_TONE_CARRIER_2000,
	V34_S_TONE_CARRIER_COUNT,
};

/* Carrier-selective S-energy acquisition. Input samples are the gain-scaled
 * real analytic stream. One group boundary is supplied for each recovered
 * symbol, matching the timing owner that consumes the same sample epoch. */
struct v34_s_tone_detector {
	int16_t stage_a[2];
	int16_t stage_b[2];
	int16_t energy;
	uint8_t warmup_groups;
	uint8_t qualifying_groups;
	enum v34_s_tone_carrier carrier;
	bool acquired;
	bool detected;
	bool initialized;
};

enum v34_s_tone_detector_result v34_s_tone_detector_init(
	struct v34_s_tone_detector *detector,
	const struct v34_phase3_profile *transmitting_profile);
bool v34_s_tone_detector_is_valid(
	const struct v34_s_tone_detector *detector);
enum v34_s_tone_detector_result v34_s_tone_detector_sample(
	struct v34_s_tone_detector *detector, int16_t sample);
enum v34_s_tone_detector_result v34_s_tone_detector_end_group(
	struct v34_s_tone_detector *detector);

#endif /* V34_S_TONE_DETECTOR_H */
