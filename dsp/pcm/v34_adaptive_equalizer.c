// SPDX-License-Identifier: BSD-3-Clause

#include "v34_adaptive_equalizer.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	V34_CARRIER_PHASE_SHIFT_A = 2,
	V34_CARRIER_PHASE_SHIFT_B = 8,
	/* The exact 32-step linear interpolation can overshoot unit Q14 by five
	 * at the axes. */
	V34_CARRIER_Q14_LIMIT = 0x4005,
};

_Static_assert(
	(V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS &
	 (V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS - 1u)) == 0,
	"V.34 quality window must remain a power of two");

/* One bounded quarter cycle reconstructs the exact 256-step Q14 receive
 * carrier profile.  Its negative real-axis endpoint is deliberately -16383. */
static const int16_t CARRIER_COSINE_QUARTER_Q14[65] = {
	16384, 16379, 16364, 16339, 16305, 16260, 16206, 16142,
	16069, 15985, 15892, 15790, 15678, 15557, 15426, 15286,
	15136, 14978, 14810, 14634, 14449, 14255, 14053, 13842,
	13622, 13395, 13159, 12916, 12665, 12406, 12139, 11866,
	11585, 11297, 11002, 10701, 10393, 10079, 9759, 9434,
	9102, 8765, 8423, 8075, 7723, 7366, 7005, 6639,
	6269, 5896, 5519, 5139, 4756, 4369, 3980, 3589,
	3196, 2801, 2404, 2005, 1605, 1205, 803, 402,
	0,
};

static int32_t sar32(uint32_t value, unsigned int shift)
{
	uint32_t shifted;

	if (shift == 0)
		shifted = value;
	else if ((value & UINT32_C(0x80000000)) == 0)
		shifted = value >> shift;
	else
		shifted = (value >> shift) | ~(UINT32_MAX >> shift);
	return shifted <= INT32_MAX ?
		       (int32_t)shifted :
		       (int32_t)((int64_t)shifted - (INT64_C(1) << 32));
}

static int32_t wrap_s32(uint32_t value)
{
	return value <= INT32_MAX ?
		       (int32_t)value :
		       (int32_t)((int64_t)value - (INT64_C(1) << 32));
}

static int16_t wrap_s16(int32_t value)
{
	uint16_t word = (uint16_t)(uint32_t)value;

	return word <= INT16_MAX ?
		       (int16_t)word : (int16_t)((int32_t)word - 65536);
}

static uint32_t product_low(int16_t left, int16_t right)
{
	return (uint32_t)((int32_t)left * right);
}

static int16_t shifted_s16(uint32_t value, unsigned int shift)
{
	return wrap_s16(sar32(value, shift));
}

static int16_t carrier_cosine_q14(uint8_t index)
{
	if (index <= 64u)
		return CARRIER_COSINE_QUARTER_Q14[index];
	if (index < 128u)
		return (int16_t)-CARRIER_COSINE_QUARTER_Q14[128u - index];
	if (index == 128u)
		return -16383;
	if (index <= 192u)
		return (int16_t)-CARRIER_COSINE_QUARTER_Q14[index - 128u];
	return CARRIER_COSINE_QUARTER_Q14[256u - index];
}

static uint64_t lineage_word(uint64_t lineage, uint16_t word)
{
	return (lineage ^ word) * UINT64_C(1099511628211);
}

static uint64_t lineage_tag(uint64_t lineage, uint16_t tag)
{
	return lineage_word(lineage_word(lineage, UINT16_C(0xa55a)), tag);
}

static bool observation_is_zero(
	const struct v34_adaptive_equalizer_observation *observation)
{
	return observation->decision.in_phase == 0 &&
	       observation->decision.quadrature == 0 &&
	       observation->epoch == 0 && observation->lineage == 0;
}

static bool observations_are_equal(
	const struct v34_adaptive_equalizer_observation *left,
	const struct v34_adaptive_equalizer_observation *right)
{
	return left->decision.in_phase == right->decision.in_phase &&
	       left->decision.quadrature == right->decision.quadrature &&
	       left->epoch == right->epoch && left->lineage == right->lineage;
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
	       quality->decision_metric >= 0 && quality->feedback_metric >= 0;
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
	    equalizer->inputs_since_observation > 2u ||
	    !feedback_mode_is_valid(equalizer->feedback_mode) ||
	    !quality_is_valid(&equalizer->quality) ||
	    equalizer->lineage == 0)
		return false;
	if (equalizer->decision_pending)
		return equalizer->inputs_since_observation == 0u &&
		       equalizer->epoch != 0 &&
		       equalizer->pending_observation.epoch == equalizer->epoch &&
		       equalizer->pending_observation.lineage == equalizer->lineage;
	return observation_is_zero(&equalizer->pending_observation) &&
	       equalizer->pending_demodulated.in_phase == 0 &&
	       equalizer->pending_demodulated.quadrature == 0;
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
	initialized.lineage = UINT64_C(14695981039346656037);
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
	if (equalizer->decision_pending ||
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
v34_adaptive_equalizer_begin_tracking(
	struct v34_adaptive_equalizer *equalizer)
{
	struct v34_adaptive_equalizer next;

	if (!v34_adaptive_equalizer_is_valid(equalizer))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (equalizer->decision_pending ||
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
	    equalizer->inputs_since_observation == 2u)
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
	uint32_t accumulator = UINT32_C(0x2000);

	for (size_t index = 0; index < 3; ++index)
		accumulator += product_low(
			feedback->coefficient_b[index],
			feedback->quadrature_history[2u - index]);
	accumulator = 0u - accumulator;
	for (size_t index = 0; index < 3; ++index)
		accumulator += product_low(
			feedback->in_phase_history[2u - index],
			feedback->coefficient_a[index]);
	return shifted_s16(accumulator, 14);
}

static int16_t feedback_shift_and_quadrature(
	struct v34_adaptive_equalizer_feedback *feedback)
{
	uint32_t accumulator = UINT32_C(0x2000);

	/* Each filter evaluation advances the four-word history: once for the
	 * received decision and once again when the resulting error is learned. */
	for (size_t index = 0; index < 3; ++index) {
		int16_t value = feedback->in_phase_history[2u - index];

		feedback->in_phase_history[3u - index] = value;
		accumulator += product_low(
			value, feedback->coefficient_b[index]);
	}
	for (size_t index = 0; index < 3; ++index) {
		int16_t value = feedback->quadrature_history[2u - index];

		feedback->quadrature_history[3u - index] = value;
		accumulator += product_low(
			value, feedback->coefficient_a[index]);
	}
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
		.in_phase = wrap_s16(
			(int32_t)input.in_phase + predicted_in_phase),
		.quadrature = wrap_s16(
			(int32_t)input.quadrature + predicted_quadrature),
	};

	feedback->in_phase_history[0] = input.in_phase;
	feedback->quadrature_history[0] = input.quadrature;
	return output;
}

static struct v34_analytic_sample rotate_q14(
	struct v34_analytic_sample input, int16_t cosine, int16_t sine)
{
	uint32_t in_phase = product_low(cosine, input.in_phase) -
		product_low(sine, input.quadrature) + UINT32_C(0x2000);
	uint32_t quadrature = product_low(sine, input.in_phase) +
		product_low(cosine, input.quadrature) + UINT32_C(0x2000);

	return (struct v34_analytic_sample){
		.in_phase = shifted_s16(in_phase, 14),
		.quadrature = shifted_s16(quadrature, 14),
	};
}

static struct v34_analytic_sample rounded_output(
	struct v34_equalizer_accumulator accumulator)
{
	return (struct v34_analytic_sample){
		.in_phase = shifted_s16(
			(uint32_t)accumulator.in_phase + UINT32_C(0x2000), 14),
		.quadrature = shifted_s16(
			(uint32_t)accumulator.quadrature + UINT32_C(0x2000),
			14),
	};
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_observe(
	struct v34_adaptive_equalizer *equalizer,
	struct v34_adaptive_equalizer_observation *observation)
{
	struct v34_adaptive_equalizer next;
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
	    equalizer->inputs_since_observation != 2u ||
	    equalizer->epoch == UINT64_MAX)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	if (v34_equalizer_filter(&next.equalizer, &accumulator) !=
	    V34_EQUALIZER_OK)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	demodulated = rounded_output(accumulator);
	if ((next.feedback_mode & V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) != 0)
		demodulated = apply_feedback(&next.feedback, demodulated);
	next.epoch++;
	generated = (struct v34_adaptive_equalizer_observation){
		.decision = rotate_q14(
			demodulated, next.rotate_in_phase,
			wrap_s16(-(int32_t)next.rotate_quadrature)),
		.epoch = next.epoch,
		.lineage = next.lineage,
	};
	next.pending_observation = generated;
	next.pending_demodulated = demodulated;
	next.inputs_since_observation = 0;
	next.decision_pending = true;
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
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
	int16_t error_in_phase = wrap_s16(
		(int32_t)decision_error.in_phase + predicted_in_phase);
	int16_t error_quadrature = wrap_s16(
		(int32_t)decision_error.quadrature + predicted_quadrature);

	feedback->in_phase_history[0] = error_in_phase;
	feedback->quadrature_history[0] = error_quadrature;
	for (size_t index = 0; index < 3; ++index) {
		int16_t a = feedback->coefficient_a[index];
		int16_t b = feedback->coefficient_b[index];
		int16_t d = feedback->in_phase_history[3u - index];
		int16_t f = feedback->quadrature_history[3u - index];
		uint32_t next_a =
			((uint32_t)(uint16_t)a << 16) -
			product_low(d, error_in_phase) -
			product_low(f, error_quadrature) + UINT32_C(0x8000);
		uint32_t next_b =
			((uint32_t)(uint16_t)b << 16) -
			product_low(d, error_quadrature) +
			product_low(f, error_in_phase) + UINT32_C(0x8000);

		feedback->coefficient_a[index] = shifted_s16(next_a, 16);
		feedback->coefficient_b[index] = shifted_s16(next_b, 16);
	}
	return (struct v34_analytic_sample){
		.in_phase = error_in_phase,
		.quadrature = error_quadrature,
	};
}

static uint32_t distance_squared(
	struct v34_analytic_sample sample,
	struct v34_analytic_sample reference)
{
	int32_t in_phase = wrap_s16(
		(int32_t)sample.in_phase - reference.in_phase);
	int32_t quadrature = wrap_s16(
		(int32_t)sample.quadrature - reference.quadrature);

	return (uint32_t)(in_phase * in_phase) +
	       (uint32_t)(quadrature * quadrature);
}

static int16_t published_metric(uint32_t accumulator)
{
	return (accumulator & UINT32_C(0x80000000)) != 0 ?
		       INT16_MAX : (int16_t)(uint16_t)(accumulator >> 16);
}

static void update_quality(
	struct v34_adaptive_equalizer_quality *quality,
	struct v34_analytic_sample demodulated,
	struct v34_analytic_sample rotated_target)
{
	uint32_t metric = quality->decision_accumulator +
		distance_squared(demodulated, rotated_target);
	uint32_t reference = distance_squared(
		rotated_target, (struct v34_analytic_sample){ 0 });
	uint16_t position = (uint16_t)(
		(quality->window_position + 1u) &
		(V34_ADAPTIVE_EQUALIZER_QUALITY_SYMBOLS - 1u));

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
	uint32_t error = product_low(target.in_phase, observation.quadrature) -
		product_low(target.quadrature, observation.in_phase);

	return wrap_s32(error << 2u);
}

static void update_carrier(
	struct v34_adaptive_equalizer *equalizer, int32_t error)
{
	uint32_t accumulator_word =
		(uint32_t)sar32((uint32_t)error, V34_CARRIER_PHASE_SHIFT_B) +
		(uint32_t)equalizer->phase_accumulator;
	int32_t accumulator = wrap_s32(accumulator_word);
	uint32_t phase_word =
		(uint32_t)sar32((uint32_t)error, V34_CARRIER_PHASE_SHIFT_A) +
		(uint32_t)sar32((uint32_t)accumulator, 5) +
		((uint32_t)equalizer->carrier_phase_fraction << 16) +
		(uint32_t)(int32_t)(int16_t)equalizer->carrier_phase_low;
	int32_t phase = wrap_s32(phase_word);
	uint16_t coarse = (uint16_t)(
		(uint32_t)sar32((uint32_t)phase, 16) & UINT32_C(0x1fff));
	uint8_t index = (uint8_t)(coarse >> 5);
	int32_t fraction = (int32_t)(coarse & UINT16_C(0x001f)) * 0x32;
	int16_t cosine = carrier_cosine_q14(index);
	int16_t sine = carrier_cosine_q14((uint8_t)(index + 64u));
	int16_t negative_sine = wrap_s16(-(int32_t)sine);
	uint32_t interpolated_quadrature =
		((uint32_t)(int32_t)negative_sine << 16) +
		(uint32_t)(fraction * cosine) + UINT32_C(0x8000);
	uint32_t interpolated_in_phase =
		((uint32_t)(int32_t)cosine << 16) -
		(uint32_t)(fraction * negative_sine) + UINT32_C(0x8000);

	equalizer->phase_accumulator = accumulator;
	equalizer->carrier_phase_low = (uint16_t)phase;
	equalizer->carrier_phase_fraction = coarse;
	equalizer->rotate_in_phase = shifted_s16(interpolated_in_phase, 16);
	equalizer->rotate_quadrature = shifted_s16(
		interpolated_quadrature, 16);
}

static bool adaptation_is_valid(
	enum v34_adaptive_equalizer_adaptation adaptation)
{
	return adaptation == V34_ADAPTIVE_EQUALIZER_SKIP ||
	       adaptation == V34_ADAPTIVE_EQUALIZER_CENTER ||
	       adaptation == V34_ADAPTIVE_EQUALIZER_FULL;
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
	equalizer->decision_pending = false;
}

enum v34_adaptive_equalizer_result v34_adaptive_equalizer_commit(
	struct v34_adaptive_equalizer *equalizer,
	const struct v34_adaptive_equalizer_decision *decision)
{
	struct v34_adaptive_equalizer next;
	struct v34_analytic_sample rotated_target;
	struct v34_analytic_sample error;

	if (decision == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    decision, sizeof(*decision)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    !equalizer->decision_pending)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (!decision_is_valid(decision) ||
	    !observations_are_equal(
		    &decision->observation,
		    &equalizer->pending_observation))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	next.phase_error = phase_error(
		decision->target, next.pending_observation.decision);
	if ((next.feedback_mode & V34_ADAPTIVE_EQUALIZER_FEEDBACK_LEARN) != 0) {
		struct v34_analytic_sample decision_error = {
			.in_phase = wrap_s16(
				(int32_t)next.pending_observation.decision.in_phase -
				decision->target.in_phase),
			.quadrature = wrap_s16(
				(int32_t)next.pending_observation.decision.quadrature -
				decision->target.quadrature),
		};

		decision_error = update_feedback_filter(
			&next.feedback, decision_error);
		next.quality.feedback_accumulator += distance_squared(
			decision_error, (struct v34_analytic_sample){ 0 });
	}
	rotated_target = rotate_q14(
		decision->target, next.rotate_in_phase,
		next.rotate_quadrature);
	if (decision->adaptation != V34_ADAPTIVE_EQUALIZER_SKIP) {
		update_carrier(&next, next.phase_error);
		error = (struct v34_analytic_sample){
			.in_phase = shifted_s16(
				product_low(
					wrap_s16(
						(int32_t)next.pending_demodulated.in_phase -
						rotated_target.in_phase),
					(int16_t)decision->decoder_power_q15),
				16),
			.quadrature = shifted_s16(
				product_low(
					wrap_s16(
						(int32_t)next.pending_demodulated.quadrature -
						rotated_target.quadrature),
					(int16_t)decision->decoder_power_q15),
				16),
		};
		update_quality(
			&next.quality, next.pending_demodulated, rotated_target);
		if (decision->adaptation == V34_ADAPTIVE_EQUALIZER_FULL) {
			if (v34_equalizer_adapt(&next.equalizer, error) !=
			    V34_EQUALIZER_OK)
				return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
		} else {
			error.in_phase = wrap_s16(
				(int32_t)error.in_phase * 2);
			error.quadrature = wrap_s16(
				(int32_t)error.quadrature * 2);
			if (v34_equalizer_adapt_center(
				    &next.equalizer, error) != V34_EQUALIZER_OK)
				return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
		}
	}
	next.lineage = lineage_word(
		lineage_word(lineage_word(
			lineage_word(
				lineage_tag(next.lineage, 3),
				(uint16_t)decision->target.in_phase),
			(uint16_t)decision->target.quadrature),
		(uint16_t)decision->decoder_power_q15),
		(uint16_t)decision->adaptation);
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
	struct v34_adaptive_equalizer next;

	if (observation == NULL ||
	    memory_regions_overlap(
		    equalizer, equalizer == NULL ? 0 : sizeof(*equalizer),
		    observation, sizeof(*observation)))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	if (!v34_adaptive_equalizer_is_valid(equalizer) ||
	    !equalizer->decision_pending ||
	    (equalizer->feedback_mode &
	     V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY) != 0)
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	if (!observations_are_equal(
		    observation, &equalizer->pending_observation))
		return V34_ADAPTIVE_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	next.lineage = lineage_tag(next.lineage, 4);
	clear_pending(&next);
	if (!v34_adaptive_equalizer_is_valid(&next))
		return V34_ADAPTIVE_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V34_ADAPTIVE_EQUALIZER_OK;
}
