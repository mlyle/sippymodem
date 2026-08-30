// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_SIGNAL_H
#define V34_PHASE2_SIGNAL_H

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
