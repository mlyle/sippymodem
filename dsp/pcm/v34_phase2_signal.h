// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_SIGNAL_H
#define V34_PHASE2_SIGNAL_H

#include "phase.h"
#include "phase2_signal.h"
#include "v34_station.h"

enum {
	V34_PHASE2_SAMPLE_RATE = 8000,
	V34_PHASE2_CALLING_CARRIER_HZ = 1200,
	V34_PHASE2_ANSWERING_GUARD_HZ = 1800,
	V34_PHASE2_ANSWERING_CARRIER_HZ = 2400,
};

static inline unsigned int
v34_phase2_carrier_hz(enum v34_station_role transmitting_role)
{
	return transmitting_role == V34_CALLING_STATION ?
		       V34_PHASE2_CALLING_CARRIER_HZ :
	       transmitting_role == V34_ANSWERING_STATION ?
		       V34_PHASE2_ANSWERING_CARRIER_HZ : 0u;
}

/* Tone A and answering INFO share the -1 dB carrier adjustment. Guard
 * levels differ between those signals and remain with their transmitters.
 * The owner supplies a validated role and positive nominal peak. */
static inline long v34_phase2_carrier_level(
	enum v34_station_role role, int16_t nominal_amplitude)
{
	return role == V34_ANSWERING_STATION ?
		lround((double)nominal_amplitude * 0.8912509381337456) : nominal_amplitude;
}

/* Calling signals have no guard; answering signals require the same 1800 Hz
 * oscillator and sufficient signed-sample headroom for the sum. Phases are
 * unrestricted so a successor can retain the exact physical boundary. */
static inline bool v34_phase2_guard_is_valid(enum v34_station_role role,
	int16_t carrier_amplitude, int16_t guard_amplitude, uint32_t guard_phase_step)
{
	if (!v34_station_role_is_valid(role) || carrier_amplitude <= 0)
		return false;
	if (role == V34_CALLING_STATION)
		return guard_amplitude == 0 && guard_phase_step == 0;
	return guard_amplitude > 0 && guard_phase_step == phase_step_u32(
		V34_PHASE2_SAMPLE_RATE, V34_PHASE2_ANSWERING_GUARD_HZ, 1) &&
	       carrier_amplitude <= INT16_MAX - guard_amplitude;
}

static inline enum phase2_signal_role
v34_phase2_signal_for_station(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ? PHASE2_SIGNAL_B :
	       role == V34_ANSWERING_STATION ? PHASE2_SIGNAL_A :
		       (enum phase2_signal_role)-1;
}

static inline enum v34_station_role
v34_phase2_station_for_signal(enum phase2_signal_role role)
{
	return role == PHASE2_SIGNAL_B ? V34_CALLING_STATION :
	       role == PHASE2_SIGNAL_A ? V34_ANSWERING_STATION :
		       (enum v34_station_role)-1;
}

#endif
