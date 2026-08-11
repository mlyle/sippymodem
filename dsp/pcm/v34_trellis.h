// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_TRELLIS_H
#define SIPPYMODEM_V34_TRELLIS_H

#include "v34_constellation.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_SUBSET_LABEL_COUNT = 8,
};

/* These values are also the two-bit MP field encodings. */
enum sippymodem_v34_trellis_selection {
	SIPPYMODEM_V34_TRELLIS_16_STATE = 0,
	SIPPYMODEM_V34_TRELLIS_32_STATE = 1,
	SIPPYMODEM_V34_TRELLIS_64_STATE = 2,
};

enum sippymodem_v34_trellis_result {
	SIPPYMODEM_V34_TRELLIS_OK,
	SIPPYMODEM_V34_TRELLIS_INVALID_ARGUMENT,
};

struct sippymodem_v34_convolutional_input {
	uint8_t y1;
	uint8_t y2;
	uint8_t y3;
	uint8_t y4;
};

struct sippymodem_v34_convolutional_encoder {
	/* The current Y0 output is bit zero. Remaining bits are delay state. */
	uint8_t state;
	enum sippymodem_v34_trellis_selection selection;
};

bool sippymodem_v34_trellis_selection_is_valid(
	enum sippymodem_v34_trellis_selection selection);

/* Return the repeated three-bit Figure 9 label for an odd-integer channel
 * output point. The destination is unchanged when the point is off-grid. */
enum sippymodem_v34_trellis_result sippymodem_v34_grid_subset_label(
	const struct sippymodem_v34_grid_point *point, uint8_t *subset_label);

/* Derive C0 from the two even-integer precoder offsets in a 4D interval.
 * The destination is unchanged when either offset is off-grid. */
enum sippymodem_v34_trellis_result sippymodem_v34_modulo_encoder_output(
	const struct sippymodem_v34_grid_point *first_offset,
	const struct sippymodem_v34_grid_point *second_offset, uint8_t *c0);

/* Convert the subset labels s(2m), s(2m + 1) into Y1 through Y4 without
 * retaining Table 13 as a 64-entry lookup table. */
enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_input_from_subset_labels(
	uint8_t first_subset_label, uint8_t second_subset_label,
	struct sippymodem_v34_convolutional_input *input);
bool sippymodem_v34_convolutional_input_is_valid(
	const struct sippymodem_v34_convolutional_input *input);

enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_encoder_init(
	struct sippymodem_v34_convolutional_encoder *encoder,
	enum sippymodem_v34_trellis_selection selection);
bool sippymodem_v34_convolutional_encoder_is_valid(
	const struct sippymodem_v34_convolutional_encoder *encoder);

/* Y0 has one 4D-interval delay and therefore precedes advance for the current
 * interval. Both operations leave their destinations unchanged on failure. */
enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_encoder_output(
	const struct sippymodem_v34_convolutional_encoder *encoder,
	uint8_t *y0);
enum sippymodem_v34_trellis_result
sippymodem_v34_convolutional_encoder_advance(
	struct sippymodem_v34_convolutional_encoder *encoder,
	const struct sippymodem_v34_convolutional_input *input);

const char *sippymodem_v34_trellis_result_name(
	enum sippymodem_v34_trellis_result result);

#endif /* SIPPYMODEM_V34_TRELLIS_H */
