// SPDX-License-Identifier: BSD-3-Clause

#include "v92_su_rx.h"
#include "integer_math.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

enum su_comparison {
	SU_COMPARISON_NONE,
	SU_COMPARISON_PERIODIC,
	SU_COMPARISON_INVERTED,
};

static bool mode_is_valid(enum v92_su_rx_mode mode)
{
	return mode == V92_SU_RX_TRANSITION_ONLY ||
	       mode == V92_SU_RX_MEASURE_PHASE;
}

static bool stage_is_valid(enum v92_su_rx_stage stage)
{
	return stage >= V92_SU_RX_ACQUIRING && stage <= V92_SU_RX_FAILED;
}

static unsigned bit_count6(uint8_t bits)
{
	unsigned count = 0;

	for (unsigned index = 0; index < 6u; ++index)
		count += (bits >> index) & 1u;
	return count;
}

static bool comparison_bits_are_valid(const struct v92_su_rx *receiver)
{
	uint8_t classified = (uint8_t)(receiver->comparison_same |
					 receiver->comparison_opposite);
	uint8_t all_bits = (uint8_t)(
		receiver->history_signs | receiver->history_valid |
		receiver->comparison_same | receiver->comparison_opposite |
		receiver->comparison_valid);

	return (all_bits & UINT8_C(0xc0)) == 0u &&
	       (classified & (uint8_t)~receiver->comparison_valid) == 0u &&
	       (receiver->comparison_same & receiver->comparison_opposite) ==
		       0u;
}

bool v92_su_rx_is_valid(const struct v92_su_rx *receiver)
{
	if (receiver == NULL || !mode_is_valid(receiver->mode) ||
	    !stage_is_valid(receiver->stage) ||
	    receiver->minimum_magnitude == 0u ||
	    receiver->phase_minimum_magnitude == 0u ||
	    receiver->phase_samples[0] > V92_SU_RX_PHASE_SYMBOLS ||
	    receiver->phase_samples[1] > V92_SU_RX_PHASE_SYMBOLS ||
	    !comparison_bits_are_valid(receiver) ||
	    receiver->last_periodic_symbol > receiver->total_symbols ||
	    receiver->transition_symbol > receiver->total_symbols)
		return false;
	/* A sign-folded int16 sample contributes at most 32768. Bounding the
	 * retained sum makes the next addition safe throughout either window. */
	for (unsigned segment = 0; segment < 2u; ++segment) {
		int32_t limit = (int32_t)receiver->phase_samples[segment] * 32768;
		if (receiver->phase_component[segment] < -limit ||
		    receiver->phase_component[segment] > limit)
			return false;
	}
	switch (receiver->stage) {
	case V92_SU_RX_ACQUIRING:
		return receiver->phase_samples[0] == 0u &&
		       receiver->phase_samples[1] == 0u &&
		       receiver->phase_component[0] == 0 &&
		       receiver->phase_component[1] == 0 &&
		       receiver->phase_extension_q16 == 0u;
	case V92_SU_RX_FIRST_PHASE:
		return receiver->mode == V92_SU_RX_MEASURE_PHASE &&
		       receiver->phase_samples[0] > 0u &&
		       receiver->phase_samples[0] < V92_SU_RX_PHASE_SYMBOLS &&
		       receiver->phase_samples[1] == 0u &&
		       receiver->phase_component[1] == 0 &&
		       receiver->phase_extension_q16 == 0u;
	case V92_SU_RX_WAITING_TRANSITION:
		return receiver->phase_samples[1] == 0u &&
		       receiver->phase_component[1] == 0 &&
		       receiver->phase_extension_q16 == 0u &&
		       (receiver->mode == V92_SU_RX_TRANSITION_ONLY ?
			receiver->phase_samples[0] == 0u &&
				receiver->phase_component[0] == 0 :
			receiver->phase_samples[0] ==
					V92_SU_RX_PHASE_SYMBOLS);
	case V92_SU_RX_SECOND_PHASE:
		return receiver->mode == V92_SU_RX_MEASURE_PHASE &&
		       receiver->phase_samples[0] == V92_SU_RX_PHASE_SYMBOLS &&
		       receiver->phase_samples[1] < V92_SU_RX_PHASE_SYMBOLS &&
		       receiver->transition_symbol != 0u &&
		       receiver->phase_extension_q16 == 0u;
	case V92_SU_RX_COMPLETE:
		return receiver->mode == V92_SU_RX_TRANSITION_ONLY ?
		       receiver->phase_samples[0] == 0u &&
			       receiver->phase_samples[1] == 0u :
		       receiver->phase_samples[0] == V92_SU_RX_PHASE_SYMBOLS &&
			       receiver->phase_samples[1] ==
				       V92_SU_RX_PHASE_SYMBOLS;
	case V92_SU_RX_FAILED:
		return receiver->mode == V92_SU_RX_MEASURE_PHASE;
	}
	return false;
}

enum v92_su_rx_result v92_su_rx_init(
	struct v92_su_rx *receiver, enum v92_su_rx_mode mode,
	uint16_t minimum_magnitude, uint16_t phase_minimum_magnitude)
{
	struct v92_su_rx initialized = {
		.minimum_magnitude = minimum_magnitude,
		.phase_minimum_magnitude = phase_minimum_magnitude,
		.mode = mode,
	};

	if (receiver == NULL || !mode_is_valid(mode) ||
	    !v92_su_rx_is_valid(&initialized))
		return V92_SU_RX_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_SU_RX_OK;
}

bool v92_su_rx_is_complete(const struct v92_su_rx *receiver)
{
	return v92_su_rx_is_valid(receiver) &&
	       receiver->stage == V92_SU_RX_COMPLETE;
}

bool v92_su_rx_has_failed(const struct v92_su_rx *receiver)
{
	return v92_su_rx_is_valid(receiver) &&
	       receiver->stage == V92_SU_RX_FAILED;
}

static enum su_comparison classify_sample(
	struct v92_su_rx *receiver, int16_t sample)
{
	uint16_t magnitude = (uint16_t)magnitude_s16(sample);
	bool valid = magnitude > receiver->minimum_magnitude;
	bool positive = sample >= 0;
	bool previous_valid = (receiver->history_valid & UINT8_C(0x20)) != 0u;
	bool previous_positive =
		(receiver->history_signs & UINT8_C(0x20)) != 0u;
	bool comparable = valid && previous_valid;
	bool same = comparable && positive == previous_positive;
	bool opposite = comparable && positive != previous_positive;
	unsigned valid_count;
	unsigned same_count;
	unsigned opposite_count;

	receiver->comparison_same = (uint8_t)(
		((receiver->comparison_same << 1u) | (uint8_t)same) & 0x3fu);
	receiver->comparison_opposite = (uint8_t)(
		((receiver->comparison_opposite << 1u) |
		 (uint8_t)opposite) & 0x3fu);
	receiver->comparison_valid = (uint8_t)(
		((receiver->comparison_valid << 1u) | (uint8_t)comparable) &
		 0x3fu);
	receiver->history_signs = (uint8_t)(
		((receiver->history_signs << 1u) | (uint8_t)positive) & 0x3fu);
	receiver->history_valid = (uint8_t)(
		((receiver->history_valid << 1u) | (uint8_t)valid) & 0x3fu);

	valid_count = bit_count6(receiver->comparison_valid);
	if (valid_count < 4u)
		return SU_COMPARISON_NONE;
	same_count = bit_count6(
		receiver->comparison_same & receiver->comparison_valid);
	if (same_count >= 4u)
		return SU_COMPARISON_PERIODIC;
	opposite_count = bit_count6(
		receiver->comparison_opposite & receiver->comparison_valid);
	/* Three reversed signs identify the transition halfway through a
	 * six-symbol window. Requiring a strict majority can miss the short
	 * Jp response when fractional reconstruction changes its zero crossings. */
	return opposite_count >= 3u && opposite_count >= same_count ?
		       SU_COMPARISON_INVERTED : SU_COMPARISON_NONE;
}

static void reset_first_phase(struct v92_su_rx *receiver)
{
	receiver->stage = V92_SU_RX_ACQUIRING;
	receiver->stable_symbols = 0u;
	receiver->peak = 0u;
	receiver->phase_samples[0] = 0u;
	receiver->phase_component[0] = 0;
}

static void accumulate_phase(
	struct v92_su_rx *receiver, unsigned segment, int16_t sample)
{
	int32_t component = (receiver->total_symbols & 1u) != 0u ?
				    -(int32_t)sample : (int32_t)sample;

	receiver->phase_component[segment] += component;
	receiver->phase_samples[segment]++;
}

static enum v92_su_rx_result acquire_sample(
	struct v92_su_rx *receiver, enum su_comparison comparison,
	int16_t sample)
{
	if (comparison != SU_COMPARISON_PERIODIC) {
		reset_first_phase(receiver);
		return V92_SU_RX_OK;
	}
	if (receiver->stable_symbols != UINT32_MAX)
		receiver->stable_symbols++;
	if (receiver->peak < magnitude_s16(sample))
		receiver->peak = (uint16_t)magnitude_s16(sample);
	if (receiver->stable_symbols < V92_SU_RX_MIN_STABLE_SYMBOLS)
		return V92_SU_RX_OK;
	receiver->last_periodic_symbol = receiver->total_symbols;
	if (receiver->mode == V92_SU_RX_TRANSITION_ONLY) {
		receiver->stage = V92_SU_RX_WAITING_TRANSITION;
		return V92_SU_RX_ACQUIRED;
	}
	receiver->stage = V92_SU_RX_FIRST_PHASE;
	accumulate_phase(receiver, 0u, sample);
	return V92_SU_RX_ACQUIRED;
}

static enum v92_su_rx_result first_phase_sample(
	struct v92_su_rx *receiver, enum su_comparison comparison,
	int16_t sample)
{
	if (comparison != SU_COMPARISON_PERIODIC) {
		reset_first_phase(receiver);
		return V92_SU_RX_OK;
	}
	receiver->last_periodic_symbol = receiver->total_symbols;
	accumulate_phase(receiver, 0u, sample);
	if (receiver->phase_samples[0] == V92_SU_RX_PHASE_SYMBOLS)
		receiver->stage = V92_SU_RX_WAITING_TRANSITION;
	return V92_SU_RX_OK;
}

static enum v92_su_rx_result wait_for_transition(
	struct v92_su_rx *receiver, enum su_comparison comparison)
{
	if (comparison == SU_COMPARISON_PERIODIC) {
		receiver->last_periodic_symbol = receiver->total_symbols;
		return V92_SU_RX_OK;
	}
	if (comparison == SU_COMPARISON_INVERTED &&
	    receiver->total_symbols - receiver->last_periodic_symbol <=
		    V92_SU_RX_PHASE_SETTLE_SYMBOLS) {
		if (receiver->mode == V92_SU_RX_TRANSITION_ONLY) {
			receiver->stage = V92_SU_RX_COMPLETE;
			return V92_SU_RX_TRANSITION;
		}
		receiver->stage = V92_SU_RX_SECOND_PHASE;
		receiver->transition_symbol = receiver->total_symbols;
		receiver->stable_symbols = 0u;
		return V92_SU_RX_TRANSITION;
	}
	if (receiver->total_symbols - receiver->last_periodic_symbol >
	    V92_SU_RX_PHASE_SETTLE_SYMBOLS)
		reset_first_phase(receiver);
	return V92_SU_RX_OK;
}

bool v92_su_phase_extension(
	int32_t first_component, int32_t second_component,
	uint16_t *extension_q16)
{
	int64_t first = first_component;
	int64_t second = second_component;
	double angle;

	if (extension_q16 == NULL ||
	    (first == 0 && second == 0))
		return false;
	if (first < 0 || (first == 0 && second > 0)) {
		first = -first;
		second = -second;
	}
	angle = atan2(-(double)second, (double)first);
	/* Canonical nonzero integer components bound the finite scaled phase
	 * to [0, 65536], safely within long. Q16 wraps at one complete symbol. */
	*extension_q16 = (uint16_t)lround(
		32768.0 - angle * (65536.0 / acos(-1.0)));
	return true;
}

static enum v92_su_rx_result second_phase_sample(
	struct v92_su_rx *receiver, enum su_comparison comparison,
	int16_t sample)
{
	uint64_t age = receiver->total_symbols - receiver->transition_symbol;
	uint32_t first_mean;
	uint32_t second_mean;
	uint32_t strength;

	if (comparison != SU_COMPARISON_PERIODIC) {
		receiver->stable_symbols = 0u;
		receiver->phase_samples[1] = 0u;
		receiver->phase_component[1] = 0;
		return V92_SU_RX_OK;
	}
	if (receiver->stable_symbols != UINT32_MAX)
		receiver->stable_symbols++;
	if (age < V92_SU_RX_PHASE_SETTLE_SYMBOLS ||
	    receiver->stable_symbols < V92_SU_RX_MIN_STABLE_SYMBOLS)
		return V92_SU_RX_OK;
	accumulate_phase(receiver, 1u, sample);
	if (receiver->phase_samples[1] != V92_SU_RX_PHASE_SYMBOLS)
		return V92_SU_RX_OK;
	first_mean = (uint32_t)llabs((long long)receiver->phase_component[0]) /
		     V92_SU_RX_PHASE_SYMBOLS;
	second_mean = (uint32_t)llabs((long long)receiver->phase_component[1]) /
		      V92_SU_RX_PHASE_SYMBOLS;
	strength = first_mean > second_mean ?
			 first_mean + second_mean / 2u :
			 second_mean + first_mean / 2u;
	if (strength < receiver->phase_minimum_magnitude ||
	    !v92_su_phase_extension(
		    receiver->phase_component[0], receiver->phase_component[1],
		    &receiver->phase_extension_q16)) {
		receiver->stage = V92_SU_RX_FAILED;
		return V92_SU_RX_FAILURE;
	}
	receiver->stage = V92_SU_RX_COMPLETE;
	return V92_SU_RX_PHASE_READY;
}

enum v92_su_rx_result v92_su_rx_sample(
	struct v92_su_rx *receiver, int16_t sample)
{
	struct v92_su_rx next;
	enum su_comparison comparison;
	enum v92_su_rx_result result = V92_SU_RX_INVALID_STATE;

	if (!v92_su_rx_is_valid(receiver))
		return V92_SU_RX_INVALID_ARGUMENT;
	if (receiver->stage == V92_SU_RX_FAILED)
		return V92_SU_RX_FAILURE;
	if (receiver->stage == V92_SU_RX_COMPLETE)
		return V92_SU_RX_DONE;
	if (receiver->total_symbols == UINT64_MAX)
		return V92_SU_RX_INVALID_STATE;
	next = *receiver;
	next.total_symbols++;
	comparison = classify_sample(&next, sample);
	switch (next.stage) {
	case V92_SU_RX_ACQUIRING:
		result = acquire_sample(&next, comparison, sample);
		break;
	case V92_SU_RX_FIRST_PHASE:
		result = first_phase_sample(&next, comparison, sample);
		break;
	case V92_SU_RX_WAITING_TRANSITION:
		result = wait_for_transition(&next, comparison);
		break;
	case V92_SU_RX_SECOND_PHASE:
		result = second_phase_sample(&next, comparison, sample);
		break;
	case V92_SU_RX_COMPLETE:
	case V92_SU_RX_FAILED:
		return V92_SU_RX_INVALID_STATE;
	}
	if (!v92_su_rx_is_valid(&next))
		return V92_SU_RX_INVALID_STATE;
	*receiver = next;
	return result;
}

bool v92_su_rx_phase_extension(
	const struct v92_su_rx *receiver, uint16_t *extension_q16)
{
	if (!v92_su_rx_is_complete(receiver) ||
	    receiver->mode != V92_SU_RX_MEASURE_PHASE ||
	    extension_q16 == NULL)
		return false;
	*extension_q16 = receiver->phase_extension_q16;
	return true;
}

const char *v92_su_rx_stage_name(enum v92_su_rx_stage stage)
{
	switch (stage) {
	case V92_SU_RX_ACQUIRING:
		return "acquiring Su";
	case V92_SU_RX_FIRST_PHASE:
		return "measuring first phase";
	case V92_SU_RX_WAITING_TRANSITION:
		return "waiting for inversion";
	case V92_SU_RX_SECOND_PHASE:
		return "measuring second phase";
	case V92_SU_RX_COMPLETE:
		return "complete";
	case V92_SU_RX_FAILED:
		return "failed";
	}
	return "unknown";
}

const char *v92_su_rx_result_name(enum v92_su_rx_result result)
{
	switch (result) {
	case V92_SU_RX_OK:
		return "ok";
	case V92_SU_RX_ACQUIRED:
		return "Su acquired";
	case V92_SU_RX_TRANSITION:
		return "inversion detected";
	case V92_SU_RX_PHASE_READY:
		return "phase ready";
	case V92_SU_RX_DONE:
		return "done";
	case V92_SU_RX_FAILURE:
		return "failed";
	case V92_SU_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_SU_RX_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
