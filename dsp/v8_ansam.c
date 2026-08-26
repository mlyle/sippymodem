// SPDX-License-Identifier: BSD-3-Clause

#include "v8_ansam.h"

#include "fixed_phase.h"
#include "memory_region.h"
#include "phase.h"

#include <limits.h>

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

static const int16_t V8_ANSAM_TONE_COEFFICIENTS[] = {
	0, 0, 0, 0, 1984, 15229, 2974, 15229,
};

static const int16_t V8_ANSAM_REJECTION_A[] = { 0, 14487 };
static const int16_t V8_ANSAM_REJECTION_B[] = { 15408, 0, 15408 };

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
	if (detector == NULL || detector->signal_timer >=
		    V8_ANSAM_SIGNAL_STABILITY_SAMPLES ||
	    detector->signal_timer % V8_ANSAM_BLOCK_SAMPLES != 0 ||
	    detector->signal_ref_gain == 0 ||
	    (int16_t)detector->signal_ref_gain <= 0 ||
	    detector->tone.warmup_count > V8_ANSAM_TONE_WARMUP_BLOCKS ||
	    (!detector->tone.locked &&
	     detector->tone.warmup_count == V8_ANSAM_TONE_WARMUP_BLOCKS) ||
	    detector->modulation.phase > V8_ANSAM_DFT_PHASE_MASK ||
	    (unsigned int)detector->stage > V8_ANSAM_DETECTED)
		return false;
	return detector->stage != V8_ANSAM_WAIT_CARRIER ||
	       detector->modulation_energy == 0;
}

enum v8_ansam_result v8_ansam_detector_init(
	struct v8_ansam_detector *detector)
{
	struct v8_ansam_detector initialized = {
		.tone = {
			.counter = (uint16_t)(0u - 42u),
		},
		.signal_ref_gain = 0x0200,
	};

	if (detector == NULL || !v8_ansam_detector_is_valid(&initialized))
		return V8_ANSAM_INVALID_ARGUMENT;
	*detector = initialized;
	return V8_ANSAM_OK;
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

static void update_signal_stability(
	struct v8_ansam_detector *detector, uint16_t line_gain)
{
	int16_t timer = (int16_t)(detector->signal_timer +
				  V8_ANSAM_BLOCK_SAMPLES);
	int16_t current_gain;
	int16_t delta;

	if (timer < V8_ANSAM_SIGNAL_STABILITY_SAMPLES) {
		int16_t reference_gain = (int16_t)detector->signal_ref_gain;

		detector->signal_timer = (uint16_t)timer;
		current_gain = (int16_t)line_gain;
		delta = (int16_t)((((int32_t)current_gain - reference_gain) << 14) /
				  reference_gain);
	} else {
		current_gain = (int16_t)line_gain;
		detector->signal_timer = 0;
		detector->signal_ref_gain = (uint16_t)current_gain;
		delta = 0;
	}
	if (delta < 0)
		delta = (int16_t)-delta;
	if (delta > 0x0333) {
		detector->signal_stable_time = 0;
		detector->signal_stable = false;
		return;
	}
	timer = (int16_t)(detector->signal_stable_time +
				  V8_ANSAM_BLOCK_SAMPLES);
	detector->signal_stable_time = (uint16_t)timer;
	if (timer >= V8_ANSAM_SIGNAL_STABILITY_SAMPLES)
		detector->signal_stable = true;
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

static bool process_block(
	struct v8_ansam_detector *detector,
	struct v8_line_conditioner *conditioner,
	struct v8_line_conditioned_block *block)
{
	enum v8_ansam_detector_stage stage = detector->stage;
	int16_t *samples = block->samples;

	if (block->gain_reset && stage == V8_ANSAM_WAIT_MODULATION) {
		detector->stage = V8_ANSAM_WAIT_CARRIER;
		detector->modulation_energy = 0;
		detector->tone.counter = 0;
		detector->tone.warmup_count = 0;
		stage = detector->stage;
	}
	if (stage == V8_ANSAM_WAIT_CARRIER) {
		bool was_locked = detector->tone.locked;
		bool carrier = detect_carrier(&detector->tone, samples);

		if (!was_locked && detector->tone.locked &&
		    v8_line_conditioner_set_gain_mode(
			    conditioner, V8_LINE_GAIN_COARSE) !=
			    V8_LINE_CONDITIONER_OK)
			return false;
		if (carrier) {
			detector->stage = V8_ANSAM_WAIT_MODULATION;
			detector->dwell_blocks = 0;
			detector->modulation_energy = 0;
		}
		return true;
	}
	if (stage != V8_ANSAM_WAIT_MODULATION)
		return true;

	update_signal_stability(detector, conditioner->gain);
	if (detector->signal_stable)
		accumulate_modulation(&detector->modulation, samples);
	detector->dwell_blocks++;
	if (detector->dwell_blocks <=
	    V8_ANSAM_POST_DETECT_DWELL_BLOCKS)
		return true;
	detector->modulation_energy = modulation_energy(&detector->modulation);
	if (v8_line_conditioner_set_gain_mode(
		    conditioner, V8_LINE_GAIN_FROZEN) !=
	    V8_LINE_CONDITIONER_OK)
		return false;
	if (detector->modulation_energy >
	    V8_ANSAM_MODULATION_ENERGY_THRESHOLD)
		detector->stage = V8_ANSAM_DETECTED;
	return true;
}

enum v8_ansam_result v8_ansam_detect(
	struct v8_ansam_detector *detector,
	struct v8_line_conditioner *conditioner,
	const int16_t *samples, size_t count, bool *detected)
{
	struct v8_ansam_detector next;
	struct v8_line_conditioner next_line;
	size_t sample_bytes;

	if (!v8_ansam_detector_is_valid(detector) ||
	    !v8_line_conditioner_is_valid(conditioner) ||
	    conditioner->role != V8_CALLING_STATION || detected == NULL ||
	    (samples == NULL && count != 0) ||
	    count > SIZE_MAX / sizeof(*samples))
		return V8_ANSAM_INVALID_ARGUMENT;
	sample_bytes = count * sizeof(*samples);
	if (memory_regions_overlap(
		    detector, sizeof(*detector), detected, sizeof(*detected)) ||
	    memory_regions_overlap(
		    conditioner, sizeof(*conditioner), detected,
		    sizeof(*detected)) ||
	    memory_regions_overlap(
		    detector, sizeof(*detector), samples, sample_bytes) ||
	    memory_regions_overlap(
		    conditioner, sizeof(*conditioner), samples, sample_bytes) ||
	    memory_regions_overlap(
		    detector, sizeof(*detector), conditioner,
		    sizeof(*conditioner)) ||
	    memory_regions_overlap(
		    samples, sample_bytes, detected, sizeof(*detected)))
		return V8_ANSAM_INVALID_ARGUMENT;
	next = *detector;
	next_line = *conditioner;
	for (size_t index = 0; index < count; ++index) {
		struct v8_line_conditioned_block block;
		bool ready;

		if (v8_line_conditioner_process_sample(
			    &next_line, samples[index], &block, &ready) !=
		    V8_LINE_CONDITIONER_OK)
			return V8_ANSAM_INVALID_ARGUMENT;
		if (ready && next.stage != V8_ANSAM_DETECTED &&
		    !process_block(&next, &next_line, &block))
			return V8_ANSAM_INVALID_ARGUMENT;
	}
	if (!v8_ansam_detector_is_valid(&next) ||
	    !v8_line_conditioner_is_valid(&next_line))
		return V8_ANSAM_INVALID_ARGUMENT;
	*detector = next;
	*conditioner = next_line;
	*detected = next.stage == V8_ANSAM_DETECTED;
	return V8_ANSAM_OK;
}
