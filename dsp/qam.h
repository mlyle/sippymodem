// SPDX-License-Identifier: BSD-3-Clause

#ifndef QAM_H
#define QAM_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	QAM_MAX_PULSE_SPAN_SYMBOLS = 14,
	QAM_MAX_ACTIVE_SYMBOLS =
		QAM_MAX_PULSE_SPAN_SYMBOLS + 1,
	QAM_MAX_MATCHED_FILTER_SAMPLES = 256,
	QAM_MAX_TIMING_CANDIDATES = 64,
	QAM_INTERPOLATION_HISTORY = 3,
	QAM_DEFAULT_TIMING_WINDOW_SYMBOLS = 256,
	QAM_MIN_TIMING_WINDOW_SYMBOLS = 32,
	QAM_MAX_TIMING_WINDOW_SYMBOLS = 4096,
};

/* Initializers may reuse the channel settings retained in their own state. */
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

/* Raw arithmetic on readable points; callers retain their numeric checks. */
static inline double qam_point_power(const struct qam_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
}

static inline double qam_point_distance_squared(
	const struct qam_point *left, const struct qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

static inline struct qam_point qam_point_multiply(
	const struct qam_point *left,
	const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

/* Counterclockwise carrier rotation in radians. Numeric validation and phase
 * wrapping belong to the caller, just as for the raw complex product. */
static inline struct qam_point qam_point_rotate(
	const struct qam_point *point, double phase)
{
	const struct qam_point rotation = { cos(phase), sin(phase) };

	return qam_point_multiply(point, &rotation);
}

/* Raw complex quotient using the denominator power already measured by the
 * caller. As with multiplication, numeric validation belongs to the owner. */
static inline struct qam_point qam_point_quotient(
	const struct qam_point *numerator,
	const struct qam_point *denominator, double denominator_power)
{
	return (struct qam_point){
		.in_phase = (numerator->in_phase * denominator->in_phase +
			     numerator->quadrature * denominator->quadrature) /
			    denominator_power,
		.quadrature = (numerator->quadrature * denominator->in_phase -
			       numerator->in_phase * denominator->quadrature) /
			      denominator_power,
	};
}

/* Complex correlation: conjugate the reference, then multiply by the sample.
 * Like the raw product above, callers retain finite-input/result checks. */
static inline struct qam_point qam_point_correlation(
	const struct qam_point *reference, const struct qam_point *sample)
{
	return (struct qam_point){
		.in_phase = reference->in_phase * sample->in_phase +
			    reference->quadrature * sample->quadrature,
		.quadrature = reference->in_phase * sample->quadrature -
			      reference->quadrature * sample->in_phase,
	};
}

static inline bool qam_adaptation_is_valid(double adaptation)
{
	return isfinite(adaptation) && adaptation >= 0.0 &&
	       adaptation <= 1.0;
}

static inline bool point_is_finite(const struct qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

/* Finite squared magnitude also proves both coordinates finite: squares
 * cannot cancel infinity or NaN. Require power strictly above 1e-12.
 * Optional power may alias a coordinate; rejection leaves it unchanged. */
static inline bool qam_point_nonzero_power(
	const struct qam_point *point, double *power)
{
	double value;

	if (point == NULL)
		return false;
	value = qam_point_power(point);
	if (!isfinite(value) || value <= 1e-12)
		return false;
	if (power != NULL)
		*power = value;
	return true;
}

/* Checked complex division. A zero numerator is allowed; the denominator
 * needs finite power above 1e-12. Both inputs may alias the output.
 * Rejection leaves the output unchanged. */
static inline bool qam_point_divide(
	const struct qam_point *numerator,
	const struct qam_point *denominator, struct qam_point *quotient)
{
	struct qam_point next;
	double power;

	if (!point_is_finite(numerator) || quotient == NULL ||
	    !qam_point_nonzero_power(denominator, &power))
		return false;
	next = qam_point_quotient(numerator, denominator, power);
	if (!point_is_finite(&next))
		return false;
	*quotient = next;
	return true;
}

/* Accept a finite point whose squared magnitude fits a finite positive limit.
 * A small relative tolerance covers floating-point roundoff at the boundary. */
static inline bool qam_point_fits_power(
	const struct qam_point *point, double maximum_point_power)
{
	double power;

	if (point == NULL || !isfinite(maximum_point_power) ||
	    maximum_point_power <= 0.0)
		return false;
	power = qam_point_power(point);
	return isfinite(power) &&
	       (power <= maximum_point_power ||
		power / maximum_point_power <= 1.0 + 1e-12);
}

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
	uint64_t recovery_phase_units;
	uint64_t timing_forward_error_units;
	uint64_t timing_backward_error_units;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	uint32_t timing_sample_count;
	uint32_t timing_sample_target;
	uint32_t acquisition_window_symbols;
	uint32_t tracking_window_symbols;
	uint32_t recovery_phase_samples;
	size_t matched_filter_count;
	size_t history_index;
	size_t history_count;
	size_t timing_candidate_count;
	/* Stable energy reference and previous clock-observation candidate. */
	size_t timing_reference_index;
	size_t timing_rate_reference_index;
	size_t timing_pending_reference_index;
	double line_power;
	double symbol_power;
	double timing_power_hysteresis;
	struct qam_point matched_history[QAM_INTERPOLATION_HISTORY];
	double recovery_fraction;
	uint8_t matched_history_count;
	uint8_t timing_error_windows;
	uint8_t timing_forward_windows;
	uint8_t timing_backward_windows;
	uint8_t timing_reference_windows;
	int8_t recovery_phase_direction;
	bool timing_locked;
	/* The initial timing window begins with carrier energy, not silence. */
	bool acquisition_started;
	bool recovery_pending;
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
/* Use a bounded initial timing window, then automatically move to the longer
 * tracking window after lock. The ordinary initializer uses the default for
 * both. Short acquisition windows are intended for finite training markers,
 * not as a substitute for a mode-specific marker detector. */
enum qam_result qam_demodulator_init_with_timing_windows(
	struct qam_demodulator *demodulator,
	const struct qam_channel *channel,
	uint32_t acquisition_symbols, uint32_t tracking_symbols);
/* Require this power ratio before tracking moves to a neighboring timing
 * candidate. Values must be finite and at least one. */
enum qam_result qam_demodulator_set_timing_power_hysteresis(
	struct qam_demodulator *demodulator, double ratio);
bool qam_demodulator_is_valid(
	const struct qam_demodulator *demodulator);
bool qam_demodulator_has_timing_lock(
	const struct qam_demodulator *demodulator);
enum qam_result qam_demodulator_reset(
	struct qam_demodulator *demodulator);
/* Reacquire symbol timing after a training-signal transition, retaining the
 * matched-filter input and carrier clock. Rejected calls preserve state. */
enum qam_result qam_demodulator_reacquire(
	struct qam_demodulator *demodulator, uint32_t acquisition_symbols);
/* Ready points are unnormalized matched-filter coordinates interpolated at
 * the exact rational symbol-clock crossing. */
enum qam_result qam_demodulator_sample(
	struct qam_demodulator *demodulator, int16_t sample,
	struct qam_point *point, bool *symbol_ready);
struct qam_measurement qam_demodulator_measure(
	const struct qam_demodulator *demodulator);


#endif
