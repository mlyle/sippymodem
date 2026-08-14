// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PROFILE_H
#define V90_PROFILE_H

#include "v90_call.h"

enum {
	/* Higher PCM rates remain unavailable until their CP mappings are tested. */
	V90_PROFILE_DOWNSTREAM_BIT_RATE = 28000,
};

enum v90_profile_result {
	V90_PROFILE_OK,
	V90_PROFILE_INVALID_ARGUMENT,
	V90_PROFILE_UNSUPPORTED_RATE,
};

/* Build the first generated operating profile. Rate limits refer to the PCM
 * downstream; the conventional upstream is selected during MP/CP exchange. */
enum v90_profile_result v90_profile_init(
	struct v90_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate);

const char *v90_profile_result_name(enum v90_profile_result result);

#endif /* V90_PROFILE_H */
