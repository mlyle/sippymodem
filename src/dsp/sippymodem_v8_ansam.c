// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v8_ansam.h"

#include <math.h>

static const double V8_ANSAM_TWO_PI =
	6.283185307179586476925286766559;
static const double V8_ANSAM_PHASE_SCALE = 4294967296.0;
static const double V8_ANSAM_MODULATION_DEPTH = 0.2;

static uint32_t phase_step(unsigned int frequency_hz)
{
	uint64_t scaled =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)((scaled + SIPPYMODEM_V8_ANSAM_SAMPLE_RATE / 2u) /
			  SIPPYMODEM_V8_ANSAM_SAMPLE_RATE);
}

static double phase_radians(uint32_t phase)
{
	return (double)phase * (V8_ANSAM_TWO_PI / V8_ANSAM_PHASE_SCALE);
}

static bool generator_is_valid(
	const struct sippymodem_v8_ansam_generator *generator)
{
	return generator != NULL && generator->average_amplitude > 0 &&
	       generator->average_amplitude <=
		       SIPPYMODEM_V8_ANSAM_MAX_AVERAGE_AMPLITUDE &&
	       generator->carrier_phase_step ==
		       phase_step(SIPPYMODEM_V8_ANSAM_CARRIER_HZ) &&
	       generator->modulation_phase_step ==
		       phase_step(SIPPYMODEM_V8_ANSAM_MODULATION_HZ) &&
	       ((!generator->phase_reversals &&
		 generator->samples_until_reversal == 0) ||
		(generator->phase_reversals &&
		 generator->samples_until_reversal > 0 &&
		 generator->samples_until_reversal <=
			 SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES));
}

enum sippymodem_v8_ansam_result sippymodem_v8_ansam_generator_init(
	struct sippymodem_v8_ansam_generator *generator,
	int16_t average_amplitude, bool phase_reversals)
{
	struct sippymodem_v8_ansam_generator initialized = {
		.carrier_phase_step =
			phase_step(SIPPYMODEM_V8_ANSAM_CARRIER_HZ),
		.modulation_phase_step =
			phase_step(SIPPYMODEM_V8_ANSAM_MODULATION_HZ),
		.samples_until_reversal =
			phase_reversals ?
				SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES : 0,
		.average_amplitude = average_amplitude,
		.phase_reversals = phase_reversals,
	};

	if (generator == NULL || !generator_is_valid(&initialized))
		return SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT;
	*generator = initialized;
	return SIPPYMODEM_V8_ANSAM_OK;
}

enum sippymodem_v8_ansam_result sippymodem_v8_ansam_generate(
	struct sippymodem_v8_ansam_generator *generator,
	int16_t *samples, size_t count)
{
	struct sippymodem_v8_ansam_generator next;

	if (!generator_is_valid(generator) || (samples == NULL && count != 0))
		return SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT;
	next = *generator;
	for (size_t index = 0; index < count; ++index) {
		double carrier = sin(phase_radians(next.carrier_phase));
		double envelope =
			1.0 + V8_ANSAM_MODULATION_DEPTH *
				      cos(phase_radians(next.modulation_phase));

		samples[index] = (int16_t)lround(
			(double)next.average_amplitude * envelope * carrier);
		next.carrier_phase += next.carrier_phase_step;
		next.modulation_phase += next.modulation_phase_step;
		if (next.phase_reversals &&
		    --next.samples_until_reversal == 0) {
			next.carrier_phase += UINT32_C(0x80000000);
			next.samples_until_reversal =
				SIPPYMODEM_V8_ANSAM_REVERSAL_SAMPLES;
		}
	}
	*generator = next;
	return SIPPYMODEM_V8_ANSAM_OK;
}

const char *sippymodem_v8_ansam_result_name(
	enum sippymodem_v8_ansam_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_ANSAM_OK:
		return "ok";
	case SIPPYMODEM_V8_ANSAM_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
