// SPDX-License-Identifier: BSD-3-Clause

#ifndef QAM_H
#define QAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	QAM_MAX_PULSE_SPAN_SYMBOLS = 8,
	QAM_MAX_ACTIVE_SYMBOLS =
		QAM_MAX_PULSE_SPAN_SYMBOLS + 1,
	QAM_MAX_MATCHED_FILTER_SAMPLES = 256,
	QAM_MAX_TIMING_CANDIDATES = 64,
};

struct qam_channel {
	uint32_t sample_rate;
	uint32_t symbol_rate_numerator;
	uint32_t symbol_rate_denominator;
	/* Carrier frequency is the exact numerator/denominator ratio in hertz. */
	uint32_t carrier_frequency_numerator;
	uint32_t carrier_frequency_denominator;
	double rolloff;
	unsigned int pulse_span_symbols;
};

enum qam_result {
	QAM_OK,
	QAM_INVALID_ARGUMENT,
};

struct qam_point {
	double in_phase;
	double quadrature;
};

struct qam_active_symbol {
	uint64_t age_units;
	struct qam_point point;
};

struct qam_modulator {
	struct qam_channel channel;
	struct qam_active_symbol
		active[QAM_MAX_ACTIVE_SYMBOLS];
	uint64_t symbol_period_units;
	uint64_t symbol_phase_units;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	double pulse_normalization;
	double maximum_point_power;
	size_t active_count;
	int16_t amplitude;
	bool needs_symbol;
};

struct qam_timing_candidate {
	uint64_t phase_units;
	double symbol_power_sum;
	uint32_t symbol_count;
};

struct qam_demodulator {
	struct qam_channel channel;
	double matched_coefficients[
		QAM_MAX_MATCHED_FILTER_SAMPLES];
	double in_phase_history[
		QAM_MAX_MATCHED_FILTER_SAMPLES];
	double quadrature_history[
		QAM_MAX_MATCHED_FILTER_SAMPLES];
	struct qam_timing_candidate
		timing[QAM_MAX_TIMING_CANDIDATES];
	uint64_t symbol_period_units;
	uint64_t timing_step_units;
	/* Recovered symbol-phase advance for each input sample. */
	uint64_t recovery_step_units;
	uint64_t locked_phase_units;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	uint32_t acquisition_sample_count;
	uint32_t acquisition_sample_target;
	size_t matched_filter_count;
	size_t history_index;
	size_t history_count;
	size_t timing_candidate_count;
	/* Candidate used as the nominal-clock reference for the next window. */
	size_t timing_reference_index;
	double line_power;
	double symbol_power;
	double previous_in_phase;
	double previous_quadrature;
	bool timing_locked;
	bool has_previous_matched_sample;
};

struct qam_measurement {
	double line_power;
	double symbol_power;
	bool timing_locked;
};

/* Quantize the rotation between two finite baseband points. */
enum qam_result qam_nearest_quarter_turn(
	const struct qam_point *previous,
	const struct qam_point *current, uint8_t *quarter_turns);

enum qam_result root_raised_cosine(
	double symbol_time, double rolloff, double *value);

enum qam_result qam_modulator_init(
	struct qam_modulator *modulator,
	const struct qam_channel *channel, int16_t amplitude);
/* Select an explicit squared-magnitude bound for a mode-specific
 * constellation. The ordinary initializer retains the 9/5 training bound. */
enum qam_result
qam_modulator_init_with_point_power_limit(
	struct qam_modulator *modulator,
	const struct qam_channel *channel, int16_t amplitude,
	double maximum_point_power);
bool qam_modulator_is_valid(
	const struct qam_modulator *modulator);
bool qam_modulator_can_accept_symbol(
	const struct qam_modulator *modulator);
/* Transmit points must fit the modulator's squared-magnitude bound. */
enum qam_result qam_modulator_sample_f64(
	struct qam_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *symbol_accepted);
/* The integer interface scales the same unit-amplitude waveform by the
 * configured amplitude, rounds it, and clamps it to that symmetric bound. */
enum qam_result qam_modulator_sample(
	struct qam_modulator *modulator,
	const struct qam_point *point, int16_t *sample,
	bool *symbol_accepted);

enum qam_result qam_demodulator_init(
	struct qam_demodulator *demodulator,
	const struct qam_channel *channel);
bool qam_demodulator_is_valid(
	const struct qam_demodulator *demodulator);
enum qam_result qam_demodulator_reset(
	struct qam_demodulator *demodulator);
/* Ready points are unnormalized matched-filter coordinates interpolated at
 * the exact rational symbol-clock crossing. */
enum qam_result qam_demodulator_sample(
	struct qam_demodulator *demodulator, int16_t sample,
	struct qam_point *point, bool *symbol_ready);
struct qam_measurement qam_demodulator_measure(
	const struct qam_demodulator *demodulator);

const char *qam_result_name(
	enum qam_result result);

#endif
