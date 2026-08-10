// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_QPSK_H
#define SIPPYMODEM_QPSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_QPSK_MAX_PULSE_SPAN_SYMBOLS = 8,
	SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS =
		SIPPYMODEM_QPSK_MAX_PULSE_SPAN_SYMBOLS + 1,
};

struct sippymodem_qpsk_channel {
	uint32_t sample_rate;
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint32_t carrier_frequency_hz;
	double rolloff;
	unsigned int pulse_span_symbols;
};

enum sippymodem_qpsk_result {
	SIPPYMODEM_QPSK_OK,
	SIPPYMODEM_QPSK_INVALID_ARGUMENT,
};

struct sippymodem_qpsk_active_symbol {
	uint64_t age_units;
	int8_t in_phase;
	int8_t quadrature;
};

struct sippymodem_qpsk_modulator {
	struct sippymodem_qpsk_channel channel;
	struct sippymodem_qpsk_active_symbol
		active[SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS];
	uint64_t symbol_period_units;
	uint64_t symbol_phase_units;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	double pulse_normalization;
	size_t active_count;
	int16_t amplitude;
	bool needs_symbol;
};

enum sippymodem_qpsk_result sippymodem_root_raised_cosine(
	double symbol_time, double rolloff, double *value);

enum sippymodem_qpsk_result sippymodem_qpsk_modulator_init(
	struct sippymodem_qpsk_modulator *modulator,
	const struct sippymodem_qpsk_channel *channel, int16_t amplitude);
bool sippymodem_qpsk_modulator_can_accept_symbol(
	const struct sippymodem_qpsk_modulator *modulator);
/* quadrant is an absolute counter-clockwise phase in quarter turns. */
enum sippymodem_qpsk_result sippymodem_qpsk_modulator_sample(
	struct sippymodem_qpsk_modulator *modulator, uint8_t quadrant,
	int16_t *sample, bool *symbol_accepted);

const char *sippymodem_qpsk_result_name(
	enum sippymodem_qpsk_result result);

#endif
