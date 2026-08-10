// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_9600_RECEIVER_H
#define SIPPYMODEM_V32_9600_RECEIVER_H

#include "sippymodem_v32_constellation.h"
#include "sippymodem_v32_framing.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v32_9600_receiver_result {
	SIPPYMODEM_V32_9600_RECEIVER_OK,
	SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT,
};

struct sippymodem_v32_9600_receiver {
	struct sippymodem_v32_symbol_decoder decoder;
	struct sippymodem_qam_point channel_gain;
};

/* Seed from the last received 4800-bit/s point before switching rates. */
enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_init(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_v32_symbol_decoder *decoder,
	const struct sippymodem_qam_point *reference_point);
bool sippymodem_v32_9600_receiver_is_valid(
	const struct sippymodem_v32_9600_receiver *receiver);
enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_decode(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t *output);

const char *sippymodem_v32_9600_receiver_result_name(
	enum sippymodem_v32_9600_receiver_result result);

#endif
