// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_info1.h"

#include "packed_bits.h"

#include <stddef.h>

enum { SHORT_HIGH_CARRIER_BIT = 33 - 12 };

bool pcm_info1d_is_valid(const struct pcm_info1d *info1d)
{
	if (info1d == NULL)
		return false;
	switch (info1d->protocol) {
	case PCM_PROTOCOL_V90:
		return v34_info1c_is_valid(&info1d->record.v90);
	case PCM_PROTOCOL_V92:
		return v92_info1d_is_valid(&info1d->record.v92);
	}
	return false;
}

bool pcm_info1_selection_is_valid(
	const struct pcm_info1_selection *selection)
{
	bool full;

	if (selection == NULL ||
	    !pcm_startup_selection_is_valid(&selection->startup))
		return false;
	full = selection->startup.phase2 == PCM_PHASE2_FULL;
	if (full && (!pcm_info1d_is_valid(&selection->digital) ||
		     selection->startup.protocol != selection->digital.protocol))
		return false;
	switch (selection->upstream) {
	case PCM_UPSTREAM_V34:
		return v90_info1a_is_valid(
			&selection->analog.conventional) &&
		       (full || selection->analog.conventional
					.frequency_offset_1_50_hz_units ==
				V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE);
	case PCM_UPSTREAM_PCM:
		return selection->startup.protocol == PCM_PROTOCOL_V92 &&
		       (full ? selection->digital.record.v92.pcm_upstream_supported :
			       !selection->short_high_carrier) &&
		       v92_pcm_info1a_is_valid(&selection->analog.pcm);
	}
	return false;
}

enum pcm_info1_result pcm_info1d_init_v90(
	struct pcm_info1d *info1d, const struct v34_info1c *record)
{
	struct pcm_info1d initialized = {
		.protocol = PCM_PROTOCOL_V90,
	};

	if (info1d == NULL || !v34_info1c_is_valid(record))
		return PCM_INFO1_INVALID_ARGUMENT;
	initialized.record.v90 = *record;
	*info1d = initialized;
	return PCM_INFO1_OK;
}

enum pcm_info1_result pcm_info1d_init_v92(
	struct pcm_info1d *info1d, const struct v92_info1d *record)
{
	struct pcm_info1d initialized = {
		.protocol = PCM_PROTOCOL_V92,
	};

	if (info1d == NULL || !v92_info1d_is_valid(record))
		return PCM_INFO1_INVALID_ARGUMENT;
	initialized.record.v92 = *record;
	*info1d = initialized;
	return PCM_INFO1_OK;
}

static enum pcm_info1_result select_conventional(
	struct pcm_info1_selection *selected,
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count)
{
	if (v90_info1a_decode(
		    payload, payload_capacity, payload_bit_count,
		    &selected->analog.conventional) != V90_INFO1_OK)
		return PCM_INFO1_INVALID_PAYLOAD;
	selected->upstream = PCM_UPSTREAM_V34;
	return PCM_INFO1_OK;
}

static enum pcm_info1_result select_pcm(
	struct pcm_info1_selection *selected,
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count)
{
	if (v92_pcm_info1a_decode(
		    payload, payload_capacity, payload_bit_count,
		    &selected->analog.pcm) != V92_PCM_INFO1A_OK)
		return PCM_INFO1_INVALID_PAYLOAD;
	if (selected->startup.protocol != PCM_PROTOCOL_V92)
		return PCM_INFO1_PCM_UNAVAILABLE;
	if (selected->startup.phase2 == PCM_PHASE2_FULL &&
	    !selected->digital.record.v92.pcm_upstream_supported)
		return PCM_INFO1_PCM_UNAVAILABLE;
	selected->upstream = PCM_UPSTREAM_PCM;
	return PCM_INFO1_OK;
}

enum pcm_info1_result pcm_info1_select(
	const struct pcm_startup_selection *startup,
	const struct pcm_info1d *digital, const uint8_t *analog_payload,
	size_t payload_capacity, size_t payload_bit_count,
	struct pcm_info1_selection *selection)
{
	struct pcm_info1_selection selected = { 0 };
	enum pcm_info1_result result;

	if (startup == NULL || digital == NULL || analog_payload == NULL ||
	    selection == NULL ||
	    payload_capacity < V90_INFO1A_PAYLOAD_BYTES)
		return PCM_INFO1_INVALID_ARGUMENT;
	if (!pcm_startup_selection_is_valid(startup) ||
	    startup->phase2 != PCM_PHASE2_FULL ||
	    !pcm_info1d_is_valid(digital))
		return PCM_INFO1_INVALID_RECORD;
	if (startup->protocol != digital->protocol)
		return PCM_INFO1_PROTOCOL_MISMATCH;
	if (payload_bit_count != V90_INFO1A_PAYLOAD_BITS)
		return PCM_INFO1_INVALID_PAYLOAD;
	selected.startup = *startup;
	selected.digital = *digital;
	result = select_conventional(
		&selected, analog_payload, payload_capacity,
		payload_bit_count);
	if (result != PCM_INFO1_OK)
		result = select_pcm(
			&selected, analog_payload, payload_capacity,
			payload_bit_count);
	if (result != PCM_INFO1_OK)
		return result;
	if (!pcm_info1_selection_is_valid(&selected))
		return PCM_INFO1_INVALID_RECORD;
	*selection = selected;
	return PCM_INFO1_OK;
}

enum pcm_info1_result pcm_info1_select_short(
	const struct pcm_startup_selection *startup,
	const uint8_t *analog_payload, size_t payload_capacity,
	size_t payload_bit_count, struct pcm_info1_selection *selection)
{
	struct pcm_info1_selection selected = { 0 };
	enum pcm_info1_result result;

	if (startup == NULL || analog_payload == NULL || selection == NULL ||
	    payload_capacity < V90_INFO1A_PAYLOAD_BYTES)
		return PCM_INFO1_INVALID_ARGUMENT;
	if (!pcm_startup_selection_is_valid(startup) ||
	    startup->phase2 != PCM_PHASE2_SHORT)
		return PCM_INFO1_INVALID_RECORD;
	selected.startup = *startup;
	result = select_conventional(
		&selected, analog_payload, payload_capacity, payload_bit_count);
	if (result == PCM_INFO1_OK) {
		selected.short_high_carrier = packed_msb_bit(
			analog_payload, SHORT_HIGH_CARRIER_BIT);
		selected.analog.conventional.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
	} else {
		result = select_pcm(
			&selected, analog_payload, payload_capacity, payload_bit_count);
	}
	if (result != PCM_INFO1_OK)
		return result;
	if (!pcm_info1_selection_is_valid(&selected))
		return PCM_INFO1_INVALID_RECORD;
	*selection = selected;
	return PCM_INFO1_OK;
}

enum pcm_info1_result pcm_info1_encode_short(
	const struct pcm_info1_selection *selection,
	uint8_t *payload, size_t payload_capacity)
{
	bool high_carrier;

	if (selection == NULL || payload == NULL ||
	    payload_capacity < V90_INFO1A_PAYLOAD_BYTES)
		return PCM_INFO1_INVALID_ARGUMENT;
	if (!pcm_info1_selection_is_valid(selection) ||
	    selection->startup.phase2 != PCM_PHASE2_SHORT)
		return PCM_INFO1_INVALID_RECORD;
	if (selection->upstream == PCM_UPSTREAM_PCM) {
		return v92_pcm_info1a_encode(
			       &selection->analog.pcm, payload, payload_capacity) ==
			       V92_PCM_INFO1A_OK ?
			       PCM_INFO1_OK : PCM_INFO1_INVALID_RECORD;
	}
	high_carrier = selection->short_high_carrier;

	if (v90_info1a_encode(
		    &selection->analog.conventional, payload, payload_capacity) !=
	    V90_INFO1_OK)
		return PCM_INFO1_INVALID_RECORD;
	packed_msb_set_bit(payload, SHORT_HIGH_CARRIER_BIT, high_carrier);
	return PCM_INFO1_OK;
}

const char *pcm_info1_result_name(enum pcm_info1_result result)
{
	switch (result) {
	case PCM_INFO1_OK:
		return "ok";
	case PCM_INFO1_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_INFO1_INVALID_RECORD:
		return "invalid record";
	case PCM_INFO1_PROTOCOL_MISMATCH:
		return "protocol mismatch";
	case PCM_INFO1_INVALID_PAYLOAD:
		return "invalid payload";
	case PCM_INFO1_PCM_UNAVAILABLE:
		return "PCM upstream unavailable";
	}
	return "unknown";
}
