// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_9600_RECEIVER_H
#define SIPPYMODEM_V32_9600_RECEIVER_H

#include "qam_equalizer.h"
#include "constellation.h"
#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v32_9600_receiver_result {
	SIPPYMODEM_V32_9600_RECEIVER_OK,
	SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v32_9600_receiver {
	struct sippymodem_v32_symbol_decoder decoder;
	struct qam_equalizer equalizer;
	struct qam_point known_targets[
		QAM_EQUALIZER_DELAY + 1];
	bool target_known[QAM_EQUALIZER_DELAY + 1];
};

/* Seed from the last received 4800-bit/s point before switching rates. */
enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_init(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_v32_symbol_decoder *decoder,
	const struct qam_point *reference_point);
bool sippymodem_v32_9600_receiver_is_valid(
	const struct sippymodem_v32_9600_receiver *receiver);
/* Decode one point after the equalizer delay.  While ready is false, the
 * point was accepted but output remains unchanged. */
enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_decode(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct qam_point *point, uint8_t *output, bool *ready);
/* Advance framing and channel state from a known post-E training symbol. */
enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_track_known(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct qam_point *point, uint8_t input);

const char *sippymodem_v32_9600_receiver_result_name(
	enum sippymodem_v32_9600_receiver_result result);

#endif
