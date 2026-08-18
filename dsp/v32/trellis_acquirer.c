// SPDX-License-Identifier: BSD-3-Clause

#include "trellis_acquirer.h"

#include <limits.h>
#include <stddef.h>

enum {
	V32_ACQUISITION_MIN_CONFIRMATIONS = 8,
	V32_ACQUISITION_MIN_LEAD = 8,
};

bool v32_trellis_acquirer_is_valid(
	const struct v32_trellis_acquirer *acquirer)
{
	uint8_t data_bits;
	enum v32_transmitter_role role;

	if (acquirer == NULL ||
	    acquirer->selected_differential_output >=
		    QPSK_QUADRANT_COUNT)
		return false;
	data_bits = acquirer->candidates[0].decoder.data_bits_per_symbol;
	role = acquirer->candidates[0].descrambler.transmitter_role;
	for (uint8_t differential_output = 0;
	     differential_output < QPSK_QUADRANT_COUNT;
	     ++differential_output) {
		const struct v32_trellis_receiver *candidate =
			&acquirer->candidates[differential_output];

		if (!v32_trellis_receiver_is_valid(candidate) ||
		    candidate->decoder.data_bits_per_symbol != data_bits ||
		    candidate->descrambler.transmitter_role != role ||
		    acquirer->confirmations[differential_output] >
			    acquirer->observations)
			return false;
	}
	return true;
}

enum v32_trellis_acquirer_result
v32_trellis_acquirer_init(
	struct v32_trellis_acquirer *acquirer,
	const struct v32_symbol_decoder *framing,
	const struct qam_point *reference_point,
	uint8_t data_bits_per_symbol)
{
	struct v32_trellis_acquirer initialized = { 0 };

	if (acquirer == NULL || framing == NULL)
		return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	for (uint8_t differential_output = 0;
	     differential_output < QPSK_QUADRANT_COUNT;
	     ++differential_output)
		if (v32_trellis_receiver_init(
			    &initialized.candidates[differential_output], framing,
			    reference_point, framing->phase_quadrant,
			    differential_output, data_bits_per_symbol) !=
		    V32_TRELLIS_RECEIVER_OK)
			return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	if (!v32_trellis_acquirer_is_valid(&initialized))
		return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	*acquirer = initialized;
	return V32_TRELLIS_ACQUIRER_OK;
}

enum v32_trellis_acquirer_result
v32_trellis_acquirer_track_known(
	struct v32_trellis_acquirer *acquirer,
	const struct qam_point *point, uint8_t input)
{
	struct v32_trellis_acquirer next;

	if (!v32_trellis_acquirer_is_valid(acquirer) ||
	    acquirer->selected || acquirer->observations == UINT16_MAX)
		return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	next = *acquirer;
	for (uint8_t differential_output = 0;
	     differential_output < QPSK_QUADRANT_COUNT;
	     ++differential_output) {
		bool confirmed;

		if (v32_trellis_receiver_track_known(
			    &next.candidates[differential_output], point, input,
			    &confirmed) !=
		    V32_TRELLIS_RECEIVER_OK)
			return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
		if (confirmed &&
		    next.confirmations[differential_output] != UINT16_MAX)
			next.confirmations[differential_output]++;
	}
	next.observations++;
	if (!v32_trellis_acquirer_is_valid(&next))
		return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	*acquirer = next;
	return V32_TRELLIS_ACQUIRER_OK;
}

enum v32_trellis_acquirer_result
v32_trellis_acquirer_select(
	struct v32_trellis_acquirer *acquirer)
{
	struct v32_trellis_acquirer next;
	uint16_t best;
	uint16_t runner_up = 0;
	uint8_t selected = 0;

	if (!v32_trellis_acquirer_is_valid(acquirer))
		return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	if (acquirer->selected)
		return V32_TRELLIS_ACQUIRER_OK;
	for (uint8_t differential_output = 1;
	     differential_output < QPSK_QUADRANT_COUNT;
	     ++differential_output)
		if (acquirer->confirmations[differential_output] >
		    acquirer->confirmations[selected])
			selected = differential_output;
	best = acquirer->confirmations[selected];
	for (uint8_t differential_output = 0;
	     differential_output < QPSK_QUADRANT_COUNT;
	     ++differential_output)
		if (differential_output != selected &&
		    acquirer->confirmations[differential_output] > runner_up)
			runner_up =
				acquirer->confirmations[differential_output];
	if (best < V32_ACQUISITION_MIN_CONFIRMATIONS ||
	    best - runner_up < V32_ACQUISITION_MIN_LEAD)
		return V32_TRELLIS_ACQUIRER_NOT_READY;
	next = *acquirer;
	next.selected_differential_output = selected;
	next.selected = true;
	*acquirer = next;
	return V32_TRELLIS_ACQUIRER_OK;
}

enum v32_trellis_acquirer_result
v32_trellis_acquirer_decode(
	struct v32_trellis_acquirer *acquirer,
	const struct qam_point *point, uint8_t *output, bool *ready)
{
	if (!v32_trellis_acquirer_is_valid(acquirer) ||
	    output == NULL || ready == NULL)
		return V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	if (!acquirer->selected)
		return V32_TRELLIS_ACQUIRER_NOT_READY;
	return v32_trellis_receiver_decode(
		       &acquirer->candidates[
			       acquirer->selected_differential_output],
		       point,
		       output, ready) == V32_TRELLIS_RECEIVER_OK ?
		       V32_TRELLIS_ACQUIRER_OK :
		       V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
}

const char *v32_trellis_acquirer_result_name(
	enum v32_trellis_acquirer_result result)
{
	switch (result) {
	case V32_TRELLIS_ACQUIRER_OK:
		return "ok";
	case V32_TRELLIS_ACQUIRER_NOT_READY:
		return "not ready";
	case V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
