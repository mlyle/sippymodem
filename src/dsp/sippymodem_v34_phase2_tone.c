// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase2_tone.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static const double V34_PHASE2_TONE_TWO_PI =
	6.283185307179586476925286766559;
static const double V34_PHASE2_TONE_PHASE_SCALE = 4294967296.0;
static const double V34_PHASE2_TONE_ANSWERING_CARRIER_GAIN =
	0.8912509381337456; /* -1 dB. */

static bool role_is_valid(enum sippymodem_v34_phase2_station_role role)
{
	return role == SIPPYMODEM_V34_PHASE2_CALLING_STATION ||
	       role == SIPPYMODEM_V34_PHASE2_ANSWERING_STATION;
}

static uint32_t phase_step(uint32_t frequency_hz)
{
	uint64_t scaled =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)(
		(scaled + SIPPYMODEM_V34_PHASE2_SAMPLE_RATE / 2u) /
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE);
}

static double phase_radians(uint32_t phase)
{
	return (double)phase *
	       (V34_PHASE2_TONE_TWO_PI / V34_PHASE2_TONE_PHASE_SCALE);
}

static uint32_t local_carrier_hz(
	enum sippymodem_v34_phase2_station_role role)
{
	return role == SIPPYMODEM_V34_PHASE2_CALLING_STATION ?
		       SIPPYMODEM_V34_PHASE2_CALLING_CARRIER_HZ :
		       SIPPYMODEM_V34_PHASE2_ANSWERING_CARRIER_HZ;
}

bool sippymodem_v34_phase2_tone_transmitter_is_valid(
	const struct sippymodem_v34_phase2_tone_transmitter *transmitter)
{
	if (transmitter == NULL || !role_is_valid(transmitter->local_role) ||
	    transmitter->carrier_amplitude <= 0 ||
	    transmitter->carrier_phase_step !=
		    phase_step(local_carrier_hz(transmitter->local_role)))
		return false;
	if (transmitter->local_role ==
	    SIPPYMODEM_V34_PHASE2_CALLING_STATION)
		return transmitter->guard_amplitude == 0 &&
		       transmitter->guard_phase_step == 0;
	return transmitter->guard_amplitude > 0 &&
	       transmitter->guard_phase_step ==
		       phase_step(SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ) &&
	       transmitter->carrier_amplitude <=
		       INT16_MAX - transmitter->guard_amplitude;
}

enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_init(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter,
	enum sippymodem_v34_phase2_station_role local_role,
	int16_t nominal_amplitude)
{
	struct sippymodem_v34_phase2_tone_transmitter initialized = {
		.local_role = local_role,
	};
	long carrier_amplitude = nominal_amplitude;
	long guard_amplitude = 0;

	if (transmitter == NULL || !role_is_valid(local_role) ||
	    nominal_amplitude <= 0)
		return SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT;
	if (local_role == SIPPYMODEM_V34_PHASE2_ANSWERING_STATION) {
		carrier_amplitude = lround(
			(double)nominal_amplitude *
			V34_PHASE2_TONE_ANSWERING_CARRIER_GAIN);
		guard_amplitude = nominal_amplitude;
		initialized.guard_phase_step = phase_step(
			SIPPYMODEM_V34_PHASE2_ANSWERING_GUARD_HZ);
	}
	if (carrier_amplitude <= 0 || guard_amplitude < 0 ||
	    carrier_amplitude + guard_amplitude > INT16_MAX)
		return SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT;
	initialized.carrier_phase_step = phase_step(
		local_carrier_hz(local_role));
	initialized.carrier_amplitude = (int16_t)carrier_amplitude;
	initialized.guard_amplitude = (int16_t)guard_amplitude;
	if (!sippymodem_v34_phase2_tone_transmitter_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SIPPYMODEM_V34_PHASE2_TONE_OK;
}

enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_reverse(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter)
{
	if (!sippymodem_v34_phase2_tone_transmitter_is_valid(transmitter))
		return SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT;
	transmitter->carrier_phase += UINT32_C(0x80000000);
	return SIPPYMODEM_V34_PHASE2_TONE_OK;
}

enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_sample(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter,
	int16_t *sample)
{
	long mixed;

	if (!sippymodem_v34_phase2_tone_transmitter_is_valid(transmitter) ||
	    sample == NULL)
		return SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT;
	mixed = lround(
		(double)transmitter->carrier_amplitude *
		sin(phase_radians(transmitter->carrier_phase)));
	transmitter->carrier_phase += transmitter->carrier_phase_step;
	if (transmitter->guard_amplitude != 0) {
		mixed += lround(
			(double)transmitter->guard_amplitude *
			sin(phase_radians(transmitter->guard_phase)));
		transmitter->guard_phase += transmitter->guard_phase_step;
	}
	*sample = (int16_t)mixed;
	return SIPPYMODEM_V34_PHASE2_TONE_OK;
}

const char *sippymodem_v34_phase2_tone_result_name(
	enum sippymodem_v34_phase2_tone_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_TONE_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
