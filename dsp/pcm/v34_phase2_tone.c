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
static const double V34_RETRAIN_SILENCE_MAX_LINE_POWER = 150000.0;
static const uint64_t V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY =
	UINT64_C(9600000);
static const uint32_t V34_RETRAIN_MAX_SAMPLE_ENERGY =
	UINT32_C(1) << 30;
static const double V34_RETRAIN_TONE_A_MAX_CARRIER_SHARE = 0.60;

_Static_assert(
	V34_RETRAIN_MIN_SILENCE_SAMPLES == 518 &&
	V34_RETRAIN_SILENCE_WINDOW_SAMPLES == 64 &&
	V34_RETRAIN_MAX_SILENCE_GLITCH_SAMPLES == 80 &&
	V34_RETRAIN_TONE_SAMPLES == 640,
	"V.34 retrain guards must retain their 8 kHz durations");
_Static_assert(
	(int)V34_PHASE2_TONE_NO_EVENT == (int)SINGLE_TONE_NO_EVENT &&
	(int)V34_PHASE2_TONE_STARTED == (int)SINGLE_TONE_STARTED &&
	(int)V34_PHASE2_TONE_PHASE_REVERSED ==
		(int)SINGLE_TONE_PHASE_REVERSED &&
	(int)V34_PHASE2_TONE_ENDED == (int)SINGLE_TONE_ENDED,
	"shared tone events must preserve the V.34 event order");

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

static double minimum_carrier_share(
	enum v34_station_role local_role)
{
	return local_role == V34_CALLING_STATION ?
		V34_PHASE2_TONE_MIN_ANSWERING_CARRIER_SHARE :
		V34_PHASE2_TONE_MIN_CALLING_CARRIER_SHARE;
}

static struct single_tone_detector_profile detector_profile(
	enum v34_station_role local_role)
{
	return (struct single_tone_detector_profile){
		.minimum_line_power = V34_PHASE2_TONE_MIN_LINE_POWER,
		.minimum_carrier_share = minimum_carrier_share(local_role),
		.maximum_carrier_share = V34_PHASE2_TONE_MAX_CARRIER_SHARE,
		.sample_rate_hz = V34_PHASE2_SAMPLE_RATE,
		.frequency_hz = (uint16_t)remote_carrier_hz(local_role),
		.window_samples = V34_PHASE2_TONE_WINDOW_SAMPLES,
		.acquire_samples = V34_PHASE2_TONE_ACQUIRE_SAMPLES,
		.loss_samples = V34_PHASE2_TONE_LOSS_SAMPLES,
		.retain_reference_on_loss = true,
	};
}

static bool profile_matches_role(
	const struct single_tone_detector_profile *profile,
	enum v34_station_role local_role)
{
	struct single_tone_detector_profile expected =
		detector_profile(local_role);

	return profile->minimum_line_power == expected.minimum_line_power &&
	       profile->minimum_carrier_share ==
		       expected.minimum_carrier_share &&
	       profile->maximum_carrier_share ==
		       expected.maximum_carrier_share &&
	       profile->sample_rate_hz == expected.sample_rate_hz &&
	       profile->frequency_hz == expected.frequency_hz &&
	       profile->window_samples == expected.window_samples &&
	       profile->acquire_samples == expected.acquire_samples &&
	       profile->loss_samples == expected.loss_samples &&
	       profile->retain_reference_on_loss;
}

static bool detector_has_role_profile(
	const struct v34_phase2_tone_detector *detector)
{
	return detector != NULL && role_is_valid(detector->local_role) &&
	       profile_matches_role(
		       &detector->carrier.profile, detector->local_role);
}

bool v34_phase2_tone_detector_is_valid(
	const struct v34_phase2_tone_detector *detector)
{
	return detector_has_role_profile(detector) &&
	       single_tone_detector_is_valid(&detector->carrier);
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_init(
	struct v34_phase2_tone_detector *detector,
	enum v34_station_role local_role)
{
	struct v34_phase2_tone_detector initialized = {
		.local_role = local_role,
	};
	struct single_tone_detector_profile profile;

	if (detector == NULL || !role_is_valid(local_role))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	profile = detector_profile(local_role);
	if (single_tone_detector_init_profile(
		    &initialized.carrier, &profile) != SINGLE_TONE_OK ||
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

static void export_event(
	const struct single_tone_event *source,
	struct v34_phase2_tone_event *event)
{
	*event = (struct v34_phase2_tone_event){
		.kind = (enum v34_phase2_tone_event_kind)source->kind,
		.reversal_delay_samples = source->reversal_delay_samples,
	};
}

static enum v34_phase2_tone_result sample_valid_tone_detector(
	struct v34_phase2_tone_detector *detector, int16_t sample,
	struct v34_phase2_tone_event *event)
{
	struct single_tone_event shared_event;

	if (single_tone_detector_sample(
		    &detector->carrier, sample, &shared_event) != SINGLE_TONE_OK)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	export_event(&shared_event, event);
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_suppress_sample(
	struct v34_phase2_tone_detector *detector)
{
	return detector_has_role_profile(detector) &&
	       single_tone_detector_suppress_sample(&detector->carrier) ==
		       SINGLE_TONE_OK ?
		       V34_PHASE2_TONE_OK :
		       V34_PHASE2_TONE_INVALID_ARGUMENT;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_resume(
	struct v34_phase2_tone_detector *detector,
	uint16_t handoff_delay_samples)
{
	return detector_has_role_profile(detector) &&
	       single_tone_detector_resume(
		       &detector->carrier, handoff_delay_samples) ==
		       SINGLE_TONE_OK ?
		       V34_PHASE2_TONE_OK :
		       V34_PHASE2_TONE_INVALID_ARGUMENT;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_sample(
	struct v34_phase2_tone_detector *detector,
	int16_t sample, struct v34_phase2_tone_event *event)
{
	if (!detector_has_role_profile(detector) || event == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	return sample_valid_tone_detector(detector, sample, event);
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_reject_reversal(
	struct v34_phase2_tone_detector *detector,
	const struct v34_phase2_tone_event *event)
{
	struct single_tone_event shared_event;

	if (!detector_has_role_profile(detector) || event == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	shared_event = (struct single_tone_event){
		.kind = (enum single_tone_event_kind)event->kind,
		.reversal_delay_samples = event->reversal_delay_samples,
	};
	return single_tone_detector_reject_reversal(
		       &detector->carrier, &shared_event) == SINGLE_TONE_OK ?
		       V34_PHASE2_TONE_OK :
		       V34_PHASE2_TONE_INVALID_ARGUMENT;
}

static struct v34_phase2_tone_measurement measure_valid_tone_detector(
	const struct v34_phase2_tone_detector *detector)
{
	struct single_tone_measurement measurement =
		single_tone_detector_measure(&detector->carrier);

	return (struct v34_phase2_tone_measurement){
		.line_power = measurement.line_power,
		.carrier_power = measurement.carrier_power,
		.carrier_share = measurement.carrier_share,
		.ready = measurement.ready,
		.tone_present = measurement.tone_present,
	};
}

struct v34_phase2_tone_measurement
v34_phase2_tone_detector_measure(
	const struct v34_phase2_tone_detector *detector)
{
	return detector_has_role_profile(detector) ?
		       measure_valid_tone_detector(detector) :
		       (struct v34_phase2_tone_measurement){ 0 };
}

static bool retrain_silence_state_is_valid(
	const struct v34_retrain_detector *detector)
{
	uint64_t energy = 0;

	if (detector->silence_history_index >=
		    V34_RETRAIN_SILENCE_WINDOW_SAMPLES ||
	    detector->silence_history_count >
		    V34_RETRAIN_SILENCE_WINDOW_SAMPLES ||
	    (detector->silence_history_count <
		     V34_RETRAIN_SILENCE_WINDOW_SAMPLES &&
	     detector->silence_history_index !=
		     detector->silence_history_count) ||
	    detector->silence_glitch_samples >
		    V34_RETRAIN_MAX_SILENCE_GLITCH_SAMPLES ||
	    (detector->silence_samples != 0 &&
	     detector->silence_samples <
		     V34_RETRAIN_SILENCE_WINDOW_SAMPLES) ||
	    (detector->silence_samples == 0 &&
	     detector->silence_glitch_samples != 0) ||
	    (detector->silence_samples ==
		     V34_RETRAIN_MIN_SILENCE_SAMPLES &&
	     detector->silence_glitch_samples != 0))
		return false;
	for (size_t index = 0;
	     index < V34_RETRAIN_SILENCE_WINDOW_SAMPLES; ++index) {
		if (detector->silence_history[index] >
		    V34_RETRAIN_MAX_SAMPLE_ENERGY)
			return false;
		energy += detector->silence_history[index];
	}
	return energy == detector->silence_energy;
}

bool v34_retrain_detector_is_valid(
	const struct v34_retrain_detector *detector)
{
	if (detector == NULL ||
	    !v34_phase2_tone_detector_is_valid(&detector->tone) ||
	    !retrain_silence_state_is_valid(detector) ||
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

static void add_retrain_silence_observation(
	struct v34_retrain_detector *detector, int16_t sample)
{
	uint32_t sample_energy = (uint32_t)((int32_t)sample * sample);
	uint32_t old_energy =
		detector->silence_history[detector->silence_history_index];

	detector->silence_history[detector->silence_history_index] =
		sample_energy;
	detector->silence_history_index =
		(detector->silence_history_index + 1u) %
		V34_RETRAIN_SILENCE_WINDOW_SAMPLES;
	detector->silence_energy += sample_energy;
	if (detector->silence_history_count <
	    V34_RETRAIN_SILENCE_WINDOW_SAMPLES)
		detector->silence_history_count++;
	else
		detector->silence_energy -= old_energy;

	if (detector->silence_history_count !=
	    V34_RETRAIN_SILENCE_WINDOW_SAMPLES)
		return;
	if (detector->silence_energy <=
	    V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY) {
		detector->silence_glitch_samples = 0;
		if (detector->silence_samples <
		    V34_RETRAIN_SILENCE_WINDOW_SAMPLES)
			detector->silence_samples =
				V34_RETRAIN_SILENCE_WINDOW_SAMPLES;
		else if (detector->silence_samples <
			 V34_RETRAIN_MIN_SILENCE_SAMPLES)
			detector->silence_samples++;
	} else if (detector->silence_samples <
		   V34_RETRAIN_MIN_SILENCE_SAMPLES) {
		if (detector->silence_samples >=
			    V34_RETRAIN_SILENCE_WINDOW_SAMPLES &&
		    detector->silence_glitch_samples <
			    V34_RETRAIN_MAX_SILENCE_GLITCH_SAMPLES) {
			detector->silence_glitch_samples++;
		} else {
			detector->silence_samples = 0;
			detector->silence_glitch_samples = 0;
		}
	}
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
	struct v34_phase2_tone_measurement measurement;
	struct v34_phase2_tone_event event;
	bool emitted = false;

	if (!v34_retrain_detector_is_valid(detector) || detected == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	if (detector->silence_samples < V34_RETRAIN_MIN_SILENCE_SAMPLES) {
		add_retrain_silence_observation(detector, sample);
		if (detector->silence_samples ==
		    V34_RETRAIN_MIN_SILENCE_SAMPLES &&
		    v34_phase2_tone_detector_reset(&detector->tone) !=
			    V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
		*detected = false;
		return V34_PHASE2_TONE_OK;
	}
	if (sample_valid_tone_detector(
		    &detector->tone, sample, &event) != V34_PHASE2_TONE_OK)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	measurement = measure_valid_tone_detector(&detector->tone);
	if (retrain_tone_matches(detector, &measurement)) {
		detector->edge_samples = 0;
		if (event.kind == V34_PHASE2_TONE_STARTED)
			detector->tone_samples =
				V34_PHASE2_TONE_ACQUIRE_SAMPLES;
		else if (detector->tone_samples < V34_RETRAIN_TONE_SAMPLES)
			detector->tone_samples++;
		if (detector->tone_samples == V34_RETRAIN_TONE_SAMPLES) {
			struct v34_phase2_tone_detector retained = detector->tone;

			emitted = true;
			if (reset_retrain_candidate(detector) !=
			    V34_PHASE2_TONE_OK)
				return V34_PHASE2_TONE_INVALID_ARGUMENT;
			/* A responding modem must retain the qualified peer tone's
			 * phase across its silence so the following reversal is still
			 * observable. Explicit reset remains available to initiators. */
			detector->tone = retained;
		}
	} else if (detector->tone_samples != 0) {
		/* A qualified request is continuous.  Once its tone breaks, a new
		 * silence preamble is required. */
		if (reset_retrain_candidate(detector) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	} else if (measurement.ready &&
		   measurement.line_power <=
			   V34_RETRAIN_SILENCE_MAX_LINE_POWER) {
		detector->edge_samples = 0;
	} else if (++detector->edge_samples >= V34_RETRAIN_EDGE_SAMPLES) {
		if (reset_retrain_candidate(detector) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	}
	*detected = emitted;
	return V34_PHASE2_TONE_OK;
}
