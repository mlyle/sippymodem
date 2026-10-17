// SPDX-License-Identifier: BSD-3-Clause

#include "equalizer.h"
#include "fixed_phase.h"
#include "fir.h"
#include "integer_math.h"
#include "memory_region.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	SAMPLES_PER_SYMBOL = 3,
	CARRIER_PHASES = 4,
	CARRIER_QUARTER_PERIOD = 0x4000,
	CARRIER_DECISION_PERIOD = 0x8000,
	CARRIER_ACCUMULATOR_PERIOD = 0x40000000,
	CARRIER_WARMUP_SYMBOLS = 0x05dc,
	CARRIER_HIGH_ERROR = 0x1988,
	CARRIER_LOW_ERROR = 0x0666,
	ERROR_SMOOTH_PRIOR_Q15 = 0x799a,
	ERROR_SMOOTH_SAMPLE_Q15 = 0x0666,
};

enum carrier_gain_mode {
	CARRIER_GAIN_WARMUP,
	CARRIER_GAIN_HIGH_ERROR,
	CARRIER_GAIN_LOW_ERROR,
};

enum equalizer_control {
	CONTROL_ADAPTATION = 1,
	CONTROL_CARRIER_TRACKING,
	CONTROL_RESTART_CARRIER_ACQUISITION,
};

static const int16_t INITIAL_I_HALF[26] = {
	-5, -2, 2, 6, 10, 10, 9, 3, -4, -11, -19, -20, -18,
	-7, 9, 28, 51, 57, 61, 26, -43, -142, -415, -582, -2129,
	-16043,
};

static const int16_t INITIAL_Q_HALF[26] = {
	6, 8, 7, 4, 0, -6, -11, -14, -14, -8, -1, 12, 23,
	31, 33, 21, 3, -39, -77, -126, -167, -112, -86, 863, 3710,
	-22796,
};

static const int16_t CARRIER_PHASE[CARRIER_PHASES] = {
	0, 8192, 16384, 24576,
};

static const int16_t CARRIER_PROPORTIONAL_GAIN[] = {
	602, 3050, 766,
};

static const int16_t CARRIER_INTEGRAL_GAIN[] = {
	0, 18, 1,
};

static int16_t smooth_q15(
	int16_t prior, int16_t sample)
{
	return wrap_s16_from_s32(
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)prior * ERROR_SMOOTH_PRIOR_Q15), 15) +
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)sample * ERROR_SMOOTH_SAMPLE_Q15), 15));
}

static uint64_t lineage_word(uint64_t lineage, uint16_t word)
{
	return (lineage ^ word) * UINT64_C(1099511628211);
}

static uint64_t lineage_tag(uint64_t lineage, uint16_t tag)
{
	return lineage_word(lineage_word(lineage, UINT16_C(0xa55a)), tag);
}

bool v32_equalizer_is_valid(
	const struct v32_equalizer *equalizer)
{
	if (equalizer == NULL ||
	    (unsigned int)equalizer->adaptation >
		    V32_EQUALIZER_ADAPTATION_FINE ||
	    equalizer->history_index >= V32_EQUALIZER_TAPS ||
	    equalizer->carrier_phase_index >= CARRIER_PHASES ||
	    equalizer->carrier_gain_index > CARRIER_GAIN_LOW_ERROR ||
	    equalizer->carrier_training_symbols > CARRIER_WARMUP_SYMBOLS + 1u ||
	    equalizer->decision_error < 0 ||
	    equalizer->carrier_phase_accumulator < -0x20000000 ||
	    equalizer->carrier_phase_accumulator > 0x20000000)
		return false;
	if (equalizer->decision_pending)
		return equalizer->samples_until_observation == 0 &&
		       equalizer->epoch != 0 &&
		       equalizer->pending_observation.epoch == equalizer->epoch &&
		       equalizer->pending_observation.lineage == equalizer->lineage &&
		       equalizer->pending_observation.phase <= INT16_MAX &&
		       equalizer->pending_observation.magnitude <= INT16_MAX;
	return equalizer->samples_until_observation >= 1 &&
	       equalizer->samples_until_observation <= SAMPLES_PER_SYMBOL &&
	       v32_equalizer_observation_is_zero(&equalizer->pending_observation) &&
	       equalizer->pending_filtered_i == 0 &&
	       equalizer->pending_filtered_q == 0 &&
	       equalizer->pending_carrier_phase == 0;
}

static void reconstruct_initial_coefficients(
	struct v32_equalizer *equalizer)
{
	for (size_t profile = 0; profile < 52; ++profile) {
		int16_t value = profile < 26 ? INITIAL_I_HALF[profile] :
			wrap_s16_from_s32(
				-(int32_t)INITIAL_I_HALF[51u - profile]);

		equalizer->i_coefficients[profile * 2u] = value;
	}
	for (size_t profile = 0; profile < 51; ++profile) {
		int16_t value = profile < 26 ? INITIAL_Q_HALF[profile] :
			INITIAL_Q_HALF[50u - profile];

		equalizer->q_coefficients[profile * 2u + 1u] = value;
	}
}

enum v32_equalizer_result v32_equalizer_init(
	struct v32_equalizer *equalizer)
{
	struct v32_equalizer initialized;

	if (equalizer == NULL)
		return V32_EQUALIZER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	reconstruct_initial_coefficients(&initialized);
	initialized.lineage = UINT64_C(14695981039346656037);
	initialized.samples_until_observation = 1;
	initialized.adaptation = V32_EQUALIZER_ADAPTATION_COARSE;
	initialized.carrier_tracking = true;
	if (!v32_equalizer_is_valid(&initialized))
		return V32_EQUALIZER_INVALID_STATE;
	*equalizer = initialized;
	return V32_EQUALIZER_OK;
}

static enum v32_equalizer_result set_control(
	struct v32_equalizer *equalizer,
	enum equalizer_control control, unsigned int value)
{
	struct v32_equalizer next;

	if (!v32_equalizer_is_valid(equalizer))
		return V32_EQUALIZER_INVALID_STATE;
	if (equalizer->decision_pending)
		return V32_EQUALIZER_DECISION_PENDING;
	next = *equalizer;
	if (control == CONTROL_ADAPTATION) {
		if (value > V32_EQUALIZER_ADAPTATION_FINE)
			return V32_EQUALIZER_INVALID_ARGUMENT;
		next.adaptation = (enum v32_equalizer_adaptation)value;
	} else if (control == CONTROL_CARRIER_TRACKING) {
		next.carrier_tracking = value != 0;
	} else {
		next.carrier_training_symbols = 0;
	}
	next.lineage = lineage_word(
		lineage_tag(next.lineage, (uint16_t)control), value);
	if (!v32_equalizer_is_valid(&next))
		return V32_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V32_EQUALIZER_OK;
}

enum v32_equalizer_result v32_equalizer_set_adaptation(
	struct v32_equalizer *equalizer,
	enum v32_equalizer_adaptation adaptation)
{
	return set_control(
		equalizer, CONTROL_ADAPTATION, (unsigned int)adaptation);
}

enum v32_equalizer_result v32_equalizer_set_carrier_tracking(
	struct v32_equalizer *equalizer, bool enabled)
{
	return set_control(equalizer, CONTROL_CARRIER_TRACKING, enabled);
}

enum v32_equalizer_result v32_equalizer_restart_carrier_acquisition(
	struct v32_equalizer *equalizer)
{
	return set_control(equalizer, CONTROL_RESTART_CARRIER_ACQUISITION, 0);
}

static int16_t filter_sample(
	const struct v32_equalizer *equalizer, const int16_t *coefficients)
{
	uint32_t accumulator = fir_read_ring_s16(equalizer->sample_history,
		coefficients, V32_EQUALIZER_TAPS, equalizer->history_index, 1);

	return wrap_s16_from_s32(
		wrap_s16_from_s32(
			arithmetic_shift_right_u32(accumulator, 15)) * 2);
}

static int16_t carrier_phase_offset(
	const struct v32_equalizer *equalizer)
{
	return wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)wrap_s32_from_u32(
			(uint32_t)equalizer->carrier_phase_accumulator + 0x4000u),
		15));
}

static int16_t half_phase(int16_t left, int16_t right, bool subtract)
{
	int32_t value = subtract ? (int32_t)left - right :
					 (int32_t)left + right;

	return wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)value, 1));
}

static int16_t wrap_quarter_phase(int16_t phase)
{
	if (phase < 0)
		phase = wrap_s16_from_s32(
			(int32_t)phase + CARRIER_QUARTER_PERIOD);
	if (phase >= CARRIER_QUARTER_PERIOD)
		phase = wrap_s16_from_s32(
			(int32_t)phase - CARRIER_QUARTER_PERIOD);
	return phase;
}

static int16_t wrap_decision_error(int16_t phase)
{
	if (phase < -CARRIER_QUARTER_PERIOD ||
	    phase > CARRIER_QUARTER_PERIOD)
		phase = wrap_s16_from_s32(
			(int32_t)phase - CARRIER_DECISION_PERIOD);
	return phase;
}

static void consume_sample(
	struct v32_equalizer *equalizer, int16_t sample)
{
	equalizer->history_index = (uint8_t)(equalizer->history_index + 1u);
	if (equalizer->history_index == V32_EQUALIZER_TAPS)
		equalizer->history_index = 0;
	equalizer->sample_history[equalizer->history_index] = sample;
	equalizer->carrier_phase_index =
		(uint8_t)((equalizer->carrier_phase_index + 1u) % CARRIER_PHASES);
	equalizer->lineage = lineage_word(lineage_tag(equalizer->lineage, 4),
					    (uint16_t)sample);
}

static struct v32_equalizer_observation make_observation(
	struct v32_equalizer *equalizer)
{
	int16_t sample_i = filter_sample(equalizer, equalizer->i_coefficients);
	int16_t sample_q = filter_sample(equalizer, equalizer->q_coefficients);
	uint16_t raw_phase = fixed_phase_atan2_u15(sample_q, sample_i);
	struct fixed_phase_q14 phasor =
		fixed_phase_sin_cos_q14((int16_t)raw_phase);
	int16_t magnitude = wrap_s16_from_s32(arithmetic_shift_right_u32(
		(uint32_t)((int32_t)phasor.cosine * sample_i +
			   (int32_t)phasor.sine * sample_q), 15));
	int16_t carrier_phase = wrap_s16_from_s32(
		(int32_t)CARRIER_PHASE[equalizer->carrier_phase_index] +
		carrier_phase_offset(equalizer));
	int16_t phase = wrap_s16_from_s32(
		wrap_quarter_phase(half_phase(
			(int16_t)raw_phase, carrier_phase, true)) * 2);

	phasor = fixed_phase_sin_cos_q14(phase);
	equalizer->pending_filtered_i = sample_i;
	equalizer->pending_filtered_q = sample_q;
	equalizer->pending_carrier_phase = carrier_phase;
	return (struct v32_equalizer_observation) {
		.i = multiply_shift_s16(phasor.cosine, magnitude, 13),
		.q = multiply_shift_s16(phasor.sine, magnitude, 13),
		.phase = (uint16_t)phase,
		.magnitude = (uint16_t)magnitude,
		.epoch = equalizer->epoch,
		.lineage = equalizer->lineage,
	};
}

static bool process_arguments_are_valid(
	const struct v32_equalizer *equalizer,
	const int16_t *samples, size_t count,
	size_t *consumed,
	struct v32_equalizer_observation *observation)
{
	size_t input_bytes;

	if ((samples == NULL && count != 0) || consumed == NULL ||
	    observation == NULL || count > SIZE_MAX / sizeof(*samples))
		return false;
	input_bytes = count * sizeof(*samples);
	return !memory_regions_overlap(samples, input_bytes,
				       equalizer, sizeof(*equalizer)) &&
	       !memory_regions_overlap(consumed, sizeof(*consumed),
				       equalizer, sizeof(*equalizer)) &&
	       !memory_regions_overlap(observation, sizeof(*observation),
				       equalizer, sizeof(*equalizer)) &&
	       !memory_regions_overlap(samples, input_bytes,
				       consumed, sizeof(*consumed)) &&
	       !memory_regions_overlap(samples, input_bytes,
				       observation, sizeof(*observation)) &&
	       !memory_regions_overlap(consumed, sizeof(*consumed),
				       observation, sizeof(*observation));
}

enum v32_equalizer_result v32_equalizer_process(
	struct v32_equalizer *equalizer,
	const int16_t *samples, size_t count,
	size_t *consumed,
	struct v32_equalizer_observation *observation)
{
	struct v32_equalizer next;
	struct v32_equalizer_observation pending = { 0 };
	size_t accepted;

	if (!v32_equalizer_is_valid(equalizer))
		return V32_EQUALIZER_INVALID_STATE;
	if (equalizer->decision_pending)
		return V32_EQUALIZER_DECISION_PENDING;
	if (!process_arguments_are_valid(
		    equalizer, samples, count, consumed, observation))
		return V32_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	accepted = count < next.samples_until_observation ?
		count : next.samples_until_observation;
	for (size_t index = 0; index < accepted; ++index)
		consume_sample(&next, samples[index]);
	next.samples_until_observation =
		(uint8_t)(next.samples_until_observation - accepted);
	if (next.samples_until_observation == 0) {
		if (next.epoch == UINT64_MAX)
			return V32_EQUALIZER_INVALID_STATE;
		next.epoch++;
		pending = make_observation(&next);
		next.pending_observation = pending;
		next.decision_pending = true;
	}
	if (!v32_equalizer_is_valid(&next))
		return V32_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	*consumed = accepted;
	*observation = pending;
	return next.decision_pending ? V32_EQUALIZER_OBSERVATION_READY :
				       V32_EQUALIZER_OK;
}

static void update_carrier_loop(
	struct v32_equalizer *equalizer, int16_t phase_error)
{
	int16_t average = smooth_q15(
		equalizer->smoothed_carrier_error, phase_error);

	equalizer->smoothed_carrier_error = average;
	if (!equalizer->carrier_tracking)
		return;
	if (equalizer->carrier_training_symbols > CARRIER_WARMUP_SYMBOLS) {
		int16_t absolute_average = absolute_wrap_s16(average);

		if (absolute_average >= CARRIER_HIGH_ERROR)
			equalizer->carrier_gain_index = CARRIER_GAIN_HIGH_ERROR;
		if (absolute_average <= CARRIER_LOW_ERROR)
			equalizer->carrier_gain_index = CARRIER_GAIN_LOW_ERROR;
	} else {
		uint16_t count = equalizer->carrier_training_symbols;

		equalizer->carrier_training_symbols = (uint16_t)(count + 1u);
		equalizer->carrier_gain_index = count < CARRIER_WARMUP_SYMBOLS ?
			CARRIER_GAIN_WARMUP : CARRIER_GAIN_HIGH_ERROR;
	}

	uint8_t gain = equalizer->carrier_gain_index;
	int32_t old_integrator = equalizer->carrier_integrator;
	int32_t integral =
		(int32_t)CARRIER_INTEGRAL_GAIN[gain] * phase_error;
	int32_t proportional =
		(int32_t)CARRIER_PROPORTIONAL_GAIN[gain] * phase_error;
	int32_t phase;

	equalizer->carrier_integrator = wrap_s32_from_u32(
		(uint32_t)integral + (uint32_t)old_integrator);
	phase = wrap_s32_from_u32(
		(uint32_t)proportional +
		(uint32_t)equalizer->carrier_phase_accumulator);
	phase = wrap_s32_from_u32(
		(uint32_t)phase + (uint32_t)old_integrator);
	if (phase < -0x20000000)
		phase = wrap_s32_from_u32(
			(uint32_t)phase + CARRIER_ACCUMULATOR_PERIOD);
	if (phase > 0x20000000)
		phase = wrap_s32_from_u32(
			(uint32_t)phase - CARRIER_ACCUMULATOR_PERIOD);
	equalizer->carrier_phase_accumulator = phase;
}

static int16_t adaptation_gain(
	enum v32_equalizer_adaptation adaptation)
{
	if (adaptation == V32_EQUALIZER_ADAPTATION_COARSE)
		return 0x0cab;
	if (adaptation == V32_EQUALIZER_ADAPTATION_FINE)
		return 0x0361;
	return 0;
}

static void lms_update(
	int16_t *coefficients, const struct v32_equalizer *equalizer,
	int16_t gain)
{
	size_t history = equalizer->history_index;

	for (size_t tap = 0; tap < V32_EQUALIZER_TAPS; ++tap) {
		int32_t product = (int32_t)equalizer->sample_history[history] *
			gain;
		int16_t delta = wrap_s16_from_s32(arithmetic_shift_right_u32(
			(uint32_t)product + UINT32_C(0x20000), 18));

		coefficients[tap] = wrap_s16_from_s32(
			(int32_t)coefficients[tap] + delta);
		history = history == 0 ? V32_EQUALIZER_TAPS - 1u : history - 1u;
	}
}

static void clear_pending(struct v32_equalizer *equalizer)
{
	memset(&equalizer->pending_observation, 0,
	       sizeof(equalizer->pending_observation));
	equalizer->pending_filtered_i = 0;
	equalizer->pending_filtered_q = 0;
	equalizer->pending_carrier_phase = 0;
	equalizer->decision_pending = false;
	equalizer->samples_until_observation = SAMPLES_PER_SYMBOL;
}

enum v32_equalizer_result v32_equalizer_commit(
	struct v32_equalizer *equalizer,
	const struct v32_equalizer_decision *decision)
{
	struct v32_equalizer next;
	struct fixed_phase_q14 phasor;
	int16_t carrier_error;
	int16_t ideal_phase;
	int16_t error_i;
	int16_t error_q;
	int16_t scaled_error;
	int32_t error_energy;
	int32_t next_error;
	bool adapt;

	if (!v32_equalizer_is_valid(equalizer))
		return V32_EQUALIZER_INVALID_STATE;
	if (!equalizer->decision_pending)
		return V32_EQUALIZER_INVALID_STATE;
	if (decision == NULL ||
	    memory_regions_overlap(
		    decision, sizeof(*decision), equalizer, sizeof(*equalizer)))
		return V32_EQUALIZER_INVALID_ARGUMENT;
	if (!v32_equalizer_observations_are_equal(
		    &decision->observation, &equalizer->pending_observation))
		return V32_EQUALIZER_INVALID_ARGUMENT;
	if (decision->ideal_phase > INT16_MAX ||
	    decision->ideal_magnitude > INT16_MAX)
		return V32_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	carrier_error = wrap_decision_error(wrap_s16_from_s32(
		(int32_t)(int16_t)next.pending_observation.phase -
		(int16_t)decision->ideal_phase));
	update_carrier_loop(&next, carrier_error);
	ideal_phase = wrap_quarter_phase(half_phase(
		(int16_t)decision->ideal_phase,
		next.pending_carrier_phase, false));
	phasor = fixed_phase_sin_cos_q14(
		wrap_s16_from_s32((int32_t)ideal_phase * 2));
	error_i = wrap_s16_from_s32(
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)phasor.cosine *
				 (int16_t)decision->ideal_magnitude), 14) -
		next.pending_filtered_i);
	error_q = wrap_s16_from_s32(
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)(int16_t)decision->ideal_magnitude *
				 phasor.sine), 14) -
		next.pending_filtered_q);
	error_energy = wrap_s32_from_u32(
		(uint32_t)((int32_t)error_i * error_i) +
		(uint32_t)((int32_t)error_q * error_q));
	scaled_error = wrap_s16_from_s32(
		arithmetic_shift_right_u32((uint32_t)error_energy, 11));
	next_error =
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)next.decision_error *
				   ERROR_SMOOTH_PRIOR_Q15), 15) +
		arithmetic_shift_right_u32(
			(uint32_t)((int32_t)scaled_error *
				   ERROR_SMOOTH_SAMPLE_Q15), 15);
	next.decision_error = (uint32_t)next_error < CARRIER_DECISION_PERIOD ?
		wrap_s16_from_s32(next_error) : INT16_MAX;
	adapt = next.adaptation != V32_EQUALIZER_ADAPTATION_DISABLED &&
		decision->adapt_on_error && next.decision_error > 0;
	if (adapt) {
		int16_t gain = adaptation_gain(next.adaptation);
		int16_t gain_i = wrap_s16_from_s32(arithmetic_shift_right_u32(
			(uint32_t)((int32_t)error_i * gain + 0x800), 12));
		int16_t gain_q = wrap_s16_from_s32(arithmetic_shift_right_u32(
			(uint32_t)((int32_t)error_q * gain + 0x800), 12));

		lms_update(next.i_coefficients, &next, gain_i);
		lms_update(next.q_coefficients, &next, gain_q);
	}
	next.lineage = lineage_word(
		lineage_word(
			lineage_word(
				lineage_tag(next.lineage, 5),
				decision->ideal_phase),
			decision->ideal_magnitude),
		decision->adapt_on_error);
	clear_pending(&next);
	if (!v32_equalizer_is_valid(&next))
		return V32_EQUALIZER_INVALID_STATE;
	*equalizer = next;
	return V32_EQUALIZER_OK;
}
