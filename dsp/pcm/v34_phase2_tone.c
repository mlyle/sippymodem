// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_tone.h"

#include "phase.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_PHASE2_TONE_ANSWERING_CARRIER_GAIN =
	0.8912509381337456; /* -1 dB. */
static const double V34_PHASE2_TONE_MIN_LINE_POWER = 256.0 * 256.0;
static const double V34_PHASE2_TONE_MIN_CALLING_CARRIER_SHARE = 0.70;
static const double V34_PHASE2_TONE_MIN_ANSWERING_CARRIER_SHARE = 0.30;
static const double V34_PHASE2_TONE_MAX_CARRIER_SHARE = 1.20;
static const double V34_PHASE2_TONE_REVERSAL_CONFIRM_COSINE = -0.80;
static const double V34_PHASE2_TONE_REFERENCE_TRACKING = 1.0 / 64.0;
static const double V34_RETRAIN_SILENCE_MAX_LINE_POWER = 150000.0;
static const double V34_RETRAIN_TONE_A_MAX_CARRIER_SHARE = 0.60;
static const double V34_PHASE2_TONE_MAX_SAMPLE_MAGNITUDE =
	-(double)INT16_MIN;
static const double V34_PHASE2_TONE_MAX_SAMPLE_POWER =
	V34_PHASE2_TONE_MAX_SAMPLE_MAGNITUDE *
	V34_PHASE2_TONE_MAX_SAMPLE_MAGNITUDE;
static const double V34_PHASE2_TONE_MAX_PROJECTION =
	V34_PHASE2_TONE_MAX_SAMPLE_MAGNITUDE *
	V34_PHASE2_TONE_WINDOW_SAMPLES;
static const double V34_PHASE2_TONE_MAX_WINDOW_ENERGY =
	V34_PHASE2_TONE_MAX_SAMPLE_POWER *
	V34_PHASE2_TONE_WINDOW_SAMPLES;

_Static_assert(
	V34_RETRAIN_MIN_SILENCE_SAMPLES == 519 &&
	V34_RETRAIN_TONE_SAMPLES == 640,
	"V.34 retrain guards must retain their 8 kHz durations");

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static uint32_t local_carrier_hz(
	enum v34_station_role role)
{
	return role == V34_CALLING_STATION ?
		       V34_PHASE2_CALLING_CARRIER_HZ :
		       V34_PHASE2_ANSWERING_CARRIER_HZ;
}

static uint32_t remote_carrier_hz(
	enum v34_station_role local_role)
{
	return local_role == V34_CALLING_STATION ?
		       V34_PHASE2_ANSWERING_CARRIER_HZ :
		       V34_PHASE2_CALLING_CARRIER_HZ;
}

bool v34_phase2_tone_transmitter_is_valid(
	const struct v34_phase2_tone_transmitter *transmitter)
{
	if (transmitter == NULL || !role_is_valid(transmitter->local_role) ||
	    transmitter->carrier_amplitude <= 0 ||
	    transmitter->carrier_phase_step !=
		    phase_step_u32(
			    V34_PHASE2_SAMPLE_RATE,
			    local_carrier_hz(transmitter->local_role), 1))
		return false;
	if (transmitter->local_role ==
	    V34_CALLING_STATION)
		return transmitter->guard_amplitude == 0 &&
		       transmitter->guard_phase_step == 0;
	return transmitter->guard_amplitude > 0 &&
	       transmitter->guard_phase_step ==
		       phase_step_u32(
			       V34_PHASE2_SAMPLE_RATE,
			       V34_PHASE2_ANSWERING_GUARD_HZ, 1) &&
	       transmitter->carrier_amplitude <=
		       INT16_MAX - transmitter->guard_amplitude;
}

enum v34_phase2_tone_result
v34_phase2_tone_transmitter_init(
	struct v34_phase2_tone_transmitter *transmitter,
	enum v34_station_role local_role,
	int16_t nominal_amplitude)
{
	struct v34_phase2_tone_transmitter initialized = {
		.local_role = local_role,
	};
	long carrier_amplitude = nominal_amplitude;
	long guard_amplitude = 0;

	if (transmitter == NULL || !role_is_valid(local_role) ||
	    nominal_amplitude <= 0)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	if (local_role == V34_ANSWERING_STATION) {
		carrier_amplitude = lround(
			(double)nominal_amplitude *
			V34_PHASE2_TONE_ANSWERING_CARRIER_GAIN);
		guard_amplitude = nominal_amplitude;
		initialized.guard_phase_step = phase_step_u32(
			V34_PHASE2_SAMPLE_RATE,
			V34_PHASE2_ANSWERING_GUARD_HZ, 1);
	}
	if (carrier_amplitude <= 0 || guard_amplitude < 0 ||
	    carrier_amplitude + guard_amplitude > INT16_MAX)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	initialized.carrier_phase_step = phase_step_u32(
		V34_PHASE2_SAMPLE_RATE,
		local_carrier_hz(local_role), 1);
	initialized.carrier_amplitude = (int16_t)carrier_amplitude;
	initialized.guard_amplitude = (int16_t)guard_amplitude;
	if (!v34_phase2_tone_transmitter_is_valid(&initialized))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result
v34_phase2_tone_transmitter_reverse(
	struct v34_phase2_tone_transmitter *transmitter)
{
	if (!v34_phase2_tone_transmitter_is_valid(transmitter))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	transmitter->carrier_phase += UINT32_C(0x80000000);
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result
v34_phase2_tone_transmitter_sample(
	struct v34_phase2_tone_transmitter *transmitter,
	int16_t *sample)
{
	long mixed;

	if (!v34_phase2_tone_transmitter_is_valid(transmitter) ||
	    sample == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	mixed = lround(
		(double)transmitter->carrier_amplitude *
		sin(phase_radians_u32(transmitter->carrier_phase)));
	transmitter->carrier_phase += transmitter->carrier_phase_step;
	if (transmitter->guard_amplitude != 0) {
		mixed += lround(
			(double)transmitter->guard_amplitude *
			sin(phase_radians_u32(transmitter->guard_phase)));
		transmitter->guard_phase += transmitter->guard_phase_step;
	}
	*sample = (int16_t)mixed;
	return V34_PHASE2_TONE_OK;
}

static bool observation_is_valid(
	const struct v34_phase2_tone_observation *observation)
{
	return isfinite(observation->in_phase) &&
	       fabs(observation->in_phase) <=
		       V34_PHASE2_TONE_MAX_SAMPLE_MAGNITUDE &&
	       isfinite(observation->quadrature) &&
	       fabs(observation->quadrature) <=
		       V34_PHASE2_TONE_MAX_SAMPLE_MAGNITUDE &&
	       isfinite(observation->energy) && observation->energy >= 0.0 &&
	       observation->energy <= V34_PHASE2_TONE_MAX_SAMPLE_POWER;
}

static bool history_is_valid(
	const struct v34_phase2_tone_detector *detector)
{
	for (size_t index = 0;
	     index < V34_PHASE2_TONE_WINDOW_SAMPLES; ++index)
		if (!observation_is_valid(&detector->history[index]))
			return false;
	return true;
}

bool v34_phase2_tone_detector_is_valid(
	const struct v34_phase2_tone_detector *detector)
{
	double reference_power;

	if (detector == NULL || !role_is_valid(detector->local_role) ||
	    detector->carrier_phase_step !=
		    phase_step_u32(
			    V34_PHASE2_SAMPLE_RATE,
			    remote_carrier_hz(detector->local_role), 1) ||
	    !history_is_valid(detector) || !isfinite(detector->in_phase) ||
	    fabs(detector->in_phase) > V34_PHASE2_TONE_MAX_PROJECTION ||
	    !isfinite(detector->quadrature) ||
	    fabs(detector->quadrature) > V34_PHASE2_TONE_MAX_PROJECTION ||
	    !isfinite(detector->line_energy) || detector->line_energy < 0.0 ||
	    detector->line_energy > V34_PHASE2_TONE_MAX_WINDOW_ENERGY ||
	    !isfinite(detector->reference_in_phase) ||
	    fabs(detector->reference_in_phase) >
		    V34_PHASE2_TONE_MAX_PROJECTION ||
	    !isfinite(detector->reference_quadrature) ||
	    fabs(detector->reference_quadrature) >
		    V34_PHASE2_TONE_MAX_PROJECTION ||
	    !isfinite(detector->line_power) || detector->line_power < 0.0 ||
	    detector->line_power > V34_PHASE2_TONE_MAX_SAMPLE_POWER ||
	    !isfinite(detector->carrier_power) ||
	    detector->carrier_power < 0.0 ||
	    detector->carrier_power >
		    2.000001 * V34_PHASE2_TONE_MAX_SAMPLE_POWER ||
	    !isfinite(detector->carrier_share) ||
	    detector->carrier_share < 0.0 ||
	    detector->carrier_share > 2.000001 ||
	    detector->history_index >=
		    V34_PHASE2_TONE_WINDOW_SAMPLES ||
	    detector->history_count >
		    V34_PHASE2_TONE_WINDOW_SAMPLES ||
	    detector->measurement_ready !=
		    (detector->history_count ==
		     V34_PHASE2_TONE_WINDOW_SAMPLES) ||
	    detector->qualifying_samples >
		    V34_PHASE2_TONE_ACQUIRE_SAMPLES ||
	    detector->missing_samples >=
		    V34_PHASE2_TONE_LOSS_SAMPLES ||
	    (!detector->tone_present &&
	     (detector->qualifying_samples ==
		      V34_PHASE2_TONE_ACQUIRE_SAMPLES ||
	      detector->missing_samples != 0 || detector->reversal_pending)) ||
	    (detector->tone_present &&
	     detector->qualifying_samples !=
		     V34_PHASE2_TONE_ACQUIRE_SAMPLES) ||
	    (detector->reversal_pending !=
	     (detector->reversal_delay_samples != 0)) ||
	    (detector->reversal_pending &&
	     (detector->reversal_delay_samples <
		      V34_PHASE2_TONE_WINDOW_SAMPLES / 2 ||
	      detector->reversal_delay_samples >
		      V34_PHASE2_TONE_WINDOW_SAMPLES * 2)))
		return false;
	reference_power =
		detector->reference_in_phase * detector->reference_in_phase +
		detector->reference_quadrature * detector->reference_quadrature;
	return detector->tone_present ? reference_power > 0.0 :
				       reference_power == 0.0;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_init(
	struct v34_phase2_tone_detector *detector,
	enum v34_station_role local_role)
{
	struct v34_phase2_tone_detector initialized = {
		.carrier_phase_step = phase_step_u32(
			V34_PHASE2_SAMPLE_RATE,
			remote_carrier_hz(local_role), 1),
		.local_role = local_role,
	};

	if (detector == NULL || !role_is_valid(local_role) ||
	    !v34_phase2_tone_detector_is_valid(&initialized))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*detector = initialized;
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_reset(
	struct v34_phase2_tone_detector *detector)
{
	enum v34_station_role local_role;

	if (!v34_phase2_tone_detector_is_valid(detector))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	local_role = detector->local_role;
	return v34_phase2_tone_detector_init(detector, local_role);
}

static void add_observation(
	struct v34_phase2_tone_detector *detector, int16_t sample)
{
	struct v34_phase2_tone_observation *old =
		&detector->history[detector->history_index];
	double radians = phase_radians_u32(detector->carrier_phase);
	struct v34_phase2_tone_observation current = {
		.in_phase = (double)sample * cos(radians),
		.quadrature = (double)sample * sin(radians),
		.energy = (double)sample * sample,
	};

	detector->carrier_phase += detector->carrier_phase_step;
	if (detector->history_count ==
	    V34_PHASE2_TONE_WINDOW_SAMPLES) {
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
		V34_PHASE2_TONE_WINDOW_SAMPLES;
	detector->measurement_ready =
		detector->history_count ==
		V34_PHASE2_TONE_WINDOW_SAMPLES;
}

static bool measurement_qualifies(
	struct v34_phase2_tone_detector *detector)
{
	double count = V34_PHASE2_TONE_WINDOW_SAMPLES;
	double projection_power =
		detector->in_phase * detector->in_phase +
		detector->quadrature * detector->quadrature;
	double minimum_share =
		detector->local_role == V34_CALLING_STATION ?
			V34_PHASE2_TONE_MIN_ANSWERING_CARRIER_SHARE :
			V34_PHASE2_TONE_MIN_CALLING_CARRIER_SHARE;

	/* The complex projection contains half the sinusoid amplitude. Convert
	 * it to mean-square carrier power before comparing it with all energy in
	 * the window; this accepts Tone A's stronger, orthogonal guard without
	 * mistaking the guard itself for the carrier. */
	detector->line_power = detector->line_energy / count;
	detector->carrier_power = 2.0 * projection_power / (count * count);
	detector->carrier_share = detector->line_power > 0.0 ?
				  detector->carrier_power /
					  detector->line_power :
				  0.0;
	return detector->line_power >= V34_PHASE2_TONE_MIN_LINE_POWER &&
	       detector->carrier_share >= minimum_share &&
	       detector->carrier_share <= V34_PHASE2_TONE_MAX_CARRIER_SHARE;
}

static void clear_tone_lock(
	struct v34_phase2_tone_detector *detector)
{
	detector->reference_in_phase = 0.0;
	detector->reference_quadrature = 0.0;
	detector->qualifying_samples = 0;
	detector->missing_samples = 0;
	detector->reversal_delay_samples = 0;
	detector->tone_present = false;
	detector->reversal_pending = false;
}

static enum v34_phase2_tone_event_kind acquire_or_release(
	struct v34_phase2_tone_detector *detector,
	bool qualifies)
{
	if (!detector->tone_present) {
		if (!qualifies) {
			detector->qualifying_samples = 0;
			return V34_PHASE2_TONE_NO_EVENT;
		}
		if (++detector->qualifying_samples <
		    V34_PHASE2_TONE_ACQUIRE_SAMPLES)
			return V34_PHASE2_TONE_NO_EVENT;
		detector->tone_present = true;
		detector->reference_in_phase = detector->in_phase;
		detector->reference_quadrature = detector->quadrature;
		return V34_PHASE2_TONE_STARTED;
	}
	if (qualifies) {
		detector->missing_samples = 0;
		return V34_PHASE2_TONE_NO_EVENT;
	}
	if (++detector->missing_samples <
	    V34_PHASE2_TONE_LOSS_SAMPLES)
		return V34_PHASE2_TONE_NO_EVENT;
	clear_tone_lock(detector);
	return V34_PHASE2_TONE_ENDED;
}

static bool observe_reversal(
	struct v34_phase2_tone_detector *detector,
	bool qualifies, uint16_t *delay_samples)
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
			V34_PHASE2_TONE_WINDOW_SAMPLES / 2;
	} else if (detector->reversal_pending &&
		   detector->reversal_delay_samples <
			   V34_PHASE2_TONE_WINDOW_SAMPLES * 2) {
		detector->reversal_delay_samples++;
	}
	if (detector->reversal_pending && qualifies &&
	    cosine <= V34_PHASE2_TONE_REVERSAL_CONFIRM_COSINE) {
		*delay_samples = detector->reversal_delay_samples;
		detector->reference_in_phase = -detector->reference_in_phase;
		detector->reference_quadrature = -detector->reference_quadrature;
		detector->reversal_pending = false;
		detector->reversal_delay_samples = 0;
		return true;
	}
	if (detector->reversal_pending &&
	    (cosine > 0.25 || detector->reversal_delay_samples ==
			      V34_PHASE2_TONE_WINDOW_SAMPLES * 2)) {
		detector->reversal_pending = false;
		detector->reversal_delay_samples = 0;
	}
	if (!detector->reversal_pending && qualifies && cosine > 0.5) {
		detector->reference_in_phase +=
			(detector->in_phase - detector->reference_in_phase) *
			V34_PHASE2_TONE_REFERENCE_TRACKING;
		detector->reference_quadrature +=
			(detector->quadrature - detector->reference_quadrature) *
			V34_PHASE2_TONE_REFERENCE_TRACKING;
	}
	return false;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_sample(
	struct v34_phase2_tone_detector *detector,
	int16_t sample, struct v34_phase2_tone_event *event)
{
	struct v34_phase2_tone_detector next;
	struct v34_phase2_tone_event emitted = { 0 };
	bool was_present;
	bool qualifies = false;

	if (!v34_phase2_tone_detector_is_valid(detector) ||
	    event == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	next = *detector;
	was_present = next.tone_present;
	add_observation(&next, sample);
	if (next.measurement_ready)
		qualifies = measurement_qualifies(&next);
	emitted.kind = acquire_or_release(&next, qualifies);
	if (was_present && next.tone_present &&
	    observe_reversal(
		    &next, qualifies, &emitted.reversal_delay_samples))
		emitted.kind = V34_PHASE2_TONE_PHASE_REVERSED;
	if (!v34_phase2_tone_detector_is_valid(&next))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*detector = next;
	*event = emitted;
	return V34_PHASE2_TONE_OK;
}

struct v34_phase2_tone_measurement
v34_phase2_tone_detector_measure(
	const struct v34_phase2_tone_detector *detector)
{
	if (!v34_phase2_tone_detector_is_valid(detector))
		return (struct v34_phase2_tone_measurement){ 0 };
	return (struct v34_phase2_tone_measurement){
		.line_power = detector->line_power,
		.carrier_power = detector->carrier_power,
		.carrier_share = detector->carrier_share,
		.ready = detector->measurement_ready,
		.tone_present = detector->tone_present,
	};
}

bool v34_retrain_detector_is_valid(
	const struct v34_retrain_detector *detector)
{
	if (detector == NULL ||
	    !v34_phase2_tone_detector_is_valid(&detector->tone) ||
	    detector->silence_samples >
		    V34_RETRAIN_MIN_SILENCE_SAMPLES ||
	    detector->tone_samples > V34_RETRAIN_TONE_SAMPLES ||
	    detector->edge_samples > V34_RETRAIN_EDGE_SAMPLES)
		return false;
	if (detector->silence_samples <
	    V34_RETRAIN_MIN_SILENCE_SAMPLES)
		return detector->tone_samples == 0 &&
		       detector->edge_samples == 0;
	return detector->tone_samples == 0 ||
	       detector->edge_samples == 0;
}

enum v34_phase2_tone_result v34_retrain_detector_init(
	struct v34_retrain_detector *detector,
	enum v34_station_role local_role)
{
	struct v34_retrain_detector initialized = { 0 };

	if (detector == NULL ||
	    v34_phase2_tone_detector_init(
		    &initialized.tone, local_role) != V34_PHASE2_TONE_OK ||
	    !v34_retrain_detector_is_valid(&initialized))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*detector = initialized;
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result v34_retrain_detector_reset(
	struct v34_retrain_detector *detector)
{
	enum v34_station_role local_role;

	if (!v34_retrain_detector_is_valid(detector))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	local_role = detector->tone.local_role;
	return v34_retrain_detector_init(detector, local_role);
}

static bool retrain_tone_matches(
	const struct v34_retrain_detector *detector,
	const struct v34_phase2_tone_measurement *measurement)
{
	if (!measurement->tone_present)
		return false;
	/* Tone A is the -1 dB 2400 Hz carrier plus the continuous 1800 Hz
	 * answer guard.  A bare 2400 Hz carrier has a share near one and must
	 * not initiate retraining.  The underlying detector already enforces
	 * the complementary lower carrier-share bounds for both roles. */
	return detector->tone.local_role != V34_CALLING_STATION ||
	       measurement->carrier_share <=
		       V34_RETRAIN_TONE_A_MAX_CARRIER_SHARE;
}

static enum v34_phase2_tone_result reset_retrain_candidate(
	struct v34_retrain_detector *detector)
{
	enum v34_station_role local_role = detector->tone.local_role;

	return v34_retrain_detector_init(detector, local_role);
}

enum v34_phase2_tone_result v34_retrain_detector_sample(
	struct v34_retrain_detector *detector, int16_t sample,
	bool *detected)
{
	struct v34_retrain_detector next;
	struct v34_phase2_tone_measurement measurement;
	struct v34_phase2_tone_event event;
	bool emitted = false;

	if (!v34_retrain_detector_is_valid(detector) || detected == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	next = *detector;
	if (v34_phase2_tone_detector_sample(
		    &next.tone, sample, &event) != V34_PHASE2_TONE_OK)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	measurement = v34_phase2_tone_detector_measure(&next.tone);
	if (next.silence_samples < V34_RETRAIN_MIN_SILENCE_SAMPLES) {
		if (measurement.ready &&
		    measurement.line_power <=
			    V34_RETRAIN_SILENCE_MAX_LINE_POWER)
			next.silence_samples++;
		else
			next.silence_samples = 0;
	} else if (retrain_tone_matches(&next, &measurement)) {
		next.edge_samples = 0;
		if (event.kind == V34_PHASE2_TONE_STARTED)
			next.tone_samples =
				V34_PHASE2_TONE_ACQUIRE_SAMPLES;
		else if (next.tone_samples < V34_RETRAIN_TONE_SAMPLES)
			next.tone_samples++;
		if (next.tone_samples == V34_RETRAIN_TONE_SAMPLES) {
			emitted = true;
			if (reset_retrain_candidate(&next) !=
			    V34_PHASE2_TONE_OK)
				return V34_PHASE2_TONE_INVALID_ARGUMENT;
		}
	} else if (next.tone_samples != 0) {
		/* A qualified request is continuous.  Once its tone breaks, a new
		 * silence preamble is required. */
		if (reset_retrain_candidate(&next) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	} else if (measurement.ready &&
		   measurement.line_power <=
			   V34_RETRAIN_SILENCE_MAX_LINE_POWER) {
		next.edge_samples = 0;
	} else if (++next.edge_samples >= V34_RETRAIN_EDGE_SAMPLES) {
		if (reset_retrain_candidate(&next) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	}
	if (!v34_retrain_detector_is_valid(&next))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*detector = next;
	*detected = emitted;
	return V34_PHASE2_TONE_OK;
}

const char *v34_phase2_tone_result_name(
	enum v34_phase2_tone_result result)
{
	switch (result) {
	case V34_PHASE2_TONE_OK:
		return "ok";
	case V34_PHASE2_TONE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
