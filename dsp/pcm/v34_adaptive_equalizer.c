// SPDX-License-Identifier: BSD-3-Clause

#include "v34_adaptive_equalizer.h"

#include "fixed_phase.h"
#include "fir.h"
#include "integer_math.h"
#include "lineage.h"
#include "memory_region.h"
#include "sample_math.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	V34_CARRIER_PHASE_SHIFT_A = 2,
	V34_CARRIER_PHASE_SHIFT_B = 8,
	/* Online decisions are tentative.  Preserve the trained frequency and
	 * give their bounded cross-product one fifth of the acquisition loop's
	 * proportional authority. */
	V34_DATA_CARRIER_PROPORTIONAL_DIVISOR = 20,
	V34_DATA_CARRIER_ERROR_LIMIT =
		V34_TRAINING_FIXED_AMPLITUDE * V34_TRAINING_FIXED_AMPLITUDE,
	/* The exact 32-step linear interpolation can overshoot unit Q14 by five
	 * at the axes. */
	V34_CARRIER_Q14_LIMIT = 0x4005,
};

_Static_assert(
	(V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS &
	 (V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS - 1u)) == 0,
	"V.34 quality window must remain a power of two");

_Static_assert(
	V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS %
		V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS == 0 &&
	V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS %
		V34_ADAPTIVE_EQUALIZER_QUALITY_BLOCK_SYMBOLS == 0,
	"Recent quality blocks must divide both measurement windows");

static int16_t shifted_s16(uint32_t value, unsigned int shift)
{
	return wrap_s16_from_s32(arithmetic_shift_right_u32(value, shift));
}

static bool feedback_mode_is_valid(unsigned int mode)
{
	return (mode & (unsigned int)~(
		V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN |
		V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY)) == 0;
}

static bool quality_is_valid(
	const struct v34_adaptive_equalizer_quality *quality)
{
	return quality->window_position <
		       V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS &&
	       quality->recent_symbol_count <=
		       V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS &&
	       quality->recent_decision_metric >= 0 &&
	       quality->decision_metric >= 0 && quality->feedback_metric >= 0;
}

static bool retained_is_clear(
	const struct v34_adaptive_equalizer_retained *retained)
{
	return !retained->pending &&
	       v34_adaptive_equalizer_observation_is_zero(
		       &retained->observation) &&
	       retained->demodulated.in_phase == 0 &&
	       retained->demodulated.quadrature == 0 &&
	       retained->rotate_in_phase == 0 &&
	       retained->rotate_quadrature == 0 &&
	       retained->equalizer_index == 0u;
}

static bool retained_is_valid(
	const struct v34_adaptive_equalizer *equalizer)
{
	const struct v34_adaptive_equalizer_retained *retained =
		&equalizer->retained;

	if (!retained->pending)
		return retained_is_clear(retained);
	if (equalizer->feedback_mode != V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE ||
	    !v34_adaptive_equalizer_observation_is_valid(
		    &retained->observation) ||
	    retained->observation.epoch == UINT64_MAX ||
	    retained->equalizer_index >= equalizer->equalizer.tap_count ||
	    retained->rotate_in_phase < -V34_CARRIER_Q14_LIMIT ||
	    retained->rotate_in_phase > V34_CARRIER_Q14_LIMIT ||
	    retained->rotate_quadrature < -V34_CARRIER_Q14_LIMIT ||
	    retained->rotate_quadrature > V34_CARRIER_Q14_LIMIT)
		return false;
	return equalizer->decision_pending ?
		       equalizer->epoch == retained->observation.epoch + 1u :
		       equalizer->epoch == retained->observation.epoch;
}

bool v34_adaptive_equalizer_is_valid(
	const struct v34_adaptive_equalizer *equalizer)
{
	if (equalizer == NULL || !equalizer->initialized ||
	    !v34_equalizer_is_valid(&equalizer->equalizer) ||
	    equalizer->rotate_in_phase < -V34_CARRIER_Q14_LIMIT ||
	    equalizer->rotate_in_phase > V34_CARRIER_Q14_LIMIT ||
	    equalizer->rotate_quadrature < -V34_CARRIER_Q14_LIMIT ||
	    equalizer->rotate_quadrature > V34_CARRIER_Q14_LIMIT ||
	    equalizer->carrier_phase_fraction > UINT16_C(0x1fff) ||
	    equalizer->inputs_since_observation >
		    V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION ||
	    !feedback_mode_is_valid(equalizer->feedback_mode) ||
	    !quality_is_valid(&equalizer->quality) ||
	    equalizer->lineage == 0 || !retained_is_valid(equalizer))
		return false;
	if (equalizer->decision_pending)
		return equalizer->inputs_since_observation == 0u &&
		       equalizer->epoch != 0 &&
		       equalizer->pending_observation.epoch == equalizer->epoch &&
		       equalizer->pending_observation.lineage == equalizer->lineage &&
		       equalizer->pending_rotate_in_phase >=
			       -V34_CARRIER_Q14_LIMIT &&
		       equalizer->pending_rotate_in_phase <=
			       V34_CARRIER_Q14_LIMIT &&
		       equalizer->pending_rotate_quadrature >=
			       -V34_CARRIER_Q14_LIMIT &&
		       equalizer->pending_rotate_quadrature <=
			       V34_CARRIER_Q14_LIMIT;
	return v34_adaptive_equalizer_observation_is_zero(
		       &equalizer->pending_observation) &&
	       equalizer->pending_demodulated.in_phase == 0 &&
	       equalizer->pending_demodulated.quadrature == 0 &&
	       equalizer->pending_rotate_in_phase == 0 &&
	       equalizer->pending_rotate_quadrature == 0;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_init(
	struct v34_adaptive_equalizer *equalizer)
{
	struct v34_adaptive_equalizer initialized;

	if (equalizer == NULL)
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (v34_equalizer_init(&initialized.equalizer) != V34_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	initialized.rotate_in_phase = 0x4000;
	initialized.lineage = LINEAGE_INITIAL;
	initialized.initialized = true;
	if (!v34_adaptive_equalizer_is_valid(&initialized))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = initialized;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_set_feedback_mode(
	struct v34_adaptive_equalizer *equalizer,
	enum v34_adaptive_equalizer_feedback_mode mode)
{
	struct v34_adaptive_equalizer next;

	if (!v34_adaptive_equalizer_is_valid(equalizer))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if ((int)mode < 0 || !feedback_mode_is_valid((unsigned int)mode))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (equalizer->decision_pending || equalizer->retained.pending ||
	    equalizer->inputs_since_observation != 0u)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (equalizer->feedback_mode == (uint8_t)mode)
		return V34_ADAPTIVE_EQUALIZER_OK;
	next = *equalizer;
	next.feedback_mode = (uint8_t)mode;
	next.lineage = lineage_word(
		lineage_tag(next.lineage, 1), (uint16_t)mode);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_set_data_carrier_tracking(
	struct v34_adaptive_equalizer *equalizer, bool data_tracking)
{
	struct v34_adaptive_equalizer next;

	if (!v34_adaptive_equalizer_is_valid(equalizer))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (equalizer->decision_pending || equalizer->retained.pending ||
	    equalizer->inputs_since_observation != 0u)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (equalizer->data_carrier_tracking == data_tracking)
		return V34_ADAPTIVE_EQUALIZER_OK;
	next = *equalizer;
	next.data_carrier_tracking = data_tracking;
	next.lineage = lineage_word(
		lineage_tag(next.lineage, 7), data_tracking ? 1u : 0u);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_finish_feedback_training(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	struct v34_adaptive_equalizer next;
	struct v34_q14_coefficient frozen[V34_PRECODER_TAP_COUNT];

	if (coefficients == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    coefficients, sizeof(frozen)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    equalizer->decision_pending || equalizer->retained.pending ||
	    equalizer->feedback_mode != V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		size_t feedback_tap = V34_PRECODER_TAP_COUNT - tap - 1u;

		frozen[tap] = (struct v34_q14_coefficient){
			.real = equalizer->feedback
				.coefficient_a[feedback_tap],
			.imaginary = equalizer->feedback
				.coefficient_b[feedback_tap],
		};
	}
	next = *equalizer;
	memset(next.feedback.in_phase_history, 0,
	       sizeof(next.feedback.in_phase_history));
	memset(next.feedback.quadrature_history, 0,
	       sizeof(next.feedback.quadrature_history));
	next.feedback_mode = V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE;
	next.lineage = lineage_tag(next.lineage, 6);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	memcpy(coefficients, frozen, sizeof(frozen));
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_begin_tracking(
	struct v34_adaptive_equalizer *equalizer)
{
	struct v34_adaptive_equalizer next;

	if (!v34_adaptive_equalizer_is_valid(equalizer))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (equalizer->decision_pending || equalizer->retained.pending ||
	    equalizer->inputs_since_observation != 0u)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	if (v34_equalizer_clear_center(&next.equalizer) != V34_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	memset(next.feedback.coefficient_a, 0,
	       sizeof(next.feedback.coefficient_a));
	memset(next.feedback.coefficient_b, 0,
	       sizeof(next.feedback.coefficient_b));
	memset(&next.quality, 0, sizeof(next.quality));
	next.lineage = lineage_tag(next.lineage, 5);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_push(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_analytic_sample sample)
{
	uint64_t next_lineage;

	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    equalizer->decision_pending ||
	    equalizer->inputs_since_observation ==
		    V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	next_lineage = lineage_word(
		lineage_word(lineage_tag(equalizer->lineage, 2),
			     (uint16_t)sample.in_phase),
		(uint16_t)sample.quadrature);
	if (next_lineage == 0)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (v34_equalizer_push(&equalizer->equalizer, sample) !=
	    V34_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	equalizer->lineage = next_lineage;
	equalizer->inputs_since_observation++;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

static int16_t feedback_in_phase(
	const struct v34_adaptive_equalizer_feedback *feedback)
{
	/* Feedback coefficients walk the three retained inputs oldest first.
	 * Keep the legacy negative rounding bias in the real projection. */
	uint32_t accumulator = fir_read_ring_s16(feedback->in_phase_history,
		feedback->coefficient_a, 3, 2, 1) - UINT32_C(0x2000) -
		fir_read_ring_s16(feedback->quadrature_history,
			feedback->coefficient_b, 3, 2, 1);

	return shifted_s16(accumulator, 14);
}

static int16_t feedback_shift_and_quadrature(
	struct v34_adaptive_equalizer_feedback *feedback)
{
	uint32_t accumulator = UINT32_C(0x2000) +
		fir_read_ring_s16(feedback->in_phase_history,
			feedback->coefficient_b, 3, 2, 1) +
		fir_read_ring_s16(feedback->quadrature_history,
			feedback->coefficient_a, 3, 2, 1);

	/* Each filter evaluation advances the four-word history: once for the
	 * received decision and once again when the resulting error is learned. */
	memmove(feedback->in_phase_history + 1, feedback->in_phase_history,
		3 * sizeof(feedback->in_phase_history[0]));
	memmove(feedback->quadrature_history + 1, feedback->quadrature_history,
		3 * sizeof(feedback->quadrature_history[0]));
	return shifted_s16(accumulator, 14);
}

static struct v34_analytic_sample apply_feedback(
	struct v34_adaptive_equalizer_feedback *feedback,
	struct v34_analytic_sample input)
{
	int16_t predicted_in_phase = feedback_in_phase(feedback);
	int16_t predicted_quadrature =
		feedback_shift_and_quadrature(feedback);
	struct v34_analytic_sample output = {
		.in_phase = wrap_s16_from_s32(
			(int32_t)input.in_phase + predicted_in_phase),
		.quadrature = wrap_s16_from_s32(
			(int32_t)input.quadrature + predicted_quadrature),
	};

	feedback->in_phase_history[0] = input.in_phase;
	feedback->quadrature_history[0] = input.quadrature;
	return output;
}

static struct v34_analytic_sample rotate_q14(
	struct v34_analytic_sample input, int16_t cosine, int16_t sine)
{
	uint32_t in_phase = 0, quadrature = 0;

	fir_accumulate_complex_s16(input.in_phase, input.quadrature,
		cosine, sine, &in_phase, &quadrature);

	return (struct v34_analytic_sample){
		.in_phase = round_shift_s32_word_to_s16(in_phase, 14),
		.quadrature = round_shift_s32_word_to_s16(quadrature, 14),
	};
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_observe(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_equalizer_accumulator accumulator;
	struct v34_analytic_sample demodulated;
	struct v34_adaptive_equalizer_observation generated;

	if (observation == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    observation, sizeof(*observation)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    equalizer->decision_pending ||
	    equalizer->inputs_since_observation !=
		    V34_ADAPTIVE_EQUALIZER_INPUTS_PER_OBSERVATION ||
	    equalizer->epoch == UINT64_MAX)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (v34_equalizer_filter(&equalizer->equalizer, &accumulator) !=
	    V34_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	demodulated = (struct v34_analytic_sample){
		.in_phase = round_shift_s32_word_to_s16(
			(uint32_t)accumulator.in_phase, 14),
		.quadrature = round_shift_s32_word_to_s16(
			(uint32_t)accumulator.quadrature, 14),
	};
	if ((equalizer->feedback_mode & V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) != 0)
		demodulated = apply_feedback(&equalizer->feedback, demodulated);
	equalizer->epoch++;
	generated = (struct v34_adaptive_equalizer_observation){
		.decision = rotate_q14(
			demodulated, equalizer->rotate_in_phase,
			wrap_s16_from_s32(
				-(int32_t)equalizer->rotate_quadrature)),
		.epoch = equalizer->epoch,
		.lineage = equalizer->lineage,
	};
	equalizer->pending_observation = generated;
	equalizer->pending_demodulated = demodulated;
	equalizer->pending_rotate_in_phase = equalizer->rotate_in_phase;
	equalizer->pending_rotate_quadrature = equalizer->rotate_quadrature;
	equalizer->inputs_since_observation = 0;
	equalizer->decision_pending = true;
	*observation = generated;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

static struct v34_analytic_sample update_feedback_filter(
	struct v34_adaptive_equalizer_feedback *feedback,
	struct v34_analytic_sample decision_error)
{
	int16_t predicted_in_phase = feedback_in_phase(feedback);
	int16_t predicted_quadrature =
		feedback_shift_and_quadrature(feedback);
	int16_t error_in_phase = wrap_s16_from_s32(
		(int32_t)decision_error.in_phase + predicted_in_phase);
	int16_t error_quadrature = wrap_s16_from_s32(
		(int32_t)decision_error.quadrature + predicted_quadrature);

	feedback->in_phase_history[0] = decision_error.in_phase;
	feedback->quadrature_history[0] = decision_error.quadrature;
	for (size_t index = 0; index < 3; ++index) {
		int16_t a = feedback->coefficient_a[index];
		int16_t b = feedback->coefficient_b[index];
		int16_t d = feedback->in_phase_history[3u - index];
		int16_t f = feedback->quadrature_history[3u - index];
		uint32_t next_a = (uint32_t)(uint16_t)a << 16;
		uint32_t next_b = (uint32_t)(uint16_t)b << 16;

		fir_subtract_correlation_s16(d, f, error_in_phase,
			error_quadrature, &next_a, &next_b);

		feedback->coefficient_a[index] =
			round_shift_s32_word_to_s16(next_a, 16);
		feedback->coefficient_b[index] =
			round_shift_s32_word_to_s16(next_b, 16);
	}
	return (struct v34_analytic_sample){
		.in_phase = error_in_phase,
		.quadrature = error_quadrature,
	};
}

static int16_t published_metric(uint32_t accumulator)
{
	return (accumulator & UINT32_C(0x80000000)) != 0 ?
		       INT16_MAX : (int16_t)(uint16_t)(accumulator >> 16);
}

static void update_recent_quality(
	struct v34_adaptive_equalizer_quality *quality, uint32_t error)
{
	unsigned int position = quality->window_position;
	unsigned int block = position /
		V34_ADAPTIVE_EQUALIZER_QUALITY_BLOCK_SYMBOLS %
		V34_ADAPTIVE_EQUALIZER_RECENT_BLOCKS;
	uint64_t accumulated;

	if (position % V34_ADAPTIVE_EQUALIZER_QUALITY_BLOCK_SYMBOLS == 0u)
		quality->recent_error_blocks[block] = 0u;
	accumulated = (uint64_t)quality->recent_error_blocks[block] + error;
	quality->recent_error_blocks[block] = accumulated > UINT32_MAX ?
		UINT32_MAX : (uint32_t)accumulated;
	if (quality->recent_symbol_count < V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS)
		quality->recent_symbol_count++;
	if (quality->recent_symbol_count == V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS &&
	    (position + 1u) % V34_ADAPTIVE_EQUALIZER_QUALITY_BLOCK_SYMBOLS == 0u) {
		uint64_t metric = 0u;

		for (unsigned int index = 0u;
		     index < V34_ADAPTIVE_EQUALIZER_RECENT_BLOCKS; ++index)
			metric += quality->recent_error_blocks[index];
		metric = metric * V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS /
			V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS >> 16;
		quality->recent_decision_metric = metric > INT16_MAX ?
			INT16_MAX : (int16_t)metric;
	}
}

static void update_quality(
	struct v34_adaptive_equalizer_quality *quality,
	struct v34_analytic_sample demodulated,
	struct v34_analytic_sample rotated_target)
{
	uint32_t error = sample_pair_wrapped_distance_s16(
		demodulated.in_phase, demodulated.quadrature,
		rotated_target.in_phase, rotated_target.quadrature);
	uint32_t metric = quality->decision_accumulator + error;
	uint32_t reference = sample_pair_power_s16(
		rotated_target.in_phase, rotated_target.quadrature);
	uint16_t position = (uint16_t)(
		(quality->window_position + 1u) &
		(V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS - 1u));

	update_recent_quality(quality, error);
	quality->reference_accumulator += reference >> 8;
	quality->window_position = position;
	if (position != 0u) {
		quality->decision_accumulator = metric;
		return;
	}
	quality->decision_metric = published_metric(metric);
	quality->feedback_metric = published_metric(
		quality->feedback_accumulator);
	quality->reference_metric = quality->reference_accumulator >> 8;
	quality->decision_accumulator = 0;
	quality->feedback_accumulator = 0;
	quality->reference_accumulator = 0;
}

static int32_t phase_error(
	struct v34_analytic_sample target,
	struct v34_analytic_sample observation)
{
	uint32_t error = multiply_low_s32(target.in_phase, observation.quadrature) -
		multiply_low_s32(target.quadrature, observation.in_phase);

	return wrap_s32_from_u32(error << 2u);
}

static void update_carrier(
	struct v34_adaptive_equalizer *equalizer, int32_t error)
{
	int32_t proportional;
	uint32_t accumulator_word = (uint32_t)equalizer->phase_accumulator;

	if (equalizer->data_carrier_tracking) {
		if (error > V34_DATA_CARRIER_ERROR_LIMIT)
			error = V34_DATA_CARRIER_ERROR_LIMIT;
		else if (error < -V34_DATA_CARRIER_ERROR_LIMIT)
			error = -V34_DATA_CARRIER_ERROR_LIMIT;
		proportional = error / V34_DATA_CARRIER_PROPORTIONAL_DIVISOR;
	} else {
		accumulator_word +=
			(uint32_t)arithmetic_shift_right_u32(
				(uint32_t)error, V34_CARRIER_PHASE_SHIFT_B);
		proportional = wrap_s32_from_u32(arithmetic_shift_right_u32(
			(uint32_t)error, V34_CARRIER_PHASE_SHIFT_A));
	}
	int32_t accumulator = wrap_s32_from_u32(accumulator_word);
	/* These are the high and low words of one phase, not a signed residue.
	 * Sign extension would lose a coarse step whenever bit 15 is set. */
	uint32_t phase_word =
		(uint32_t)proportional +
		(uint32_t)arithmetic_shift_right_u32(
			(uint32_t)accumulator, 5) +
		((uint32_t)equalizer->carrier_phase_fraction << 16) +
		(uint32_t)equalizer->carrier_phase_low;
	int32_t phase = wrap_s32_from_u32(phase_word);
	uint16_t coarse = (uint16_t)(
		(uint32_t)arithmetic_shift_right_u32(
			(uint32_t)phase, 16) & UINT32_C(0x1fff));
	uint8_t index = (uint8_t)(coarse >> 5);
	int32_t fraction = (int32_t)(coarse & UINT16_C(0x001f)) * 0x32;
	int16_t cosine = fixed_phase_cosine_u8_q14(index);
	int16_t sine = fixed_phase_cosine_u8_q14((uint8_t)(index + 64u));
	int16_t negative_sine = wrap_s16_from_s32(-(int32_t)sine);
	uint32_t interpolated_quadrature =
		((uint32_t)(int32_t)negative_sine << 16) +
		(uint32_t)(fraction * cosine);
	uint32_t interpolated_in_phase =
		((uint32_t)(int32_t)cosine << 16) -
		(uint32_t)(fraction * negative_sine);

	equalizer->phase_accumulator = accumulator;
	equalizer->carrier_phase_low = (uint16_t)phase;
	equalizer->carrier_phase_fraction = coarse;
	equalizer->rotate_in_phase = round_shift_s32_word_to_s16(
		interpolated_in_phase, 16);
	equalizer->rotate_quadrature = round_shift_s32_word_to_s16(
		interpolated_quadrature, 16);
}

static bool adaptation_is_valid(
	enum v34_adaptive_equalizer_adaptation adaptation)
{
	return adaptation == V34_ADAPTIVE_EQUALIZER_SKIP ||
	       adaptation == V34_ADAPTIVE_EQUALIZER_CENTER ||
	       adaptation == V34_ADAPTIVE_EQUALIZER_FULL ||
	       adaptation == V34_ADAPTIVE_EQUALIZER_CARRIER ||
	       adaptation == V34_ADAPTIVE_EQUALIZER_CENTER_TAPS;
}

static bool decision_is_valid(
	const struct v34_adaptive_equalizer_decision *decision)
{
	if (decision == NULL || !adaptation_is_valid(decision->adaptation))
		return false;
	return decision->adaptation == V34_ADAPTIVE_EQUALIZER_SKIP ?
		       decision->decoder_power_q15 == 0 :
		       decision->decoder_power_q15 != 0 &&
		       decision->decoder_power_q15 <= INT16_MAX;
}

static void clear_pending(struct v34_adaptive_equalizer *equalizer)
{
	equalizer->pending_observation =
		(struct v34_adaptive_equalizer_observation){ 0 };
	equalizer->pending_demodulated = (struct v34_analytic_sample){ 0 };
	equalizer->pending_rotate_in_phase = 0;
	equalizer->pending_rotate_quadrature = 0;
	equalizer->decision_pending = false;
}

static void clear_retained(struct v34_adaptive_equalizer *equalizer)
{
	memset(&equalizer->retained, 0, sizeof(equalizer->retained));
}

enum v34_adaptive_equalizer_result
v34_adaptive_equalizer_retain_observation(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_adaptive_equalizer_retained *retained;

	if (observation == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    observation, sizeof(*observation)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    !equalizer->decision_pending || equalizer->retained.pending ||
	    equalizer->feedback_mode != V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (!v34_adaptive_equalizer_observations_are_equal(
		    observation, &equalizer->pending_observation))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	retained = &equalizer->retained;
	*retained = (struct v34_adaptive_equalizer_retained){
		.observation = equalizer->pending_observation,
		.demodulated = equalizer->pending_demodulated,
		.rotate_in_phase = equalizer->pending_rotate_in_phase,
		.rotate_quadrature = equalizer->pending_rotate_quadrature,
		.equalizer_index = equalizer->equalizer.index,
		.pending = true,
	};
	memcpy(retained->in_phase_delay,
	       equalizer->equalizer.in_phase_delay,
	       sizeof(retained->in_phase_delay));
	memcpy(retained->quadrature_delay,
	       equalizer->equalizer.quadrature_delay,
	       sizeof(retained->quadrature_delay));
	clear_pending(equalizer);
	return V34_ADAPTIVE_EQUALIZER_OK;
}

static enum v34_adaptive_equalizer_result adapt_retained(
	struct v34_equalizer *equalizer,
	const struct v34_adaptive_equalizer_retained *retained,
	struct v34_analytic_sample error, bool center_only)
{
	return v34_equalizer_adapt_history(equalizer,
		retained->in_phase_delay, retained->quadrature_delay,
		retained->equalizer_index, error, center_only) == V34_EQUALIZER_OK ?
		V34_ADAPTIVE_EQUALIZER_OK : V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
}

static enum v34_adaptive_equalizer_result apply_decision(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision *decision,
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_analytic_sample demodulated, int16_t rotate_in_phase,
	int16_t rotate_quadrature,
	const struct v34_adaptive_equalizer_retained *retained)
{
	struct v34_analytic_sample rotated_target;
	struct v34_analytic_sample error;

	equalizer->phase_error = phase_error(
		decision->target, observation->decision);
	if ((equalizer->feedback_mode &
	     V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) != 0) {
		struct v34_analytic_sample decision_error = {
			.in_phase = wrap_s16_from_s32(
				(int32_t)observation->decision.in_phase -
				decision->target.in_phase),
			.quadrature = wrap_s16_from_s32(
				(int32_t)observation->decision.quadrature -
				decision->target.quadrature),
		};

		decision_error = update_feedback_filter(
			&equalizer->feedback, decision_error);
		equalizer->quality.feedback_accumulator += sample_pair_power_s16(
			decision_error.in_phase, decision_error.quadrature);
	}
	rotated_target = rotate_q14(
		decision->target, rotate_in_phase, rotate_quadrature);
	if (decision->adaptation != V34_ADAPTIVE_EQUALIZER_SKIP) {
		if (decision->adaptation !=
		    V34_ADAPTIVE_EQUALIZER_CENTER_TAPS)
			update_carrier(equalizer, equalizer->phase_error);
		error = (struct v34_analytic_sample){
			.in_phase = shifted_s16(
				multiply_low_s32(
					wrap_s16_from_s32(
						(int32_t)demodulated.in_phase -
						rotated_target.in_phase),
					(int16_t)decision->decoder_power_q15),
				16),
			.quadrature = shifted_s16(
				multiply_low_s32(
					wrap_s16_from_s32(
						(int32_t)demodulated.quadrature -
						rotated_target.quadrature),
					(int16_t)decision->decoder_power_q15),
				16),
		};
		update_quality(&equalizer->quality, demodulated, rotated_target);
		if (decision->adaptation == V34_ADAPTIVE_EQUALIZER_FULL) {
			if (retained != NULL) {
				if (adapt_retained(
					    &equalizer->equalizer, retained, error,
					    false) != V34_ADAPTIVE_EQUALIZER_OK)
					return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
			} else if (v34_equalizer_adapt(
					   &equalizer->equalizer, error) !=
				   V34_EQUALIZER_OK) {
				return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
			}
		} else if (decision->adaptation ==
				   V34_ADAPTIVE_EQUALIZER_CENTER ||
			   decision->adaptation ==
				   V34_ADAPTIVE_EQUALIZER_CENTER_TAPS) {
			error.in_phase = wrap_s16_from_s32(
				(int32_t)error.in_phase * 2);
			error.quadrature = wrap_s16_from_s32(
				(int32_t)error.quadrature * 2);
			if (retained != NULL) {
				if (adapt_retained(
					    &equalizer->equalizer, retained, error,
					    true) != V34_ADAPTIVE_EQUALIZER_OK)
					return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
			} else if (v34_equalizer_adapt_center(
					   &equalizer->equalizer, error) !=
				   V34_EQUALIZER_OK) {
				return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
			}
		}
	}
	equalizer->lineage = lineage_word(
		lineage_word(lineage_word(
			lineage_word(
				lineage_tag(equalizer->lineage, 3),
				(uint16_t)decision->target.in_phase),
			(uint16_t)decision->target.quadrature),
		(uint16_t)decision->decoder_power_q15),
		(uint16_t)decision->adaptation);
	return equalizer->lineage == 0u ?
		       V34_ADAPTIVE_EQUALIZER_INVALID_STATE :
		       V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_commit(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision *decision)
{
	struct v34_adaptive_equalizer next;

	if (decision == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    decision, sizeof(*decision)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    !equalizer->decision_pending || equalizer->retained.pending)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (!decision_is_valid(decision) ||
	    !v34_adaptive_equalizer_observations_are_equal(
		    &decision->observation,
		    &equalizer->pending_observation))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	if (apply_decision(
		    &next, decision, &next.pending_observation,
		    next.pending_demodulated, next.pending_rotate_in_phase,
		    next.pending_rotate_quadrature, NULL) !=
	    V34_ADAPTIVE_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	clear_pending(&next);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_commit_pair(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision decisions[2])
{
	struct v34_adaptive_equalizer next;

	if (decisions == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    decisions, sizeof(*decisions) * 2u))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    !equalizer->retained.pending || !equalizer->decision_pending)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (!decision_is_valid(&decisions[0]) ||
	    !decision_is_valid(&decisions[1]) ||
	    !v34_adaptive_equalizer_observations_are_equal(
		    &decisions[0].observation,
		    &equalizer->retained.observation) ||
	    !v34_adaptive_equalizer_observations_are_equal(
		    &decisions[1].observation,
		    &equalizer->pending_observation))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	if (apply_decision(
		    &next, &decisions[0], &next.retained.observation,
		    next.retained.demodulated, next.retained.rotate_in_phase,
		    next.retained.rotate_quadrature, &next.retained) !=
		    V34_ADAPTIVE_EQUALIZER_OK ||
	    apply_decision(
		    &next, &decisions[1], &next.pending_observation,
		    next.pending_demodulated, next.pending_rotate_in_phase,
		    next.pending_rotate_quadrature, NULL) !=
		    V34_ADAPTIVE_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	clear_retained(&next);
	clear_pending(&next);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_discard(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation)
{
	uint64_t next_lineage;

	if (observation == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    observation, sizeof(*observation)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    !equalizer->decision_pending || equalizer->retained.pending ||
	    (equalizer->feedback_mode &
	     V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) != 0)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (!v34_adaptive_equalizer_observations_are_equal(
		    observation, &equalizer->pending_observation))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	next_lineage = lineage_tag(equalizer->lineage, 4);
	if (next_lineage == 0)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	equalizer->lineage = next_lineage;
	clear_pending(equalizer);
	return V34_ADAPTIVE_EQUALIZER_OK;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_coast(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_observation *observation)
{
	enum v34_adaptive_equalizer_result result =
		v34_adaptive_equalizer_discard(equalizer, observation);

	if (result == V34_ADAPTIVE_EQUALIZER_OK)
		update_carrier(equalizer, 0);
	return result;
}
