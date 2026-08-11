// SPDX-License-Identifier: BSD-3-Clause

#include "qam.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	QAM_NORMALIZATION_PHASES = 512,
	/* Structured training can bias a short energy comparison toward a
	 * neighboring symbol phase; average across the modem training interval. */
	QAM_ACQUISITION_SYMBOLS = 256,
	QAM_MIN_ACQUISITION_SYMBOLS = 192,
	/* Bound the recovered clock to one percent around the configured rate,
	 * retaining one phase unit of range for very small exact numerators. */
	QAM_CLOCK_TRACKING_RANGE_DIVISOR = 100,
};

static const double QAM_PI = 3.1415926535897932384626433832795;
static const double QAM_TWO_PI = 6.283185307179586476925286766559;
static const double QAM_PHASE_SCALE = 4294967296.0;
static const double QAM_SINGULARITY_EPSILON = 1e-10;
static const double QAM_POINT_EPSILON = 1e-12;
/* Unit-average-power square training constellations reach 9/5 at a corner. */
static const double QAM_MAX_POINT_POWER = 9.0 / 5.0;
static const double QAM_MIN_ACQUISITION_POWER = 64.0;
static const double QAM_TIMING_POWER_HYSTERESIS = 1.005;

static bool point_is_finite(const struct qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool transmit_point_is_valid(
	const struct qam_point *point, double maximum_point_power)
{
	double power;

	if (!point_is_finite(point) || !isfinite(maximum_point_power) ||
	    maximum_point_power <= 0.0)
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) &&
	       (power <= maximum_point_power ||
		power / maximum_point_power <= 1.0 + QAM_POINT_EPSILON);
}

static uint32_t phase_step(
	uint32_t sample_rate, uint32_t frequency_numerator,
	uint32_t frequency_denominator)
{
	uint64_t numerator;
	uint64_t denominator;
	uint64_t quotient;
	uint64_t remainder;

	if (sample_rate == 0 || frequency_numerator == 0 ||
	    frequency_denominator == 0)
		return 0;
	numerator = (uint64_t)frequency_numerator << 32;
	denominator = (uint64_t)sample_rate * frequency_denominator;
	quotient = numerator / denominator;
	remainder = numerator % denominator;
	if (remainder >= (denominator + 1u) / 2u)
		quotient++;
	return (uint32_t)quotient;
}

enum qam_result qam_nearest_quarter_turn(
	const struct qam_point *previous,
	const struct qam_point *current, uint8_t *quarter_turns)
{
	double cross;
	double dot;
	int turns;

	if (!point_is_finite(previous) || !point_is_finite(current) ||
	    quarter_turns == NULL)
		return QAM_INVALID_ARGUMENT;
	dot = previous->in_phase * current->in_phase +
	      previous->quadrature * current->quadrature;
	cross = previous->in_phase * current->quadrature -
		previous->quadrature * current->in_phase;
	if (!isfinite(dot) || !isfinite(cross))
		return QAM_INVALID_ARGUMENT;
	turns = (int)lround(atan2(cross, dot) / (QAM_PI / 2.0));
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
	    phase_step(channel->sample_rate,
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
		    phase_step(modulator->channel.sample_rate,
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
		    !transmit_point_is_valid(
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

	if (modulator == NULL || amplitude <= 0 ||
	    !isfinite(maximum_point_power) || maximum_point_power <= 0.0 ||
	    !channel_is_valid(channel, &period))
		return QAM_INVALID_ARGUMENT;
	normalization = pulse_normalization(channel);
	if (!isfinite(normalization) || normalization <= 0.0)
		return QAM_INVALID_ARGUMENT;
	memset(modulator, 0, sizeof(*modulator));
	modulator->channel = *channel;
	modulator->symbol_period_units = period;
	modulator->carrier_phase_step = phase_step(
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
	if (step >= modulator->symbol_period_units -
			    modulator->symbol_phase_units) {
		modulator->symbol_phase_units =
			step - (modulator->symbol_period_units -
				modulator->symbol_phase_units);
		modulator->needs_symbol = true;
	} else {
		modulator->symbol_phase_units += step;
	}
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
	    !transmit_point_is_valid(point, modulator->maximum_point_power) ||
	    sample == NULL || symbol_accepted == NULL ||
	    (modulator->needs_symbol &&
	     modulator->active_count == QAM_MAX_ACTIVE_SYMBOLS))
		return QAM_INVALID_ARGUMENT;
	*symbol_accepted = modulator->needs_symbol;
	if (modulator->needs_symbol)
		append_symbol(modulator, point);
	baseband_sample(modulator, &in_phase, &quadrature);
	carrier_phase = (double)modulator->carrier_phase *
			(QAM_TWO_PI / QAM_PHASE_SCALE);
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
	long rounded;
	enum qam_result result;

	if (sample == NULL || symbol_accepted == NULL)
		return QAM_INVALID_ARGUMENT;
	result = qam_modulator_sample_f64(
		modulator, point, &waveform, &accepted);
	if (result != QAM_OK)
		return result;
	rounded = lround(waveform * modulator->amplitude);
	if (rounded > modulator->amplitude)
		rounded = modulator->amplitude;
	if (rounded < -modulator->amplitude)
		rounded = -modulator->amplitude;
	*sample = (int16_t)rounded;
	*symbol_accepted = accepted;
	return QAM_OK;
}

static uint64_t greatest_common_divisor(uint64_t left, uint64_t right)
{
	while (right != 0) {
		uint64_t remainder = left % right;

		left = right;
		right = remainder;
	}
	return left;
}

static bool demodulator_shape_is_valid(
	const struct qam_channel *channel, uint64_t period,
	size_t *filter_count, size_t *candidate_count,
	uint64_t *timing_step, uint32_t *acquisition_samples)
{
	uint64_t step = channel->symbol_rate_numerator;
	uint64_t support = period * channel->pulse_span_symbols;
	uint64_t count = support / step + (support % step != 0 ? 1u : 0u) + 1u;
	uint64_t timing_gcd = greatest_common_divisor(period, step);
	uint64_t candidates = period / timing_gcd;
	uint64_t whole_samples = period / step;
	uint64_t remainder_units = period % step;
	uint64_t target = QAM_ACQUISITION_SYMBOLS * whole_samples +
		(QAM_ACQUISITION_SYMBOLS * remainder_units + step - 1u) /
			step +
		candidates;

	if (count < 2 || count > QAM_MAX_MATCHED_FILTER_SAMPLES ||
	    candidates == 0 ||
	    candidates > QAM_MAX_TIMING_CANDIDATES ||
	    target > UINT32_MAX)
		return false;
	*filter_count = (size_t)count;
	*candidate_count = (size_t)candidates;
	*timing_step = timing_gcd;
	*acquisition_samples = (uint32_t)target;
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

bool qam_demodulator_is_valid(
	const struct qam_demodulator *demodulator)
{
	uint64_t period = 0;
	size_t filter_count = 0;
	size_t candidate_count = 0;
	uint64_t timing_step = 0;
	uint32_t acquisition_samples = 0;

	if (demodulator == NULL ||
	    !channel_is_valid(&demodulator->channel, &period) ||
	    !demodulator_shape_is_valid(
		    &demodulator->channel, period, &filter_count,
		    &candidate_count, &timing_step, &acquisition_samples) ||
	    demodulator->symbol_period_units != period ||
	    demodulator->timing_step_units != timing_step ||
	    !recovery_step_is_valid(demodulator) ||
	    demodulator->matched_filter_count != filter_count ||
	    demodulator->timing_candidate_count != candidate_count ||
	    demodulator->timing_reference_index >= candidate_count ||
	    demodulator->acquisition_sample_target != acquisition_samples ||
	    demodulator->acquisition_sample_count >= acquisition_samples ||
	    demodulator->history_index >= filter_count ||
	    demodulator->history_count > filter_count ||
	    demodulator->carrier_phase_step !=
		    phase_step(demodulator->channel.sample_rate,
			       demodulator->channel.carrier_frequency_numerator,
			       demodulator->channel.carrier_frequency_denominator) ||
	    !isfinite(demodulator->line_power) ||
	    demodulator->line_power < 0.0 ||
	    !isfinite(demodulator->symbol_power) ||
	    demodulator->symbol_power < 0.0 ||
	    !isfinite(demodulator->previous_in_phase) ||
	    !isfinite(demodulator->previous_quadrature) ||
	    (demodulator->timing_locked &&
	     (demodulator->locked_phase_units >= period ||
	      !demodulator->has_previous_matched_sample)))
		return false;
	for (size_t index = 0; index < candidate_count; ++index) {
		const struct qam_timing_candidate *candidate =
			&demodulator->timing[index];

		if (candidate->phase_units >= period ||
		    !isfinite(candidate->symbol_power_sum) ||
		    candidate->symbol_power_sum < 0.0)
			return false;
	}
	return true;
}

enum qam_result qam_demodulator_init(
	struct qam_demodulator *demodulator,
	const struct qam_channel *channel)
{
	uint64_t period = 0;
	size_t filter_count = 0;
	size_t candidate_count = 0;
	uint64_t timing_step = 0;
	uint32_t acquisition_samples = 0;

	if (demodulator == NULL || !channel_is_valid(channel, &period) ||
	    !demodulator_shape_is_valid(
		    channel, period, &filter_count, &candidate_count,
		    &timing_step, &acquisition_samples))
		return QAM_INVALID_ARGUMENT;
	memset(demodulator, 0, sizeof(*demodulator));
	demodulator->channel = *channel;
	demodulator->symbol_period_units = period;
	demodulator->timing_step_units = timing_step;
	demodulator->recovery_step_units = channel->symbol_rate_numerator;
	demodulator->carrier_phase_step = phase_step(
		channel->sample_rate, channel->carrier_frequency_numerator,
		channel->carrier_frequency_denominator);
	demodulator->matched_filter_count = filter_count;
	demodulator->timing_candidate_count = candidate_count;
	demodulator->acquisition_sample_target = acquisition_samples;
	build_matched_filter(demodulator);
	initialize_timing_candidates(demodulator);
	return QAM_OK;
}

enum qam_result qam_demodulator_reset(
	struct qam_demodulator *demodulator)
{
	struct qam_channel channel;

	if (!qam_demodulator_is_valid(demodulator))
		return QAM_INVALID_ARGUMENT;
	channel = demodulator->channel;
	return qam_demodulator_init(demodulator, &channel);
}

static void mix_sample(
	struct qam_demodulator *demodulator, int16_t sample)
{
	double radians = (double)demodulator->carrier_phase *
			 (QAM_TWO_PI / QAM_PHASE_SCALE);
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
	*in_phase = 0.0;
	*quadrature = 0.0;
	for (size_t tap = 0; tap < demodulator->matched_filter_count; ++tap) {
		size_t index =
			(demodulator->history_index +
			 demodulator->matched_filter_count - 1 - tap) %
			demodulator->matched_filter_count;
		double coefficient = demodulator->matched_coefficients[tap];

		*in_phase += demodulator->in_phase_history[index] * coefficient;
		*quadrature +=
			demodulator->quadrature_history[index] * coefficient;
	}
}

static bool advance_symbol_phase(
	uint64_t *phase, uint64_t period, uint64_t step,
	double *crossing_fraction)
{
	uint64_t remaining = period - *phase;

	if (step < remaining) {
		*phase += step;
		return false;
	}
	*phase = step - remaining;
	if (crossing_fraction != NULL)
		*crossing_fraction = (double)remaining / (double)step;
	return true;
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
	demodulator->acquisition_sample_count = 0;
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

static void track_recovery_step(
	struct qam_demodulator *demodulator,
	uint64_t reference_phase, uint64_t target_phase)
{
	uint64_t minimum;
	uint64_t maximum;
	uint64_t distance = timing_phase_distance(
		reference_phase, target_phase, demodulator->symbol_period_units);
	uint64_t samples = demodulator->acquisition_sample_target;
	uint64_t adjustment = distance / samples;
	uint64_t remainder = distance % samples;
	uint64_t nominal = demodulator->channel.symbol_rate_numerator;
	int direction = timing_phase_direction(
		reference_phase, target_phase,
		demodulator->symbol_period_units);

	/* The reference candidate has advanced at the nominal clock since the
	 * previous window. Its displacement from the new best candidate is
	 * therefore the accumulated symbol-rate error over this window. */
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
}

static void select_timing_candidate(
	struct qam_demodulator *demodulator)
{
	struct qam_timing_candidate *best = NULL;
	struct qam_timing_candidate *reference = NULL;
	double best_power = 0.0;
	double reference_power = 0.0;
	size_t best_index = 0;

	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct qam_timing_candidate *candidate =
			&demodulator->timing[index];
		double average;

		if (candidate->symbol_count < QAM_MIN_ACQUISITION_SYMBOLS)
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
	/* Companded media can decode an idle octet to a small nonzero sample.
	 * Do not mistake that codec baseline for a carrier. */
	if (best == NULL || best_power <= QAM_MIN_ACQUISITION_POWER) {
		reset_timing_scores(demodulator);
		return;
	}
	if (reference != NULL && best != reference &&
	    best_power <= reference_power * QAM_TIMING_POWER_HYSTERESIS) {
		best = reference;
		best_power = reference_power;
		best_index = demodulator->timing_reference_index;
	}
	if (demodulator->timing_locked) {
		track_recovery_step(
			demodulator,
			demodulator
				->timing[demodulator->timing_reference_index]
				.phase_units,
			best->phase_units);
	} else {
		demodulator->locked_phase_units = best->phase_units;
	}
	demodulator->timing_reference_index = best_index;
	reset_timing_scores(demodulator);
	demodulator->symbol_power = best_power;
	demodulator->timing_locked = true;
}

static void acquire_symbol_timing(
	struct qam_demodulator *demodulator,
	double in_phase, double quadrature)
{
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct qam_timing_candidate *candidate =
			&demodulator->timing[index];
		double fraction = 0.0;

		if (advance_symbol_phase(
			    &candidate->phase_units,
			    demodulator->symbol_period_units,
			    demodulator->channel.symbol_rate_numerator,
			    &fraction) &&
		    demodulator->has_previous_matched_sample)
			observe_timing_candidate(
				candidate,
				interpolate(
					demodulator->previous_in_phase, in_phase,
					fraction),
				interpolate(
					demodulator->previous_quadrature,
					quadrature, fraction));
	}
	demodulator->acquisition_sample_count++;
	if (demodulator->acquisition_sample_count ==
	    demodulator->acquisition_sample_target)
		select_timing_candidate(demodulator);
}

static bool recover_symbol(
	struct qam_demodulator *demodulator,
	double in_phase, double quadrature, struct qam_point *point)
{
	double fraction = 0.0;

	if (!advance_symbol_phase(
		    &demodulator->locked_phase_units,
		    demodulator->symbol_period_units,
		    demodulator->recovery_step_units, &fraction))
		return false;
	*point = (struct qam_point){
		.in_phase = interpolate(
			demodulator->previous_in_phase, in_phase, fraction),
		.quadrature = interpolate(
			demodulator->previous_quadrature, quadrature, fraction),
	};
	demodulator->symbol_power +=
		(point->in_phase * point->in_phase +
		 point->quadrature * point->quadrature -
		 demodulator->symbol_power) /
		64.0;
	return true;
}

static void remember_matched_sample(
	struct qam_demodulator *demodulator,
	double in_phase, double quadrature)
{
	demodulator->previous_in_phase = in_phase;
	demodulator->previous_quadrature = quadrature;
	demodulator->has_previous_matched_sample = true;
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
	*symbol_ready = recover_symbol(
		demodulator, in_phase, quadrature, point);
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

const char *qam_result_name(
	enum qam_result result)
{
	switch (result) {
	case QAM_OK:
		return "ok";
	case QAM_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
