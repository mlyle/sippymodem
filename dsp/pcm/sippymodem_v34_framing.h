// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_FRAMING_H
#define SIPPYMODEM_V34_FRAMING_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_SCRAMBLER_BITS = 23,
	SIPPYMODEM_V34_J7_INVERSION_BITS = 14,
	SIPPYMODEM_V34_J8_INVERSION_BITS = 16,
};

/* V.34 clause 7 names the 18/23 and 5/23 polynomials GPC and GPA. */
enum sippymodem_v34_scrambler_polynomial {
	SIPPYMODEM_V34_POLYNOMIAL_GPC,
	SIPPYMODEM_V34_POLYNOMIAL_GPA,
};

enum sippymodem_v34_inversion_pattern {
	SIPPYMODEM_V34_INVERSION_J7,
	SIPPYMODEM_V34_INVERSION_J8,
};

enum sippymodem_v34_framing_result {
	SIPPYMODEM_V34_FRAMING_OK,
	SIPPYMODEM_V34_FRAMING_INVALID_ARGUMENT,
};

struct sippymodem_v34_scrambler {
	uint32_t history;
	enum sippymodem_v34_scrambler_polynomial polynomial;
};

enum sippymodem_v34_framing_result sippymodem_v34_scrambler_init(
	struct sippymodem_v34_scrambler *scrambler,
	enum sippymodem_v34_scrambler_polynomial polynomial);
bool sippymodem_v34_scrambler_is_valid(
	const struct sippymodem_v34_scrambler *scrambler);

/* Process between one and eight bits, most-significant bit first. */
enum sippymodem_v34_framing_result sippymodem_v34_scramble_group(
	struct sippymodem_v34_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);
enum sippymodem_v34_framing_result sippymodem_v34_descramble_group(
	struct sippymodem_v34_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);

enum sippymodem_v34_framing_result
sippymodem_v34_superframe_inversion_bit(
	enum sippymodem_v34_inversion_pattern pattern,
	unsigned int index, uint8_t *bit);

const char *sippymodem_v34_framing_result_name(
	enum sippymodem_v34_framing_result result);

#endif
