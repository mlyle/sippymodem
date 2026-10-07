// SPDX-License-Identifier: BSD-3-Clause

#ifndef V22BIS_RECEIVER_H
#define V22BIS_RECEIVER_H

#include "qam_gain.h"
#include "v22bis_symbols.h"

#include <stdbool.h>
#include <stdint.h>

enum v22bis_receiver_result {
	V22BIS_RECEIVER_OK,
	V22BIS_RECEIVER_INVALID_ARGUMENT,
};

struct v22bis_receiver {
	struct v22_symbol_decoder decoder;
	struct qam_gain gain;
	bool v22bis_started;
};

/* Seed from the last received V.22 point before switching to 16-QAM. */
enum v22bis_receiver_result v22bis_receiver_init(
	struct v22bis_receiver *receiver,
	const struct qam_point *reference_point);
bool v22bis_receiver_is_valid(
	const struct v22bis_receiver *receiver);
enum v22bis_receiver_result
v22bis_receiver_matches_v22(
	const struct v22bis_receiver *receiver,
	const struct qam_point *point, bool *matches);
enum v22bis_receiver_result
v22bis_receiver_track_v22(
	struct v22bis_receiver *receiver, uint8_t quarter_turns,
	const struct qam_point *point);
enum v22bis_receiver_result v22bis_receiver_decode(
	struct v22bis_receiver *receiver,
	const struct qam_point *point,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL]);

const char *v22bis_receiver_result_name(
	enum v22bis_receiver_result result);

#endif /* V22BIS_RECEIVER_H */
