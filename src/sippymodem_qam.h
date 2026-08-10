// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_QAM_H
#define SIPPYMODEM_QAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_QAM_MAX_PULSE_SPAN_SYMBOLS = 8,
	SIPPYMODEM_QAM_MAX_ACTIVE_SYMBOLS =
		SIPPYMODEM_QAM_MAX_PULSE_SPAN_SYMBOLS + 1,
	SIPPYMODEM_QAM_MAX_MATCHED_FILTER_SAMPLES = 256,
	SIPPYMODEM_QAM_MAX_TIMING_CANDIDATES = 64,
};

struct sippymodem_qam_channel {
	uint32_t sample_rate;
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	uint32_t carrier_frequency_hz;
	double rolloff;
	unsigned int pulse_span_symbols;
};

enum sippymodem_qam_result {
	SIPPYMODEM_QAM_OK,
	SIPPYMODEM_QAM_INVALID_ARGUMENT,
};

struct sippymodem_qam_point {
	double in_phase;
	double quadrature;
};

struct sippymodem_qam_active_symbol {
	uint64_t age_units;
	struct sippymodem_qam_point point;
};

struct sippymodem_qam_modulator {
	struct sippymodem_qam_channel channel;
	struct sippymodem_qam_active_symbol
		active[SIPPYMODEM_QAM_MAX_ACTIVE_SYMBOLS];
	uint64_t symbol_period_units;
	uint64_t symbol_phase_units;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	double pulse_normalization;
	size_t active_count;
	int16_t amplitude;
	bool needs_symbol;
};

struct sippymodem_qam_timing_candidate {
	uint64_t phase_units;
	double symbol_power_sum;
	uint32_t symbol_count;
};

struct sippymodem_qam_demodulator {
	struct sippymodem_qam_channel channel;
	double matched_coefficients[
		SIPPYMODEM_QAM_MAX_MATCHED_FILTER_SAMPLES];
	double in_phase_history[
		SIPPYMODEM_QAM_MAX_MATCHED_FILTER_SAMPLES];
	double quadrature_history[
		SIPPYMODEM_QAM_MAX_MATCHED_FILTER_SAMPLES];
	struct sippymodem_qam_timing_candidate
		timing[SIPPYMODEM_QAM_MAX_TIMING_CANDIDATES];
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
	double line_power;
	double symbol_power;
	bool timing_locked;
};

struct sippymodem_qam_measurement {
	double line_power;
	double symbol_power;
	bool timing_locked;
};

enum sippymodem_qam_result sippymodem_root_raised_cosine(
	double symbol_time, double rolloff, double *value);

enum sippymodem_qam_result sippymodem_qam_modulator_init(
	struct sippymodem_qam_modulator *modulator,
	const struct sippymodem_qam_channel *channel, int16_t amplitude);
bool sippymodem_qam_modulator_can_accept_symbol(
	const struct sippymodem_qam_modulator *modulator);
/* Transmit points are finite and normalized to a maximum magnitude of one. */
enum sippymodem_qam_result sippymodem_qam_modulator_sample(
	struct sippymodem_qam_modulator *modulator,
	const struct sippymodem_qam_point *point, int16_t *sample,
	bool *symbol_accepted);

enum sippymodem_qam_result sippymodem_qam_demodulator_init(
	struct sippymodem_qam_demodulator *demodulator,
	const struct sippymodem_qam_channel *channel);
enum sippymodem_qam_result sippymodem_qam_demodulator_reset(
	struct sippymodem_qam_demodulator *demodulator);
/* Ready points are unnormalized matched-filter coordinates. */
enum sippymodem_qam_result sippymodem_qam_demodulator_sample(
	struct sippymodem_qam_demodulator *demodulator, int16_t sample,
	struct sippymodem_qam_point *point, bool *symbol_ready);
struct sippymodem_qam_measurement sippymodem_qam_demodulator_measure(
	const struct sippymodem_qam_demodulator *demodulator);

const char *sippymodem_qam_result_name(
	enum sippymodem_qam_result result);

#endif
