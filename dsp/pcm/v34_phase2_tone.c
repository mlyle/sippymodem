// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_tone.h"

#include "phase.h"
#include "v34_gain_control.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static const double V34_PHASE2_TONE_ANSWERING_CARRIER_GAIN =
	0.8912509381337456; /* -1 dB. */
static const double V34_PHASE2_TONE_MIN_LINE_POWER = 256.0 * 256.0;
static const double V34_PHASE2_TONE_MIN_CALLING_CARRIER_SHARE = 0.70;
static const double V34_PHASE2_TONE_MIN_ANSWERING_CARRIER_SHARE = 0.30;
static const double V34_PHASE2_TONE_MAX_CARRIER_SHARE = 1.20;
static const uint32_t V34_RETRAIN_MAX_SAMPLE_ENERGY =
	UINT32_C(1) << 30;
static const double V34_RETRAIN_MIN_GUARD_SHARE = 0.10;

_Static_assert(
	V34_RETRAIN_MIN_SILENCE_SAMPLES == 478 &&
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

bool v34_phase2_tone_transmitter_is_valid(
	const struct v34_phase2_tone_transmitter *transmitter)
{
	if (transmitter == NULL ||
	    !v34_station_role_is_valid(transmitter->local_role) ||
	    transmitter->carrier_amplitude <= 0 ||
	    transmitter->carrier_phase_step !=
		    phase_step_u32(
			    V34_PHASE2_SAMPLE_RATE,
			    v34_phase2_carrier_hz(
				    transmitter->local_role), 1))
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

	if (transmitter == NULL || !v34_station_role_is_valid(local_role) ||
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
		v34_phase2_carrier_hz(local_role), 1);
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
	struct single_tone_detector_profile profile;

	memset(&profile, 0, sizeof(profile));
	profile.minimum_line_power = V34_PHASE2_TONE_MIN_LINE_POWER;
	profile.minimum_carrier_share = minimum_carrier_share(local_role);
	profile.maximum_carrier_share = V34_PHASE2_TONE_MAX_CARRIER_SHARE;
	profile.sample_rate_hz = V34_PHASE2_SAMPLE_RATE;
	profile.frequency_hz = (uint16_t)v34_phase2_carrier_hz(v34_station_peer_role(local_role));
	profile.window_samples = V34_PHASE2_TONE_WINDOW_SAMPLES;
	profile.acquire_samples = V34_PHASE2_TONE_ACQUIRE_SAMPLES;
	profile.loss_samples = V34_PHASE2_TONE_LOSS_SAMPLES;
	profile.retain_reference_on_loss = true;
	return profile;
}

static bool profile_matches_role(
	const struct single_tone_detector_profile *profile,
	enum v34_station_role local_role)
{
	struct single_tone_detector_profile expected =
		detector_profile(local_role);

	return profile->minimum_line_power > 0.0 &&
	       profile->minimum_line_power <= expected.minimum_line_power &&
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
	return detector != NULL &&
	       v34_station_role_is_valid(detector->local_role) &&
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

	if (detector == NULL || !v34_station_role_is_valid(local_role))
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
v34_phase2_tone_detector_set_receive_gain(
	struct v34_phase2_tone_detector *detector, int16_t receive_gain)
{
	const uint64_t unity = UINT64_C(1) << V34_GAIN_FRACTION_BITS;
	uint64_t gain;

	if (!v34_phase2_tone_detector_is_valid(detector) || receive_gain <= 0)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	gain = (uint64_t)receive_gain < unity ? unity : (uint64_t)receive_gain;
	/* Keep raw observations and phase; express the normalized power floor
	 * in their amplitude domain. Positive signed-Q10 gains bound the squared
	 * denominator by 32767^2 and the numerator by 256^2 * 1024^2 = 2^36.
	 * Gain below unity must not raise the default acquisition floor. */
	detector->carrier.profile.minimum_line_power =
		V34_PHASE2_TONE_MIN_LINE_POWER * (double)(unity * unity) /
		(double)(gain * gain);
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result
v34_phase2_tone_detector_reset(
	struct v34_phase2_tone_detector *detector)
{
	enum v34_station_role local_role;
	double minimum_line_power;

	if (!v34_phase2_tone_detector_is_valid(detector))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	local_role = detector->local_role;
	minimum_line_power = detector->carrier.profile.minimum_line_power;
	if (v34_phase2_tone_detector_init(detector, local_role) !=
	    V34_PHASE2_TONE_OK)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	detector->carrier.profile.minimum_line_power = minimum_line_power;
	return V34_PHASE2_TONE_OK;
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

static struct single_tone_detector_profile retrain_guard_profile(void)
{
	struct single_tone_detector_profile profile =
		detector_profile(V34_CALLING_STATION);

	profile.frequency_hz = V34_PHASE2_ANSWERING_GUARD_HZ;
	profile.minimum_carrier_share = V34_RETRAIN_MIN_GUARD_SHARE;
	profile.retain_reference_on_loss = false;
	return profile;
}

static bool retrain_guard_is_valid(
	const struct single_tone_detector *guard)
{
	struct single_tone_detector_profile expected = retrain_guard_profile();
	const struct single_tone_detector_profile *profile = &guard->profile;

	return single_tone_detector_is_valid(guard) &&
	       profile->minimum_line_power > 0.0 &&
	       profile->minimum_line_power <= expected.minimum_line_power &&
	       profile->minimum_carrier_share == expected.minimum_carrier_share &&
	       profile->maximum_carrier_share == expected.maximum_carrier_share &&
	       profile->sample_rate_hz == expected.sample_rate_hz &&
	       profile->frequency_hz == expected.frequency_hz &&
	       profile->window_samples == expected.window_samples &&
	       profile->acquire_samples == expected.acquire_samples &&
	       profile->loss_samples == expected.loss_samples &&
	       profile->retain_reference_on_loss == expected.retain_reference_on_loss;
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
	    !retrain_guard_is_valid(&detector->answer_guard) ||
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
	struct v34_retrain_detector *detector, int16_t sample,
	uint32_t maximum_window_energy)
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
	if (detector->silence_energy <= maximum_window_energy) {
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
	struct single_tone_detector_profile guard = retrain_guard_profile();

	if (detector == NULL ||
	    v34_phase2_tone_detector_init(
		    &initialized.tone, local_role) != V34_PHASE2_TONE_OK ||
	    single_tone_detector_init_profile(
		    &initialized.answer_guard, &guard) != SINGLE_TONE_OK ||
	    !v34_retrain_detector_is_valid(&initialized))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*detector = initialized;
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result v34_retrain_detector_reset(
	struct v34_retrain_detector *detector)
{
	enum v34_station_role local_role;
	double minimum_line_power;

	if (!v34_retrain_detector_is_valid(detector))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	local_role = detector->tone.local_role;
	minimum_line_power = detector->tone.carrier.profile.minimum_line_power;
	if (v34_retrain_detector_init(detector, local_role) != V34_PHASE2_TONE_OK)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	detector->tone.carrier.profile.minimum_line_power = minimum_line_power;
	return V34_PHASE2_TONE_OK;
}

static bool retrain_tone_matches(
	const struct v34_retrain_detector *detector,
	const struct v34_phase2_tone_measurement *measurement)
{
	struct single_tone_measurement guard;

	if (!measurement->tone_present)
		return false;
	if (detector->tone.local_role != V34_CALLING_STATION)
		return true;
	/* Line filtering can weaken the 1800 Hz answer guard.
	 * Require that guard itself, rather than inferring it from energy
	 * missing from the 2400 Hz carrier. Ten percent tolerates guard loss
	 * while rejecting a bare carrier mixed with unrelated tones. */
	guard =
		single_tone_detector_measure(&detector->answer_guard);

	return guard.tone_present &&
	       guard.carrier_share >= V34_RETRAIN_MIN_GUARD_SHARE;
}

enum v34_phase2_tone_result v34_retrain_detector_sample(
	struct v34_retrain_detector *detector, int16_t sample,
	bool *detected)
{
	return v34_retrain_detector_sample_with_power_limit(
		detector, sample, V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY,
		detected);
}

enum v34_phase2_tone_result v34_retrain_detector_sample_with_power_limit(
	struct v34_retrain_detector *detector, int16_t sample,
	uint32_t maximum_window_energy, bool *detected)
{
	struct v34_phase2_tone_measurement measurement;
	struct v34_phase2_tone_event event;
	struct single_tone_event guard_event;
	bool emitted = false;

	if (!v34_retrain_detector_is_valid(detector) || detected == NULL ||
	    maximum_window_energy == 0u ||
	    maximum_window_energy > V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	/* Keep observations in raw physical units, including any retained phase.
	 * Uniform power scaling changes sensitivity without waveform clipping or
	 * a gain-domain discontinuity when a retrain hands its tone to Phase 2. */
	detector->tone.carrier.profile.minimum_line_power =
		V34_PHASE2_TONE_MIN_LINE_POWER * maximum_window_energy /
		V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY;
	detector->answer_guard.profile.minimum_line_power =
		detector->tone.carrier.profile.minimum_line_power;
	if (detector->silence_samples < V34_RETRAIN_MIN_SILENCE_SAMPLES) {
		add_retrain_silence_observation(
			detector, sample, maximum_window_energy);
		if (detector->silence_samples ==
		    V34_RETRAIN_MIN_SILENCE_SAMPLES &&
		    (v34_phase2_tone_detector_reset(&detector->tone) !=
			     V34_PHASE2_TONE_OK ||
		     single_tone_detector_reset(&detector->answer_guard) !=
			     SINGLE_TONE_OK))
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
		*detected = false;
		return V34_PHASE2_TONE_OK;
	}
	if (sample_valid_tone_detector(
		    &detector->tone, sample, &event) != V34_PHASE2_TONE_OK ||
	    (detector->tone.local_role == V34_CALLING_STATION &&
	     single_tone_detector_sample(
		     &detector->answer_guard, sample, &guard_event) !=
		     SINGLE_TONE_OK))
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
			if (v34_retrain_detector_reset(detector) !=
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
		if (v34_retrain_detector_reset(detector) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	} else if (measurement.ready &&
		   measurement.line_power <=
			   (double)maximum_window_energy /
				   V34_RETRAIN_SILENCE_WINDOW_SAMPLES) {
		detector->edge_samples = 0;
	} else if (++detector->edge_samples >= V34_RETRAIN_EDGE_SAMPLES) {
		if (v34_retrain_detector_reset(detector) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	}
	*detected = emitted;
	return V34_PHASE2_TONE_OK;
}

static bool retrain_request_state_is_clear(
	const struct v34_retrain_detector *detector)
{
	return detector->silence_samples == 0u &&
	       detector->tone_samples == 0u && detector->edge_samples == 0u;
}

bool v34_full_retrain_is_valid(
	const struct v34_full_retrain *retrain)
{
	if (retrain == NULL ||
	    !v34_retrain_detector_is_valid(&retrain->detector) ||
	    retrain->response_silence_samples_remaining >
		    V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES ||
	    (retrain->response_silence_samples_remaining == 0u &&
	     retrain->peer_tone_retained))
		return false;
	if (retrain->response_silence_samples_remaining == 0u)
		return true;
	return retrain_request_state_is_clear(&retrain->detector) &&
	       (!retrain->peer_tone_retained ||
		v34_phase2_tone_detector_measure(&retrain->detector.tone)
			.tone_present);
}

bool v34_full_retrain_response_is_active(
	const struct v34_full_retrain *retrain)
{
	return v34_full_retrain_is_valid(retrain) &&
	       retrain->response_silence_samples_remaining != 0u;
}

enum v34_phase2_tone_result v34_full_retrain_init(
	struct v34_full_retrain *retrain,
	enum v34_station_role local_role)
{
	struct v34_full_retrain initialized = { 0 };

	if (retrain == NULL ||
	    v34_retrain_detector_init(&initialized.detector, local_role) !=
		    V34_PHASE2_TONE_OK ||
	    !v34_full_retrain_is_valid(&initialized))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*retrain = initialized;
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result v34_full_retrain_begin_response(
	struct v34_full_retrain *retrain, bool retain_peer_tone)
{
	struct v34_full_retrain next;

	if (!v34_full_retrain_is_valid(retrain) ||
	    v34_full_retrain_response_is_active(retrain))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	next = *retrain;
	if (retain_peer_tone) {
		if (!retrain_request_state_is_clear(&next.detector) ||
		    !v34_phase2_tone_detector_measure(&next.detector.tone)
			     .tone_present)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
	} else if (v34_retrain_detector_reset(&next.detector) !=
		   V34_PHASE2_TONE_OK) {
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	}
	next.response_silence_samples_remaining =
		V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES;
	next.peer_tone_retained = retain_peer_tone;
	if (!v34_full_retrain_is_valid(&next))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*retrain = next;
	return V34_PHASE2_TONE_OK;
}

enum v34_phase2_tone_result v34_full_retrain_sample_response(
	struct v34_full_retrain *retrain, int16_t received_sample,
	enum v34_full_retrain_response_event *event)
{
	struct v34_full_retrain next;
	enum v34_full_retrain_response_event emitted =
		V34_FULL_RETRAIN_RESPONSE_CONTINUES;

	if (!v34_full_retrain_response_is_active(retrain) || event == NULL)
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	next = *retrain;
	if (next.peer_tone_retained) {
		struct v34_phase2_tone_event tone_event;

		if (v34_phase2_tone_detector_sample(
			    &next.detector.tone, received_sample,
			    &tone_event) != V34_PHASE2_TONE_OK)
			return V34_PHASE2_TONE_INVALID_ARGUMENT;
		/* A lost phase reference is a channel event, not corrupt state.
		 * Finish the original silence interval and reacquire in Phase 2. */
		if (tone_event.kind == V34_PHASE2_TONE_ENDED)
			next.peer_tone_retained = false;
	}
	if (--next.response_silence_samples_remaining == 0u) {
		emitted = next.peer_tone_retained ?
			V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE :
			V34_FULL_RETRAIN_RESPONSE_COMPLETES;
		next.peer_tone_retained = false;
	}
	if (!v34_full_retrain_is_valid(&next))
		return V34_PHASE2_TONE_INVALID_ARGUMENT;
	*retrain = next;
	*event = emitted;
	return V34_PHASE2_TONE_OK;
}
