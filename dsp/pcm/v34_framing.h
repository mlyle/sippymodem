// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_FRAMING_H
#define V34_FRAMING_H

#include "v34_station.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_SCRAMBLER_BITS = 23,
	V34_J7_INVERSION_BITS = 14,
	V34_J8_INVERSION_BITS = 16,
};

/* V.34 clause 7 names the 18/23 and 5/23 polynomials GPC and GPA. */
enum v34_scrambler_polynomial {
	V34_POLYNOMIAL_GPC,
	V34_POLYNOMIAL_GPA,
};

enum v34_inversion_pattern {
	V34_INVERSION_J7,
	V34_INVERSION_J8,
};

enum v34_framing_result {
	V34_FRAMING_OK,
	V34_FRAMING_INVALID_ARGUMENT,
};

struct v34_scrambler {
	uint32_t history;
	enum v34_scrambler_polynomial polynomial;
};

enum v34_framing_result v34_scrambler_init(
	struct v34_scrambler *scrambler,
	enum v34_scrambler_polynomial polynomial);
bool v34_scrambler_is_valid(
	const struct v34_scrambler *scrambler);

/* Select clause 7's generating polynomial for the transmitting station. */
enum v34_framing_result
v34_transmit_scrambler_init(
	struct v34_scrambler *scrambler,
	enum v34_station_role role);
bool v34_transmit_scrambler_is_valid(
	const struct v34_scrambler *scrambler,
	enum v34_station_role role);

/* Process between one and eight bits, most-significant bit first.
 * Rejected calls leave state and output unchanged. */
enum v34_framing_result v34_scramble_group(
	struct v34_scrambler *scrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);
enum v34_framing_result v34_descramble_group(
	struct v34_scrambler *descrambler, uint8_t input,
	unsigned int bit_count, uint8_t *output);

/* Process nonempty, unpacked bit arrays in transmission order. Input and
 * output may be the same array but must not otherwise overlap. Rejected calls
 * leave state and output unchanged. */
enum v34_framing_result v34_scramble_unpacked_bits(
	struct v34_scrambler *scrambler, const uint8_t *input,
	size_t bit_count, uint8_t *output);
enum v34_framing_result v34_descramble_unpacked_bits(
	struct v34_scrambler *descrambler, const uint8_t *input,
	size_t bit_count, uint8_t *output);

/* Scramble a first-then-second pair and return 2*second + first. Rejected
 * calls leave state and output unchanged. */
enum v34_framing_result
v34_scramble_mapped_dibit(
	struct v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t *mapped_dibit);

/* Undo the mapped bit order and descramble the first-then-second pair.
 * Rejected calls leave state and output unchanged. */
enum v34_framing_result
v34_descramble_mapped_dibit(
	struct v34_scrambler *descrambler, uint8_t mapped_dibit,
	uint8_t *output_dibit);

/* Scramble I1 followed by I2, form In = 2*I2 + I1, and add In modulo
 * four to the previous clockwise rotation. Rejected calls leave state and
 * output unchanged. */
enum v34_framing_result
v34_scramble_differential_dibit(
	struct v34_scrambler *scrambler, uint8_t input_dibit,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t *clockwise_quarter_turns);

/* Recover the modulo-four increment between consecutive clockwise rotations,
 * then undo its mapped bit order and descramble it. Rejected calls leave state
 * and output unchanged. */
enum v34_framing_result
v34_descramble_differential_dibit(
	struct v34_scrambler *descrambler,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t current_clockwise_quarter_turns, uint8_t *output_dibit);

/* Decode one MP, CP or E symbol in transmission order: differential I1/I2
 * followed by mapped Q1/Q2 for the sixteen-point constellation. bit_count
 * is two or four; quarter_point must be zero for a four-point symbol.
 * Rejected calls leave state and output unchanged. */
enum v34_framing_result v34_descramble_control_symbol(
	struct v34_scrambler *descrambler,
	uint8_t previous_clockwise_quarter_turns,
	uint8_t current_clockwise_quarter_turns, uint8_t quarter_point,
	unsigned int bit_count, uint8_t *output);

enum v34_framing_result
v34_superframe_inversion_bit(
	enum v34_inversion_pattern pattern,
	unsigned int index, uint8_t *bit);

#endif
