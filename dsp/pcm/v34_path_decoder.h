// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PATH_DECODER_H
#define V34_PATH_DECODER_H

#include "v34_4d_decoder.h"
#include "v34_precoder.h"
#include "v34_survivor.h"

enum v34_path_decoder_result {
	V34_PATH_DECODER_OK,
	V34_PATH_DECODER_INVALID_ARGUMENT,
	V34_PATH_DECODER_OUT_OF_RANGE,
};

/* Reverse the precoder for one survivor-selected interval and reconstruct the
 * mapper's U0 bit as Y0 xor C0 xor V0. Decisions must arrive in transmission
 * order; the first one starts with the zeroed precoder history established at
 * B1. State and output are unchanged on failure. */
enum v34_path_decoder_result
v34_decode_survivor_path(
	struct v34_precoder *inverse_precoder,
	const struct v34_survivor_decision *survivor_decision,
	struct v34_decided_4d_interval *decided);

#endif /* V34_PATH_DECODER_H */
