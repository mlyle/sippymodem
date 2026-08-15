// SPDX-License-Identifier: BSD-3-Clause

#ifndef BINARY_EQUALIZER_H
#define BINARY_EQUALIZER_H

#include "qam_equalizer.h"

#include <stdbool.h>
#include <stdint.h>

enum binary_equalizer_result {
	BINARY_EQUALIZER_OK,
	BINARY_EQUALIZER_INVALID_ARGUMENT,
	BINARY_EQUALIZER_INVALID_STATE,
	BINARY_EQUALIZER_INTERNAL_ERROR,
};

/* Continue a trained symbol-spaced equalizer as a decision-directed binary
 * line. The instance owns the equalizer and its channel polarity; callers
 * retain framing, scrambling, and protocol state. */
struct binary_equalizer {
	struct qam_equalizer equalizer;
	double magnitude;
	double adaptation;
	int8_t polarity;
};

enum binary_equalizer_result binary_equalizer_init(
	struct binary_equalizer *receiver,
	const struct qam_equalizer *trained_equalizer,
	double magnitude, double adaptation, int8_t polarity);
bool binary_equalizer_is_valid(
	const struct binary_equalizer *receiver);

/* Consume one signed sample and produce its polarity-normalized hard sign.
 * A trained equalizer always yields one decision. Rejected calls preserve
 * state and output. */
enum binary_equalizer_result binary_equalizer_receive(
	struct binary_equalizer *receiver, int16_t sample,
	uint8_t *sign);

const char *binary_equalizer_result_name(
	enum binary_equalizer_result result);

#endif /* BINARY_EQUALIZER_H */
