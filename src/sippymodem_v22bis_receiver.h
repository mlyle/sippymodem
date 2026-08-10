// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V22BIS_RECEIVER_H
#define SIPPYMODEM_V22BIS_RECEIVER_H

#include "sippymodem_v22bis_symbols.h"

#include <stdint.h>

enum sippymodem_v22bis_receiver_result {
	SIPPYMODEM_V22BIS_RECEIVER_OK,
	SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v22bis_receiver {
	struct sippymodem_v22bis_symbol_decoder decoder;
	struct sippymodem_qam_point channel_gain;
};

/* Seed from the last received V.22 point before switching to 16-QAM. */
enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_init(
	struct sippymodem_v22bis_receiver *receiver,
	const struct sippymodem_qam_point *reference_point);
enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_decode(
	struct sippymodem_v22bis_receiver *receiver,
	const struct sippymodem_qam_point *point,
	uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL]);

const char *sippymodem_v22bis_receiver_result_name(
	enum sippymodem_v22bis_receiver_result result);

#endif
