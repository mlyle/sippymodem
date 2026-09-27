// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PHASE4_MODULATOR_H
#define V92_PHASE4_MODULATOR_H

#include "v34_framing.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum v92_phase4_modulator_result {
	V92_PHASE4_MODULATOR_OK,
	V92_PHASE4_MODULATOR_INVALID_ARGUMENT,
	V92_PHASE4_MODULATOR_INVALID_STATE,
};

/* Continuous modulation state shared by TRN2u, CPu, SUVu, and E2u. Bit zero
 * of each input group enters the GPA scrambler first. The highest-position
 * scrambled bit supplies the sign input, which is differentially encoded
 * across sequence boundaries. */
struct v92_phase4_modulator {
	struct v34_scrambler scrambler;
	enum v92_constellation_size constellation;
	int16_t lu;
	uint8_t previous_encoded_sign;
};

/* Invert the same normalized levels while preserving GPA descrambling and
 * differential-sign continuity. The observed level may be an equalizer's
 * floating-point output; the nearest valid level is returned for adaptation. */
struct v92_phase4_demodulator {
	struct v34_scrambler descrambler;
	enum v92_constellation_size constellation;
	int16_t lu;
	uint8_t previous_encoded_sign;
};

unsigned int v92_phase4_modulator_bits_per_symbol(
	enum v92_constellation_size constellation);

/* Generate one of Tables 28 and 29's normalized signed levels. The formula
 * uses sqrt((M*M - 1)/3), where M is four or eight, instead of a table. LU
 * must keep the selected constellation within int16_t. Rejected calls leave
 * sample unchanged. */
enum v92_phase4_modulator_result v92_phase4_modulator_level(
	enum v92_constellation_size constellation, int16_t lu,
	uint8_t magnitude_index, uint8_t encoded_sign, int16_t *sample);

enum v92_phase4_modulator_result v92_phase4_modulator_init(
	struct v92_phase4_modulator *modulator,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu);
bool v92_phase4_modulator_is_valid(
	const struct v92_phase4_modulator *modulator);

/* Scramble and modulate one complete group. input_group must fit the selected
 * two- or three-bit width; sample must lie outside the modulator. Rejected
 * calls leave state and sample unchanged. */
enum v92_phase4_modulator_result v92_phase4_modulator_next(
	struct v92_phase4_modulator *modulator,
	uint8_t input_group, int16_t *sample);

enum v92_phase4_modulator_result v92_phase4_demodulator_init(
	struct v92_phase4_demodulator *demodulator,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu);
bool v92_phase4_demodulator_is_valid(
	const struct v92_phase4_demodulator *demodulator);

/* Recover one earliest-bit-first input group. Rejected calls leave the state,
 * group, and nearest_level unchanged. */
enum v92_phase4_modulator_result v92_phase4_demodulator_next(
	struct v92_phase4_demodulator *demodulator, double observed_level,
	uint8_t *input_group, int16_t *nearest_level);

const char *v92_phase4_modulator_result_name(
	enum v92_phase4_modulator_result result);

#endif /* V92_PHASE4_MODULATOR_H */
