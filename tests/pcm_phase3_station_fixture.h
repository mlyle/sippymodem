// SPDX-License-Identifier: BSD-3-Clause
#ifndef PCM_PHASE3_STATION_FIXTURE_H
#define PCM_PHASE3_STATION_FIXTURE_H

#include <v90_phase3_analog_station.h>
#include <v90_phase3_digital_station.h>
#include "pcm_sign_state_fixture.h"
#include "v34_state_fixture.h"

/* Keep all other state bytes; compare the nested owners by their fields so
 * padding in copied training lines and J records cannot affect the result. */
static inline bool v90_analog_station_states_equal(
	const struct v90_phase3_analog_station *left,
	const struct v90_phase3_analog_station *right)
{
	struct v90_phase3_analog_station copies[2];
	const struct v34_training_line *a, *b;

	if (left->transmit_stage != right->transmit_stage)
		return false;
	if (left->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_INITIAL) {
		a = &left->transmit.initial.line;
		b = &right->transmit.initial.line;
	} else if (left->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_RETURN ||
		   left->transmit_stage == V90_PHASE3_ANALOG_TRANSMIT_DIL_RETURN) {
		a = &left->transmit.returning.line;
		b = &right->transmit.returning.line;
	} else {
		/* Invalid-tag fixtures are completely initialized byte sentinels. */
		return memcmp(left, right, sizeof(*left)) == 0;
	}
	if (!v34_training_lines_equal(a, b) ||
	    left->receive_stage != right->receive_stage)
		return false;
	if (left->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS &&
	    !pcm_j_receivers_equal(&left->receive.signs, &right->receive.signs))
		return false;
	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	/* Both union variants begin with the same training-line owner. */
	memset(&copies[0].transmit.initial.line, 0, sizeof(*a));
	memset(&copies[1].transmit.initial.line, 0, sizeof(*b));
	if (left->receive_stage == V90_PHASE3_ANALOG_RECEIVE_SIGNS) {
		memset(&copies[0].receive.signs, 0, sizeof(left->receive.signs));
		memset(&copies[1].receive.signs, 0, sizeof(right->receive.signs));
	}
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

static inline bool v90_digital_station_states_equal(
	const struct v90_phase3_digital_station *left,
	const struct v90_phase3_digital_station *right)
{
	struct v90_phase3_digital_station copies[2];

	if (!v90_jd_parameters_equal(&left->jd, &right->jd))
		return false;
	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	memset(&copies[0].jd, 0, sizeof(left->jd));
	memset(&copies[1].jd, 0, sizeof(right->jd));
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

#endif
