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
	SIPPYMODEM_QPSK_MAX_MATCHED_FILTER_SAMPLES = 256,
	SIPPYMODEM_QPSK_MAX_TIMING_CANDIDATES = 64,
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

struct sippymodem_qpsk_timing_candidate {
	uint64_t phase_units;
	double previous_in_phase;
	double previous_quadrature;
	double symbol_power_sum;
	uint32_t symbol_count;
	bool has_previous;
};

struct sippymodem_qpsk_demodulator {
	struct sippymodem_qpsk_channel channel;
	double matched_coefficients[
		SIPPYMODEM_QPSK_MAX_MATCHED_FILTER_SAMPLES];
	double in_phase_history[
		SIPPYMODEM_QPSK_MAX_MATCHED_FILTER_SAMPLES];
	double quadrature_history[
		SIPPYMODEM_QPSK_MAX_MATCHED_FILTER_SAMPLES];
	struct sippymodem_qpsk_timing_candidate
		timing[SIPPYMODEM_QPSK_MAX_TIMING_CANDIDATES];
	uint64_t symbol_period_units;
	uint64_t timing_step_units;
	uint64_t locked_phase_units;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	uint32_t acquisition_sample_count;
	uint32_t acquisition_sample_target;
	size_t matched_filter_count;
	size_t history_index;
	size_t history_count;
	size_t timing_candidate_count;
	double previous_in_phase;
	double previous_quadrature;
	double line_power;
	double symbol_power;
	bool has_previous;
	bool timing_locked;
};

struct sippymodem_qpsk_measurement {
	double line_power;
	double symbol_power;
	bool timing_locked;
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

enum sippymodem_qpsk_result sippymodem_qpsk_demodulator_init(
	struct sippymodem_qpsk_demodulator *demodulator,
	const struct sippymodem_qpsk_channel *channel);
enum sippymodem_qpsk_result sippymodem_qpsk_demodulator_reset(
	struct sippymodem_qpsk_demodulator *demodulator);
enum sippymodem_qpsk_result sippymodem_qpsk_demodulator_sample(
	struct sippymodem_qpsk_demodulator *demodulator, int16_t sample,
	uint8_t *quarter_turns, bool *symbol_ready);
struct sippymodem_qpsk_measurement sippymodem_qpsk_demodulator_measure(
	const struct sippymodem_qpsk_demodulator *demodulator);

const char *sippymodem_qpsk_result_name(
	enum sippymodem_qpsk_result result);

#endif
