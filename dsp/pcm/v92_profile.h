// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PROFILE_H
#define V92_PROFILE_H

#include "pcm_call.h"

enum {
	V92_PROFILE_MIN_DOWNSTREAM_BIT_RATE = 28000,
	V92_PROFILE_MAX_DOWNSTREAM_BIT_RATE = 56000,
	V92_PROFILE_MIN_UPSTREAM_DATA_RATE_NUMBER = 1,
	V92_PROFILE_MAX_UPSTREAM_DATA_RATE_NUMBER = 19,
	V92_PROFILE_INITIAL_TRN1U_SYMBOLS = 10056,
};

enum v92_profile_result {
	V92_PROFILE_OK,
	V92_PROFILE_INVALID_ARGUMENT,
	V92_PROFILE_UNSUPPORTED_RATE,
};

/* Build the full-startup PCM-upstream profile. The maximum upstream data-rate
 * number advertises every V.92 PCM-upstream rate through that selection.
 * Quick connect and conventional upstream are separate procedures. */
enum v92_profile_result v92_pcm_profile_init(
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_downstream_rate,
	unsigned int maximum_downstream_rate,
	uint8_t maximum_upstream_data_rate_number);

const char *v92_profile_result_name(enum v92_profile_result result);

#endif /* V92_PROFILE_H */
