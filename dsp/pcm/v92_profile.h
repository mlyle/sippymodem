// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PROFILE_H
#define V92_PROFILE_H

#include "pcm_call.h"

struct v90_upstream_policy;

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

/* Build a full-startup V.92 profile. Zero maximum PCM-upstream rate selects
 * conventional upstream only; its limits apply through INFO1, data and
 * retrain, including when a PCM-capable peer selects conventional operation.
 * With PCM upstream enabled, the analogue role advertises
 * every upstream rate through the maximum. The digital role chooses the
 * highest peer-enabled rate within that maximum after receiving Ja, then
 * constructs its complete direct CPd mapping for the selected rate.
 * PCM upstream is preferred, but a peer may select conventional upstream
 * within the same V.92 session. Quick connect remains a separate procedure. */
enum v92_profile_result v92_profile_init(
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_downstream_rate,
	unsigned int maximum_downstream_rate,
	uint8_t maximum_upstream_data_rate_number,
	const struct v90_upstream_policy *conventional_upstream);

const char *v92_profile_result_name(enum v92_profile_result result);

#endif /* V92_PROFILE_H */
