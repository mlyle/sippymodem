// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_JD_H
#define V90_JD_H

#include "pcm_j_frame.h"
#include "pcm_rate.h"
#include "v34_constellation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_JD_FRAME_SYNC_BITS = PCM_J_FRAME_SYNC_BITS,
	V90_JD_RATE_CAPABILITY_BITS = PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER,
	V90_JD_RATE_CAPABILITY_MASK = PCM_DOWNSTREAM_RATE_CAPABILITY_MASK,
	V90_JD_MINIMUM_LOOKAHEAD = 1,
	V90_JD_MAXIMUM_LOOKAHEAD = 3,
	V90_JD_FRAME_BITS = PCM_J_FRAME_BITS,
	V90_JD_FRAME_BYTES = PCM_J_FRAME_BYTES,
};

enum v90_jd_result {
	V90_JD_OK,
	V90_JD_INVALID_ARGUMENT,
	V90_JD_INVALID_PARAMETERS,
	V90_JD_INVALID_FRAME,
	V90_JD_CRC_MISMATCH,
};

struct v90_jd_parameters {
	/* Bit zero represents 28 000 bit/s; bit 21 represents 56 000 bit/s. */
	uint32_t data_rate_capability_mask;
	enum v34_constellation_size training_constellation;
	enum v34_constellation_size rate_renegotiation_constellation;
	uint8_t maximum_spectral_shaping_lookahead;
};

bool v90_jd_parameters_are_valid(
	const struct v90_jd_parameters *parameters);

/* The frame is packed in transmission order, most-significant bit first in
 * each octet. Encoding canonicalizes reserved bits to zero; decoding ignores
 * their values after checking the CRC. */
enum v90_jd_result v90_jd_encode(
	const struct v90_jd_parameters *parameters,
	uint8_t *frame, size_t frame_capacity);
enum v90_jd_result v90_jd_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v90_jd_parameters *parameters);

const char *v90_jd_result_name(enum v90_jd_result result);

#endif
