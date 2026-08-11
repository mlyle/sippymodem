// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PATH_DECODER_H
#define SIPPYMODEM_V34_PATH_DECODER_H

#include "sippymodem_v34_4d_decoder.h"
#include "v34_precoder.h"
#include "v34_survivor.h"

enum sippymodem_v34_path_decoder_result {
	SIPPYMODEM_V34_PATH_DECODER_OK,
	SIPPYMODEM_V34_PATH_DECODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_PATH_DECODER_OUT_OF_RANGE,
};

/* Reverse the precoder for one survivor-selected interval and reconstruct the
 * mapper's U0 bit as Y0 xor C0 xor V0. Decisions must arrive in transmission
 * order; the first one starts with the zeroed precoder history established at
 * B1. State and output are unchanged on failure. */
enum sippymodem_v34_path_decoder_result
sippymodem_v34_decode_survivor_path(
	struct sippymodem_v34_precoder *inverse_precoder,
	const struct sippymodem_v34_survivor_decision *survivor_decision,
	struct sippymodem_v34_decided_4d_interval *decided);

const char *sippymodem_v34_path_decoder_result_name(
	enum sippymodem_v34_path_decoder_result result);

#endif /* SIPPYMODEM_V34_PATH_DECODER_H */
