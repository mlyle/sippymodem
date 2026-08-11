// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PARSER_H
#define SIPPYMODEM_V34_PARSER_H

#include "sippymodem_v34_mapping_parameters.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL = 2,
};

enum sippymodem_v34_parser_result {
	SIPPYMODEM_V34_PARSER_OK,
	SIPPYMODEM_V34_PARSER_INVALID_ARGUMENT,
};

struct sippymodem_v34_4d_mapping_input {
	/* I1 rotates the second 2D symbol by an additional 180 degrees. */
	uint8_t rotation_bit;
	/* I2 + 2*I3 is the input to the differential encoder. */
	uint8_t differential_input;
	/* Q values, with each subgroup's earliest bit in bit zero. */
	uint8_t point_index[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
};

struct sippymodem_v34_parsed_mapping_frame {
	/* S1 is bit zero, matching the shell mapper's integer R0. */
	uint32_t shell_label;
	struct sippymodem_v34_4d_mapping_input
		interval[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
};

/* Parse one clause 9.3 mapping frame. Primary bits are already scrambled and
 * are supplied one bit per byte, earliest first. auxiliary_bit must be non-NULL
 * exactly when the schedule assigns raw auxiliary data to I1 of interval 0. */
enum sippymodem_v34_parser_result sippymodem_v34_parse_mapping_frame(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const uint8_t *scrambled_primary_bits,
	size_t scrambled_primary_bit_count, const uint8_t *auxiliary_bit,
	struct sippymodem_v34_parsed_mapping_frame *parsed);

/* Recover the scrambled primary bits and raw auxiliary bit from exact parsed
 * fields. The primary count must match the scheduled frame, and auxiliary_bit
 * must be non-NULL exactly when the schedule carries one. Rejection leaves
 * both destinations unchanged. */
enum sippymodem_v34_parser_result
sippymodem_v34_recover_mapping_frame_bits(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_mapping_frame *mapping_frame,
	const struct sippymodem_v34_parsed_mapping_frame *parsed,
	uint8_t *scrambled_primary_bits, size_t scrambled_primary_bit_count,
	uint8_t *auxiliary_bit);

bool sippymodem_v34_parsed_mapping_frame_is_valid(
	const struct sippymodem_v34_parsed_mapping_frame *parsed,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_mapping_frame *mapping_frame);

const char *sippymodem_v34_parser_result_name(
	enum sippymodem_v34_parser_result result);

#endif
