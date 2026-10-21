// SPDX-License-Identifier: BSD-3-Clause

#include "qam.h"

#include "fractional_delay.h"

#include "integer_math.h"
#include "phase.h"
#include "sample_math.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	QAM_NORMALIZATION_PHASES = 512,
	/* Bound the recovered clock to one percent around the configured rate,
	 * retaining one phase unit of range for very small exact numerators. */
	QAM_CLOCK_TRACKING_RANGE_DIVISOR = 100,
	QAM_CLOCK_TRACKING_FILTER_WINDOWS = 3,
};

static const double QAM_PI = 3.1415926535897932384626433832795;
static const double QAM_SINGULARITY_EPSILON = 1e-10;
/* Unit-average-power square training constellations reach 9/5 at a corner. */
static const double QAM_MAX_POINT_POWER = 9.0 / 5.0;
static const double QAM_MIN_ACQUISITION_POWER = 64.0;
static const double QAM_DEFAULT_TIMING_POWER_HYSTERESIS = 1.005;

enum qam_result qam_nearest_quarter_turn(
	const struct qam_point *previous,
	const struct qam_point *current, uint8_t *quarter_turns)
{
	struct qam_point correlation;
	int turns;

	if (!point_is_finite(previous) || !point_is_finite(current) ||
	    quarter_turns == NULL)
		return QAM_INVALID_ARGUMENT;
	correlation = qam_point_correlation(previous, current);
	if (!point_is_finite(&correlation))
		return QAM_INVALID_ARGUMENT;
	turns = (int)lround(atan2(correlation.quadrature, correlation.in_phase) /
		(QAM_PI / 2.0));
	*quarter_turns = (uint8_t)((unsigned int)turns & 3u);
	return QAM_OK;
}

static bool channel_is_valid(
	const struct qam_channel *channel,
	uint64_t *symbol_period_units)
{
	uint64_t carrier_denominator;
	uint64_t period;

	if (channel == NULL || channel->sample_rate == 0 ||
	    channel->symbol_rate_numerator == 0 ||
	    channel->symbol_rate_denominator == 0 ||
	    channel->carrier_frequency_numerator == 0 ||
	    channel->carrier_frequency_denominator == 0 ||
	    !isfinite(channel->rolloff) || channel->rolloff <= 0.0 ||
	    channel->rolloff > 1.0 || channel->pulse_span_symbols < 2 ||
	    channel->pulse_span_symbols >
		    QAM_MAX_PULSE_SPAN_SYMBOLS ||
	    (channel->pulse_span_symbols & 1u) != 0)
		return false;
	carrier_denominator =
		(uint64_t)channel->sample_rate *
		channel->carrier_frequency_denominator;
	if ((uint64_t)channel->carrier_frequency_numerator * 2u >=
			carrier_denominator ||
	    phase_step_u32(channel->sample_rate,
			  channel->carrier_frequency_numerator,
			  channel->carrier_frequency_denominator) == 0)
		return false;
	period = (uint64_t)channel->sample_rate *
		 channel->symbol_rate_denominator;
	if (period == 0 || channel->symbol_rate_numerator > period ||
	    period > (UINT64_MAX - channel->symbol_rate_numerator) /
			     channel->pulse_span_symbols)
		return false;
	*symbol_period_units = period;
	return true;
}

enum qam_result root_raised_cosine(
	double symbol_time, double rolloff, double *value)
{
	double four_rolloff_time;
	double numerator;
	double denominator;

	if (!isfinite(symbol_time) || !isfinite(rolloff) || rolloff <= 0.0 ||
	    rolloff > 1.0 || value == NULL)
		return QAM_INVALID_ARGUMENT;
	if (fabs(symbol_time) < QAM_SINGULARITY_EPSILON) {
		*value = 1.0 + rolloff * (4.0 / QAM_PI - 1.0);
		return QAM_OK;
	}
	four_rolloff_time = 4.0 * rolloff * symbol_time;
	if (fabs(fabs(four_rolloff_time) - 1.0) <
	    QAM_SINGULARITY_EPSILON) {
		double angle = QAM_PI / (4.0 * rolloff);

		*value = rolloff / sqrt(2.0) *
			 ((1.0 + 2.0 / QAM_PI) * sin(angle) +
			  (1.0 - 2.0 / QAM_PI) * cos(angle));
		return QAM_OK;
	}
	numerator = sin(QAM_PI * symbol_time * (1.0 - rolloff)) +
		    four_rolloff_time *
			    cos(QAM_PI * symbol_time * (1.0 + rolloff));
	denominator = QAM_PI * symbol_time *
		      (1.0 - four_rolloff_time * four_rolloff_time);
	*value = numerator / denominator;
	return isfinite(*value) ? QAM_OK :
				 QAM_INVALID_ARGUMENT;
}

static double pulse_normalization(
	const struct qam_channel *channel)
{
	double maximum = 0.0;
	double delay = (double)channel->pulse_span_symbols / 2.0;

	/* Generate a conservative peak bound instead of storing polyphase
	 * coefficients. The final clamp covers the space between scan points. */
	for (unsigned int phase = 0; phase < QAM_NORMALIZATION_PHASES;
	     ++phase) {
		double fractional =
			(double)phase / (double)QAM_NORMALIZATION_PHASES;
		double sum = 0.0;

		for (unsigned int age = 0;
		     age <= channel->pulse_span_symbols; ++age) {
			double pulse = 0.0;

			(void)root_raised_cosine(
				(double)age + fractional - delay,
				channel->rolloff, &pulse);
			sum += fabs(pulse);
		}
		if (sum > maximum)
			maximum = sum;
	}
	return maximum * 1.001;
}

bool qam_modulator_is_valid(
	const struct qam_modulator *modulator)
{
	uint64_t period = 0;
	uint64_t support;

	if (modulator == NULL ||
	    !channel_is_valid(&modulator->channel, &period) ||
	    modulator->symbol_period_units != period ||
	    modulator->symbol_phase_units >= period ||
	    modulator->carrier_phase_step !=
		    phase_step_u32(
			    modulator->channel.sample_rate,
			    modulator->channel.carrier_frequency_numerator,
			    modulator->channel.carrier_frequency_denominator) ||
	    modulator->amplitude <= 0 ||
	    !isfinite(modulator->pulse_normalization) ||
	    modulator->pulse_normalization <= 0.0 ||
	    !isfinite(modulator->maximum_point_power) ||
	    modulator->maximum_point_power <= 0.0 ||
	    modulator->active_count > QAM_MAX_ACTIVE_SYMBOLS)
		return false;
	support = period * modulator->channel.pulse_span_symbols;
	if (modulator->active_count == 0 && !modulator->needs_symbol)
		return false;
	for (size_t index = 0; index < modulator->active_count; ++index)
		if (modulator->active[index].age_units > support ||
		    !qam_point_fits_power(
			    &modulator->active[index].point,
			    modulator->maximum_point_power))
			return false;
	return true;
}

enum qam_result qam_modulator_init(
	struct qam_modulator *modulator,
	const struct qam_channel *channel, int16_t amplitude)
{
	return qam_modulator_init_with_point_power_limit(
		modulator, channel, amplitude, QAM_MAX_POINT_POWER);
}

enum qam_result
qam_modulator_init_with_point_power_limit(
	struct qam_modulator *modulator,
	const struct qam_channel *channel, int16_t amplitude,
	double maximum_point_power)
{
	uint64_t period = 0;
	double normalization;
	struct qam_channel settings;

	if (modulator == NULL || amplitude <= 0 ||
	    !isfinite(maximum_point_power) || maximum_point_power <= 0.0 ||
	    !channel_is_valid(channel, &period))
		return QAM_INVALID_ARGUMENT;
	normalization = pulse_normalization(channel);
	if (!isfinite(normalization) || normalization <= 0.0)
		return QAM_INVALID_ARGUMENT;
	settings = *channel;
	channel = &settings;
	memset(modulator, 0, sizeof(*modulator));
	modulator->channel = *channel;
	modulator->symbol_period_units = period;
	modulator->carrier_phase_step = phase_step_u32(
		channel->sample_rate, channel->carrier_frequency_numerator,
		channel->carrier_frequency_denominator);
	modulator->pulse_normalization = normalization;
	modulator->maximum_point_power = maximum_point_power;
	modulator->amplitude = amplitude;
	modulator->needs_symbol = true;
	return QAM_OK;
}

bool qam_modulator_can_accept_symbol(
	const struct qam_modulator *modulator)
{
	return qam_modulator_is_valid(modulator) &&
	       modulator->needs_symbol &&
	       modulator->active_count < QAM_MAX_ACTIVE_SYMBOLS;
}

static void append_symbol(
	struct qam_modulator *modulator,
	const struct qam_point *point)
{
	struct qam_active_symbol *symbol =
		&modulator->active[modulator->active_count++];

	*symbol = (struct qam_active_symbol){
		.age_units = modulator->symbol_phase_units,
		.point = *point,
	};
	modulator->needs_symbol = false;
}

static void baseband_sample(
	const struct qam_modulator *modulator,
	double *in_phase, double *quadrature)
{
	double delay = (double)modulator->channel.pulse_span_symbols / 2.0;

	*in_phase = 0.0;
	*quadrature = 0.0;
	for (size_t index = 0; index < modulator->active_count; ++index) {
		const struct qam_active_symbol *symbol =
			&modulator->active[index];
		double age = (double)symbol->age_units /
			     (double)modulator->symbol_period_units;
		double pulse = 0.0;

		(void)root_raised_cosine(
			age - delay, modulator->channel.rolloff, &pulse);
		*in_phase += pulse * symbol->point.in_phase;
		*quadrature += pulse * symbol->point.quadrature;
	}
}

static void advance_time(struct qam_modulator *modulator)
{
	uint64_t step = modulator->channel.symbol_rate_numerator;
	uint64_t support = modulator->symbol_period_units *
			   modulator->channel.pulse_span_symbols;
	size_t retained = 0;

	for (size_t index = 0; index < modulator->active_count; ++index) {
		struct qam_active_symbol symbol =
			modulator->active[index];

		if (symbol.age_units <= support - step) {
			symbol.age_units += step;
			modulator->active[retained++] = symbol;
		}
	}
	modulator->active_count = retained;
	if (phase_advance_u64(&modulator->symbol_phase_units,
		modulator->symbol_period_units, step, NULL))
		modulator->needs_symbol = true;
	modulator->carrier_phase += modulator->carrier_phase_step;
}

enum qam_result qam_modulator_sample_f64(
	struct qam_modulator *modulator,
	const struct qam_point *point, double *sample,
	bool *symbol_accepted)
{
	double carrier_phase;
	double in_phase;
	double quadrature;
	double waveform;

	if (!qam_modulator_is_valid(modulator) ||
	    !qam_point_fits_power(point, modulator->maximum_point_power) ||
	    sample == NULL || symbol_accepted == NULL ||
	    (modulator->needs_symbol &&
	     modulator->active_count == QAM_MAX_ACTIVE_SYMBOLS))
		return QAM_INVALID_ARGUMENT;
	*symbol_accepted = modulator->needs_symbol;
	if (modulator->needs_symbol)
		append_symbol(modulator, point);
	baseband_sample(modulator, &in_phase, &quadrature);
	carrier_phase = phase_radians_u32(modulator->carrier_phase);
	waveform = (in_phase * cos(carrier_phase) -
		    quadrature * sin(carrier_phase)) /
		   modulator->pulse_normalization;
	*sample = waveform;
	advance_time(modulator);
	return QAM_OK;
}

enum qam_result qam_modulator_sample(
	struct qam_modulator *modulator,
	const struct qam_point *point, int16_t *sample,
	bool *symbol_accepted)
{
	double waveform;
	bool accepted;
	int16_t rounded;
	enum qam_result result;

	if (sample == NULL || symbol_accepted == NULL)
		return QAM_INVALID_ARGUMENT;
	result = qam_modulator_sample_f64(
		modulator, point, &waveform, &accepted);
	if (result != QAM_OK)
		return result;
	rounded = sample_round_s16(waveform * modulator->amplitude);
	if (rounded > modulator->amplitude)
		rounded = modulator->amplitude;
	if (rounded < -modulator->amplitude)
		rounded = -modulator->amplitude;
	*sample = rounded;
	*symbol_accepted = accepted;
	return QAM_OK;
}

static bool demodulator_shape_is_valid(
	const struct qam_channel *channel, uint64_t period,
	size_t *filter_count, size_t *candidate_count,
	uint64_t *timing_step)
{
	uint64_t step = channel->symbol_rate_numerator;
	uint64_t support = period * channel->pulse_span_symbols;
	uint64_t count = support / step + (support % step != 0 ? 1u : 0u) + 1u;
	uint64_t timing_gcd = gcd_u64(period, step);
	uint64_t candidates = period / timing_gcd;

	if (count < 2 || count > QAM_MAX_MATCHED_FILTER_SAMPLES ||
	    candidates == 0 ||
	    candidates > QAM_MAX_TIMING_CANDIDATES)
		return false;
	*filter_count = (size_t)count;
	*candidate_count = (size_t)candidates;
	*timing_step = timing_gcd;
	return true;
}

static bool timing_window_is_valid(uint32_t symbols)
{
	return symbols >= QAM_MIN_TIMING_WINDOW_SYMBOLS &&
	       symbols <= QAM_MAX_TIMING_WINDOW_SYMBOLS;
}

static bool timing_sample_target(
	const struct qam_channel *channel, uint64_t period,
	size_t candidate_count, uint32_t symbols, uint32_t *samples)
{
	uint64_t step = channel->symbol_rate_numerator;
	uint64_t whole_samples = period / step;
	uint64_t remainder_units = period % step;
	uint64_t fractional_samples;
	uint64_t target;

	if (!timing_window_is_valid(symbols) || samples == NULL)
		return false;
	if (whole_samples > UINT32_MAX / symbols)
		return false;
	fractional_samples =
		((uint64_t)symbols * remainder_units + step - 1u) / step;
	target = (uint64_t)symbols * whole_samples;
	if (fractional_samples > UINT32_MAX - target ||
	    candidate_count > UINT32_MAX - target - fractional_samples)
		return false;
	target += fractional_samples + candidate_count;
	*samples = (uint32_t)target;
	return true;
}

static void build_matched_filter(
	struct qam_demodulator *demodulator)
{
	double delay = (double)demodulator->channel.pulse_span_symbols / 2.0;

	for (size_t tap = 0; tap < demodulator->matched_filter_count; ++tap) {
		double symbol_time =
			(double)tap *
				(double)demodulator->channel.symbol_rate_numerator /
			((double)demodulator->channel.sample_rate *
			 (double)demodulator->channel.symbol_rate_denominator);

		(void)root_raised_cosine(
			symbol_time - delay, demodulator->channel.rolloff,
			&demodulator->matched_coefficients[tap]);
	}
}

static void initialize_timing_candidates(
	struct qam_demodulator *demodulator)
{
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index)
		demodulator->timing[index].phase_units =
			index * demodulator->timing_step_units;
}

static void recovery_step_bounds(
	const struct qam_demodulator *demodulator,
	uint64_t *minimum, uint64_t *maximum)
{
	uint64_t nominal = demodulator->channel.symbol_rate_numerator;
	uint64_t adjustment = nominal / QAM_CLOCK_TRACKING_RANGE_DIVISOR;

	if (adjustment == 0)
		adjustment = 1;
	*minimum = nominal > adjustment ? nominal - adjustment : 1;
	*maximum = nominal + adjustment;
	if (*maximum > demodulator->symbol_period_units)
		*maximum = demodulator->symbol_period_units;
}

static bool recovery_step_is_valid(
	const struct qam_demodulator *demodulator)
{
	uint64_t minimum;
	uint64_t maximum;

	recovery_step_bounds(demodulator, &minimum, &maximum);
	return demodulator->recovery_step_units >= minimum &&
	       demodulator->recovery_step_units <= maximum;
}

static bool recovery_phase_is_valid(
	const struct qam_demodulator *demodulator)
{
	uint64_t maximum_samples =
		(uint64_t)demodulator->timing_sample_target *
		QAM_CLOCK_TRACKING_FILTER_WINDOWS;
	bool inactive = demodulator->recovery_phase_units == 0u &&
		demodulator->recovery_phase_samples == 0u &&
		demodulator->recovery_phase_direction == 0;

	if (inactive)
		return true;
	return demodulator->timing_locked &&
	       demodulator->recovery_phase_units <=
		       demodulator->symbol_period_units / 2u &&
	       demodulator->recovery_phase_samples != 0u &&
	       demodulator->recovery_phase_samples <= maximum_samples &&
	       (demodulator->recovery_phase_direction == -1 ||
		demodulator->recovery_phase_direction == 1);
}

static bool matched_history_is_valid(
	const struct qam_demodulator *demodulator)
{
	if (demodulator->matched_history_count > QAM_INTERPOLATION_HISTORY)
		return false;
	for (size_t index = 0; index < QAM_INTERPOLATION_HISTORY; ++index)
		if (!point_is_finite(&demodulator->matched_history[index]))
			return false;
	return true;
}

static bool clock_tracking_filter_is_valid(
	const struct qam_demodulator *demodulator)
{
	if (demodulator->timing_error_windows >=
		QAM_CLOCK_TRACKING_FILTER_WINDOWS ||
	    demodulator->timing_forward_windows >
		    demodulator->timing_error_windows ||
	    demodulator->timing_backward_windows >
		    demodulator->timing_error_windows ||
	    demodulator->timing_forward_windows +
		    demodulator->timing_backward_windows >
		    demodulator->timing_error_windows ||
	    (demodulator->timing_forward_error_units == 0u) !=
		    (demodulator->timing_forward_windows == 0u) ||
	    (demodulator->timing_backward_error_units == 0u) !=
		    (demodulator->timing_backward_windows == 0u))
		return false;
	if (!demodulator->timing_locked &&
	    (demodulator->timing_error_windows != 0u ||
	     demodulator->timing_forward_error_units != 0u ||
	     demodulator->timing_backward_error_units != 0u ||
	     demodulator->timing_forward_windows != 0u ||
	     demodulator->timing_backward_windows != 0u))
		return false;
	return demodulator->timing_error_windows != 0u ||
	       (demodulator->timing_forward_error_units == 0u &&
		demodulator->timing_backward_error_units == 0u &&
		demodulator->timing_forward_windows == 0u &&
		demodulator->timing_backward_windows == 0u);
}

static bool timing_reference_filter_is_valid(
	const struct qam_demodulator *demodulator)
{
	return demodulator->timing_reference_windows <
		       QAM_CLOCK_TRACKING_FILTER_WINDOWS &&
	       (demodulator->timing_locked ||
		demodulator->timing_reference_windows == 0u) &&
	       (demodulator->timing_reference_windows != 0u ||
		demodulator->timing_pending_reference_index ==
			demodulator->timing_reference_index) &&
	       (demodulator->timing_reference_windows == 0u ||
		demodulator->timing_pending_reference_index !=
			demodulator->timing_reference_index);
}

bool qam_demodulator_is_valid(
	const struct qam_demodulator *demodulator)
{
	uint64_t period = 0;
	size_t filter_count = 0;
	size_t candidate_count = 0;
	uint64_t timing_step = 0;
	uint32_t acquisition_samples = 0;
	uint32_t tracking_samples = 0;
	uint32_t expected_samples;

	if (demodulator == NULL ||
	    !channel_is_valid(&demodulator->channel, &period) ||
	    !demodulator_shape_is_valid(
		    &demodulator->channel, period, &filter_count,
		    &candidate_count, &timing_step) ||
	    !timing_window_is_valid(
		    demodulator->acquisition_window_symbols) ||
	    !timing_window_is_valid(
		    demodulator->tracking_window_symbols) ||
	    !timing_sample_target(
		    &demodulator->channel, period, candidate_count,
		    demodulator->acquisition_window_symbols,
		    &acquisition_samples) ||
	    !timing_sample_target(
		    &demodulator->channel, period, candidate_count,
		    demodulator->tracking_window_symbols,
		    &tracking_samples) ||
	    demodulator->symbol_period_units != period ||
	    demodulator->timing_step_units != timing_step ||
	    !recovery_step_is_valid(demodulator) ||
	    !recovery_phase_is_valid(demodulator) ||
	    demodulator->matched_filter_count != filter_count ||
	    demodulator->timing_candidate_count != candidate_count ||
	    demodulator->timing_reference_index >= candidate_count ||
	    demodulator->timing_rate_reference_index >= candidate_count ||
	    demodulator->timing_pending_reference_index >= candidate_count ||
	    demodulator->history_index >= filter_count ||
	    demodulator->history_count > filter_count ||
	    demodulator->carrier_phase_step !=
		    phase_step_u32(
			    demodulator->channel.sample_rate,
			    demodulator->channel.carrier_frequency_numerator,
			    demodulator->channel.carrier_frequency_denominator) ||
	    !isfinite(demodulator->line_power) ||
	    demodulator->line_power < 0.0 ||
	    !isfinite(demodulator->symbol_power) ||
	    demodulator->symbol_power < 0.0 ||
	    !isfinite(demodulator->timing_power_hysteresis) ||
	    demodulator->timing_power_hysteresis < 1.0 ||
	    !matched_history_is_valid(demodulator) ||
	    !clock_tracking_filter_is_valid(demodulator) ||
	    !timing_reference_filter_is_valid(demodulator) ||
	    !isfinite(demodulator->recovery_fraction) ||
	    (demodulator->recovery_pending &&
	     (!demodulator->timing_locked ||
	      demodulator->matched_history_count < QAM_INTERPOLATION_HISTORY ||
	      demodulator->recovery_fraction < 0.0 ||
	      demodulator->recovery_fraction > 1.0)) ||
	    (demodulator->timing_locked &&
	     !demodulator->acquisition_started) ||
	    (!demodulator->acquisition_started &&
	     demodulator->timing_sample_count != 0) ||
	    (demodulator->timing_locked &&
	     (demodulator->locked_phase_units >= period ||
	      demodulator->matched_history_count == 0u)))
		return false;
	expected_samples = demodulator->timing_locked ?
			   tracking_samples : acquisition_samples;
	if (demodulator->timing_sample_target != expected_samples ||
	    demodulator->timing_sample_count >=
		    demodulator->timing_sample_target)
		return false;
	for (size_t index = 0; index < candidate_count; ++index) {
		const struct qam_timing_candidate *candidate =
			&demodulator->timing[index];

		if (candidate->phase_units >= period ||
		    !isfinite(candidate->symbol_power_sum) ||
		    candidate->symbol_power_sum < 0.0 ||
		    (!demodulator->acquisition_started &&
		     (candidate->symbol_count != 0 ||
		      candidate->symbol_power_sum != 0.0)))
			return false;
	}
	return true;
}

bool qam_demodulator_has_timing_lock(
	const struct qam_demodulator *demodulator)
{
	return qam_demodulator_is_valid(demodulator) &&
	       demodulator->timing_locked;
}

enum qam_result qam_demodulator_init(
	struct qam_demodulator *demodulator,
	const struct qam_channel *channel)
{
	return qam_demodulator_init_with_timing_windows(
		demodulator, channel, QAM_DEFAULT_TIMING_WINDOW_SYMBOLS,
		QAM_DEFAULT_TIMING_WINDOW_SYMBOLS);
}

enum qam_result qam_demodulator_init_with_timing_windows(
	struct qam_demodulator *demodulator,
	const struct qam_channel *channel,
	uint32_t acquisition_symbols, uint32_t tracking_symbols)
{
	uint64_t period = 0;
	size_t filter_count = 0;
	size_t candidate_count = 0;
	uint64_t timing_step = 0;
	uint32_t acquisition_samples = 0;
	uint32_t tracking_samples = 0;
	struct qam_channel settings;

	if (demodulator == NULL || !channel_is_valid(channel, &period) ||
	    !demodulator_shape_is_valid(
		    channel, period, &filter_count, &candidate_count,
		    &timing_step) ||
	    !timing_window_is_valid(acquisition_symbols) ||
	    !timing_window_is_valid(tracking_symbols) ||
	    !timing_sample_target(
		    channel, period, candidate_count, acquisition_symbols,
		    &acquisition_samples) ||
	    !timing_sample_target(
		    channel, period, candidate_count, tracking_symbols,
		    &tracking_samples))
		return QAM_INVALID_ARGUMENT;
	settings = *channel;
	channel = &settings;
	memset(demodulator, 0, sizeof(*demodulator));
	demodulator->channel = *channel;
	demodulator->symbol_period_units = period;
	demodulator->timing_step_units = timing_step;
	demodulator->recovery_step_units = channel->symbol_rate_numerator;
	demodulator->carrier_phase_step = phase_step_u32(
		channel->sample_rate, channel->carrier_frequency_numerator,
		channel->carrier_frequency_denominator);
	demodulator->matched_filter_count = filter_count;
	demodulator->timing_candidate_count = candidate_count;
	demodulator->timing_sample_target = acquisition_samples;
	demodulator->acquisition_window_symbols = acquisition_symbols;
	demodulator->tracking_window_symbols = tracking_symbols;
	demodulator->timing_power_hysteresis =
		QAM_DEFAULT_TIMING_POWER_HYSTERESIS;
	build_matched_filter(demodulator);
	initialize_timing_candidates(demodulator);
	return QAM_OK;
}

enum qam_result qam_demodulator_set_timing_power_hysteresis(
	struct qam_demodulator *demodulator, double ratio)
{
	if (!qam_demodulator_is_valid(demodulator) || !isfinite(ratio) ||
	    ratio < 1.0)
		return QAM_INVALID_ARGUMENT;
	demodulator->timing_power_hysteresis = ratio;
	return QAM_OK;
}

enum qam_result qam_demodulator_reset(
	struct qam_demodulator *demodulator)
{
	uint32_t acquisition_symbols;
	uint32_t tracking_symbols;
	double timing_power_hysteresis;
	enum qam_result result;

	if (!qam_demodulator_is_valid(demodulator))
		return QAM_INVALID_ARGUMENT;
	acquisition_symbols = demodulator->acquisition_window_symbols;
	tracking_symbols = demodulator->tracking_window_symbols;
	timing_power_hysteresis = demodulator->timing_power_hysteresis;
	result = qam_demodulator_init_with_timing_windows(
		demodulator, &demodulator->channel,
		acquisition_symbols, tracking_symbols);
	if (result != QAM_OK)
		return result;
	return qam_demodulator_set_timing_power_hysteresis(
		demodulator, timing_power_hysteresis);
}

static void mix_sample(
	struct qam_demodulator *demodulator, int16_t sample)
{
	double radians = phase_radians_u32(demodulator->carrier_phase);
	double in_phase = sample * cos(radians);
	double quadrature = -sample * sin(radians);

	demodulator->in_phase_history[demodulator->history_index] = in_phase;
	demodulator->quadrature_history[demodulator->history_index] =
		quadrature;
	demodulator->history_index =
		(demodulator->history_index + 1) %
		demodulator->matched_filter_count;
	if (demodulator->history_count < demodulator->matched_filter_count)
		demodulator->history_count++;
	demodulator->carrier_phase += demodulator->carrier_phase_step;
	demodulator->line_power +=
		((double)sample * sample - demodulator->line_power) / 256.0;
}

static void matched_filter_sample(
	const struct qam_demodulator *demodulator,
	double *in_phase, double *quadrature)
{
	size_t index = demodulator->history_index;

	*in_phase = 0.0;
	*quadrature = 0.0;
	for (size_t tap = 0; tap < demodulator->matched_filter_count; ++tap) {
		double coefficient = demodulator->matched_coefficients[tap];

		index = index != 0 ? index - 1 :
			demodulator->matched_filter_count - 1;
		*in_phase += demodulator->in_phase_history[index] * coefficient;
		*quadrature +=
			demodulator->quadrature_history[index] * coefficient;
	}
}

static double interpolate(
	double previous, double current, double fraction)
{
	return previous + (current - previous) * fraction;
}

static void observe_timing_candidate(
	struct qam_timing_candidate *candidate,
	double in_phase, double quadrature)
{
	candidate->symbol_power_sum +=
		in_phase * in_phase + quadrature * quadrature;
	candidate->symbol_count++;
}

static void reset_timing_scores(
	struct qam_demodulator *demodulator)
{
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct qam_timing_candidate *candidate =
			&demodulator->timing[index];

		candidate->symbol_power_sum = 0.0;
		candidate->symbol_count = 0;
	}
	demodulator->timing_sample_count = 0;
}

static uint64_t timing_phase_distance(
	uint64_t left, uint64_t right, uint64_t period)
{
	uint64_t forward = left >= right ? left - right : period - (right - left);
	uint64_t backward = right >= left ? right - left : period - (left - right);

	return forward < backward ? forward : backward;
}

static int timing_phase_direction(
	uint64_t current, uint64_t target, uint64_t period)
{
	uint64_t forward = target >= current ? target - current :
					       period - (current - target);
	uint64_t backward = current >= target ? current - target :
					       period - (target - current);

	if (forward == 0 || forward == backward)
		return 0;
	return forward < backward ? 1 : -1;
}

static void reset_clock_tracking_filter(
	struct qam_demodulator *demodulator)
{
	demodulator->timing_forward_error_units = 0;
	demodulator->timing_backward_error_units = 0;
	demodulator->timing_error_windows = 0;
	demodulator->timing_forward_windows = 0;
	demodulator->timing_backward_windows = 0;
}

static void reset_timing_reference_filter(
	struct qam_demodulator *demodulator)
{
	demodulator->timing_pending_reference_index =
		demodulator->timing_reference_index;
	demodulator->timing_reference_windows = 0;
}

static void reset_recovery_phase(struct qam_demodulator *demodulator)
{
	demodulator->recovery_phase_units = 0;
	demodulator->recovery_phase_samples = 0;
	demodulator->recovery_phase_direction = 0;
}

static bool track_recovery_step(
	struct qam_demodulator *demodulator,
	uint64_t reference_phase, uint64_t target_phase)
{
	uint64_t minimum;
	uint64_t maximum;
	uint64_t distance = timing_phase_distance(
		reference_phase, target_phase, demodulator->symbol_period_units);
	uint64_t samples;
	uint64_t adjustment;
	uint64_t remainder;
	uint64_t nominal = demodulator->channel.symbol_rate_numerator;
	uint8_t supporting_windows;
	int direction = timing_phase_direction(
		reference_phase, target_phase,
		demodulator->symbol_period_units);

	/* The reference candidate has advanced at the nominal clock since the
	 * previous window. Its displacement from the new best candidate is
	 * therefore the accumulated symbol-rate error over this window. Average
	 * several windows so adjacent wrap candidates selected by structured data
	 * do not make an exact recovered clock oscillate. */
	if ((direction > 0 &&
	     UINT64_MAX - demodulator->timing_forward_error_units < distance) ||
	    (direction < 0 &&
	     UINT64_MAX - demodulator->timing_backward_error_units < distance)) {
		reset_clock_tracking_filter(demodulator);
		demodulator->recovery_step_units = nominal;
		return false;
	}
	if (direction > 0) {
		demodulator->timing_forward_error_units += distance;
		demodulator->timing_forward_windows++;
	} else if (direction < 0) {
		demodulator->timing_backward_error_units += distance;
		demodulator->timing_backward_windows++;
	}
	demodulator->timing_error_windows++;
	if (demodulator->timing_error_windows <
	    QAM_CLOCK_TRACKING_FILTER_WINDOWS)
		return false;
	samples = (uint64_t)demodulator->timing_sample_target *
		demodulator->timing_error_windows;
	if (demodulator->timing_forward_error_units >=
	    demodulator->timing_backward_error_units) {
		distance = demodulator->timing_forward_error_units -
			demodulator->timing_backward_error_units;
		direction = distance == 0u ? 0 : 1;
		supporting_windows = demodulator->timing_forward_windows;
	} else {
		distance = demodulator->timing_backward_error_units -
			demodulator->timing_forward_error_units;
		direction = -1;
		supporting_windows = demodulator->timing_backward_windows;
	}
	/* One signal transition can move the energy maximum by several timing
	 * candidates. Require one direction in a majority of windows with no
	 * reversal so a transition or alternating choice cannot become a
	 * persistent recovered-clock error. */
	if (supporting_windows <
		    (QAM_CLOCK_TRACKING_FILTER_WINDOWS + 1u) / 2u ||
	    (demodulator->timing_forward_windows != 0u &&
	     demodulator->timing_backward_windows != 0u)) {
		distance = 0;
		direction = 0;
	}
	reset_clock_tracking_filter(demodulator);
	adjustment = distance / samples;
	remainder = distance % samples;
	if (remainder >= (samples + 1u) / 2u)
		adjustment++;
	recovery_step_bounds(demodulator, &minimum, &maximum);
	if (direction > 0) {
		if (adjustment > maximum - nominal)
			adjustment = maximum - nominal;
		demodulator->recovery_step_units = nominal + adjustment;
	} else if (direction < 0) {
		if (adjustment > nominal - minimum)
			adjustment = nominal - minimum;
		demodulator->recovery_step_units = nominal - adjustment;
	} else {
		demodulator->recovery_step_units = nominal;
	}
	return direction != 0;
}

static void start_recovery_phase(
	struct qam_demodulator *demodulator, uint64_t target_phase)
{
	uint64_t distance;
	uint64_t samples;
	uint64_t maximum_step;
	uint64_t available_step;
	int direction;

	distance = timing_phase_distance(
		demodulator->locked_phase_units, target_phase,
		demodulator->symbol_period_units);
	direction = timing_phase_direction(
		demodulator->locked_phase_units, target_phase,
		demodulator->symbol_period_units);
	samples = (uint64_t)demodulator->timing_sample_target *
		QAM_CLOCK_TRACKING_FILTER_WINDOWS;
	reset_recovery_phase(demodulator);
	if (direction != 0 && samples <= UINT32_MAX) {
		demodulator->recovery_phase_direction = (int8_t)direction;
		demodulator->recovery_phase_units = distance;
		maximum_step =
			(demodulator->recovery_phase_units + samples - 1u) /
			samples;
		available_step = demodulator->recovery_phase_direction > 0 ?
			demodulator->symbol_period_units -
				demodulator->recovery_step_units :
			demodulator->recovery_step_units - 1u;
		if (maximum_step <= available_step)
			demodulator->recovery_phase_samples = (uint32_t)samples;
		else
			reset_recovery_phase(demodulator);
	}
}

static void observe_timing_reference(
	struct qam_demodulator *demodulator, size_t best_index)
{
	if (best_index == demodulator->timing_reference_index) {
		reset_timing_reference_filter(demodulator);
		return;
	}
	if (best_index != demodulator->timing_pending_reference_index) {
		demodulator->timing_pending_reference_index = best_index;
		demodulator->timing_reference_windows = 1;
		return;
	}
	demodulator->timing_reference_windows++;
	if (demodulator->timing_reference_windows <
	    QAM_CLOCK_TRACKING_FILTER_WINDOWS)
		return;
	demodulator->timing_reference_index = best_index;
	start_recovery_phase(
		demodulator, demodulator->timing[best_index].phase_units);
	reset_timing_reference_filter(demodulator);
}

static void select_timing_candidate(
	struct qam_demodulator *demodulator, double current_power)
{
	struct qam_timing_candidate *best = NULL;
	struct qam_timing_candidate *reference = NULL;
	double best_power = 0.0;
	double reference_power = 0.0;
	size_t best_index = 0;
	uint32_t active_window = demodulator->timing_locked ?
				 demodulator->tracking_window_symbols :
				 demodulator->acquisition_window_symbols;
	uint32_t minimum_symbols =
		(active_window * 3u + 3u) / 4u;
	bool acquiring = !demodulator->timing_locked;
	bool rate_changed = false;
	uint32_t next_sample_target = demodulator->timing_sample_target;

	/* Structured training can favor a neighboring phase in a short energy
	 * comparison. Require observations across three quarters of the window. */
	if (acquiring && !timing_sample_target(
				 &demodulator->channel,
				 demodulator->symbol_period_units,
				 demodulator->timing_candidate_count,
				 demodulator->tracking_window_symbols,
				 &next_sample_target)) {
		reset_timing_scores(demodulator);
		return;
	}

	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct qam_timing_candidate *candidate =
			&demodulator->timing[index];
		double average;

		if (candidate->symbol_count < minimum_symbols)
			continue;
		average = candidate->symbol_power_sum / candidate->symbol_count;
		if (average > best_power) {
			best = candidate;
			best_power = average;
			best_index = index;
		}
		if (demodulator->timing_locked &&
		    index == demodulator->timing_reference_index) {
			reference = candidate;
			reference_power = average;
		}
	}
	/* Companded media can decode an idle octet to a small nonzero sample, and
	 * a decaying carrier tail can dominate a short acquisition average after
	 * the line enters silence. Require both average and final carrier energy. */
	if (best == NULL || best_power <= QAM_MIN_ACQUISITION_POWER ||
	    (acquiring && current_power <= QAM_MIN_ACQUISITION_POWER)) {
		reset_timing_scores(demodulator);
		if (acquiring)
			demodulator->acquisition_started = false;
		else {
			reset_clock_tracking_filter(demodulator);
			reset_timing_reference_filter(demodulator);
			reset_recovery_phase(demodulator);
		}
		return;
	}
	if (reference != NULL && best != reference &&
	    best_power <=
		    reference_power * demodulator->timing_power_hysteresis) {
		best = reference;
		best_power = reference_power;
		best_index = demodulator->timing_reference_index;
	}
	if (demodulator->timing_locked) {
		rate_changed = track_recovery_step(
			demodulator,
			demodulator
				->timing[
					demodulator->timing_rate_reference_index]
				.phase_units,
			best->phase_units);
		if (rate_changed)
			start_recovery_phase(demodulator, best->phase_units);
		demodulator->timing_rate_reference_index = best_index;
		observe_timing_reference(demodulator, best_index);
	} else {
		demodulator->locked_phase_units = best->phase_units;
		demodulator->timing_reference_index = best_index;
		demodulator->timing_rate_reference_index = best_index;
		reset_timing_reference_filter(demodulator);
	}
	reset_timing_scores(demodulator);
	demodulator->symbol_power = best_power;
	demodulator->timing_locked = true;
	demodulator->timing_sample_target = next_sample_target;
}

enum qam_result qam_demodulator_reacquire(
	struct qam_demodulator *demodulator, uint32_t acquisition_symbols)
{
	struct qam_demodulator next;
	uint32_t target;

	if (!qam_demodulator_is_valid(demodulator) ||
	    !timing_sample_target(&demodulator->channel,
		    demodulator->symbol_period_units,
		    demodulator->timing_candidate_count, acquisition_symbols, &target))
		return QAM_INVALID_ARGUMENT;
	next = *demodulator;
	next.acquisition_window_symbols = acquisition_symbols;
	next.timing_sample_target = target;
	next.timing_locked = false;
	next.acquisition_started = false;
	next.recovery_pending = false;
	reset_timing_scores(&next);
	reset_clock_tracking_filter(&next);
	reset_timing_reference_filter(&next);
	reset_recovery_phase(&next);
	initialize_timing_candidates(&next);
	if (!qam_demodulator_is_valid(&next))
		return QAM_INVALID_ARGUMENT;
	*demodulator = next;
	return QAM_OK;
}

static void acquire_symbol_timing(
	struct qam_demodulator *demodulator,
	double in_phase, double quadrature)
{
	bool observe = demodulator->timing_locked ||
		       demodulator->acquisition_started;
	const struct qam_point *previous =
		demodulator->matched_history_count == 0u ? NULL :
		&demodulator->matched_history[
			demodulator->matched_history_count - 1u];

	if (!observe && in_phase * in_phase + quadrature * quadrature >
			QAM_MIN_ACQUISITION_POWER) {
		demodulator->acquisition_started = true;
		observe = true;
	}
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct qam_timing_candidate *candidate =
			&demodulator->timing[index];
		double fraction = 0.0;

		if (phase_advance_u64(
			    &candidate->phase_units,
			    demodulator->symbol_period_units,
			    demodulator->channel.symbol_rate_numerator,
			    &fraction) &&
		    previous != NULL && observe)
			observe_timing_candidate(
				candidate,
				interpolate(
					previous->in_phase, in_phase,
					fraction),
				interpolate(
					previous->quadrature,
					quadrature, fraction));
	}
	if (!observe)
		return;
	demodulator->timing_sample_count++;
	if (demodulator->timing_sample_count ==
	    demodulator->timing_sample_target)
		select_timing_candidate(
			demodulator,
			in_phase * in_phase + quadrature * quadrature);
}

static enum qam_result recover_symbol(
	struct qam_demodulator *demodulator,
	double in_phase, double quadrature, struct qam_point *point,
	bool *symbol_ready)
{
	double fraction = 0.0;
	bool ready = demodulator->recovery_pending;
	uint64_t recovery_step = demodulator->recovery_step_units;

	if (ready) {
		double in_phase_samples[4] = {
			demodulator->matched_history[0].in_phase,
			demodulator->matched_history[1].in_phase,
			demodulator->matched_history[2].in_phase,
			in_phase,
		};
		double quadrature_samples[4] = {
			demodulator->matched_history[0].quadrature,
			demodulator->matched_history[1].quadrature,
			demodulator->matched_history[2].quadrature,
			quadrature,
		};
		struct qam_point recovered;

		if (fractional_delay_interpolate_cubic(
			    in_phase_samples, demodulator->recovery_fraction,
			    &recovered.in_phase) != FRACTIONAL_DELAY_OK ||
		    fractional_delay_interpolate_cubic(
			    quadrature_samples, demodulator->recovery_fraction,
			    &recovered.quadrature) != FRACTIONAL_DELAY_OK)
			return QAM_INVALID_ARGUMENT;
		*point = recovered;
		demodulator->symbol_power +=
			(point->in_phase * point->in_phase +
			 point->quadrature * point->quadrature -
			 demodulator->symbol_power) /
			64.0;
		demodulator->recovery_pending = false;
	}
	if (demodulator->recovery_phase_samples != 0u) {
		uint64_t phase_step =
			demodulator->recovery_phase_units /
			demodulator->recovery_phase_samples;
		uint64_t remainder =
			demodulator->recovery_phase_units %
			demodulator->recovery_phase_samples;

		if (remainder >=
		    (demodulator->recovery_phase_samples + 1u) / 2u)
			phase_step++;
		if ((demodulator->recovery_phase_direction > 0 &&
		     phase_step > demodulator->symbol_period_units -
				  recovery_step) ||
		    (demodulator->recovery_phase_direction < 0 &&
		     phase_step >= recovery_step))
			return QAM_INVALID_ARGUMENT;
		if (demodulator->recovery_phase_direction > 0)
			recovery_step += phase_step;
		else
			recovery_step -= phase_step;
		demodulator->recovery_phase_units -= phase_step;
		demodulator->recovery_phase_samples--;
		if (demodulator->recovery_phase_units == 0u)
			reset_recovery_phase(demodulator);
	}
	if (phase_advance_u64(
		    &demodulator->locked_phase_units,
		    demodulator->symbol_period_units,
		    recovery_step, &fraction)) {
		demodulator->recovery_fraction = fraction;
		demodulator->recovery_pending = true;
	}
	*symbol_ready = ready;
	return QAM_OK;
}

static void remember_matched_sample(
	struct qam_demodulator *demodulator,
	double in_phase, double quadrature)
{
	struct qam_point matched = {
		.in_phase = in_phase,
		.quadrature = quadrature,
	};

	if (demodulator->matched_history_count < QAM_INTERPOLATION_HISTORY) {
		demodulator->matched_history[
			demodulator->matched_history_count++] = matched;
		return;
	}
	for (size_t index = 1; index < QAM_INTERPOLATION_HISTORY; ++index)
		demodulator->matched_history[index - 1u] =
			demodulator->matched_history[index];
	demodulator->matched_history[
		QAM_INTERPOLATION_HISTORY - 1u] = matched;
}

enum qam_result qam_demodulator_sample(
	struct qam_demodulator *demodulator, int16_t sample,
	struct qam_point *point, bool *symbol_ready)
{
	double in_phase;
	double quadrature;

	if (!qam_demodulator_is_valid(demodulator) || point == NULL ||
	    symbol_ready == NULL)
		return QAM_INVALID_ARGUMENT;
	*point = (struct qam_point){ 0 };
	*symbol_ready = false;
	mix_sample(demodulator, sample);
	if (demodulator->history_count < demodulator->matched_filter_count)
		return QAM_OK;
	matched_filter_sample(demodulator, &in_phase, &quadrature);
	if (!demodulator->timing_locked) {
		acquire_symbol_timing(demodulator, in_phase, quadrature);
		remember_matched_sample(demodulator, in_phase, quadrature);
		return QAM_OK;
	}
	if (recover_symbol(
		    demodulator, in_phase, quadrature, point,
		    symbol_ready) != QAM_OK)
		return QAM_INVALID_ARGUMENT;
	acquire_symbol_timing(demodulator, in_phase, quadrature);
	remember_matched_sample(demodulator, in_phase, quadrature);
	return QAM_OK;
}

struct qam_measurement qam_demodulator_measure(
	const struct qam_demodulator *demodulator)
{
	if (!qam_demodulator_is_valid(demodulator))
		return (struct qam_measurement){ 0 };
	return (struct qam_measurement){
		.line_power = demodulator->line_power,
		.symbol_power = demodulator->symbol_power,
		.timing_locked = demodulator->timing_locked,
	};
}
