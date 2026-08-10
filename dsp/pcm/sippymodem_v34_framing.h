// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_FRAMING_H
#define SIPPYMODEM_V34_FRAMING_H

#include "sippymodem_v34_station.h"

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

/* Select clause 7's generating polynomial for the transmitting station. */
enum sippymodem_v34_framing_result
sippymodem_v34_transmit_scrambler_init(
	struct sippymodem_v34_scrambler *scrambler,
	enum sippymodem_v34_station_role role);
bool sippymodem_v34_transmit_scrambler_is_valid(
	const struct sippymodem_v34_scrambler *scrambler,
	enum sippymodem_v34_station_role role);

/* Process between one and eight bits, most-significant bit first. */
enum sippymodem_v34_framing_result sippymodem_v34_scramble_group(
	struct sippymodem_v34_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);
enum sippymodem_v34_framing_result sippymodem_v34_descramble_group(
	struct sippymodem_v34_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);

/* Scramble a first-then-second pair and return 2*second + first. Rejected
 * calls leave state and output unchanged. */
enum sippymodem_v34_framing_result
sippymodem_v34_scramble_mapped_dibit(
	struct sippymodem_v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t *mapped_dibit);

/* Scramble I1 followed by I2, form In = 2*I2 + I1, and add In modulo
 * four to the previous clockwise rotation. Rejected calls leave state and
 * output unchanged. */
enum sippymodem_v34_framing_result
sippymodem_v34_scramble_differential_dibit(
	struct sippymodem_v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t *clockwise_quarter_turns);

enum sippymodem_v34_framing_result
sippymodem_v34_superframe_inversion_bit(
	enum sippymodem_v34_inversion_pattern pattern,
	unsigned int index, uint8_t *bit);

const char *sippymodem_v34_framing_result_name(
	enum sippymodem_v34_framing_result result);

#endif
