// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_J_H
#define V92_J_H

#include "pcm_j_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_J_FRAME_BITS = PCM_J_FRAME_BITS,
	V92_J_FRAME_BYTES = PCM_J_FRAME_BYTES,
	V92_JD_RATE_CAPABILITY_BITS = 22,
	V92_JD_RATE_CAPABILITY_MASK =
		(1u << V92_JD_RATE_CAPABILITY_BITS) - 1u,
	V92_JD_MINIMUM_LOOKAHEAD = 1,
	V92_JD_MAXIMUM_LOOKAHEAD = 3,
	V92_PHASE3_JP_PRIME_BITS = 12,
};

enum v92_constellation_size {
	V92_CONSTELLATION_4_POINT = 4,
	V92_CONSTELLATION_8_POINT = 8,
};

enum v92_j_result {
	V92_J_OK,
	V92_J_INVALID_ARGUMENT,
	V92_J_INVALID_PARAMETERS,
	V92_J_INVALID_FRAME,
	V92_J_CRC_MISMATCH,
	V92_J_WRONG_RECORD,
};

struct v92_jd {
	/* Bit zero represents 28 000 bit/s; bit 21 represents 56 000 bit/s. */
	uint32_t data_rate_capability_mask;
	uint8_t maximum_spectral_shaping_lookahead;
};

struct v92_jp {
	/* Fraction of one symbol interval in unsigned Q0.16 units. */
	uint16_t su_phase_extension_q16;
	enum v92_constellation_size training_constellation;
	enum v92_constellation_size rate_renegotiation_constellation;
};

bool v92_constellation_size_is_valid(
	enum v92_constellation_size constellation);
bool v92_jd_is_valid(const struct v92_jd *jd);
bool v92_jp_is_valid(const struct v92_jp *jp);

/* Both records use the common PCM J envelope. Encoding canonicalizes reserved
 * bits to zero; decoding ignores their values after validating the CRC. */
enum v92_j_result v92_jd_encode(
	const struct v92_jd *jd, uint8_t *frame, size_t frame_capacity);
enum v92_j_result v92_jd_decode(
	const uint8_t *frame, size_t frame_capacity, struct v92_jd *jd);
enum v92_j_result v92_jp_encode(
	const struct v92_jp *jp, uint8_t *frame, size_t frame_capacity);
enum v92_j_result v92_jp_decode(
	const uint8_t *frame, size_t frame_capacity, struct v92_jp *jp);

const char *v92_j_result_name(enum v92_j_result result);

#endif /* V92_J_H */
