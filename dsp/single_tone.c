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

static bool detector_profile_is_valid(
	const struct single_tone_detector_profile *profile)
{
	return profile != NULL && profile->sample_rate_hz > 2 &&
	       profile->frequency_hz > 0 &&
	       profile->frequency_hz < profile->sample_rate_hz / 2 &&
	       profile->window_samples >= 2 &&
	       profile->window_samples <= SINGLE_TONE_MAX_WINDOW_SAMPLES &&
	       profile->acquire_samples > 0 &&
	       profile->acquire_samples <=
		       UINT16_MAX - profile->window_samples + 2 &&
	       profile->loss_samples > 0 &&
	       isfinite(profile->minimum_line_power) &&
	       profile->minimum_line_power >= 0.0 &&
	       profile->minimum_line_power <= SINGLE_TONE_MAX_POWER &&
	       isfinite(profile->minimum_carrier_share) &&
	       profile->minimum_carrier_share > 0.0 &&
	       isfinite(profile->maximum_carrier_share) &&
	       profile->maximum_carrier_share >=
		       profile->minimum_carrier_share &&
	       profile->maximum_carrier_share <= 2.000001;
}

static bool observation_is_valid(
	const struct single_tone_observation *observation)
{
	/* Evaluate every bound so the fixed-size history scan can vectorize. */
	/* Bitwise conjunction is intentional: no per-field short circuit. */
	return (observation->in_phase >= -SINGLE_TONE_MAX_SAMPLE) &
	       (observation->in_phase <= SINGLE_TONE_MAX_SAMPLE) &
	       (observation->quadrature >= -SINGLE_TONE_MAX_SAMPLE) &
	       (observation->quadrature <= SINGLE_TONE_MAX_SAMPLE) &
	       (observation->energy >= 0.0) &
	       (observation->energy <= SINGLE_TONE_MAX_POWER);
}

static bool history_is_valid(
	const struct single_tone_detector *detector)
{
	unsigned int valid = 1;

	for (size_t index = 0;
	     index < SINGLE_TONE_MAX_WINDOW_SAMPLES; ++index)
		valid &= observation_is_valid(&detector->history[index]);
	return valid != 0;
}

static double retained_power(
	const struct single_tone_detector *detector)
{
	return detector->reference_in_phase * detector->reference_in_phase +
	       detector->reference_quadrature * detector->reference_quadrature;
}

bool single_tone_detector_is_valid(
	const struct single_tone_detector *detector)
{
	double maximum_projection;
	double reference_power;
	uint16_t window_samples;

	if (detector == NULL ||
	    !detector_profile_is_valid(&detector->profile))
		return false;
	window_samples = detector->profile.window_samples;
	maximum_projection = SINGLE_TONE_MAX_SAMPLE * window_samples;
	if (detector->phase_step != phase_step_u32(
		    detector->profile.sample_rate_hz,
		    detector->profile.frequency_hz, 1) ||
	    !history_is_valid(detector) ||
	    detector->history_index >= window_samples ||
	    detector->history_count > window_samples ||
	    (detector->history_count < window_samples &&
	     detector->history_index != detector->history_count) ||
	    !isfinite(detector->in_phase) ||
	    fabs(detector->in_phase) > maximum_projection ||
	    !isfinite(detector->quadrature) ||
	    fabs(detector->quadrature) > maximum_projection ||
	    !isfinite(detector->line_energy) || detector->line_energy < 0.0 ||
	    detector->line_energy > SINGLE_TONE_MAX_POWER * window_samples ||
	    !isfinite(detector->reference_in_phase) ||
	    fabs(detector->reference_in_phase) > maximum_projection ||
	    !isfinite(detector->reference_quadrature) ||
	    fabs(detector->reference_quadrature) > maximum_projection ||
	    detector->qualifying_samples > detector->profile.acquire_samples ||
	    detector->missing_samples >= detector->profile.loss_samples ||
	    (!detector->tone_present &&
	     (detector->qualifying_samples == detector->profile.acquire_samples ||
	      detector->missing_samples != 0)) ||
	    (detector->tone_present &&
	     detector->qualifying_samples != detector->profile.acquire_samples) ||
	    detector->reversal_pending !=
		    (detector->reversal_delay_samples != 0) ||
	    (detector->rejected_reversal_active &&
	     (!detector->tone_present || detector->reversal_pending ||
	      detector->interruption != SINGLE_TONE_UNINTERRUPTED)) ||
	    detector->interruption > SINGLE_TONE_REACQUIRING)
		return false;
	reference_power = retained_power(detector);
	switch (detector->interruption) {
	case SINGLE_TONE_UNINTERRUPTED:
		return (!detector->reversal_pending ||
			(detector->tone_present &&
			 detector->reversal_delay_samples >= window_samples / 2 &&
			 detector->reversal_delay_samples <= window_samples * 2)) &&
		       (detector->tone_present ? reference_power > 0.0 :
			(detector->profile.retain_reference_on_loss ||
			 reference_power == 0.0));
	case SINGLE_TONE_SUPPRESSING:
		return !detector->tone_present &&
		       detector->qualifying_samples == 0 &&
		       !detector->reversal_pending;
	case SINGLE_TONE_REACQUIRING:
		return !detector->tone_present && reference_power > 0.0 &&
		       detector->reversal_delay_samples <=
			       UINT16_MAX - window_samples -
			       detector->profile.acquire_samples + 2;
	}
	return false;
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
	const struct single_tone_detector_profile profile = {
		.minimum_line_power = SINGLE_TONE_MIN_LINE_POWER,
		.minimum_carrier_share = minimum_carrier_share,
		.maximum_carrier_share = SINGLE_TONE_MAX_CARRIER_SHARE,
		.sample_rate_hz = SINGLE_TONE_SAMPLE_RATE,
		.frequency_hz = (uint16_t)frequency_hz,
		.window_samples = SINGLE_TONE_WINDOW_SAMPLES,
		.acquire_samples = SINGLE_TONE_ACQUIRE_SAMPLES,
		.loss_samples = SINGLE_TONE_LOSS_SAMPLES,
	};

	if (frequency_hz > UINT16_MAX)
		return SINGLE_TONE_INVALID_ARGUMENT;
	return single_tone_detector_init_profile(detector, &profile);
}

enum single_tone_result single_tone_detector_init_profile(
	struct single_tone_detector *detector,
	const struct single_tone_detector_profile *profile)
{
	struct single_tone_detector initialized = { 0 };

	if (detector == NULL || !detector_profile_is_valid(profile))
		return SINGLE_TONE_INVALID_ARGUMENT;
	initialized.profile = *profile;
	initialized.phase_step = phase_step_u32(
		profile->sample_rate_hz, profile->frequency_hz, 1);
	if (!single_tone_detector_is_valid(&initialized))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*detector = initialized;
	return SINGLE_TONE_OK;
}

enum single_tone_result single_tone_detector_reset(
	struct single_tone_detector *detector)
{
	struct single_tone_detector_profile profile;

	if (!single_tone_detector_is_valid(detector))
		return SINGLE_TONE_INVALID_ARGUMENT;
	profile = detector->profile;
	return single_tone_detector_init_profile(detector, &profile);
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
	if (detector->history_count == detector->profile.window_samples) {
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
		detector->profile.window_samples;
}

static struct single_tone_measurement measure_valid_detector(
	const struct single_tone_detector *detector)
{
	double count = detector->profile.window_samples;
	double projection_power =
		detector->in_phase * detector->in_phase +
		detector->quadrature * detector->quadrature;
	struct single_tone_measurement measurement = {
		.ready = detector->history_count ==
			 detector->profile.window_samples,
		.tone_present = detector->tone_present,
	};

	if (!measurement.ready)
		return measurement;
	measurement.line_power = detector->line_energy / count;
	/* A complex projection contains half the sinusoid amplitude; convert it
	 * to mean-square carrier power before comparing it with total energy. */
	measurement.carrier_power =
		2.0 * projection_power / (count * count);
	measurement.carrier_share = measurement.line_power > 0.0 ?
		measurement.carrier_power / measurement.line_power : 0.0;
	return measurement;
}

static bool measurement_qualifies(
	const struct single_tone_detector *detector,
	const struct single_tone_measurement *measurement)
{
	return measurement->ready &&
	       measurement->line_power >= detector->profile.minimum_line_power &&
	       measurement->carrier_share >=
		       detector->profile.minimum_carrier_share &&
	       measurement->carrier_share <=
		       detector->profile.maximum_carrier_share;
}

static void clear_lock(struct single_tone_detector *detector)
{
	if (!detector->profile.retain_reference_on_loss) {
		detector->reference_in_phase = 0.0;
		detector->reference_quadrature = 0.0;
	}
	detector->qualifying_samples = 0;
	detector->missing_samples = 0;
	detector->reversal_delay_samples = 0;
	detector->interruption = SINGLE_TONE_UNINTERRUPTED;
	detector->tone_present = false;
	detector->reversal_pending = false;
	detector->rejected_reversal_active = false;
}

static double phase_dot(const struct single_tone_detector *detector)
{
	return detector->in_phase * detector->reference_in_phase +
	       detector->quadrature * detector->reference_quadrature;
}

static double phase_cosine(
	const struct single_tone_detector *detector, double dot)
{
	double current_power = detector->in_phase * detector->in_phase +
			       detector->quadrature * detector->quadrature;
	double reference_power = retained_power(detector);

	return current_power > 0.0 && reference_power > 0.0 ?
		dot / sqrt(current_power * reference_power) : 1.0;
}

static enum single_tone_event_kind finish_acquisition(
	struct single_tone_detector *detector,
	uint16_t *reversal_delay_samples)
{
	bool reversed = detector->interruption == SINGLE_TONE_REACQUIRING &&
			phase_dot(detector) < 0.0;
	enum single_tone_event_kind kind = reversed ?
		SINGLE_TONE_PHASE_REVERSED : SINGLE_TONE_STARTED;

	if (reversed)
		*reversal_delay_samples = (uint16_t)(
			detector->reversal_delay_samples +
			detector->profile.window_samples +
			detector->profile.acquire_samples - 2);
	detector->tone_present = true;
	detector->interruption = SINGLE_TONE_UNINTERRUPTED;
	/* The retained phase classifies the boundary.  Lock the following
	 * carrier to its measured phase so channel rotation during an INFO or
	 * silence interval cannot hide its next physical reversal. */
	detector->reference_in_phase = detector->in_phase;
	detector->reference_quadrature = detector->quadrature;
	detector->reversal_pending = false;
	detector->reversal_delay_samples = 0;
	return kind;
}

static enum single_tone_event_kind acquire_or_release(
	struct single_tone_detector *detector, bool qualifies,
	uint16_t *reversal_delay_samples)
{
	if (!detector->tone_present) {
		if (!qualifies) {
			detector->qualifying_samples = 0;
			return SINGLE_TONE_NO_EVENT;
		}
		if (++detector->qualifying_samples <
		    detector->profile.acquire_samples)
			return SINGLE_TONE_NO_EVENT;
		return finish_acquisition(detector, reversal_delay_samples);
	}
	if (qualifies) {
		detector->missing_samples = 0;
		return SINGLE_TONE_NO_EVENT;
	}
	if (++detector->missing_samples < detector->profile.loss_samples)
		return SINGLE_TONE_NO_EVENT;
	clear_lock(detector);
	return SINGLE_TONE_ENDED;
}

static bool observe_reversal(
	struct single_tone_detector *detector, bool qualifies,
	uint16_t *delay_samples)
{
	double dot = phase_dot(detector);
	double cosine = phase_cosine(detector, dot);
	uint16_t window_samples = detector->profile.window_samples;

	if (detector->rejected_reversal_active) {
		if (qualifies && cosine > 0.5)
			detector->rejected_reversal_active = false;
		return false;
	}

	if (!detector->reversal_pending && dot < 0.0) {
		detector->reversal_pending = true;
		detector->reversal_delay_samples =
			window_samples / 2;
	} else if (detector->reversal_pending &&
		   detector->reversal_delay_samples < window_samples * 2) {
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
			      window_samples * 2)) {
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
	struct single_tone_measurement measurement;
	bool was_present;
	bool qualifies;

	if (!single_tone_detector_is_valid(detector) || event == NULL ||
	    detector->interruption == SINGLE_TONE_SUPPRESSING)
		return SINGLE_TONE_INVALID_ARGUMENT;
	next = *detector;
	was_present = next.tone_present;
	add_observation(&next, sample);
	measurement = measure_valid_detector(&next);
	qualifies = measurement_qualifies(&next, &measurement);
	emitted.kind = acquire_or_release(
		&next, qualifies, &emitted.reversal_delay_samples);
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

static void clear_observations(struct single_tone_detector *detector)
{
	*detector = (struct single_tone_detector){
		.profile = detector->profile,
		.reference_in_phase = detector->reference_in_phase,
		.reference_quadrature = detector->reference_quadrature,
		.phase = detector->phase,
		.phase_step = detector->phase_step,
		.interruption = detector->interruption,
	};
}

enum single_tone_result single_tone_detector_suppress_sample(
	struct single_tone_detector *detector)
{
	struct single_tone_detector next;

	if (!single_tone_detector_is_valid(detector))
		return SINGLE_TONE_INVALID_ARGUMENT;
	next = *detector;
	if (next.interruption != SINGLE_TONE_SUPPRESSING) {
		clear_observations(&next);
		next.interruption = SINGLE_TONE_SUPPRESSING;
	}
	next.phase += next.phase_step;
	if (!single_tone_detector_is_valid(&next))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*detector = next;
	return SINGLE_TONE_OK;
}

enum single_tone_result single_tone_detector_resume(
	struct single_tone_detector *detector,
	uint16_t handoff_delay_samples)
{
	struct single_tone_detector next;
	uint32_t acquisition_delay;

	if (!single_tone_detector_is_valid(detector) ||
	    detector->interruption != SINGLE_TONE_SUPPRESSING)
		return SINGLE_TONE_INVALID_ARGUMENT;
	acquisition_delay = detector->profile.window_samples +
			    detector->profile.acquire_samples - 2u;
	if (handoff_delay_samples > UINT16_MAX - acquisition_delay)
		return SINGLE_TONE_INVALID_ARGUMENT;
	next = *detector;
	clear_observations(&next);
	if (retained_power(&next) > 0.0) {
		next.interruption = SINGLE_TONE_REACQUIRING;
		next.reversal_pending = handoff_delay_samples != 0;
		next.reversal_delay_samples = handoff_delay_samples;
	} else {
		next.interruption = SINGLE_TONE_UNINTERRUPTED;
	}
	if (!single_tone_detector_is_valid(&next))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*detector = next;
	return SINGLE_TONE_OK;
}

enum single_tone_result single_tone_detector_reject_reversal(
	struct single_tone_detector *detector,
	const struct single_tone_event *event)
{
	struct single_tone_detector next;

	if (!single_tone_detector_is_valid(detector) || event == NULL ||
	    event->kind != SINGLE_TONE_PHASE_REVERSED ||
	    event->reversal_delay_samples == 0 ||
	    detector->rejected_reversal_active)
		return SINGLE_TONE_INVALID_ARGUMENT;
	next = *detector;
	next.reference_in_phase = -next.reference_in_phase;
	next.reference_quadrature = -next.reference_quadrature;
	next.rejected_reversal_active = true;
	if (!single_tone_detector_is_valid(&next))
		return SINGLE_TONE_INVALID_ARGUMENT;
	*detector = next;
	return SINGLE_TONE_OK;
}

struct single_tone_measurement
single_tone_detector_measure(
	const struct single_tone_detector *detector)
{
	if (!single_tone_detector_is_valid(detector))
		return (struct single_tone_measurement){ 0 };
	return measure_valid_detector(detector);
}
