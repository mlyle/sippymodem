// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V22BIS_RECEIVER_H
#define SIPPYMODEM_V22BIS_RECEIVER_H

#include "qam_gain.h"
#include "v22bis_symbols.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v22bis_receiver_result {
	SIPPYMODEM_V22BIS_RECEIVER_OK,
	SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v22bis_receiver {
	struct sippymodem_v22bis_symbol_decoder decoder;
	struct qam_gain gain;
	bool v22bis_started;
};

/* Seed from the last received V.22 point before switching to 16-QAM. */
enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_init(
	struct sippymodem_v22bis_receiver *receiver,
	const struct qam_point *reference_point);
bool sippymodem_v22bis_receiver_is_valid(
	const struct sippymodem_v22bis_receiver *receiver);
enum sippymodem_v22bis_receiver_result
sippymodem_v22bis_receiver_matches_v22(
	const struct sippymodem_v22bis_receiver *receiver,
	const struct qam_point *point, bool *matches);
enum sippymodem_v22bis_receiver_result
sippymodem_v22bis_receiver_track_v22(
	struct sippymodem_v22bis_receiver *receiver, uint8_t quarter_turns,
	const struct qam_point *point);
enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_decode(
	struct sippymodem_v22bis_receiver *receiver,
	const struct qam_point *point,
	uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL]);

const char *sippymodem_v22bis_receiver_result_name(
	enum sippymodem_v22bis_receiver_result result);

#endif /* SIPPYMODEM_V22BIS_RECEIVER_H */
