// SPDX-License-Identifier: BSD-3-Clause

#include "v90_ja.h"

#include "v34_constellation.h"
#include "v34_phase3_trn.h"
#include "packed_bits.h"

#include <stddef.h>
#include <string.h>

static bool analogue_boundary_is_valid(const struct v34_phase3_trn *trn)
{
	return v34_phase3_trn_is_valid(trn) && trn->has_last_symbol &&
	       trn->role == V34_ANSWERING_STATION;
}

static bool analogue_scrambler_is_valid(
	const struct v34_scrambler *scrambler)
{
	return v34_transmit_scrambler_is_valid(
		scrambler, V34_ANSWERING_STATION);
}

static uint8_t descriptor_dibit(
	const uint8_t *descriptor, uint16_t bit_index)
{
	unsigned int shift = 6u - bit_index % 8u;

	return (uint8_t)((descriptor[bit_index / 8u] >> shift) & 3u);
}

static void store_descriptor_dibit(
	uint8_t *descriptor, uint16_t bit_index, uint8_t dibit)
{
	uint8_t mask = (uint8_t)(3u << (6u - bit_index % 8u));
	uint8_t *octet = &descriptor[bit_index / 8u];

	*octet = (uint8_t)((*octet & (uint8_t)~mask) |
			   (uint8_t)(dibit << (6u - bit_index % 8u)));
}

static bool unread_bits_are_zero(
	const uint8_t *descriptor, uint16_t first_bit)
{
	for (uint16_t bit = first_bit; bit < V90_DIL_ZERO_DESCRIPTOR_BITS;
	     ++bit) {
		if (packed_msb_bit(descriptor, bit) != 0)
			return false;
	}
	return true;
}

bool v90_ja_tx_is_valid(const struct v90_ja_tx *tx)
{
	return tx != NULL && analogue_scrambler_is_valid(&tx->scrambler) &&
	       tx->previous_clockwise_quarter_turns < 4u &&
	       tx->bit_index < V90_DIL_ZERO_DESCRIPTOR_BITS &&
	       tx->bit_index % 2u == 0 &&
	       v90_dil_decode_zero(tx->descriptor, sizeof(tx->descriptor)) ==
		       V90_DIL_OK;
}

enum v90_ja_result v90_ja_tx_init_from_trn(
	struct v90_ja_tx *tx, const struct v34_phase3_trn *trn)
{
	struct v90_ja_tx initialized = { 0 };

	if (tx == NULL || !analogue_boundary_is_valid(trn) ||
	    v90_dil_encode_zero(
		    initialized.descriptor,
		    sizeof(initialized.descriptor)) != V90_DIL_OK)
		return V90_JA_INVALID_ARGUMENT;
	initialized.scrambler = trn->scrambler;
	initialized.previous_clockwise_quarter_turns =
		trn->last_clockwise_quarter_turns;
	if (!v90_ja_tx_is_valid(&initialized))
		return V90_JA_INVALID_ARGUMENT;
	*tx = initialized;
	return V90_JA_OK;
}

bool v90_ja_tx_at_descriptor_boundary(const struct v90_ja_tx *tx)
{
	return v90_ja_tx_is_valid(tx) && tx->bit_index == 0;
}

enum v90_ja_result v90_ja_tx_next(
	struct v90_ja_tx *tx, struct qam_point *point,
	uint8_t *clockwise_quarter_turns)
{
	struct v90_ja_tx next;
	struct qam_point next_point;
	uint8_t rotation;

	if (!v90_ja_tx_is_valid(tx) || point == NULL ||
	    clockwise_quarter_turns == NULL)
		return V90_JA_INVALID_ARGUMENT;
	next = *tx;
	if (v34_scramble_differential_dibit(
		    &next.scrambler,
		    descriptor_dibit(next.descriptor, next.bit_index),
		    next.previous_clockwise_quarter_turns, &rotation) !=
	    V34_FRAMING_OK)
		return V90_JA_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = rotation;
	next.bit_index += 2u;
	if (next.bit_index == V90_DIL_ZERO_DESCRIPTOR_BITS)
		next.bit_index = 0;
	next_point = v34_rotated_point_zero(-(int)rotation);
	if (!v90_ja_tx_is_valid(&next))
		return V90_JA_INVALID_ARGUMENT;
	*tx = next;
	*point = next_point;
	*clockwise_quarter_turns = rotation;
	return V90_JA_OK;
}

bool v90_ja_rx_is_valid(const struct v90_ja_rx *rx)
{
	if (rx == NULL || !analogue_scrambler_is_valid(&rx->descrambler) ||
	    rx->previous_clockwise_quarter_turns >= 4u ||
	    rx->bit_index % 2u != 0)
		return false;
	if (rx->complete)
		return rx->bit_index == V90_DIL_ZERO_DESCRIPTOR_BITS &&
		       v90_dil_decode_zero(
			       rx->descriptor, sizeof(rx->descriptor)) == V90_DIL_OK;
	return rx->bit_index < V90_DIL_ZERO_DESCRIPTOR_BITS &&
	       unread_bits_are_zero(rx->descriptor, rx->bit_index);
}

enum v90_ja_result v90_ja_rx_init_from_trn(
	struct v90_ja_rx *rx, const struct v34_phase3_trn *trn,
	uint8_t observed_previous_clockwise_quarter_turns)
{
	struct v90_ja_rx initialized = { 0 };

	if (rx == NULL || !analogue_boundary_is_valid(trn) ||
	    observed_previous_clockwise_quarter_turns >= 4u)
		return V90_JA_INVALID_ARGUMENT;
	initialized.descrambler = trn->scrambler;
	initialized.previous_clockwise_quarter_turns =
		observed_previous_clockwise_quarter_turns;
	if (!v90_ja_rx_is_valid(&initialized))
		return V90_JA_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_JA_OK;
}

bool v90_ja_rx_is_complete(const struct v90_ja_rx *rx)
{
	return v90_ja_rx_is_valid(rx) && rx->complete;
}

enum v90_ja_result v90_ja_rx_receive(
	struct v90_ja_rx *rx, uint8_t clockwise_quarter_turns)
{
	struct v90_ja_rx next;
	enum v90_dil_result descriptor_result;
	uint8_t dibit;

	if (!v90_ja_rx_is_valid(rx) || clockwise_quarter_turns >= 4u)
		return V90_JA_INVALID_ARGUMENT;
	if (rx->complete)
		return V90_JA_COMPLETE;
	next = *rx;
	if (v34_descramble_differential_dibit(
		    &next.descrambler,
		    next.previous_clockwise_quarter_turns,
		    clockwise_quarter_turns, &dibit) != V34_FRAMING_OK)
		return V90_JA_INVALID_ARGUMENT;
	store_descriptor_dibit(next.descriptor, next.bit_index, dibit);
	next.bit_index += 2u;
	next.previous_clockwise_quarter_turns = clockwise_quarter_turns;
	if (next.bit_index < V90_DIL_ZERO_DESCRIPTOR_BITS) {
		if (!v90_ja_rx_is_valid(&next))
			return V90_JA_INVALID_ARGUMENT;
		*rx = next;
		return V90_JA_OK;
	}
	descriptor_result = v90_dil_decode_zero(
		next.descriptor, sizeof(next.descriptor));
	if (descriptor_result == V90_DIL_OK) {
		next.complete = true;
		*rx = next;
		return V90_JA_DETECTED;
	}
	memset(next.descriptor, 0, sizeof(next.descriptor));
	next.bit_index = 0;
	if (!v90_ja_rx_is_valid(&next))
		return V90_JA_INVALID_ARGUMENT;
	*rx = next;
	return descriptor_result == V90_DIL_NONZERO_DESCRIPTOR ?
		       V90_JA_UNSUPPORTED :
		       V90_JA_REJECTED;
}

const char *v90_ja_result_name(enum v90_ja_result result)
{
	switch (result) {
	case V90_JA_OK:
		return "ok";
	case V90_JA_DETECTED:
		return "detected";
	case V90_JA_REJECTED:
		return "rejected";
	case V90_JA_UNSUPPORTED:
		return "unsupported";
	case V90_JA_COMPLETE:
		return "complete";
	case V90_JA_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
