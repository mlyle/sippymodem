// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_STATE_FIXTURE_H
#define V90_PHASE4_STATE_FIXTURE_H

#include <v90_phase4_analog_startup.h>
#include <v90_phase4_digital_startup.h>
#include "qam_state_fixture.h"

/* Normalize copies only. Keep exact named-field bytes and retained buffers;
 * padding and storage outside the active union member are not modem state. */
static inline void v90_snapshot_ri_transmit(struct v90_phase4_ri_tx *tx)
{
	unsigned char bytes[sizeof(*tx)] = { 0 };
#define COPY(field) memcpy(bytes + offsetof(struct v90_phase4_ri_tx, field), \
	&tx->field, sizeof(tx->field))
	COPY(stage);
	COPY(law);
	COPY(ri_symbols);
	COPY(ri_bar_symbols);
	COPY(phase);
	COPY(training_ucode);
	COPY(finish_requested);
#undef COPY
	memcpy(tx, bytes, sizeof(bytes));
}

static inline bool v90_cp_line_transmit_states_equal(
	const struct v90_phase4_cp_line_tx *left,
	const struct v90_phase4_cp_line_tx *right)
{
	struct v90_phase4_cp_line_tx copies[2];

	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	for (size_t i = 0; i < 2; ++i)
		qam_snapshot_channel(&copies[i].line.carrier.channel);
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

static inline bool v90_analog_station_states_equal(
	const struct v90_phase4_analog_station *left,
	const struct v90_phase4_analog_station *right)
{
	struct v90_phase4_analog_station copies[2];

	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	for (size_t i = 0; i < 2; ++i)
		qam_snapshot_channel(&copies[i].transmit.line.carrier.channel);
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

static inline bool v90_digital_station_states_equal(
	const struct v90_phase4_digital_station *left,
	const struct v90_phase4_digital_station *right)
{
	struct v90_phase4_digital_station copies[2];

	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	for (size_t i = 0; i < 2; ++i)
		v90_snapshot_ri_transmit(&copies[i].transmit);
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

static inline bool v90_analog_startup_states_equal(
	const struct v90_phase4_analog_startup *left,
	const struct v90_phase4_analog_startup *right)
{
	struct v90_phase4_analog_startup copies[2];

	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	for (size_t i = 0; i < 2; ++i) {
		struct v90_phase4_analog_startup *state = &copies[i];
		struct v90_phase4_cp_line_tx *tx;
		size_t active_size;

		switch (state->stage) {
		case V90_PHASE4_STARTUP_INITIAL:
			tx = &state->state.initial.transmit;
			active_size = sizeof(state->state.initial);
			break;
		case V90_PHASE4_STARTUP_CONTINUING:
			tx = &state->state.continuing.station.transmit;
			active_size = sizeof(state->state.continuing);
			break;
		case V90_PHASE4_STARTUP_COMPLETE:
			tx = &state->state.complete.transmit;
			active_size = sizeof(state->state.complete);
			break;
		default:
			/* Invalid sentinels still compare byte for byte. */
			continue;
		}
		qam_snapshot_channel(&tx->line.carrier.channel);
		memset((unsigned char *)&state->state + active_size, 0,
		       sizeof(state->state) - active_size);
	}
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

static inline bool v90_digital_startup_states_equal(
	const struct v90_phase4_digital_startup *left,
	const struct v90_phase4_digital_startup *right)
{
	struct v90_phase4_digital_startup copies[2];

	memcpy(&copies[0], left, sizeof(*left));
	memcpy(&copies[1], right, sizeof(*right));
	for (size_t i = 0; i < 2; ++i) {
		struct v90_phase4_digital_startup *state = &copies[i];
		size_t active_size;

		switch (state->stage) {
		case V90_PHASE4_STARTUP_INITIAL:
			v90_snapshot_ri_transmit(&state->state.initial.transmit);
			active_size = sizeof(state->state.initial);
			break;
		case V90_PHASE4_STARTUP_CONTINUING:
			v90_snapshot_ri_transmit(
				&state->state.continuing.station.transmit);
			active_size = sizeof(state->state.continuing);
			break;
		case V90_PHASE4_STARTUP_COMPLETE:
			active_size = sizeof(state->state.complete);
			break;
		default:
			continue;
		}
		memset((unsigned char *)&state->state + active_size, 0,
		       sizeof(state->state) - active_size);
	}
	return memcmp(&copies[0], &copies[1], sizeof(*left)) == 0;
}

#endif
