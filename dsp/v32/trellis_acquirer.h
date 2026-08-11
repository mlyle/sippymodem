// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_TRELLIS_ACQUIRER_H
#define SIPPYMODEM_V32_TRELLIS_ACQUIRER_H

#include "trellis_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v32_trellis_acquirer_result {
	SIPPYMODEM_V32_TRELLIS_ACQUIRER_OK,
	SIPPYMODEM_V32_TRELLIS_ACQUIRER_NOT_READY,
	SIPPYMODEM_V32_TRELLIS_ACQUIRER_INVALID_ARGUMENT,
};

/* Resolve the fourfold carrier-phase ambiguity while B1 supplies known data.
 * Each candidate owns its complete equalizer, trellis, and framing state. */
struct sippymodem_v32_trellis_acquirer {
	struct sippymodem_v32_trellis_receiver candidates[
		QPSK_QUADRANT_COUNT];
	uint16_t confirmations[QPSK_QUADRANT_COUNT];
	uint16_t observations;
	uint8_t selected_phase;
	bool selected;
};

enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_init(
	struct sippymodem_v32_trellis_acquirer *acquirer,
	const struct sippymodem_v32_symbol_decoder *framing,
	const struct qam_point *reference_point,
	uint8_t data_bits_per_symbol);
bool sippymodem_v32_trellis_acquirer_is_valid(
	const struct sippymodem_v32_trellis_acquirer *acquirer);

/* Consume one known post-E group without assuming an absolute carrier phase. */
enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_track_known(
	struct sippymodem_v32_trellis_acquirer *acquirer,
	const struct qam_point *point, uint8_t input);

/* Select a unique, well-separated phase hypothesis after known training. */
enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_select(
	struct sippymodem_v32_trellis_acquirer *acquirer);

enum sippymodem_v32_trellis_acquirer_result
sippymodem_v32_trellis_acquirer_decode(
	struct sippymodem_v32_trellis_acquirer *acquirer,
	const struct qam_point *point, uint8_t *output, bool *ready);

const char *sippymodem_v32_trellis_acquirer_result_name(
	enum sippymodem_v32_trellis_acquirer_result result);

#endif
