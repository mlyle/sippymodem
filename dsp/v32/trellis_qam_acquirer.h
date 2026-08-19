// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_TRELLIS_QAM_ACQUIRER_H
#define V32_TRELLIS_QAM_ACQUIRER_H

#include "trellis_qam_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_trellis_qam_acquirer_result {
	V32_TRELLIS_QAM_ACQUIRER_OK,
	V32_TRELLIS_QAM_ACQUIRER_NOT_READY,
	V32_TRELLIS_QAM_ACQUIRER_INVALID_ARGUMENT,
};

/* Resolve the peer's four possible differential start states while B1
 * supplies known data. Each candidate owns its complete equalizer, trellis,
 * and framing state. */
struct v32_trellis_qam_acquirer {
	struct v32_trellis_qam_receiver candidates[
		QPSK_QUADRANT_COUNT];
	uint16_t confirmations[QPSK_QUADRANT_COUNT];
	uint16_t observations;
	uint8_t selected_differential_output;
	bool selected;
};

enum v32_trellis_qam_acquirer_result
v32_trellis_qam_acquirer_init(
	struct v32_trellis_qam_acquirer *acquirer,
	const struct v32_symbol_decoder *framing,
	const struct qam_point *reference_point,
	uint8_t data_bits_per_symbol);
bool v32_trellis_qam_acquirer_is_valid(
	const struct v32_trellis_qam_acquirer *acquirer);

/* Consume one known post-E group under every differential-state hypothesis. */
enum v32_trellis_qam_acquirer_result
v32_trellis_qam_acquirer_track_known(
	struct v32_trellis_qam_acquirer *acquirer,
	const struct qam_point *point, uint8_t input);

/* Select a unique, well-separated differential-state hypothesis. */
enum v32_trellis_qam_acquirer_result
v32_trellis_qam_acquirer_select(
	struct v32_trellis_qam_acquirer *acquirer);

enum v32_trellis_qam_acquirer_result
v32_trellis_qam_acquirer_decode(
	struct v32_trellis_qam_acquirer *acquirer,
	const struct qam_point *point, uint8_t *output, bool *ready);

const char *v32_trellis_qam_acquirer_result_name(
	enum v32_trellis_qam_acquirer_result result);

#endif
