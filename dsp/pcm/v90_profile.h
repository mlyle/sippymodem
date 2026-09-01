// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PROFILE_H
#define V90_PROFILE_H

#include "pcm_call.h"
#include "pcm_rate.h"

enum {
	V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE = 28000,
	V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE = 56000,
};

/* Zero fields select the generated conventional-upstream defaults. */
struct v90_upstream_policy {
	unsigned int minimum_bit_rate;
	unsigned int maximum_bit_rate;
	/* Analogue INFO1a selection; digital INFO1d retains mandatory rates. */
	unsigned int maximum_symbol_rate;
};

enum v90_profile_result {
	V90_PROFILE_OK,
	V90_PROFILE_INVALID_ARGUMENT,
	V90_PROFILE_UNSUPPORTED_RATE,
};

/* Build a generated exact-G.711 operating profile. Rate limits refer to the
 * PCM downstream; the conventional upstream is selected during MP/CP
 * exchange. */
enum v90_profile_result v90_profile_init(
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate,
	const struct v90_upstream_policy *upstream);

const char *v90_profile_result_name(enum v90_profile_result result);

#endif /* V90_PROFILE_H */
