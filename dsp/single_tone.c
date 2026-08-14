// SPDX-License-Identifier: BSD-3-Clause

#include "single_tone.h"

#include "phase.h"

#include <limits.h>
#include <math.h>

static const double SINGLE_TONE_MIN_LINE_POWER = 256.0 * 256.0;
static const double SINGLE_TONE_DEFAULT_MIN_CARRIER_SHARE = 0.70;
static const double SINGLE_TONE_MAX_CARRIER_SHARE = 1.20;
static const double SINGLE_TONE_REVERSAL_COSINE = -0.80;
static const double SINGLE_TONE_REFERENCE_TRACKING = 1.0 / 64.0;
static const double SINGLE_TONE_MAX_SAMPLE = -(double)INT16_MIN;
static const double SINGLE_TONE_MAX_POWER =
	SINGLE_TONE_MAX_SAMPLE * SINGLE_TONE_MAX_SAMPLE;
static const double SINGLE_TONE_MAX_PROJECTION =
	SINGLE_TONE_MAX_SAMPLE * SINGLE_TONE_WINDOW_SAMPLES;
static const double SINGLE_TONE_MAX_WINDOW_ENERGY =
	SINGLE_TONE_MAX_POWER * SINGLE_TONE_WINDOW_SAMPLES;

static bool frequency_is_valid(unsigned int frequency_hz)
{
	return frequency_hz > 0 &&
	       frequency_hz < SINGLE_TONE_SAMPLE_RATE / 2;
}

bool single_tone_transmitter_is_valid(
	const struct single_tone_transmitter *transmitter)
{
	return transmitter != NULL &&
	       frequency_is_valid(transmitter->frequency_hz) &&
	       transmitter->phase_step == phase_step_u32(
		       SINGLE_TONE_SAMPLE_RATE,
		       transmitter->frequency_hz, 1) &&
	       transmitter->amplitude > 0;
}

enum single_tone_result
single_tone_transmitter_init(
	struct single_tone_transmitter *transmitter,
	unsigned int frequency_hz, int16_t amplitude)
{
	struct single_tone_transmitter initialized = {
		.phase_step = phase_step_u32(
			SINGLE_TONE_SAMPLE_RATE, frequency_hz, 1),
		.frequency_hz = (uint16_t)frequency_hz,
		.amplitude = amplitude,
	};

	if (transmitter == NULL || !frequency_is_valid(frequency_hz) ||
	    !single_tone_transmitter_is_valid(&initialized))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SINGLE_TONE_OK;
}

enum single_tone_result
single_tone_transmitter_reverse(
	struct single_tone_transmitter *transmitter)
{
	if (!single_tone_transmitter_is_valid(transmitter))
		return SINGLE_TONE_INVALID_ARGUMENT;
	transmitter->phase += UINT32_C(0x80000000);
	return SINGLE_TONE_OK;
}

enum single_tone_result
single_tone_transmitter_sample(
	struct single_tone_transmitter *transmitter,
	int16_t *sample)
{
	if (!single_tone_transmitter_is_valid(transmitter) ||
	    sample == NULL)
		return SINGLE_TONE_INVALID_ARGUMENT;
	*sample = (int16_t)lround(
		(double)transmitter->amplitude *
		sin(phase_radians_u32(transmitter->phase)));
	transmitter->phase += transmitter->phase_step;
	return SINGLE_TONE_OK;
}

bool single_tone_detector_is_valid(
	const struct single_tone_detector *detector)
{
	double reference_power;

	if (detector == NULL || !frequency_is_valid(detector->frequency_hz) ||
	    detector->phase_step != phase_step_u32(
		    SINGLE_TONE_SAMPLE_RATE, detector->frequency_hz, 1) ||
	    detector->history_index >=
		    SINGLE_TONE_WINDOW_SAMPLES ||
	    detector->history_count > SINGLE_TONE_WINDOW_SAMPLES ||
	    detector->measurement_ready !=
		    (detector->history_count ==
		     SINGLE_TONE_WINDOW_SAMPLES) ||
	    !isfinite(detector->in_phase) ||
	    fabs(detector->in_phase) > SINGLE_TONE_MAX_PROJECTION ||
	    !isfinite(detector->quadrature) ||
	    fabs(detector->quadrature) > SINGLE_TONE_MAX_PROJECTION ||
	    !isfinite(detector->line_energy) || detector->line_energy < 0.0 ||
	    detector->line_energy > SINGLE_TONE_MAX_WINDOW_ENERGY ||
	    !isfinite(detector->reference_in_phase) ||
	    fabs(detector->reference_in_phase) > SINGLE_TONE_MAX_PROJECTION ||
	    !isfinite(detector->reference_quadrature) ||
	    fabs(detector->reference_quadrature) > SINGLE_TONE_MAX_PROJECTION ||
	    !isfinite(detector->line_power) || detector->line_power < 0.0 ||
	    detector->line_power > SINGLE_TONE_MAX_POWER ||
	    !isfinite(detector->carrier_power) ||
	    detector->carrier_power < 0.0 ||
	    detector->carrier_power > 2.000001 * SINGLE_TONE_MAX_POWER ||
	    !isfinite(detector->carrier_share) ||
	    detector->carrier_share < 0.0 || detector->carrier_share > 2.000001 ||
	    !isfinite(detector->minimum_carrier_share) ||
	    detector->minimum_carrier_share <= 0.0 ||
	    detector->minimum_carrier_share >
		    SINGLE_TONE_MAX_CARRIER_SHARE ||
	    detector->qualifying_samples >
		    SINGLE_TONE_ACQUIRE_SAMPLES ||
	    detector->missing_samples >= SINGLE_TONE_LOSS_SAMPLES ||
	    (!detector->tone_present &&
	     (detector->qualifying_samples ==
		      SINGLE_TONE_ACQUIRE_SAMPLES ||
	      detector->missing_samples != 0 || detector->reversal_pending)) ||
	    (detector->tone_present &&
	     detector->qualifying_samples !=
		     SINGLE_TONE_ACQUIRE_SAMPLES) ||
	    detector->reversal_pending !=
		    (detector->reversal_delay_samples != 0) ||
	    (detector->reversal_pending &&
	     (detector->reversal_delay_samples <
		      SINGLE_TONE_WINDOW_SAMPLES / 2 ||
	      detector->reversal_delay_samples >=
		      SINGLE_TONE_WINDOW_SAMPLES * 2)))
		return false;
	reference_power =
		detector->reference_in_phase * detector->reference_in_phase +
		detector->reference_quadrature * detector->reference_quadrature;
	return detector->tone_present ? reference_power > 0.0 :
				       reference_power == 0.0;
}

enum single_tone_result single_tone_detector_init(
	struct single_tone_detector *detector,
	unsigned int frequency_hz)
{
	return single_tone_detector_init_with_minimum_carrier_share(
		detector, frequency_hz,
		SINGLE_TONE_DEFAULT_MIN_CARRIER_SHARE);
}

enum single_tone_result
single_tone_detector_init_with_minimum_carrier_share(
	struct single_tone_detector *detector,
	unsigned int frequency_hz, double minimum_carrier_share)
{
	struct single_tone_detector initialized = {
		.minimum_carrier_share = minimum_carrier_share,
		.phase_step = phase_step_u32(
			SINGLE_TONE_SAMPLE_RATE, frequency_hz, 1),
		.frequency_hz = (uint16_t)frequency_hz,
	};

	if (detector == NULL || !frequency_is_valid(frequency_hz) ||
	    !isfinite(minimum_carrier_share) ||
	    !single_tone_detector_is_valid(&initialized))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*detector = initialized;
	return SINGLE_TONE_OK;
}

enum single_tone_result single_tone_detector_reset(
	struct single_tone_detector *detector)
{
	unsigned int frequency_hz;
	double minimum_carrier_share;

	if (!single_tone_detector_is_valid(detector))
		return SINGLE_TONE_INVALID_ARGUMENT;
	frequency_hz = detector->frequency_hz;
	minimum_carrier_share = detector->minimum_carrier_share;
	return single_tone_detector_init_with_minimum_carrier_share(
		detector, frequency_hz, minimum_carrier_share);
}

static void add_observation(
	struct single_tone_detector *detector, int16_t sample)
{
	struct single_tone_observation *old =
		&detector->history[detector->history_index];
	double radians = phase_radians_u32(detector->phase);
	struct single_tone_observation current = {
		.in_phase = (double)sample * cos(radians),
		.quadrature = (double)sample * sin(radians),
		.energy = (double)sample * sample,
	};

	detector->phase += detector->phase_step;
	if (detector->history_count ==
	    SINGLE_TONE_WINDOW_SAMPLES) {
		detector->in_phase -= old->in_phase;
		detector->quadrature -= old->quadrature;
		detector->line_energy -= old->energy;
	} else {
		detector->history_count++;
	}
	detector->in_phase += current.in_phase;
	detector->quadrature += current.quadrature;
	detector->line_energy += current.energy;
	*old = current;
	detector->history_index =
		(detector->history_index + 1u) %
		SINGLE_TONE_WINDOW_SAMPLES;
	detector->measurement_ready =
		detector->history_count == SINGLE_TONE_WINDOW_SAMPLES;
}

static bool measurement_qualifies(
	struct single_tone_detector *detector)
{
	double count = SINGLE_TONE_WINDOW_SAMPLES;
	double projection_power =
		detector->in_phase * detector->in_phase +
		detector->quadrature * detector->quadrature;

	detector->line_power = detector->line_energy / count;
	detector->carrier_power =
		2.0 * projection_power / (count * count);
	detector->carrier_share = detector->line_power > 0.0 ?
					  detector->carrier_power /
						  detector->line_power :
					  0.0;
	return detector->line_power >= SINGLE_TONE_MIN_LINE_POWER &&
	       detector->carrier_share >= detector->minimum_carrier_share &&
	       detector->carrier_share <= SINGLE_TONE_MAX_CARRIER_SHARE;
}

static void clear_lock(struct single_tone_detector *detector)
{
	detector->reference_in_phase = 0.0;
	detector->reference_quadrature = 0.0;
	detector->qualifying_samples = 0;
	detector->missing_samples = 0;
	detector->reversal_delay_samples = 0;
	detector->tone_present = false;
	detector->reversal_pending = false;
}

static enum single_tone_event_kind acquire_or_release(
	struct single_tone_detector *detector, bool qualifies)
{
	if (!detector->tone_present) {
		if (!qualifies) {
			detector->qualifying_samples = 0;
			return SINGLE_TONE_NO_EVENT;
		}
		if (++detector->qualifying_samples <
		    SINGLE_TONE_ACQUIRE_SAMPLES)
			return SINGLE_TONE_NO_EVENT;
		detector->tone_present = true;
		detector->reference_in_phase = detector->in_phase;
		detector->reference_quadrature = detector->quadrature;
		return SINGLE_TONE_STARTED;
	}
	if (qualifies) {
		detector->missing_samples = 0;
		return SINGLE_TONE_NO_EVENT;
	}
	if (++detector->missing_samples < SINGLE_TONE_LOSS_SAMPLES)
		return SINGLE_TONE_NO_EVENT;
	clear_lock(detector);
	return SINGLE_TONE_ENDED;
}

static bool observe_reversal(
	struct single_tone_detector *detector, bool qualifies,
	uint16_t *delay_samples)
{
	double current_power =
		detector->in_phase * detector->in_phase +
		detector->quadrature * detector->quadrature;
	double reference_power =
		detector->reference_in_phase * detector->reference_in_phase +
		detector->reference_quadrature * detector->reference_quadrature;
	double dot = detector->in_phase * detector->reference_in_phase +
		     detector->quadrature * detector->reference_quadrature;
	double cosine = current_power > 0.0 && reference_power > 0.0 ?
				dot / sqrt(current_power * reference_power) :
				1.0;

	if (!detector->reversal_pending && dot < 0.0) {
		detector->reversal_pending = true;
		detector->reversal_delay_samples =
			SINGLE_TONE_WINDOW_SAMPLES / 2;
	} else if (detector->reversal_pending) {
		detector->reversal_delay_samples++;
	}
	if (detector->reversal_pending && qualifies &&
	    cosine <= SINGLE_TONE_REVERSAL_COSINE) {
		*delay_samples = detector->reversal_delay_samples;
		detector->reference_in_phase = -detector->reference_in_phase;
		detector->reference_quadrature =
			-detector->reference_quadrature;
		detector->reversal_pending = false;
		detector->reversal_delay_samples = 0;
		return true;
	}
	if (detector->reversal_pending &&
	    (cosine > 0.25 || detector->reversal_delay_samples ==
			      SINGLE_TONE_WINDOW_SAMPLES * 2)) {
		detector->reversal_pending = false;
		detector->reversal_delay_samples = 0;
	}
	if (!detector->reversal_pending && qualifies && cosine > 0.5) {
		detector->reference_in_phase +=
			(detector->in_phase - detector->reference_in_phase) *
			SINGLE_TONE_REFERENCE_TRACKING;
		detector->reference_quadrature +=
			(detector->quadrature - detector->reference_quadrature) *
			SINGLE_TONE_REFERENCE_TRACKING;
	}
	return false;
}

enum single_tone_result single_tone_detector_sample(
	struct single_tone_detector *detector, int16_t sample,
	struct single_tone_event *event)
{
	struct single_tone_detector next;
	struct single_tone_event emitted = { 0 };
	bool was_present;
	bool qualifies = false;

	if (!single_tone_detector_is_valid(detector) || event == NULL)
		return SINGLE_TONE_INVALID_ARGUMENT;
	next = *detector;
	was_present = next.tone_present;
	add_observation(&next, sample);
	if (next.measurement_ready)
		qualifies = measurement_qualifies(&next);
	emitted.kind = acquire_or_release(&next, qualifies);
	if (was_present && next.tone_present &&
	    observe_reversal(
		    &next, qualifies, &emitted.reversal_delay_samples))
		emitted.kind = SINGLE_TONE_PHASE_REVERSED;
	if (!single_tone_detector_is_valid(&next))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*detector = next;
	*event = emitted;
	return SINGLE_TONE_OK;
}

struct single_tone_measurement
single_tone_detector_measure(
	const struct single_tone_detector *detector)
{
	if (!single_tone_detector_is_valid(detector))
		return (struct single_tone_measurement){ 0 };
	return (struct single_tone_measurement){
		.line_power = detector->line_power,
		.carrier_power = detector->carrier_power,
		.carrier_share = detector->carrier_share,
		.ready = detector->measurement_ready,
		.tone_present = detector->tone_present,
	};
}

const char *single_tone_result_name(
	enum single_tone_result result)
{
	switch (result) {
	case SINGLE_TONE_OK:
		return "ok";
	case SINGLE_TONE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
