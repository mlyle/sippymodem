// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_TEST_WIRE_H
#define V44_TEST_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v44_test_control {
	V44_TEST_ETM = 0,
	V44_TEST_FLUSH = 1,
	V44_TEST_STEPUP = 2,
	V44_TEST_REINIT = 3,
};

struct v44_test_wire;

struct v44_test_wire *v44_test_wire_create(
	uint32_t max_string_length,
	size_t wire_capacity);
void v44_test_wire_destroy(
	struct v44_test_wire *wire);

bool v44_test_wire_ordinal(
	struct v44_test_wire *wire,
	uint8_t ordinal);
bool v44_test_wire_codeword(
	struct v44_test_wire *wire,
	uint32_t codeword);
bool v44_test_wire_extension(
	struct v44_test_wire *wire,
	uint32_t extension_length);
bool v44_test_wire_control(
	struct v44_test_wire *wire,
	enum v44_test_control control);

/* Emit STEPUP and resolve its width from the following data-code prefix. */
bool v44_test_wire_stepup_ordinal(
	struct v44_test_wire *wire);
bool v44_test_wire_stepup_codeword(
	struct v44_test_wire *wire);

/* Deliberately malformed-wire tests may append an unchecked raw field. */
bool v44_test_wire_put(
	struct v44_test_wire *wire,
	uint32_t value,
	unsigned int bit_count);

bool v44_test_wire_finish(
	struct v44_test_wire *wire,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

#endif
