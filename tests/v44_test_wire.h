// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_TEST_WIRE_H
#define SIPPYMODEM_V44_TEST_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_v44_test_control {
	SIPPYMODEM_V44_TEST_ETM = 0,
	SIPPYMODEM_V44_TEST_FLUSH = 1,
	SIPPYMODEM_V44_TEST_STEPUP = 2,
	SIPPYMODEM_V44_TEST_REINIT = 3,
};

struct sippymodem_v44_test_wire;

struct sippymodem_v44_test_wire *sippymodem_v44_test_wire_create(
	uint32_t max_string_length,
	size_t wire_capacity);
void sippymodem_v44_test_wire_destroy(
	struct sippymodem_v44_test_wire *wire);

bool sippymodem_v44_test_wire_ordinal(
	struct sippymodem_v44_test_wire *wire,
	uint8_t ordinal);
bool sippymodem_v44_test_wire_codeword(
	struct sippymodem_v44_test_wire *wire,
	uint32_t codeword);
bool sippymodem_v44_test_wire_extension(
	struct sippymodem_v44_test_wire *wire,
	uint32_t extension_length);
bool sippymodem_v44_test_wire_control(
	struct sippymodem_v44_test_wire *wire,
	enum sippymodem_v44_test_control control);

/* Emit STEPUP and resolve its width from the following data-code prefix. */
bool sippymodem_v44_test_wire_stepup_ordinal(
	struct sippymodem_v44_test_wire *wire);
bool sippymodem_v44_test_wire_stepup_codeword(
	struct sippymodem_v44_test_wire *wire);

/* Deliberately malformed-wire tests may append an unchecked raw field. */
bool sippymodem_v44_test_wire_put(
	struct sippymodem_v44_test_wire *wire,
	uint32_t value,
	unsigned int bit_count);

bool sippymodem_v44_test_wire_finish(
	struct sippymodem_v44_test_wire *wire,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

#endif
