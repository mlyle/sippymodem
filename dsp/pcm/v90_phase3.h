// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_H
#define V90_PHASE3_H

#include "v34_training_line.h"

#include <stdbool.h>

enum {
	V90_PHASE3_SAMPLE_RATE = 8000,
};

/* Conventional upstream keeps one zero-MD analogue profile and one 8 kHz
 * line clock continuous from Phase 3 through CP and E. */
static inline bool v90_phase3_analog_profile_is_valid(
	const struct v34_phase3_profile *profile)
{
	return v34_phase3_profile_is_valid(profile) &&
	       profile->local_role == V34_ANSWERING_STATION &&
	       profile->md_symbol_count == 0;
}

static inline bool v90_phase3_training_line_is_valid(
	const struct v34_training_line *line)
{
	return v34_training_line_is_valid(line) &&
	       line->carrier.channel.sample_rate == V90_PHASE3_SAMPLE_RATE &&
	       v90_phase3_analog_profile_is_valid(&line->profile);
}

#endif /* V90_PHASE3_H */
