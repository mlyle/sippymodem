// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_UNCODED_QAM_H
#define V32_UNCODED_QAM_H

#include "qam_equalizer.h"
#include "constellation.h"
#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_uncoded_qam_result {
	V32_UNCODED_QAM_OK,
	V32_UNCODED_QAM_INVALID_ARGUMENT,
};

struct v32_uncoded_qam {
	struct v32_symbol_decoder decoder;
	struct qam_equalizer equalizer;
	struct qam_point known_targets[
		QAM_EQUALIZER_DELAY + 1];
	bool target_known[QAM_EQUALIZER_DELAY + 1];
};

/* Seed from the last received 4800-bit/s point before switching rates. */
enum v32_uncoded_qam_result
v32_uncoded_qam_init(
	struct v32_uncoded_qam *receiver,
	const struct v32_symbol_decoder *decoder,
	const struct qam_point *reference_point);
bool v32_uncoded_qam_is_valid(
	const struct v32_uncoded_qam *receiver);
/* Decode one point after the equalizer delay.  While ready is false, the
 * point was accepted but output remains unchanged. */
enum v32_uncoded_qam_result
v32_uncoded_qam_decode(
	struct v32_uncoded_qam *receiver,
	const struct qam_point *point, uint8_t *output, bool *ready);
/* Advance framing and channel state from a known post-E training symbol. */
enum v32_uncoded_qam_result
v32_uncoded_qam_track_known(
	struct v32_uncoded_qam *receiver,
	const struct qam_point *point, uint8_t input);

const char *v32_uncoded_qam_result_name(
	enum v32_uncoded_qam_result result);

#endif
