// SPDX-License-Identifier: BSD-3-Clause

#include "differential_qpsk.h"

#include <stddef.h>

enum differential_qpsk_result
differential_qpsk_encode(
	uint8_t previous_quadrant, uint8_t dibit, uint8_t *quadrant)
{
	/* Dibits 00, 01, 10, and 11 advance by 90, 0, 180, and 270 degrees. */
	static const uint8_t quarter_turns[QPSK_QUADRANT_COUNT] = {
		1, 0, 2, 3,
	};

	if (previous_quadrant >= QPSK_QUADRANT_COUNT ||
	    dibit >= QPSK_QUADRANT_COUNT || quadrant == NULL)
		return DIFFERENTIAL_QPSK_INVALID_ARGUMENT;
	*quadrant = (uint8_t)((previous_quadrant + quarter_turns[dibit]) & 3u);
	return DIFFERENTIAL_QPSK_OK;
}

enum differential_qpsk_result
differential_qpsk_decode(
	uint8_t previous_quadrant, uint8_t quadrant, uint8_t *dibit)
{
	/* The normative dibit-to-quarter-turn permutation is self-inverse. */
	static const uint8_t dibits[QPSK_QUADRANT_COUNT] = {
		1, 0, 2, 3,
	};
	uint8_t quarter_turns;

	if (previous_quadrant >= QPSK_QUADRANT_COUNT ||
	    quadrant >= QPSK_QUADRANT_COUNT || dibit == NULL)
		return DIFFERENTIAL_QPSK_INVALID_ARGUMENT;
	quarter_turns = (uint8_t)((quadrant - previous_quadrant) & 3u);
	*dibit = dibits[quarter_turns];
	return DIFFERENTIAL_QPSK_OK;
}

const char *differential_qpsk_result_name(
	enum differential_qpsk_result result)
{
	switch (result) {
	case DIFFERENTIAL_QPSK_OK:
		return "ok";
	case DIFFERENTIAL_QPSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
