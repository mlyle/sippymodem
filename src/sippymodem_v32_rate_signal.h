// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_RATE_SIGNAL_H
#define SIPPYMODEM_V32_RATE_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_RATE_SIGNAL_BITS = 16,
	SIPPYMODEM_V32_RATE_SIGNAL_DIBITS = 8,
};

enum sippymodem_v32_rate_capability {
	SIPPYMODEM_V32_RATE_CAPABILITY_2400 = 1u << 0,
	SIPPYMODEM_V32_RATE_CAPABILITY_4800 = 1u << 1,
	SIPPYMODEM_V32_RATE_CAPABILITY_9600 = 1u << 2,
};

enum sippymodem_v32_rate_signal_type {
	SIPPYMODEM_V32_RATE_SIGNAL,
	SIPPYMODEM_V32_END_SIGNAL,
};

enum sippymodem_v32_rate_signal_result {
	SIPPYMODEM_V32_RATE_SIGNAL_OK,
	SIPPYMODEM_V32_RATE_SIGNAL_INVALID_ARGUMENT,
	SIPPYMODEM_V32_RATE_SIGNAL_INVALID_OFFER,
	SIPPYMODEM_V32_RATE_SIGNAL_INVALID_WORD,
};

struct sippymodem_v32_rate_offer {
	uint8_t rates;
	bool trellis_at_highest_rate;
};

/* Rate words are represented in transmission order: B0 is bit 15. */
enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_encode(
	enum sippymodem_v32_rate_signal_type type,
	const struct sippymodem_v32_rate_offer *offer, uint16_t *word);
enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_decode(
	uint16_t word, enum sippymodem_v32_rate_signal_type *type,
	struct sippymodem_v32_rate_offer *offer);
enum sippymodem_v32_rate_signal_result sippymodem_v32_rate_signal_dibit(
	uint16_t word, unsigned int index, uint8_t *dibit);

const char *sippymodem_v32_rate_signal_result_name(
	enum sippymodem_v32_rate_signal_result result);

#endif
