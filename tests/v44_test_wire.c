// SPDX-License-Identifier: BSD-3-Clause

#include "v44_test_wire.h"

#include <v44_stream.h>

#include <stdbool.h>
#include <stdlib.h>

enum {
	V44_FIRST_CODEWORD = 4,
	V44_INITIAL_CODEWORD_BITS = 6,
	V44_INITIAL_ORDINAL_BITS = 7,
	V44_MAX_CODEWORD_BITS = 16,
};

struct v44_test_wire {
	struct v44_bit_writer *writer;
	unsigned int codeword_bits;
	unsigned int ordinal_bits;
	uint32_t max_string_length;
	bool last_code_was_codeword;
};

static bool value_fits(uint32_t value, unsigned int bit_count)
{
	return bit_count < 32 && value < (UINT32_C(1) << bit_count);
}

struct v44_test_wire *v44_test_wire_create(
	uint32_t max_string_length,
	size_t wire_capacity)
{
	struct v44_test_wire *wire;

	if (max_string_length < V44_MIN_STRING_LENGTH ||
	    max_string_length > V44_MAX_STRING_LENGTH)
		return NULL;
	wire = calloc(1, sizeof(*wire));
	if (wire == NULL)
		return NULL;
	wire->writer = v44_bit_writer_create(wire_capacity);
	if (wire->writer == NULL) {
		free(wire);
		return NULL;
	}
	wire->codeword_bits = V44_INITIAL_CODEWORD_BITS;
	wire->ordinal_bits = V44_INITIAL_ORDINAL_BITS;
	wire->max_string_length = max_string_length;
	return wire;
}

void v44_test_wire_destroy(
	struct v44_test_wire *wire)
{
	if (wire == NULL)
		return;
	v44_bit_writer_destroy(wire->writer);
	free(wire);
}

bool v44_test_wire_put(
	struct v44_test_wire *wire,
	uint32_t value,
	unsigned int bit_count)
{
	return wire != NULL &&
	       v44_bit_writer_put(
		       wire->writer, value, bit_count) ==
		       V44_STREAM_OK;
}

bool v44_test_wire_control(
	struct v44_test_wire *wire,
	enum v44_test_control control)
{
	if (wire == NULL || control < V44_TEST_ETM ||
	    control > V44_TEST_REINIT ||
	    !v44_test_wire_put(wire, 1, 1) ||
	    !v44_test_wire_put(
		    wire, (uint32_t)control, wire->codeword_bits))
		return false;
	wire->last_code_was_codeword = false;
	if (control == V44_TEST_REINIT) {
		wire->codeword_bits = V44_INITIAL_CODEWORD_BITS;
		wire->ordinal_bits = V44_INITIAL_ORDINAL_BITS;
	}
	if (control == V44_TEST_FLUSH ||
	    control == V44_TEST_ETM)
		return v44_bit_writer_align(wire->writer) ==
		       V44_STREAM_OK;
	return true;
}

bool v44_test_wire_stepup_ordinal(
	struct v44_test_wire *wire)
{
	if (wire == NULL || wire->ordinal_bits >= 8 ||
	    !v44_test_wire_control(
		    wire, V44_TEST_STEPUP))
		return false;
	wire->ordinal_bits = 8;
	return true;
}

bool v44_test_wire_stepup_codeword(
	struct v44_test_wire *wire)
{
	if (wire == NULL || wire->codeword_bits >= V44_MAX_CODEWORD_BITS ||
	    !v44_test_wire_control(
		    wire, V44_TEST_STEPUP))
		return false;
	wire->codeword_bits++;
	return true;
}

bool v44_test_wire_ordinal(
	struct v44_test_wire *wire,
	uint8_t ordinal)
{
	if (wire == NULL || !value_fits(ordinal, wire->ordinal_bits) ||
	    (wire->last_code_was_codeword &&
	     !v44_test_wire_put(wire, 0, 1)) ||
	    !v44_test_wire_put(wire, 0, 1) ||
	    !v44_test_wire_put(
		    wire, ordinal, wire->ordinal_bits))
		return false;
	wire->last_code_was_codeword = false;
	return true;
}

bool v44_test_wire_codeword(
	struct v44_test_wire *wire,
	uint32_t codeword)
{
	if (wire == NULL || codeword < V44_FIRST_CODEWORD ||
	    !value_fits(codeword, wire->codeword_bits) ||
	    !v44_test_wire_put(wire, 1, 1) ||
	    !v44_test_wire_put(
		    wire, codeword, wire->codeword_bits))
		return false;
	wire->last_code_was_codeword = true;
	return true;
}

static unsigned int extension_value_bits(
	const struct v44_test_wire *wire)
{
	if (wire->max_string_length <= 46)
		return 5;
	if (wire->max_string_length <= 78)
		return 6;
	if (wire->max_string_length <= 142)
		return 7;
	return 8;
}

bool v44_test_wire_extension(
	struct v44_test_wire *wire,
	uint32_t extension_length)
{
	if (wire == NULL || !wire->last_code_was_codeword ||
	    extension_length == 0 || extension_length > 253 ||
	    !v44_test_wire_put(wire, 0, 1) ||
	    !v44_test_wire_put(wire, 1, 1))
		return false;
	if (extension_length == 1) {
		if (!v44_test_wire_put(wire, 1, 1))
			return false;
	} else if (extension_length <= 4) {
		if (!v44_test_wire_put(wire, 0, 1) ||
		    !v44_test_wire_put(
			    wire, extension_length - 1, 2))
			return false;
	} else if (extension_length <= 12) {
		if (!v44_test_wire_put(wire, 0, 3) ||
		    !v44_test_wire_put(wire, 0, 1) ||
		    !v44_test_wire_put(
			    wire, extension_length - 5, 3))
			return false;
	} else if (!v44_test_wire_put(wire, 0, 3) ||
		   !v44_test_wire_put(wire, 1, 1) ||
		   !v44_test_wire_put(
			   wire, extension_length - 13,
			   extension_value_bits(wire))) {
		return false;
	}
	wire->last_code_was_codeword = false;
	return true;
}

bool v44_test_wire_finish(
	struct v44_test_wire *wire,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	size_t pending;

	if (wire == NULL || output == NULL || output_length == NULL ||
	    v44_bit_writer_align(wire->writer) !=
		    V44_STREAM_OK)
		return false;
	pending = v44_bit_writer_pending(wire->writer);
	if (pending > output_capacity ||
	    v44_bit_writer_read(
		    wire->writer, output, output_capacity) != pending)
		return false;
	*output_length = pending;
	return true;
}
