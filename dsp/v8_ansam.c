// SPDX-License-Identifier: BSD-3-Clause

#include "v8_ansam.h"

#include "fixed_phase.h"
#include "integer_math.h"
#include "memory_region.h"
#include "phase.h"

#include <limits.h>
#include <string.h>

static int16_t mul_shift_s16(
	int16_t left, int16_t right, unsigned int shift);

static int16_t cycle_cosine_q14(uint8_t index)
{
	return fixed_phase_cosine_u8_q14(index);
}

static int16_t phase_cosine_q14(uint32_t phase)
{
	return cycle_cosine_q14(
		(uint8_t)((phase + UINT32_C(0x00800000)) >> 24));
}

static bool generator_is_valid(
	const struct v8_ansam_generator *generator)
{
	int32_t amplitude;

	if (generator == NULL || generator->signed_amplitude == 0)
		return false;
	amplitude = generator->signed_amplitude;
	if (amplitude < 0)
		amplitude = -amplitude;
	return amplitude <= V8_ANSAM_MAX_AVERAGE_AMPLITUDE &&
	       ((!generator->phase_reversals &&
		 generator->samples_until_reversal == 0) ||
		(generator->phase_reversals &&
		 generator->samples_until_reversal > 0 &&
		 generator->samples_until_reversal <=
			 V8_ANSAM_REVERSAL_SAMPLES));
}

enum v8_ansam_result v8_ansam_generator_init(
	struct v8_ansam_generator *generator,
	int16_t average_amplitude, bool phase_reversals)
{
	struct v8_ansam_generator initialized = {
		.samples_until_reversal =
			phase_reversals ?
				V8_ANSAM_REVERSAL_SAMPLES : 0,
		.signed_amplitude = average_amplitude,
		.phase_reversals = phase_reversals,
	};

	if (generator == NULL || !generator_is_valid(&initialized))
		return V8_ANSAM_INVALID_ARGUMENT;
	*generator = initialized;
	return V8_ANSAM_OK;
}

enum v8_ansam_result v8_ansam_generate(
	struct v8_ansam_generator *generator,
	int16_t *samples, size_t count)
{
	struct v8_ansam_generator next;
	size_t bytes;

	if (!generator_is_valid(generator) ||
	    count > SIZE_MAX / sizeof(*samples) ||
	    (samples == NULL && count != 0))
		return V8_ANSAM_INVALID_ARGUMENT;
	bytes = count * sizeof(*samples);
	if (memory_regions_overlap(
		    generator, sizeof(*generator), samples, bytes))
		return V8_ANSAM_INVALID_ARGUMENT;
	next = *generator;
	for (size_t index = 0; index < count; ++index) {
		int16_t envelope;
		int16_t scaled_envelope;
		int16_t carrier;

		next.modulation_phase += phase_step_u32(
			V8_ANSAM_SAMPLE_RATE, V8_ANSAM_MODULATION_HZ, 1);
		next.carrier_phase += phase_step_u32(
			V8_ANSAM_SAMPLE_RATE, V8_ANSAM_CARRIER_HZ, 1);
		envelope = mul_shift_s16(
			0x0ccd, phase_cosine_q14(next.modulation_phase), 14);
		scaled_envelope = mul_shift_s16(
			(int16_t)(envelope + 0x4000),
			next.signed_amplitude, 14);
		carrier = phase_cosine_q14(next.carrier_phase);
		samples[index] = mul_shift_s16(
			carrier, scaled_envelope, 14);
		if (next.phase_reversals &&
		    --next.samples_until_reversal == 0) {
			next.signed_amplitude =
				(int16_t)-next.signed_amplitude;
			next.samples_until_reversal =
				V8_ANSAM_REVERSAL_SAMPLES;
		}
	}
	*generator = next;
	return V8_ANSAM_OK;
}

enum {
	V8_ANSAM_POWER_WEIGHT_Q15 =
		(0x8000 + V8_ANSAM_POWER_HISTORY_SAMPLES / 2) /
		V8_ANSAM_POWER_HISTORY_SAMPLES,
	V8_ANSAM_SIGNAL_STABILITY_SAMPLES = V8_ANSAM_SAMPLE_RATE / 10,
	V8_ANSAM_TONE_WARMUP_BLOCKS = 43,
	V8_ANSAM_TONE_HIGH_BLOCKS = 84,
	V8_ANSAM_TONE_LOW_THRESHOLD = 0x05dc,
	V8_ANSAM_TONE_ENERGY_POLE_Q14 = 0x3c2e,
	V8_ANSAM_TONE_ENERGY_GAIN_Q14 = 0x706d,
	V8_ANSAM_MODULATION_ENERGY_THRESHOLD = 0x018f,
	V8_ANSAM_DFT_PHASE_MASK = 0x3fff,
	V8_ANSAM_DFT_PHASE_STEP =
		(V8_ANSAM_MODULATION_HZ * (V8_ANSAM_DFT_PHASE_MASK + 1) +
		 V8_ANSAM_SAMPLE_RATE / 2) /
		V8_ANSAM_SAMPLE_RATE,
};

static const int16_t V8_ANSAM_ENTRANCE_FILTER[] = {
	-16, 11, 39, 21, -59, -111, 2, 190, 282, -281, -312, -636,
	1241, 272, 1444, -5382, 1199, 7549, -7124, 895, -42, 1626,
	-430, -276, -469, 210, 221, 75, -94, -87, 0, 41, 21, 0,
};

static const int16_t V8_ANSAM_TONE_COEFFICIENTS[] = {
	0, 0, 0, 0, 1984, 15229, 2974, 15229,
};

static const int16_t V8_ANSAM_REJECTION_A[] = { 0, 14487 };
static const int16_t V8_ANSAM_REJECTION_B[] = { 15408, 0, 15408 };

_Static_assert(
	sizeof(V8_ANSAM_ENTRANCE_FILTER) /
			sizeof(V8_ANSAM_ENTRANCE_FILTER[0]) ==
		V8_ANSAM_ENTRANCE_FILTER_TAPS,
	"ANSam entrance filter tap count changed");
_Static_assert(
	sizeof(V8_ANSAM_TONE_COEFFICIENTS) /
			sizeof(V8_ANSAM_TONE_COEFFICIENTS[0]) == 8,
	"ANSam tone filter profile changed");
_Static_assert(
	sizeof(V8_ANSAM_REJECTION_A) /
			sizeof(V8_ANSAM_REJECTION_A[0]) == 2 &&
		sizeof(V8_ANSAM_REJECTION_B) /
			sizeof(V8_ANSAM_REJECTION_B[0]) == 3,
	"ANSam rejection filter profile changed");

static int32_t sar32(uint32_t value, unsigned int shift)
{
	if (shift == 0)
		return value <= INT32_MAX ?
			       (int32_t)value :
			       (int32_t)((int64_t)value - (INT64_C(1) << 32));
	if ((value & UINT32_C(0x80000000)) == 0)
		return (int32_t)(value >> shift);
	value = (value >> shift) | ~(UINT32_MAX >> shift);
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

static int16_t mul_shift_s16(
	int16_t left, int16_t right, unsigned int shift)
{
	return wrap_s16(sar32(
		(uint32_t)((int32_t)left * right), shift));
}

static int32_t magnitude_s16(int16_t value)
{
	return value < 0 ? -(int32_t)value : value;
}

bool v8_ansam_detector_is_valid(
	const struct v8_ansam_detector *detector)
{
	if (detector == NULL || detector->agc.gain == 0 ||
	    (int16_t)detector->agc.gain <= 0 ||
	    detector->agc.power_index >= V8_ANSAM_POWER_HISTORY_SAMPLES ||
	    detector->agc.signal_timer >=
		    V8_ANSAM_SIGNAL_STABILITY_SAMPLES ||
	    detector->agc.signal_timer % V8_ANSAM_BLOCK_SAMPLES != 0 ||
	    detector->agc.signal_ref_gain == 0 ||
	    (int16_t)detector->agc.signal_ref_gain <= 0 ||
	    detector->agc.overflow_count >= 10 ||
	    detector->tone.warmup_count > V8_ANSAM_TONE_WARMUP_BLOCKS ||
	    (!detector->tone.locked &&
	     detector->tone.warmup_count == V8_ANSAM_TONE_WARMUP_BLOCKS) ||
	    detector->modulation.phase > V8_ANSAM_DFT_PHASE_MASK ||
	    detector->input_count >= V8_ANSAM_INPUT_QUEUE_SAMPLES ||
	    (unsigned int)detector->stage > V8_ANSAM_DETECTED)
		return false;
	return detector->stage != V8_ANSAM_WAIT_CARRIER ||
	       detector->modulation_energy == 0;
}

enum v8_ansam_result v8_ansam_detector_init(
	struct v8_ansam_detector *detector)
{
	struct v8_ansam_detector initialized = {
		.agc = {
			.gain = 0x0200,
			.signal_ref_gain = 0x0200,
		},
		.tone = {
			.counter = (uint16_t)(0u - 42u),
		},
	};

	if (detector == NULL || !v8_ansam_detector_is_valid(&initialized))
		return V8_ANSAM_INVALID_ARGUMENT;
	*detector = initialized;
	return V8_ANSAM_OK;
}

static int16_t entrance_filter_sample(
	struct v8_ansam_detector *detector, int16_t sample)
{
	uint32_t accumulator = 0;

	for (size_t index = 0;
	     index < V8_ANSAM_ENTRANCE_FILTER_TAPS; ++index) {
		int16_t previous = detector->entrance_history[index];

		detector->entrance_history[index] = sample;
		accumulator += (uint32_t)((int32_t)sample *
					  V8_ANSAM_ENTRANCE_FILTER[index]);
		sample = previous;
	}
	return wrap_s16(sar32(accumulator, 14));
}

static void adapt_gain(
	struct v8_ansam_agc *gain, uint32_t block_energy, bool enabled)
{
	int32_t filtered = (int16_t)gain->filtered_energy;
	int32_t block_high = (uint16_t)(block_energy >> 16);
	int32_t next_filtered =
		sar32((uint32_t)(filtered * 0x6952), 15) + block_high;
	int32_t deviation;
	int32_t delta;

	if (((uint32_t)next_filtered >> 15) == 0u ||
	    ((uint32_t)next_filtered >> 15) == 0x1ffffu)
		gain->filtered_energy = (uint16_t)next_filtered;
	else
		gain->filtered_energy = 0x7f00;
	if (!enabled)
		return;

	deviation = (int16_t)gain->filtered_energy - 4000;
	if (magnitude_s16(wrap_s16(deviation)) <= 2000)
		return;
	delta = sar32((uint32_t)(0x3333 * deviation), 16) +
		(int16_t)gain->gain_delta;
	if (magnitude_s16(wrap_s16(delta)) <= 1000) {
		gain->gain_delta = (uint16_t)delta;
		return;
	}

	gain->gain_delta = 0;
	if (delta > 0) {
		gain->gain = (uint16_t)sar32(
			(uint32_t)((int16_t)gain->gain * 0x390a), 14);
	} else if ((int16_t)gain->gain <= 0x6a00) {
		gain->gain = (uint16_t)sar32(
			(uint32_t)((int16_t)gain->gain * 0x47cf), 14);
	}
}

static int16_t scale_sample(
	int16_t gain, int16_t sample, bool *overflow)
{
	int32_t product = (int32_t)gain * sample;
	uint32_t top = (uint32_t)product >> 25;

	if (top == 0u || top == 0x7fu)
		return wrap_s16(sar32((uint32_t)product, 10));
	*overflow = true;
	return wrap_s16(sar32((uint32_t)product, 16)) < 1 ?
		       (int16_t)0x8100 : (int16_t)0x7f00;
}

static void apply_gain(
	struct v8_ansam_detector *detector,
	int16_t samples[V8_ANSAM_BLOCK_SAMPLES])
{
	struct v8_ansam_agc *gain = &detector->agc;
	uint32_t history_energy = 0;
	uint16_t energy_level;

	for (size_t index = 0; index < V8_ANSAM_BLOCK_SAMPLES; ++index)
		samples[index] = entrance_filter_sample(detector, samples[index]);
	for (size_t index = 0; index < V8_ANSAM_BLOCK_SAMPLES; ++index) {
		gain->power_history[gain->power_index] = (uint16_t)samples[index];
		gain->power_index++;
		if (gain->power_index == V8_ANSAM_POWER_HISTORY_SAMPLES)
			gain->power_index = 0;
	}
	for (size_t index = 0;
	     index < V8_ANSAM_POWER_HISTORY_SAMPLES; ++index) {
		int16_t sample = (int16_t)gain->power_history[index];
		int32_t weighted = sar32(
			(uint32_t)((int32_t)sample * V8_ANSAM_POWER_WEIGHT_Q15),
			15);

		history_energy += (uint32_t)(weighted * sample);
	}
	energy_level = fixed_square_root_u32(history_energy);

	for (size_t index = 0; index < V8_ANSAM_BLOCK_SAMPLES; ++index) {
		bool overflow = false;

		samples[index] = scale_sample(
			(int16_t)gain->gain, samples[index], &overflow);
		if (!overflow)
			continue;
		if ((uint16_t)(gain->overflow_count + 1u) != 10u) {
			gain->overflow_count++;
			continue;
		}
		gain->gain = 0x0400;
		if (detector->stage == V8_ANSAM_WAIT_MODULATION) {
			detector->stage = V8_ANSAM_WAIT_CARRIER;
			detector->modulation_energy = 0;
			detector->tone.counter = 0;
			detector->tone.warmup_count = 0;
			gain->overflow_count = 0;
		}
	}
	if (energy_level > 0x001fu) {
		uint32_t block_energy = 0;

		for (size_t index = 0;
		     index < V8_ANSAM_BLOCK_SAMPLES; ++index)
			block_energy += (uint32_t)(
				(int32_t)samples[index] * samples[index]);
		adapt_gain(gain, block_energy, detector->gain_tracking);
	}
}

static bool detect_carrier(
	struct v8_ansam_tone_detector *tone,
	int16_t samples[V8_ANSAM_BLOCK_SAMPLES])
{
	for (size_t sample_index = 0;
	     sample_index < V8_ANSAM_BLOCK_SAMPLES; ++sample_index) {
		int32_t output = 0;

		tone->tone_input[0] = (uint16_t)samples[sample_index];
		for (size_t index = 0; index < 3; ++index)
			output += mul_shift_s16(
				(int16_t)tone->tone_input[index],
				V8_ANSAM_REJECTION_B[index], 14);
		for (size_t index = 0; index < 2; ++index)
			output -= mul_shift_s16(
				(int16_t)tone->tone_output[index],
				V8_ANSAM_REJECTION_A[index], 14);
		for (size_t index = 0; index < 2; ++index) {
			tone->tone_input[2 - index] =
				tone->tone_input[1 - index];
			tone->tone_output[2 - index] =
				tone->tone_output[1 - index];
		}
		tone->tone_output[0] = (uint16_t)output;
		samples[sample_index] = wrap_s16(output);

		{
			int32_t stage = sar32((uint32_t)output, 4);

			for (size_t index = 0; index < 2; ++index) {
				stage += mul_shift_s16(
					(int16_t)tone->iir_input[index],
					V8_ANSAM_TONE_COEFFICIENTS[index], 14);
				stage -= mul_shift_s16(
					(int16_t)tone->iir_output[index],
					V8_ANSAM_TONE_COEFFICIENTS[4 + index],
					14);
			}
			tone->iir_output[1] = tone->iir_output[0];
			tone->iir_input[1] = tone->iir_input[0];
			tone->iir_output[0] = (uint16_t)stage;
			tone->iir_input[0] = (uint16_t)sar32(
				(uint32_t)output, 4);

			output = sar32((uint32_t)stage, 4);
			for (size_t index = 0; index < 2; ++index) {
				output += mul_shift_s16(
					(int16_t)tone->iir_input[2 + index],
					V8_ANSAM_TONE_COEFFICIENTS[2 + index],
					14);
				output -= mul_shift_s16(
					(int16_t)tone->iir_output[2 + index],
					V8_ANSAM_TONE_COEFFICIENTS[6 + index],
					14);
			}
			tone->iir_output[3] = tone->iir_output[2];
			tone->iir_input[3] = tone->iir_input[2];
			tone->iir_output[2] = (uint16_t)output;
			tone->iir_input[2] = (uint16_t)sar32(
				(uint32_t)stage, 4);

			output = mul_shift_s16(
				wrap_s16(magnitude_s16(wrap_s16(sar32(
					(uint32_t)output, 4)))),
				V8_ANSAM_TONE_ENERGY_GAIN_Q14, 14);
			output += mul_shift_s16(
				(int16_t)tone->filtered_energy,
				V8_ANSAM_TONE_ENERGY_POLE_Q14, 14);
			tone->filtered_energy = (uint16_t)output;
		}
	}

	if (tone->locked) {
		if ((int16_t)tone->filtered_energy <=
		    V8_ANSAM_TONE_LOW_THRESHOLD)
			tone->counter = 0;
		else
			tone->counter++;
	} else {
		if ((int16_t)tone->filtered_energy > 0x30) {
			tone->warmup_count++;
			if (tone->warmup_count ==
			    V8_ANSAM_TONE_WARMUP_BLOCKS) {
				tone->locked = 1;
				tone->counter = 0;
			}
			return false;
		}
		tone->warmup_count = 0;
		return false;
	}
	return (int16_t)tone->counter > V8_ANSAM_TONE_HIGH_BLOCKS;
}

static void update_signal_stability(struct v8_ansam_agc *gain)
{
	int16_t timer = (int16_t)(gain->signal_timer +
				  V8_ANSAM_BLOCK_SAMPLES);
	int16_t current_gain;
	int16_t delta;

	if (timer < V8_ANSAM_SIGNAL_STABILITY_SAMPLES) {
		int16_t reference_gain = (int16_t)gain->signal_ref_gain;

		gain->signal_timer = (uint16_t)timer;
		current_gain = (int16_t)gain->gain;
		delta = (int16_t)((((int32_t)current_gain - reference_gain) << 14) /
				  reference_gain);
	} else {
		current_gain = (int16_t)gain->gain;
		gain->signal_timer = 0;
		gain->signal_ref_gain = (uint16_t)current_gain;
		delta = 0;
	}
	if (delta < 0)
		delta = (int16_t)-delta;
	if (delta > 0x0333) {
		gain->signal_stable_time = 0;
		gain->signal_stable = false;
		return;
	}
	timer = (int16_t)(gain->signal_stable_time +
				  V8_ANSAM_BLOCK_SAMPLES);
	gain->signal_stable_time = (uint16_t)timer;
	if (timer >= V8_ANSAM_SIGNAL_STABILITY_SAMPLES)
		gain->signal_stable = true;
}

static void accumulate_modulation(
	struct v8_ansam_dft *dft,
	const int16_t samples[V8_ANSAM_BLOCK_SAMPLES])
{
	for (size_t index = 0; index < V8_ANSAM_BLOCK_SAMPLES; ++index) {
		uint8_t phase_index;
		int16_t cosine;
		int16_t quadrature;
		int16_t magnitude = samples[index] == INT16_MIN ?
					    INT16_MAX :
					    (int16_t)magnitude_s16(samples[index]);

		dft->phase = (uint16_t)(
			(dft->phase + V8_ANSAM_DFT_PHASE_STEP) &
			V8_ANSAM_DFT_PHASE_MASK);
		phase_index = (uint8_t)(dft->phase >> 6);
		cosine = cycle_cosine_q14(phase_index);
		phase_index = (uint8_t)(phase_index + 64u);
		quadrature = cycle_cosine_q14(phase_index);
		dft->real_accumulator += (uint32_t)sar32(
			(uint32_t)((int32_t)cosine * magnitude), 6);
		dft->imaginary_accumulator += (uint32_t)sar32(
			(uint32_t)((int32_t)quadrature * magnitude), 6);
	}
}

static uint16_t modulation_energy(const struct v8_ansam_dft *dft)
{
	int32_t real = sar32(dft->real_accumulator << 1, 16);
	int32_t imaginary = sar32(dft->imaginary_accumulator << 1, 16);
	uint32_t energy = (uint32_t)real * (uint32_t)real;

	energy += (uint32_t)imaginary * (uint32_t)imaginary;
	return (uint16_t)sar32(energy, 16);
}

static void process_block(
	struct v8_ansam_detector *detector,
	int16_t samples[V8_ANSAM_BLOCK_SAMPLES])
{
	enum v8_ansam_detector_stage stage = detector->stage;

	apply_gain(detector, samples);
	if (stage == V8_ANSAM_WAIT_CARRIER) {
		bool was_locked = detector->tone.locked;
		bool carrier = detect_carrier(&detector->tone, samples);

		if (!was_locked && detector->tone.locked)
			detector->gain_tracking = true;
		if (carrier) {
			detector->stage = V8_ANSAM_WAIT_MODULATION;
			detector->dwell_blocks = 0;
			detector->modulation_energy = 0;
		}
		return;
	}
	if (stage != V8_ANSAM_WAIT_MODULATION)
		return;

	update_signal_stability(&detector->agc);
	if (detector->agc.signal_stable)
		accumulate_modulation(&detector->modulation, samples);
	detector->dwell_blocks++;
	if (detector->dwell_blocks <=
	    V8_ANSAM_POST_DETECT_DWELL_BLOCKS)
		return;
	detector->modulation_energy = modulation_energy(&detector->modulation);
	detector->gain_tracking = false;
	if (detector->modulation_energy >
	    V8_ANSAM_MODULATION_ENERGY_THRESHOLD)
		detector->stage = V8_ANSAM_DETECTED;
}

static void stage_sample(
	struct v8_ansam_detector *detector, int16_t sample)
{
	uint16_t filtered_word = (uint16_t)sample +
		detector->post_filter_word;
	int16_t filtered = wrap_s16(filtered_word);
	uint32_t feedback =
		(uint32_t)((int32_t)filtered * 0x0f6d) -
		((uint32_t)(int32_t)sample << 12);

	detector->post_filter_word = (uint16_t)sar32(feedback, 12);
	detector->input_queue[detector->input_count++] = filtered;
	if (detector->input_count == V8_ANSAM_INPUT_QUEUE_SAMPLES) {
		process_block(detector, detector->input_queue);
		memmove(
			detector->input_queue,
			&detector->input_queue[V8_ANSAM_BLOCK_SAMPLES],
			(V8_ANSAM_INPUT_QUEUE_SAMPLES -
			 V8_ANSAM_BLOCK_SAMPLES) *
				sizeof(detector->input_queue[0]));
		detector->input_count =
			V8_ANSAM_INPUT_QUEUE_SAMPLES -
			V8_ANSAM_BLOCK_SAMPLES;
	}
}

enum v8_ansam_result v8_ansam_detect(
	struct v8_ansam_detector *detector,
	const int16_t *samples, size_t count, bool *detected)
{
	struct v8_ansam_detector next;
	size_t sample_bytes;

	if (!v8_ansam_detector_is_valid(detector) || detected == NULL ||
	    (samples == NULL && count != 0) ||
	    count > SIZE_MAX / sizeof(*samples))
		return V8_ANSAM_INVALID_ARGUMENT;
	sample_bytes = count * sizeof(*samples);
	if (memory_regions_overlap(
		    detector, sizeof(*detector), detected, sizeof(*detected)) ||
	    memory_regions_overlap(
		    detector, sizeof(*detector), samples, sample_bytes) ||
	    memory_regions_overlap(
		    samples, sample_bytes, detected, sizeof(*detected)))
		return V8_ANSAM_INVALID_ARGUMENT;
	next = *detector;
	if (next.stage != V8_ANSAM_DETECTED)
		for (size_t index = 0;
		     index < count && next.stage != V8_ANSAM_DETECTED;
		     ++index)
			stage_sample(&next, samples[index]);
	if (!v8_ansam_detector_is_valid(&next))
		return V8_ANSAM_INVALID_ARGUMENT;
	*detector = next;
	*detected = next.stage == V8_ANSAM_DETECTED;
	return V8_ANSAM_OK;
}
