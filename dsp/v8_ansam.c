// SPDX-License-Identifier: BSD-3-Clause

#include "v8_ansam.h"

#include "phase.h"

#include <math.h>

static const double V8_ANSAM_MODULATION_DEPTH = 0.2;
static const double V8_ANSAM_MIN_MEAN_SQUARE = 256.0 * 256.0;
static const double V8_ANSAM_MIN_CARRIER_SHARE = 0.75;
static const double V8_ANSAM_MAX_CARRIER_SHARE = 1.10;
/* A 0.2-depth envelope places 0.01 of carrier power in each sideband. */
static const double V8_ANSAM_MIN_MODULATION_RATIO = 0.008;
static const double V8_ANSAM_MAX_MODULATION_RATIO = 0.050;

static bool generator_is_valid(
	const struct v8_ansam_generator *generator)
{
	return generator != NULL && generator->average_amplitude > 0 &&
	       generator->average_amplitude <=
		       V8_ANSAM_MAX_AVERAGE_AMPLITUDE &&
	       generator->carrier_phase_step ==
		       phase_step_u32(
			       V8_ANSAM_SAMPLE_RATE,
			       V8_ANSAM_CARRIER_HZ, 1) &&
	       generator->modulation_phase_step ==
		       phase_step_u32(
			       V8_ANSAM_SAMPLE_RATE,
			       V8_ANSAM_MODULATION_HZ, 1) &&
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
		.carrier_phase_step =
			phase_step_u32(
				V8_ANSAM_SAMPLE_RATE,
				V8_ANSAM_CARRIER_HZ, 1),
		.modulation_phase_step =
			phase_step_u32(
				V8_ANSAM_SAMPLE_RATE,
				V8_ANSAM_MODULATION_HZ, 1),
		.samples_until_reversal =
			phase_reversals ?
				V8_ANSAM_REVERSAL_SAMPLES : 0,
		.average_amplitude = average_amplitude,
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

	if (!generator_is_valid(generator) || (samples == NULL && count != 0))
		return V8_ANSAM_INVALID_ARGUMENT;
	next = *generator;
	for (size_t index = 0; index < count; ++index) {
		double carrier = sin(phase_radians_u32(next.carrier_phase));
		double envelope =
			1.0 + V8_ANSAM_MODULATION_DEPTH *
				      cos(phase_radians_u32(
					      next.modulation_phase));

		samples[index] = (int16_t)lround(
			(double)next.average_amplitude * envelope * carrier);
		next.carrier_phase += next.carrier_phase_step;
		next.modulation_phase += next.modulation_phase_step;
		if (next.phase_reversals &&
		    --next.samples_until_reversal == 0) {
			next.carrier_phase += UINT32_C(0x80000000);
			next.samples_until_reversal =
				V8_ANSAM_REVERSAL_SAMPLES;
		}
	}
	*generator = next;
	return V8_ANSAM_OK;
}

static struct v8_ansam_projection projection(unsigned int frequency)
{
	return (struct v8_ansam_projection){
		.phase_step = phase_step_u32(
			V8_ANSAM_SAMPLE_RATE, frequency, 1),
	};
}

static bool projection_is_valid(
	const struct v8_ansam_projection *projection,
	unsigned int frequency)
{
	return projection != NULL && isfinite(projection->in_phase) &&
	       isfinite(projection->quadrature) &&
	       projection->phase_step == phase_step_u32(
		       V8_ANSAM_SAMPLE_RATE, frequency, 1);
}

static bool detector_is_valid(
	const struct v8_ansam_detector *detector)
{
	return detector != NULL &&
	       projection_is_valid(
		       &detector->lower_sideband,
		       V8_ANSAM_CARRIER_HZ -
			       V8_ANSAM_MODULATION_HZ) &&
	       projection_is_valid(
		       &detector->carrier,
		       V8_ANSAM_CARRIER_HZ) &&
	       projection_is_valid(
		       &detector->upper_sideband,
		       V8_ANSAM_CARRIER_HZ +
			       V8_ANSAM_MODULATION_HZ) &&
	       isfinite(detector->line_energy) && detector->line_energy >= 0.0 &&
	       isfinite(detector->carrier_share) &&
	       detector->carrier_share >= 0.0 &&
	       isfinite(detector->modulation_ratio) &&
	       detector->modulation_ratio >= 0.0 &&
	       detector->sample_count <
		       V8_ANSAM_DETECTOR_WINDOW_SAMPLES &&
	       detector->qualifying_windows <=
		       V8_ANSAM_REQUIRED_WINDOWS &&
	       detector->missed_windows <= 1 &&
	       (detector->qualifying_windows != 0 ||
		detector->missed_windows == 0) &&
	       detector->detected ==
		       (detector->qualifying_windows ==
			V8_ANSAM_REQUIRED_WINDOWS) &&
	       (!detector->detected || detector->missed_windows == 0) &&
	       (detector->measurement_ready ||
		(detector->carrier_share == 0.0 &&
		 detector->modulation_ratio == 0.0));
}

enum v8_ansam_result v8_ansam_detector_init(
	struct v8_ansam_detector *detector)
{
	struct v8_ansam_detector initialized = {
		.lower_sideband = projection(
			V8_ANSAM_CARRIER_HZ -
			V8_ANSAM_MODULATION_HZ),
		.carrier = projection(V8_ANSAM_CARRIER_HZ),
		.upper_sideband = projection(
			V8_ANSAM_CARRIER_HZ +
			V8_ANSAM_MODULATION_HZ),
	};

	if (detector == NULL || !detector_is_valid(&initialized))
		return V8_ANSAM_INVALID_ARGUMENT;
	*detector = initialized;
	return V8_ANSAM_OK;
}

void v8_ansam_detector_reset(
	struct v8_ansam_detector *detector)
{
	if (detector != NULL)
		(void)v8_ansam_detector_init(detector);
}

static void project_sample(
	struct v8_ansam_projection *projection, int16_t sample)
{
	double radians = phase_radians_u32(projection->phase);

	projection->in_phase += (double)sample * cos(radians);
	projection->quadrature += (double)sample * sin(radians);
	projection->phase += projection->phase_step;
}

static double projection_power(
	const struct v8_ansam_projection *projection)
{
	return projection->in_phase * projection->in_phase +
	       projection->quadrature * projection->quadrature;
}

static void reset_window(struct v8_ansam_detector *detector)
{
	detector->lower_sideband.in_phase = 0.0;
	detector->lower_sideband.quadrature = 0.0;
	detector->carrier.in_phase = 0.0;
	detector->carrier.quadrature = 0.0;
	detector->upper_sideband.in_phase = 0.0;
	detector->upper_sideband.quadrature = 0.0;
	detector->line_energy = 0.0;
	detector->sample_count = 0;
}

static void evaluate_window(struct v8_ansam_detector *detector)
{
	double carrier_power = projection_power(&detector->carrier);
	double sideband_power =
		projection_power(&detector->lower_sideband) +
		projection_power(&detector->upper_sideband);
	double mean_square =
		detector->line_energy /
		V8_ANSAM_DETECTOR_WINDOW_SAMPLES;
	bool qualifies = false;

	detector->carrier_share = 0.0;
	detector->modulation_ratio = 0.0;
	if (carrier_power > 0.0 && detector->line_energy > 0.0) {
		detector->carrier_share =
			2.0 * carrier_power /
			((double)V8_ANSAM_DETECTOR_WINDOW_SAMPLES *
			 detector->line_energy);
		detector->modulation_ratio = sideband_power / carrier_power;
		qualifies = mean_square >= V8_ANSAM_MIN_MEAN_SQUARE &&
			    detector->carrier_share >=
				    V8_ANSAM_MIN_CARRIER_SHARE &&
			    detector->carrier_share <=
				    V8_ANSAM_MAX_CARRIER_SHARE &&
			    detector->modulation_ratio >=
				    V8_ANSAM_MIN_MODULATION_RATIO &&
			    detector->modulation_ratio <=
				    V8_ANSAM_MAX_MODULATION_RATIO;
	}
	detector->measurement_ready = true;
	if (!detector->detected) {
		if (qualifies) {
			detector->qualifying_windows++;
			detector->missed_windows = 0;
			if (detector->qualifying_windows ==
			    V8_ANSAM_REQUIRED_WINDOWS)
				detector->detected = true;
		} else if (detector->qualifying_windows != 0 &&
			   detector->missed_windows == 0) {
			/* A prescribed 450 ms phase reversal may disrupt one
			 * coherent window. Keep the preceding evidence for only
			 * this single bounded miss. */
			detector->missed_windows = 1;
		} else {
			detector->qualifying_windows = 0;
			detector->missed_windows = 0;
		}
	}
	reset_window(detector);
}

enum v8_ansam_result v8_ansam_detect(
	struct v8_ansam_detector *detector,
	const int16_t *samples, size_t count, bool *detected)
{
	struct v8_ansam_detector next;

	if (!detector_is_valid(detector) || detected == NULL ||
	    (samples == NULL && count != 0))
		return V8_ANSAM_INVALID_ARGUMENT;
	next = *detector;
	for (size_t index = 0; index < count; ++index) {
		double sample = samples[index];

		project_sample(&next.lower_sideband, samples[index]);
		project_sample(&next.carrier, samples[index]);
		project_sample(&next.upper_sideband, samples[index]);
		next.line_energy += sample * sample;
		next.sample_count++;
		if (next.sample_count ==
		    V8_ANSAM_DETECTOR_WINDOW_SAMPLES)
			evaluate_window(&next);
	}
	if (!detector_is_valid(&next))
		return V8_ANSAM_INVALID_ARGUMENT;
	*detector = next;
	*detected = next.detected;
	return V8_ANSAM_OK;
}

struct v8_ansam_measurement v8_ansam_measure(
	const struct v8_ansam_detector *detector)
{
	if (!detector_is_valid(detector))
		return (struct v8_ansam_measurement){ 0 };
	return (struct v8_ansam_measurement){
		.carrier_share = detector->carrier_share,
		.modulation_ratio = detector->modulation_ratio,
		.qualifying_windows = detector->qualifying_windows,
		.ready = detector->measurement_ready,
		.detected = detector->detected,
	};
}

const char *v8_ansam_result_name(
	enum v8_ansam_result result)
{
	switch (result) {
	case V8_ANSAM_OK:
		return "ok";
	case V8_ANSAM_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
