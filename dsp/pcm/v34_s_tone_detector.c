// SPDX-License-Identifier: BSD-3-Clause

#include "v34_s_tone_detector.h"

#include "integer_math.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	V34_S_TONE_WARMUP_GROUPS = 10,
	V34_S_TONE_QUALIFYING_GROUPS = 9,
	V34_S_TONE_ACQUIRE_THRESHOLD = 0x0100,
	V34_S_TONE_HIGH_THRESHOLD = 0x0600,
	V34_S_TONE_ENERGY_POLE_Q14 = 0x3c2e,
	V34_S_TONE_FEEDBACK_Q14 = 15608,
};

struct detector_profile {
	int16_t stage_a_feedback;
	int16_t stage_b_feedback;
	uint16_t threshold_scale_q15;
};

/* These compact resonator profiles preserve the carrier-specific S detector
 * response after mapping its sample clock to 8 kHz. */
static const struct detector_profile PROFILES[] = {
	{ -9932, -9836, 0x3e2b },
	{ -8002, -7905, 0x3f73 },
	{ -5053, -4954, 0x419b },
	{ -4333, -4233, 0x422a },
	{ -3385, -3285, 0x42f2 },
	{ -2058, -1958, 0x4416 },
	{ -979, -1080, 0x44f9 },
	{ -50, 50, 0x45f5 },
};

_Static_assert(
	sizeof(PROFILES) / sizeof(PROFILES[0]) == V34_S_TONE_CARRIER_COUNT,
	"V.34 S detector carrier profile count changed");

static int16_t multiply_shift_q14(int16_t left, int16_t right)
{
	return wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)left * right), 14));
}

static int16_t absolute_shift4(int16_t value)
{
	int16_t shifted = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)(int32_t)value, 4));

	return shifted < 0 ?
		wrap_s16_from_s32(-(int32_t)shifted) : shifted;
}

static bool select_carrier(
	const struct v34_phase3_profile *profile,
	enum v34_s_tone_carrier *carrier)
{
	switch (profile->symbol_rate) {
	case V34_SYMBOL_RATE_2400:
		*carrier = profile->high_carrier ?
			V34_S_TONE_CARRIER_1800 : V34_S_TONE_CARRIER_1600;
		return true;
	case V34_SYMBOL_RATE_2743:
		/* The low 1646-Hz carrier is covered by the adjacent 1680-Hz
		 * resonator; the high selection is 1829 Hz. */
		*carrier = profile->high_carrier ?
			V34_S_TONE_CARRIER_1829 : V34_S_TONE_CARRIER_1680;
		return true;
	case V34_SYMBOL_RATE_2800:
		*carrier = profile->high_carrier ?
			V34_S_TONE_CARRIER_1867 : V34_S_TONE_CARRIER_1680;
		return true;
	case V34_SYMBOL_RATE_3000:
		*carrier = profile->high_carrier ?
			V34_S_TONE_CARRIER_2000 : V34_S_TONE_CARRIER_1800;
		return true;
	case V34_SYMBOL_RATE_3200:
		*carrier = profile->high_carrier ?
			V34_S_TONE_CARRIER_1920 : V34_S_TONE_CARRIER_1829;
		return true;
	case V34_SYMBOL_RATE_3429:
		*carrier = V34_S_TONE_CARRIER_1959;
		return true;
	case V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

bool v34_s_tone_detector_is_valid(
	const struct v34_s_tone_detector *detector)
{
	return detector != NULL && detector->initialized &&
	       detector->carrier < V34_S_TONE_CARRIER_COUNT &&
	       detector->warmup_groups <= V34_S_TONE_WARMUP_GROUPS &&
	       detector->qualifying_groups <= V34_S_TONE_QUALIFYING_GROUPS &&
	       (!detector->acquired ||
		detector->warmup_groups == V34_S_TONE_WARMUP_GROUPS) &&
	       (!detector->acquired ? detector->qualifying_groups == 0u : true) &&
	       (!detector->detected ? true :
		(detector->acquired &&
		 detector->qualifying_groups == V34_S_TONE_QUALIFYING_GROUPS));
}

enum v34_s_tone_detector_result v34_s_tone_detector_init(
	struct v34_s_tone_detector *detector,
	const struct v34_phase3_profile *transmitting_profile)
{
	struct v34_s_tone_detector initialized = { 0 };

	if (detector == NULL ||
	    !v34_phase3_profile_is_valid(transmitting_profile) ||
	    !select_carrier(transmitting_profile, &initialized.carrier))
		return V34_S_TONE_DETECTOR_INVALID_ARGUMENT;
	initialized.initialized = true;
	if (!v34_s_tone_detector_is_valid(&initialized))
		return V34_S_TONE_DETECTOR_INVALID_ARGUMENT;
	*detector = initialized;
	return V34_S_TONE_DETECTOR_OK;
}

enum v34_s_tone_detector_result v34_s_tone_detector_sample(
	struct v34_s_tone_detector *detector, int16_t sample)
{
	const struct detector_profile *profile;
	int32_t stage_a;
	int16_t shifted_a;
	int32_t stage_b;
	int16_t stage_b_word;
	int16_t energy;

	if (!v34_s_tone_detector_is_valid(detector))
		return V34_S_TONE_DETECTOR_INVALID_STATE;
	if (detector->detected)
		return V34_S_TONE_DETECTOR_DETECTED;
	profile = &PROFILES[detector->carrier];
	stage_a = arithmetic_shift_right_u32(
		(uint32_t)(int32_t)sample, 4) -
		  multiply_shift_q14(
			detector->stage_a[0], profile->stage_a_feedback) -
		  multiply_shift_q14(
			detector->stage_a[1], V34_S_TONE_FEEDBACK_Q14);
	shifted_a = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)stage_a, 4));
	detector->stage_a[1] = detector->stage_a[0];
	detector->stage_a[0] = wrap_s16_from_s32(stage_a);
	stage_b = shifted_a -
		  multiply_shift_q14(
			detector->stage_b[0], profile->stage_b_feedback) -
		  multiply_shift_q14(
			detector->stage_b[1], V34_S_TONE_FEEDBACK_Q14);
	stage_b_word = wrap_s16_from_s32(stage_b);
	detector->stage_b[1] = detector->stage_b[0];
	detector->stage_b[0] = stage_b_word;
	energy = wrap_s16_from_s32(
		(int32_t)absolute_shift4(stage_b_word) +
		multiply_shift_q14(detector->energy, V34_S_TONE_ENERGY_POLE_Q14));
	detector->energy = energy;
	return V34_S_TONE_DETECTOR_OK;
}

static int16_t scaled_threshold(
	const struct detector_profile *profile, uint16_t threshold)
{
	return (int16_t)(((uint32_t)threshold *
			 profile->threshold_scale_q15 +
			 0x4000u) >>
			15);
}

enum v34_s_tone_detector_result v34_s_tone_detector_end_group(
	struct v34_s_tone_detector *detector)
{
	const struct detector_profile *profile;

	if (!v34_s_tone_detector_is_valid(detector))
		return V34_S_TONE_DETECTOR_INVALID_STATE;
	if (detector->detected)
		return V34_S_TONE_DETECTOR_DETECTED;
	if (detector->warmup_groups < V34_S_TONE_WARMUP_GROUPS) {
		detector->warmup_groups++;
		return V34_S_TONE_DETECTOR_OK;
	}
	profile = &PROFILES[detector->carrier];
	if (!detector->acquired) {
		if (detector->energy <= scaled_threshold(
					 profile, V34_S_TONE_ACQUIRE_THRESHOLD))
			return V34_S_TONE_DETECTOR_OK;
		detector->acquired = true;
		return V34_S_TONE_DETECTOR_OK;
	}
	if (detector->energy > scaled_threshold(
				 profile, V34_S_TONE_HIGH_THRESHOLD)) {
		if (detector->qualifying_groups < V34_S_TONE_QUALIFYING_GROUPS)
			detector->qualifying_groups++;
	} else {
		detector->qualifying_groups = 0;
	}
	if (detector->qualifying_groups == V34_S_TONE_QUALIFYING_GROUPS) {
		detector->detected = true;
		return V34_S_TONE_DETECTOR_DETECTED;
	}
	return V34_S_TONE_DETECTOR_OK;
}
