// SPDX-License-Identifier: BSD-3-Clause

#include "v90_ja_search.h"

#include "packed_bits.h"

#include <stddef.h>
#include <string.h>

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		if (bytes[index] != 0)
			return false;
	}
	return true;
}

static bool collected_bits_are_valid(
	const struct v90_ja_search *search)
{
	if (!search->collecting)
		return search->bit_index == 0 &&
		       bytes_are_zero(
			       search->descriptor,
			       sizeof(search->descriptor));
	if (search->bit_index < V90_DIL_FRAME_SYNC_BITS + 1u ||
	    search->bit_index > V90_DIL_ZERO_DESCRIPTOR_BITS)
		return false;
	for (size_t bit = 0; bit < V90_DIL_FRAME_SYNC_BITS; ++bit) {
		if (packed_msb_bit(search->descriptor, bit) == 0)
			return false;
	}
	if (packed_msb_bit(
		    search->descriptor, V90_DIL_FRAME_SYNC_BITS) != 0)
		return false;
	for (size_t bit = search->bit_index;
	     bit < V90_DIL_ZERO_DESCRIPTOR_BITS; ++bit) {
		if (packed_msb_bit(search->descriptor, bit) != 0)
			return false;
	}
	return true;
}

bool v90_ja_search_is_valid(const struct v90_ja_search *search)
{
	if (search == NULL ||
	    !v34_transmit_scrambler_is_valid(
		    &search->descrambler, V34_ANSWERING_STATION) ||
	    search->previous_clockwise_quarter_turns >= 4u ||
	    search->warmup_bits > V90_JA_SEARCH_WARMUP_BITS ||
	    search->consecutive_ones > V90_DIL_FRAME_SYNC_BITS ||
	    (!search->has_previous_rotation &&
	     search->previous_clockwise_quarter_turns != 0) ||
	    !collected_bits_are_valid(search))
		return false;
	if (!search->complete)
		return search->bit_index < V90_DIL_ZERO_DESCRIPTOR_BITS;
	return search->collecting &&
	       search->bit_index == V90_DIL_ZERO_DESCRIPTOR_BITS &&
	       v90_dil_decode_zero(
		       search->descriptor, sizeof(search->descriptor)) ==
		       V90_DIL_OK;
}

enum v90_ja_search_result v90_ja_search_init(
	struct v90_ja_search *search)
{
	struct v90_ja_search initialized = { 0 };

	if (search == NULL ||
	    v34_transmit_scrambler_init(
		    &initialized.descrambler,
		    V34_ANSWERING_STATION) != V34_FRAMING_OK ||
	    !v90_ja_search_is_valid(&initialized))
		return V90_JA_SEARCH_INVALID_ARGUMENT;
	*search = initialized;
	return V90_JA_SEARCH_OK;
}

bool v90_ja_search_is_complete(const struct v90_ja_search *search)
{
	return v90_ja_search_is_valid(search) && search->complete;
}

static void store_bit(
	uint8_t *descriptor, uint16_t index, uint8_t bit)
{
	if (bit != 0)
		descriptor[index / 8u] |=
			(uint8_t)(1u << (7u - index % 8u));
}

static void start_descriptor(struct v90_ja_search *search)
{
	memset(search->descriptor, 0, sizeof(search->descriptor));
	for (size_t bit = 0; bit < V90_DIL_FRAME_SYNC_BITS; ++bit)
		store_bit(search->descriptor, (uint16_t)bit, 1);
	search->bit_index = V90_DIL_FRAME_SYNC_BITS + 1u;
	search->consecutive_ones = 0;
	search->collecting = true;
}

static void discard_descriptor(struct v90_ja_search *search)
{
	memset(search->descriptor, 0, sizeof(search->descriptor));
	search->bit_index = 0;
	search->consecutive_ones = 0;
	search->collecting = false;
}

static enum v90_ja_search_result receive_bit(
	struct v90_ja_search *search, uint8_t bit)
{
	enum v90_dil_result descriptor_result;

	if (search->warmup_bits < V90_JA_SEARCH_WARMUP_BITS) {
		search->warmup_bits++;
		return V90_JA_SEARCH_OK;
	}
	if (!search->collecting) {
		if (bit != 0) {
			if (search->consecutive_ones < V90_DIL_FRAME_SYNC_BITS)
				search->consecutive_ones++;
		} else if (search->consecutive_ones ==
			   V90_DIL_FRAME_SYNC_BITS) {
			start_descriptor(search);
		} else {
			search->consecutive_ones = 0;
		}
		return V90_JA_SEARCH_OK;
	}
	store_bit(search->descriptor, search->bit_index, bit);
	search->bit_index++;
	if (search->bit_index < V90_DIL_ZERO_DESCRIPTOR_BITS)
		return V90_JA_SEARCH_OK;
	descriptor_result = v90_dil_decode_zero(
		search->descriptor, sizeof(search->descriptor));
	if (descriptor_result == V90_DIL_OK) {
		search->complete = true;
		return V90_JA_SEARCH_DETECTED;
	}
	discard_descriptor(search);
	return descriptor_result == V90_DIL_NONZERO_DESCRIPTOR ?
		       V90_JA_SEARCH_UNSUPPORTED :
		       V90_JA_SEARCH_REJECTED;
}

enum v90_ja_search_result v90_ja_search_receive(
	struct v90_ja_search *search,
	uint8_t clockwise_quarter_turns)
{
	struct v90_ja_search next;
	enum v90_ja_search_result event = V90_JA_SEARCH_OK;
	uint8_t dibit;

	if (!v90_ja_search_is_valid(search) ||
	    clockwise_quarter_turns >= 4u)
		return V90_JA_SEARCH_INVALID_ARGUMENT;
	if (search->complete)
		return V90_JA_SEARCH_COMPLETE;
	next = *search;
	if (!next.has_previous_rotation) {
		next.previous_clockwise_quarter_turns =
			clockwise_quarter_turns;
		next.has_previous_rotation = true;
	} else {
		if (v34_descramble_differential_dibit(
			    &next.descrambler,
			    next.previous_clockwise_quarter_turns,
			    clockwise_quarter_turns, &dibit) != V34_FRAMING_OK)
			return V90_JA_SEARCH_INVALID_ARGUMENT;
		next.previous_clockwise_quarter_turns =
			clockwise_quarter_turns;
		event = receive_bit(&next, (uint8_t)((dibit >> 1) & 1u));
		if (event != V90_JA_SEARCH_DETECTED) {
			enum v90_ja_search_result second =
				receive_bit(&next, dibit & 1u);

			if (event == V90_JA_SEARCH_OK)
				event = second;
		}
	}
	if (!v90_ja_search_is_valid(&next))
		return V90_JA_SEARCH_INVALID_ARGUMENT;
	*search = next;
	return event;
}

const char *v90_ja_search_result_name(
	enum v90_ja_search_result result)
{
	switch (result) {
	case V90_JA_SEARCH_OK:
		return "ok";
	case V90_JA_SEARCH_DETECTED:
		return "detected";
	case V90_JA_SEARCH_REJECTED:
		return "rejected";
	case V90_JA_SEARCH_UNSUPPORTED:
		return "unsupported";
	case V90_JA_SEARCH_COMPLETE:
		return "complete";
	case V90_JA_SEARCH_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
