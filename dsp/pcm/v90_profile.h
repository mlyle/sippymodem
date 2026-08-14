// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PROFILE_H
#define V90_PROFILE_H

#include "v90_call.h"

enum {
	V90_PROFILE_DOWNSTREAM_RATE_COUNT = V90_CP_RATE_INDEX_MAX,
	V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE = 28000,
	V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE = 56000,
};

struct v90_downstream_selection {
	uint32_t enabled_rate_mask;
	uint32_t selected_bit_rate;
	uint8_t selected_rate_index;
};

enum v90_profile_result {
	V90_PROFILE_OK,
	V90_PROFILE_INVALID_ARGUMENT,
	V90_PROFILE_UNSUPPORTED_RATE,
};

/* Convert the CP/Jd index to its nearest integral bit/s representation.
 * Zero is returned for the cleardown value and out-of-range indices. */
uint32_t v90_profile_downstream_bit_rate(uint8_t rate_index);

/* Enable every standard downstream rate in the inclusive caller range and
 * select the highest one. Rejected calls leave selection unchanged. */
enum v90_profile_result v90_profile_select_downstream(
	struct v90_downstream_selection *selection,
	unsigned int minimum_rate, unsigned int maximum_rate);

/* Build a generated exact-G.711 operating profile. Rate limits refer to the
 * PCM downstream; the conventional upstream is selected during MP/CP
 * exchange. */
enum v90_profile_result v90_profile_init(
	struct v90_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate);

const char *v90_profile_result_name(enum v90_profile_result result);

#endif /* V90_PROFILE_H */
