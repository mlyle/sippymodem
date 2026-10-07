// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpd.h"

#include "integer_math.h"
#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CPD_MODULUS_PRESENT_BIT = 19,
	CPD_FILTERS_PRESENT_BIT = 20,
	CPD_CONSTELLATIONS_PRESENT_BIT = 21,
	CPD_RATE_FIRST_BIT = 22,
	CPD_RATE_BITS = 5,
	CPD_TRELLIS_FIRST_BIT = 27,
	CPD_TRELLIS_BITS = 2,
	CPD_GAIN_FIRST_BIT = 35,
	CPD_GAIN_BITS = 16,
	CPD_CRC_BITS = 16,
};

struct cpd_wire {
	const uint8_t *input;
	uint8_t *output;
	size_t limit;
	size_t cursor;
	bool valid;
};

struct cpd_presence {
	bool modulus;
	bool filters;
	bool constellations;
};

static bool alignment_is_valid(unsigned int mapping_frame_bits)
{
	return mapping_frame_bits >= V92_CPD_DOWNSTREAM_FRAME_MIN_BITS &&
	       mapping_frame_bits <= V92_CPD_DOWNSTREAM_DATA_MAX_BITS;
}

static bool filter_is_canonical(const struct v92_cpd_filter *filter)
{
	if (filter->count > V92_CPD_MAX_SECTION_COEFFICIENTS)
		return false;
	for (size_t index = filter->count;
	     index < V92_CPD_MAX_SECTION_COEFFICIENTS; ++index) {
		if (filter->coefficient[index] != 0)
			return false;
	}
	return true;
}

static bool filters_are_valid(const struct v92_cpd_parameters *parameters)
{
	size_t total = 0u;

	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		const struct v92_cpd_filter *filter =
			&parameters->filter[section];

		if (!filter_is_canonical(filter))
			return false;
		total += filter->count;
	}
	return total <= V92_CPD_MAX_TOTAL_COEFFICIENTS &&
	       (parameters->filters_present || total == 0u);
}

static bool modulus_is_valid(const struct v92_cpd_parameters *parameters)
{
	if (parameters->modulus_present)
		return true;
	for (size_t index = 0u; index < V92_CPD_MODULUS_COUNT; ++index) {
		if (parameters->modulus[index] != 0u)
			return false;
	}
	return true;
}

static bool constellation_is_canonical(
	const struct v92_cpd_constellation *constellation)
{
	int16_t previous = 0;

	if (constellation->count > V92_CPD_MAX_CONSTELLATION_POINTS)
		return false;
	for (size_t index = 0u; index < constellation->count; ++index) {
		int16_t current = constellation->positive_linear[index];

		if (current <= previous)
			return false;
		previous = current;
	}
	for (size_t index = constellation->count;
	     index < V92_CPD_MAX_CONSTELLATION_POINTS; ++index) {
		if (constellation->positive_linear[index] != 0)
			return false;
	}
	return true;
}

static bool constellations_are_valid(
	const struct v92_cpd_parameters *parameters)
{
	uint8_t referenced = 0u;
	uint8_t populated = 0u;
	bool saw_empty = false;

	for (size_t interval = 0u;
	     interval < V92_CPD_FRAME_INTERVALS; ++interval) {
		uint8_t index = parameters->frame_constellation[interval];

		if (index >= V92_CPD_MAX_CONSTELLATIONS)
			return false;
		referenced |= (uint8_t)(1u << index);
	}
	for (size_t index = 0u;
	     index < V92_CPD_MAX_CONSTELLATIONS; ++index) {
		const struct v92_cpd_constellation *constellation =
			&parameters->constellation[index];

		if (!constellation_is_canonical(constellation))
			return false;
		if (constellation->count == 0u) {
			saw_empty = true;
			continue;
		}
		if (saw_empty)
			return false;
		populated |= (uint8_t)(1u << index);
	}
	if (!parameters->constellations_present) {
		for (size_t interval = 0u;
		     interval < V92_CPD_FRAME_INTERVALS; ++interval) {
			if (parameters->frame_constellation[interval] != 0u)
				return false;
		}
		return populated == 0u;
	}
	/* Table 30 requires each selected set to exist, not every set to be used. */
	return (referenced & populated) == referenced;
}

bool v92_cpd_parameters_are_valid(
	const struct v92_cpd_parameters *parameters)
{
	struct v92_cpd_base_parameters base;

	if (parameters == NULL)
		return false;
	base = (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 =
			parameters->four_times_gain_q0_16,
		.data_rate_number = parameters->data_rate_number,
		.trellis = parameters->trellis,
		.extend_e2u = parameters->extend_e2u,
		.acknowledges_cpu = parameters->acknowledges_cpu,
	};
	return v92_cpd_base_parameters_are_valid(&base) &&
	       modulus_is_valid(parameters) && filters_are_valid(parameters) &&
	       constellations_are_valid(parameters);
}

bool v92_cpd_base_parameters_are_valid(
	const struct v92_cpd_base_parameters *parameters)
{
	return parameters != NULL &&
	       parameters->data_rate_number <=
		       V92_CPD_MAX_DATA_RATE_NUMBER &&
	       (unsigned int)parameters->trellis <=
		       (unsigned int)V92_CPD_TRELLIS_64_STATE &&
	       parameters->four_times_gain_q0_16 != 0u;
}

size_t v92_cpd_payload_bits(
	const struct v92_cpd_parameters *parameters)
{
	size_t bits = V92_CPD_FIXED_BITS;

	if (!v92_cpd_parameters_are_valid(parameters))
		return 0u;
	if (parameters->modulus_present)
		bits += V92_CPD_MODULUS_BITS;
	if (parameters->filters_present) {
		bits += V92_CPD_FILTER_HEADER_BITS;
		for (size_t section = 0u;
		     section < V92_CPD_FILTER_SECTION_COUNT; ++section)
			bits += parameters->filter[section].count *
				V92_CPD_WORD_BITS;
	}
	if (parameters->constellations_present) {
		bits += V92_CPD_CONSTELLATION_HEADER_BITS;
		for (size_t index = 0u;
		     index < V92_CPD_MAX_CONSTELLATIONS; ++index)
			bits += parameters->constellation[index].count *
				V92_CPD_WORD_BITS;
	}
	return bits;
}

size_t v92_cpd_frame_bits(
	const struct v92_cpd_parameters *parameters,
	unsigned int mapping_frame_bits)
{
	size_t payload = v92_cpd_payload_bits(parameters);

	if (payload == 0u || !alignment_is_valid(mapping_frame_bits))
		return 0u;
	return pcm_control_frame_align_bits(
		payload + V92_CPD_TRAILER_BITS, mapping_frame_bits);
}

size_t v92_cpd_frame_bytes(
	const struct v92_cpd_parameters *parameters,
	unsigned int mapping_frame_bits)
{
	size_t bits = v92_cpd_frame_bits(parameters, mapping_frame_bits);

	return bits == 0u ? 0u : (bits + 7u) / 8u;
}

static bool received_contains(
	size_t received_bit_count, size_t first_bit, size_t bit_count)
{
	return first_bit <= received_bit_count &&
	       bit_count <= received_bit_count - first_bit;
}

static enum v92_cpd_result probe_constellation_extent(
	const uint8_t *frame, size_t received_bit_count,
	size_t *payload_bits)
{
	size_t first_bit = *payload_bits;
	size_t point_count = 0u;
	uint8_t referenced = 0u;
	uint8_t populated = 0u;
	bool saw_empty = false;

	if (!received_contains(
		    received_bit_count, first_bit,
		    V92_CPD_CONSTELLATION_HEADER_BITS))
		return V92_CPD_INCOMPLETE;
	if (!pcm_control_frame_starts_are_zero(
		    frame, first_bit, first_bit + V92_CPD_CONSTELLATION_HEADER_BITS))
		return V92_CPD_INVALID_FRAME;
	for (size_t interval = 0u;
	     interval < V92_CPD_FRAME_INTERVALS; ++interval) {
		size_t row = interval < 4u ? 0u : 1u;
		size_t within_row = interval < 4u ? interval : interval - 4u;
		uint8_t index = (uint8_t)packed_msb_read_lsb_field(
			frame, first_bit + row * V92_CPD_WORD_BITS + 1u +
				within_row * 4u,
			4u);

		if (index >= V92_CPD_MAX_CONSTELLATIONS)
			return V92_CPD_INVALID_FRAME;
		referenced |= (uint8_t)(1u << index);
	}
	for (size_t index = 0u;
	     index < V92_CPD_MAX_CONSTELLATIONS; ++index) {
		size_t row = index / 2u;
		size_t within_row = index % 2u;
		uint32_t count = packed_msb_read_lsb_field(
			frame, first_bit + 2u * V92_CPD_WORD_BITS +
				row * V92_CPD_WORD_BITS + 1u + within_row * 8u,
			8u);

		if (count > V92_CPD_MAX_CONSTELLATION_POINTS)
			return V92_CPD_INVALID_FRAME;
		if (count == 0u) {
			saw_empty = true;
		} else {
			if (saw_empty)
				return V92_CPD_INVALID_FRAME;
			populated |= (uint8_t)(1u << index);
		}
		point_count += count;
	}
	if ((referenced & populated) != referenced)
		return V92_CPD_INVALID_FRAME;
	*payload_bits += V92_CPD_CONSTELLATION_HEADER_BITS +
		point_count * V92_CPD_WORD_BITS;
	return V92_CPD_OK;
}

enum v92_cpd_result v92_cpd_probe_frame_bits(
	const uint8_t *frame, size_t frame_capacity,
	size_t received_bit_count, unsigned int mapping_frame_bits,
	size_t *frame_bit_count)
{
	size_t payload_bits = V92_CPD_FIXED_BITS;
	size_t expected_bits;
	bool filters_present;
	bool constellations_present;

	if (frame == NULL || frame_bit_count == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!alignment_is_valid(mapping_frame_bits))
		return V92_CPD_INVALID_ALIGNMENT;
	if (received_bit_count > V92_CPD_MAX_FRAME_BITS)
		return V92_CPD_INVALID_FRAME;
	if (frame_capacity < (received_bit_count + 7u) / 8u)
		return V92_CPD_INVALID_ARGUMENT;
	if (received_bit_count < V92_CPD_FIXED_BITS)
		return V92_CPD_INCOMPLETE;
	if (!pcm_control_frame_prefix_is_valid(frame) ||
	    packed_msb_bit(frame, 34u) != 0u ||
	    packed_msb_read_lsb_field(
		    frame, CPD_RATE_FIRST_BIT, CPD_RATE_BITS) >
		    V92_CPD_MAX_DATA_RATE_NUMBER ||
	    packed_msb_read_lsb_field(
		    frame, CPD_TRELLIS_FIRST_BIT, CPD_TRELLIS_BITS) >
		    V92_CPD_TRELLIS_64_STATE ||
	    packed_msb_read_lsb_field(
		    frame, CPD_GAIN_FIRST_BIT, CPD_GAIN_BITS) == 0u)
		return V92_CPD_INVALID_FRAME;
	filters_present =
		packed_msb_bit(frame, CPD_FILTERS_PRESENT_BIT) != 0u;
	constellations_present =
		packed_msb_bit(frame, CPD_CONSTELLATIONS_PRESENT_BIT) != 0u;
	if (packed_msb_bit(frame, CPD_MODULUS_PRESENT_BIT) != 0u) {
		if (!received_contains(
			    received_bit_count, payload_bits,
			    V92_CPD_MODULUS_BITS))
			return V92_CPD_INCOMPLETE;
		if (!pcm_control_frame_starts_are_zero(
			    frame, payload_bits, payload_bits + V92_CPD_MODULUS_BITS))
			return V92_CPD_INVALID_FRAME;
		payload_bits += V92_CPD_MODULUS_BITS;
	}
	if (filters_present) {
		size_t coefficient_count = 0u;

		if (!received_contains(
			    received_bit_count, payload_bits,
			    V92_CPD_FILTER_HEADER_BITS))
			return V92_CPD_INCOMPLETE;
		if (!pcm_control_frame_starts_are_zero(
			    frame, payload_bits,
			    payload_bits + V92_CPD_FILTER_HEADER_BITS))
			return V92_CPD_INVALID_FRAME;
		for (size_t section = 0u;
		     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
			uint32_t count = packed_msb_read_lsb_field(
				frame, payload_bits +
					section * V92_CPD_WORD_BITS + 1u,
				9u);

			if (count > V92_CPD_MAX_SECTION_COEFFICIENTS)
				return V92_CPD_INVALID_FRAME;
			coefficient_count += count;
		}
		if (coefficient_count > V92_CPD_MAX_TOTAL_COEFFICIENTS)
			return V92_CPD_INVALID_FRAME;
		payload_bits += V92_CPD_FILTER_HEADER_BITS;
		if (constellations_present &&
		    !received_contains(
			    received_bit_count, payload_bits,
			    coefficient_count * V92_CPD_WORD_BITS))
			return V92_CPD_INCOMPLETE;
		if (constellations_present &&
		    !pcm_control_frame_starts_are_zero(
			    frame, payload_bits,
			    payload_bits + coefficient_count * V92_CPD_WORD_BITS))
			return V92_CPD_INVALID_FRAME;
		payload_bits += coefficient_count * V92_CPD_WORD_BITS;
	}
	if (constellations_present) {
		enum v92_cpd_result result = probe_constellation_extent(
			frame, received_bit_count, &payload_bits);

		if (result != V92_CPD_OK)
			return result;
	}
	expected_bits = pcm_control_frame_align_bits(
		payload_bits + V92_CPD_TRAILER_BITS, mapping_frame_bits);
	if (expected_bits > V92_CPD_MAX_FRAME_BITS ||
	    received_bit_count > expected_bits)
		return V92_CPD_INVALID_FRAME;
	*frame_bit_count = expected_bits;
	return V92_CPD_OK;
}

static uint32_t wire_field(
	struct cpd_wire *wire, uint32_t value, unsigned int bit_count)
{
	if (!wire->valid || wire->cursor > wire->limit ||
	    bit_count > wire->limit - wire->cursor) {
		wire->valid = false;
		return 0u;
	}
	if (wire->input != NULL)
		value = packed_msb_read_lsb_field(
			wire->input, wire->cursor, bit_count);
	else
		packed_msb_write_lsb_field(
			wire->output, wire->cursor, bit_count, value);
	wire->cursor += bit_count;
	return value;
}

static void wire_fixed(
	struct cpd_wire *wire, uint32_t value, unsigned int bit_count)
{
	if (wire_field(wire, value, bit_count) != value)
		wire->valid = false;
}

static void wire_reserved(struct cpd_wire *wire, unsigned int bit_count)
{
	(void)wire_field(wire, 0u, bit_count);
}

static uint32_t wire_row(
	struct cpd_wire *wire, uint32_t value, unsigned int value_bits)
{
	wire_fixed(wire, 0u, 1u);
	value = wire_field(wire, value, value_bits);
	wire_reserved(wire, V92_CPD_WORD_BITS - value_bits - 1u);
	return value;
}

static bool wire_filters(
	struct cpd_wire *wire, struct v92_cpd_parameters *parameters)
{
	size_t total = 0u;

	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		uint32_t count = wire_row(
			wire, parameters->filter[section].count, 9u);

		if (count > V92_CPD_MAX_SECTION_COEFFICIENTS)
			return false;
		parameters->filter[section].count = (uint16_t)count;
		total += count;
	}
	if (total > V92_CPD_MAX_TOTAL_COEFFICIENTS)
		return false;
	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		struct v92_cpd_filter *filter = &parameters->filter[section];

		for (size_t index = 0u; index < filter->count; ++index) {
			uint32_t value = wire_row(
				wire, (uint16_t)filter->coefficient[index], 16u);

			filter->coefficient[index] =
				wrap_s16_from_s32((int32_t)value);
		}
	}
	return wire->valid;
}

static bool wire_constellations(
	struct cpd_wire *wire, struct v92_cpd_parameters *parameters)
{
	wire_fixed(wire, 0u, 1u);
	for (size_t interval = 0u; interval < 4u; ++interval) {
		parameters->frame_constellation[interval] =
			(uint8_t)wire_field(
				wire, parameters->frame_constellation[interval], 4u);
	}
	wire_fixed(wire, 0u, 1u);
	for (size_t interval = 4u;
	     interval < V92_CPD_FRAME_INTERVALS; ++interval) {
		parameters->frame_constellation[interval] =
			(uint8_t)wire_field(
				wire, parameters->frame_constellation[interval], 4u);
	}
	wire_reserved(wire, 8u);
	for (size_t index = 0u;
	     index < V92_CPD_MAX_CONSTELLATIONS; index += 2u) {
		wire_fixed(wire, 0u, 1u);
		for (size_t pair = 0u; pair < 2u; ++pair) {
			uint32_t count = wire_field(
				wire, parameters->constellation[index + pair].count,
				8u);

			if (count > V92_CPD_MAX_CONSTELLATION_POINTS)
				return false;
			parameters->constellation[index + pair].count =
				(uint8_t)count;
		}
	}
	for (size_t set = 0u;
	     set < V92_CPD_MAX_CONSTELLATIONS; ++set) {
		struct v92_cpd_constellation *constellation =
			&parameters->constellation[set];

		for (size_t index = 0u;
		     index < constellation->count; ++index) {
			uint32_t value = wire_row(
				wire,
				(uint16_t)constellation->positive_linear[index],
				16u);

			constellation->positive_linear[index] =
				wrap_s16_from_s32((int32_t)value);
		}
	}
	return wire->valid;
}

static bool wire_base_fields(
	struct cpd_wire *wire,
	struct v92_cpd_base_parameters *parameters,
	struct cpd_presence *presence)
{
	wire_fixed(wire, UINT32_C(0x1ffff), 17u);
	wire_fixed(wire, 0u, 1u);
	wire_reserved(wire, 1u);
	presence->modulus =
		wire_field(wire, presence->modulus, 1u) != 0u;
	presence->filters =
		wire_field(wire, presence->filters, 1u) != 0u;
	presence->constellations =
		wire_field(wire, presence->constellations, 1u) != 0u;
	parameters->data_rate_number = (uint8_t)wire_field(
		wire, parameters->data_rate_number, 5u);
	parameters->trellis = (enum v92_cpd_trellis)wire_field(
		wire, (uint32_t)parameters->trellis, 2u);
	parameters->extend_e2u =
		wire_field(wire, parameters->extend_e2u, 1u) != 0u;
	wire_reserved(wire, 3u);
	parameters->acknowledges_cpu =
		wire_field(wire, parameters->acknowledges_cpu, 1u) != 0u;
	wire_fixed(wire, 0u, 1u);
	parameters->four_times_gain_q0_16 = (uint16_t)wire_field(
		wire, parameters->four_times_gain_q0_16, 16u);
	return wire->valid;
}

static bool wire_payload(
	struct cpd_wire *wire, struct v92_cpd_parameters *parameters)
{
	struct v92_cpd_base_parameters base = {
		.four_times_gain_q0_16 =
			parameters->four_times_gain_q0_16,
		.data_rate_number = parameters->data_rate_number,
		.trellis = parameters->trellis,
		.extend_e2u = parameters->extend_e2u,
		.acknowledges_cpu = parameters->acknowledges_cpu,
	};
	struct cpd_presence presence = {
		.modulus = parameters->modulus_present,
		.filters = parameters->filters_present,
		.constellations = parameters->constellations_present,
	};

	if (!wire_base_fields(wire, &base, &presence))
		return false;
	parameters->four_times_gain_q0_16 = base.four_times_gain_q0_16;
	parameters->data_rate_number = base.data_rate_number;
	parameters->trellis = base.trellis;
	parameters->extend_e2u = base.extend_e2u;
	parameters->acknowledges_cpu = base.acknowledges_cpu;
	parameters->modulus_present = presence.modulus;
	parameters->filters_present = presence.filters;
	parameters->constellations_present = presence.constellations;

	if (parameters->modulus_present) {
		for (size_t index = 0u;
		     index < V92_CPD_MODULUS_COUNT; index += 2u) {
			wire_fixed(wire, 0u, 1u);
			parameters->modulus[index] = (uint8_t)wire_field(
				wire, parameters->modulus[index], 8u);
			parameters->modulus[index + 1u] =
				(uint8_t)wire_field(
					wire, parameters->modulus[index + 1u], 8u);
		}
	}
	if (parameters->filters_present &&
	    !wire_filters(wire, parameters))
		return false;
	if (parameters->constellations_present &&
	    !wire_constellations(wire, parameters))
		return false;
	return wire->valid;
}

enum v92_cpd_result v92_cpd_encode_base(
	const struct v92_cpd_base_parameters *parameters,
	unsigned int mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	struct v92_cpd_base_parameters encoded_parameters;
	struct cpd_presence presence = { 0 };
	uint8_t encoded[V92_CPD_BASE_MAX_FRAME_BYTES] = { 0 };
	struct cpd_wire wire;
	size_t bit_count;
	size_t byte_count;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!v92_cpd_base_parameters_are_valid(parameters))
		return V92_CPD_INVALID_PARAMETERS;
	if (!alignment_is_valid(mapping_frame_bits))
		return V92_CPD_INVALID_ALIGNMENT;
	bit_count = pcm_control_frame_align_bits(
		V92_CPD_FIXED_BITS + V92_CPD_TRAILER_BITS, mapping_frame_bits);
	byte_count = (bit_count + 7u) / 8u;
	if (frame_capacity < byte_count)
		return V92_CPD_OUTPUT_TOO_SMALL;

	encoded_parameters = *parameters;
	wire = (struct cpd_wire){
		.output = encoded,
		.limit = V92_CPD_FIXED_BITS,
		.valid = true,
	};
	if (!wire_base_fields(&wire, &encoded_parameters, &presence) ||
	    wire.cursor != V92_CPD_FIXED_BITS || presence.modulus ||
	    presence.filters || presence.constellations)
		return V92_CPD_INVALID_PARAMETERS;
	packed_msb_write_msb_field(
		encoded, V92_CPD_FIXED_BITS + 1u, CPD_CRC_BITS,
		pcm_control_frame_crc(encoded, V92_CPD_FIXED_BITS));
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V92_CPD_OK;
}

enum v92_cpd_result v92_cpd_encode(
	const struct v92_cpd_parameters *parameters,
	unsigned int mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	struct v92_cpd_parameters encoded_parameters;
	uint8_t encoded[V92_CPD_MAX_FRAME_BYTES] = { 0 };
	struct cpd_wire wire;
	size_t payload_end;
	size_t bit_count;
	size_t byte_count;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!v92_cpd_parameters_are_valid(parameters))
		return V92_CPD_INVALID_PARAMETERS;
	if (!alignment_is_valid(mapping_frame_bits))
		return V92_CPD_INVALID_ALIGNMENT;
	payload_end = v92_cpd_payload_bits(parameters);
	bit_count = v92_cpd_frame_bits(parameters, mapping_frame_bits);
	byte_count = (bit_count + 7u) / 8u;
	if (frame_capacity < byte_count)
		return V92_CPD_OUTPUT_TOO_SMALL;

	encoded_parameters = *parameters;
	wire = (struct cpd_wire){
		.output = encoded,
		.limit = payload_end,
		.valid = true,
	};
	if (!wire_payload(&wire, &encoded_parameters) ||
	    wire.cursor != payload_end)
		return V92_CPD_INVALID_PARAMETERS;
	packed_msb_write_msb_field(
		encoded, payload_end + 1u, CPD_CRC_BITS,
		pcm_control_frame_crc(encoded, payload_end));
	memcpy(frame, encoded, byte_count);
	*frame_bit_count = bit_count;
	return V92_CPD_OK;
}

enum v92_cpd_result v92_cpd_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int mapping_frame_bits,
	struct v92_cpd_parameters *parameters)
{
	struct v92_cpd_parameters decoded = { 0 };
	struct cpd_wire wire;
	size_t frame_bytes;
	size_t expected_bits;
	uint16_t received_crc;

	if (frame == NULL || parameters == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!alignment_is_valid(mapping_frame_bits))
		return V92_CPD_INVALID_ALIGNMENT;
	if (frame_bit_count < V92_CPD_FIXED_BITS + V92_CPD_TRAILER_BITS ||
	    frame_bit_count > V92_CPD_MAX_FRAME_BITS ||
	    frame_bit_count % mapping_frame_bits != 0u)
		return V92_CPD_INVALID_FRAME;
	frame_bytes = (frame_bit_count + 7u) / 8u;
	if (frame_capacity < frame_bytes)
		return V92_CPD_INVALID_ARGUMENT;
	if (!packed_msb_tail_is_zero(frame, frame_bit_count, frame_bytes))
		return V92_CPD_INVALID_FRAME;

	wire = (struct cpd_wire){
		.input = frame,
		.limit = frame_bit_count,
		.valid = true,
	};
	if (!wire_payload(&wire, &decoded) ||
	    !v92_cpd_parameters_are_valid(&decoded))
		return V92_CPD_INVALID_FRAME;
	expected_bits = v92_cpd_frame_bits(&decoded, mapping_frame_bits);
	if (expected_bits != frame_bit_count ||
	    wire.cursor + V92_CPD_TRAILER_BITS > frame_bit_count ||
	    packed_msb_bit(frame, wire.cursor) != 0u ||
	    !packed_msb_bits_are_zero(
		    frame, wire.cursor + 1u + CPD_CRC_BITS,
		    frame_bit_count - wire.cursor - 1u - CPD_CRC_BITS))
		return V92_CPD_INVALID_FRAME;
	received_crc = (uint16_t)packed_msb_read_msb_field(
		frame, wire.cursor + 1u, CPD_CRC_BITS);
	if (received_crc != pcm_control_frame_crc(frame, wire.cursor))
		return V92_CPD_CRC_MISMATCH;
	*parameters = decoded;
	return V92_CPD_OK;
}

const char *v92_cpd_result_name(enum v92_cpd_result result)
{
	switch (result) {
	case V92_CPD_OK:
		return "ok";
	case V92_CPD_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_CPD_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_CPD_INVALID_ALIGNMENT:
		return "invalid alignment";
	case V92_CPD_OUTPUT_TOO_SMALL:
		return "output too small";
	case V92_CPD_INVALID_FRAME:
		return "invalid frame";
	case V92_CPD_CRC_MISMATCH:
		return "CRC mismatch";
	case V92_CPD_INCOMPLETE:
		return "incomplete";
	}
	return "unknown";
}
