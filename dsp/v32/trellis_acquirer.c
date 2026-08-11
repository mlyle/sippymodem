// SPDX-License-Identifier: BSD-3-Clause

#include "trellis_acquirer.h"

#include <limits.h>
#include <stddef.h>

enum {
	V32_ACQUISITION_MIN_CONFIRMATIONS = 8,
	V32_ACQUISITION_MIN_LEAD = 8,
};

bool sippymodem_v32_trellis_acquirer_is_valid(
	const struct sippymodem_v32_trellis_acquirer *acquirer)
{
	uint8_t data_bits;
	enum sippymodem_v32_transmitter_role role;

	if (acquirer == NULL ||
	    acquirer->selected_phase >= QPSK_QUADRANT_COUNT)
		return false;
	data_bits = acquirer->candidates[0].decoder.data_bits_per_symbol;
	role = acquirer->candidates[0].descrambler.transmitter_role;
	for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT;
	     ++phase) {
		const struct sippymodem_v32_trellis_receiver *candidate =
			&acquirer->candidates[phase];

		if (!sippymodem_v32_trellis_receiver_is_valid(candidate) ||
		    candidate->decoder.data_bits_per_symbol != data_bits ||
		    candidate->descrambler.transmitter_role != role ||
		    acquirer->confirmations[phase] > acquirer->observations)
			return false;
	}
	return true;
}

enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_init(
	struct sippymodem_v32_trellis_acquirer *acquirer,
	const struct sippymodem_v32_symbol_decoder *framing,
	const struct qam_point *reference_point,
	uint8_t data_bits_per_symbol)
{
	struct sippymodem_v32_trellis_acquirer initialized = { 0 };

	if (acquirer == NULL)
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT;
	     ++phase)
		if (sippymodem_v32_trellis_receiver_init(
			    &initialized.candidates[phase], framing,
			    reference_point, phase, data_bits_per_symbol) !=
		    SIPPYMODEM_V32_TRELLIS_RECEIVER_OK)
			return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	if (!sippymodem_v32_trellis_acquirer_is_valid(&initialized))
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	*acquirer = initialized;
	return SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK;
}

enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_track_known(
	struct sippymodem_v32_trellis_acquirer *acquirer,
	const struct qam_point *point, uint8_t input)
{
	struct sippymodem_v32_trellis_acquirer next;

	if (!sippymodem_v32_trellis_acquirer_is_valid(acquirer) ||
	    acquirer->selected || acquirer->observations == UINT16_MAX)
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	next = *acquirer;
	for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT;
	     ++phase) {
		bool confirmed;

		if (sippymodem_v32_trellis_receiver_track_known(
			    &next.candidates[phase], point, input, &confirmed) !=
		    SIPPYMODEM_V32_TRELLIS_RECEIVER_OK)
			return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
		if (confirmed && next.confirmations[phase] != UINT16_MAX)
			next.confirmations[phase]++;
	}
	next.observations++;
	if (!sippymodem_v32_trellis_acquirer_is_valid(&next))
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	*acquirer = next;
	return SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK;
}

enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_select(
	struct sippymodem_v32_trellis_acquirer *acquirer)
{
	struct sippymodem_v32_trellis_acquirer next;
	uint16_t best;
	uint16_t runner_up = 0;
	uint8_t selected = 0;

	if (!sippymodem_v32_trellis_acquirer_is_valid(acquirer))
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	if (acquirer->selected)
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK;
	for (uint8_t phase = 1; phase < QPSK_QUADRANT_COUNT;
	     ++phase)
		if (acquirer->confirmations[phase] >
		    acquirer->confirmations[selected])
			selected = phase;
	best = acquirer->confirmations[selected];
	for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT;
	     ++phase)
		if (phase != selected &&
		    acquirer->confirmations[phase] > runner_up)
			runner_up = acquirer->confirmations[phase];
	if (best < V32_ACQUISITION_MIN_CONFIRMATIONS ||
	    best - runner_up < V32_ACQUISITION_MIN_LEAD)
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_NOT_READY;
	next = *acquirer;
	next.selected_phase = selected;
	next.selected = true;
	*acquirer = next;
	return SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK;
}

enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_decode(
	struct sippymodem_v32_trellis_acquirer *acquirer,
	const struct qam_point *point, uint8_t *output, bool *ready)
{
	if (!sippymodem_v32_trellis_acquirer_is_valid(acquirer) ||
	    output == NULL || ready == NULL)
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
	if (!acquirer->selected)
		return SIPPYMODEM_V32_TRELLIS_ACQUIRER_NOT_READY;
	return sippymodem_v32_trellis_receiver_decode(
		       &acquirer->candidates[acquirer->selected_phase], point,
		       output, ready) == SIPPYMODEM_V32_TRELLIS_RECEIVER_OK ?
		       SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK :
		       SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT;
}

const char *sippymodem_v32_trellis_acquirer_result_name(
	enum sippymodem_v32_trellis_acquirer_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK:
		return "ok";
	case SIPPYMODEM_V32_TRELLIS_ACQUIRER_NOT_READY:
		return "not ready";
	case SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
