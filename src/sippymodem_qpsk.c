// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_qpsk.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	QPSK_NORMALIZATION_PHASES = 512,
	QPSK_QUADRANT_COUNT = 4,
	QPSK_ACQUISITION_SYMBOLS = 64,
	QPSK_MIN_ACQUISITION_SYMBOLS = 48,
};

static const double QPSK_PI = 3.1415926535897932384626433832795;
static const double QPSK_TWO_PI = 6.283185307179586476925286766559;
static const double QPSK_PHASE_SCALE = 4294967296.0;
static const double QPSK_SINGULARITY_EPSILON = 1e-10;

static bool channel_is_valid(
	const struct sippymodem_qpsk_channel *channel,
	uint64_t *symbol_period_units)
{
	uint64_t period;

	if (channel == NULL || channel->sample_rate == 0 ||
	    channel->symbol_rate_numerator == 0 ||
	    channel->symbol_rate_denominator == 0 ||
	    channel->carrier_frequency_hz == 0 ||
	    (uint64_t)channel->carrier_frequency_hz * 2 >=
		    channel->sample_rate ||
	    !isfinite(channel->rolloff) || channel->rolloff <= 0.0 ||
	    channel->rolloff > 1.0 || channel->pulse_span_symbols < 2 ||
	    channel->pulse_span_symbols >
		    SIPPYMODEM_QPSK_MAX_PULSE_SPAN_SYMBOLS ||
	    (channel->pulse_span_symbols & 1u) != 0)
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

enum sippymodem_qpsk_result sippymodem_root_raised_cosine(
	double symbol_time, double rolloff, double *value)
{
	double four_rolloff_time;
	double numerator;
	double denominator;

	if (!isfinite(symbol_time) || !isfinite(rolloff) || rolloff <= 0.0 ||
	    rolloff > 1.0 || value == NULL)
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	if (fabs(symbol_time) < QPSK_SINGULARITY_EPSILON) {
		*value = 1.0 + rolloff * (4.0 / QPSK_PI - 1.0);
		return SIPPYMODEM_QPSK_OK;
	}
	four_rolloff_time = 4.0 * rolloff * symbol_time;
	if (fabs(fabs(four_rolloff_time) - 1.0) <
	    QPSK_SINGULARITY_EPSILON) {
		double angle = QPSK_PI / (4.0 * rolloff);

		*value = rolloff / sqrt(2.0) *
			 ((1.0 + 2.0 / QPSK_PI) * sin(angle) +
			  (1.0 - 2.0 / QPSK_PI) * cos(angle));
		return SIPPYMODEM_QPSK_OK;
	}
	numerator = sin(QPSK_PI * symbol_time * (1.0 - rolloff)) +
		    four_rolloff_time *
			    cos(QPSK_PI * symbol_time * (1.0 + rolloff));
	denominator = QPSK_PI * symbol_time *
		      (1.0 - four_rolloff_time * four_rolloff_time);
	*value = numerator / denominator;
	return isfinite(*value) ? SIPPYMODEM_QPSK_OK :
				 SIPPYMODEM_QPSK_INVALID_ARGUMENT;
}

static double pulse_normalization(
	const struct sippymodem_qpsk_channel *channel)
{
	double maximum = 0.0;
	double delay = (double)channel->pulse_span_symbols / 2.0;

	/* Generate a conservative peak bound instead of storing polyphase
	 * coefficients. The final clamp covers the space between scan points. */
	for (unsigned int phase = 0; phase < QPSK_NORMALIZATION_PHASES;
	     ++phase) {
		double fractional =
			(double)phase / (double)QPSK_NORMALIZATION_PHASES;
		double sum = 0.0;

		for (unsigned int age = 0;
		     age <= channel->pulse_span_symbols; ++age) {
			double pulse = 0.0;

			(void)sippymodem_root_raised_cosine(
				(double)age + fractional - delay,
				channel->rolloff, &pulse);
			sum += fabs(pulse);
		}
		if (sum > maximum)
			maximum = sum;
	}
	return maximum * 1.001;
}

static uint32_t phase_step(uint32_t sample_rate, uint32_t frequency_hz)
{
	uint64_t numerator =
		(uint64_t)frequency_hz * (UINT64_C(1) << 32);

	return (uint32_t)((numerator + sample_rate / 2) / sample_rate);
}

static bool modulator_is_valid(
	const struct sippymodem_qpsk_modulator *modulator)
{
	uint64_t period = 0;
	uint64_t support;

	if (modulator == NULL ||
	    !channel_is_valid(&modulator->channel, &period) ||
	    modulator->symbol_period_units != period ||
	    modulator->symbol_phase_units >= period ||
	    modulator->carrier_phase_step == 0 || modulator->amplitude <= 0 ||
	    !isfinite(modulator->pulse_normalization) ||
	    modulator->pulse_normalization <= 0.0 ||
	    modulator->active_count > SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS)
		return false;
	support = period * modulator->channel.pulse_span_symbols;
	if (modulator->active_count == 0 && !modulator->needs_symbol)
		return false;
	for (size_t index = 0; index < modulator->active_count; ++index)
		if (modulator->active[index].age_units > support ||
		    modulator->active[index].in_phase *
				    modulator->active[index].in_phase +
			    modulator->active[index].quadrature *
				    modulator->active[index].quadrature !=
			    1)
			return false;
	return true;
}

enum sippymodem_qpsk_result sippymodem_qpsk_modulator_init(
	struct sippymodem_qpsk_modulator *modulator,
	const struct sippymodem_qpsk_channel *channel, int16_t amplitude)
{
	uint64_t period = 0;
	double normalization;

	if (modulator == NULL || amplitude <= 0 ||
	    !channel_is_valid(channel, &period))
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	normalization = pulse_normalization(channel);
	if (!isfinite(normalization) || normalization <= 0.0)
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	memset(modulator, 0, sizeof(*modulator));
	modulator->channel = *channel;
	modulator->symbol_period_units = period;
	modulator->carrier_phase_step = phase_step(
		channel->sample_rate, channel->carrier_frequency_hz);
	modulator->pulse_normalization = normalization;
	modulator->amplitude = amplitude;
	modulator->needs_symbol = true;
	return SIPPYMODEM_QPSK_OK;
}

bool sippymodem_qpsk_modulator_can_accept_symbol(
	const struct sippymodem_qpsk_modulator *modulator)
{
	return modulator_is_valid(modulator) && modulator->needs_symbol &&
	       modulator->active_count < SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS;
}

static void append_symbol(
	struct sippymodem_qpsk_modulator *modulator, uint8_t quadrant)
{
	static const int8_t in_phase[QPSK_QUADRANT_COUNT] = { 1, 0, -1, 0 };
	static const int8_t quadrature[QPSK_QUADRANT_COUNT] = { 0, 1, 0, -1 };
	struct sippymodem_qpsk_active_symbol *symbol =
		&modulator->active[modulator->active_count++];

	*symbol = (struct sippymodem_qpsk_active_symbol){
		.age_units = modulator->symbol_phase_units,
		.in_phase = in_phase[quadrant],
		.quadrature = quadrature[quadrant],
	};
	modulator->needs_symbol = false;
}

static void baseband_sample(
	const struct sippymodem_qpsk_modulator *modulator,
	double *in_phase, double *quadrature)
{
	double delay = (double)modulator->channel.pulse_span_symbols / 2.0;

	*in_phase = 0.0;
	*quadrature = 0.0;
	for (size_t index = 0; index < modulator->active_count; ++index) {
		const struct sippymodem_qpsk_active_symbol *symbol =
			&modulator->active[index];
		double age = (double)symbol->age_units /
			     (double)modulator->symbol_period_units;
		double pulse = 0.0;

		(void)sippymodem_root_raised_cosine(
			age - delay, modulator->channel.rolloff, &pulse);
		*in_phase += pulse * symbol->in_phase;
		*quadrature += pulse * symbol->quadrature;
	}
}

static void advance_time(struct sippymodem_qpsk_modulator *modulator)
{
	uint64_t step = modulator->channel.symbol_rate_numerator;
	uint64_t support = modulator->symbol_period_units *
			   modulator->channel.pulse_span_symbols;
	size_t retained = 0;

	for (size_t index = 0; index < modulator->active_count; ++index) {
		struct sippymodem_qpsk_active_symbol symbol =
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

enum sippymodem_qpsk_result sippymodem_qpsk_modulator_sample(
	struct sippymodem_qpsk_modulator *modulator, uint8_t quadrant,
	int16_t *sample, bool *symbol_accepted)
{
	double carrier_phase;
	double in_phase;
	double quadrature;
	double waveform;
	long rounded;

	if (!modulator_is_valid(modulator) || quadrant >= QPSK_QUADRANT_COUNT ||
	    sample == NULL || symbol_accepted == NULL ||
	    (modulator->needs_symbol &&
	     modulator->active_count == SIPPYMODEM_QPSK_MAX_ACTIVE_SYMBOLS))
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	*symbol_accepted = modulator->needs_symbol;
	if (modulator->needs_symbol)
		append_symbol(modulator, quadrant);
	baseband_sample(modulator, &in_phase, &quadrature);
	carrier_phase = (double)modulator->carrier_phase *
			(QPSK_TWO_PI / QPSK_PHASE_SCALE);
	waveform = (in_phase * cos(carrier_phase) -
		    quadrature * sin(carrier_phase)) /
		   modulator->pulse_normalization;
	rounded = lround(waveform * modulator->amplitude);
	if (rounded > modulator->amplitude)
		rounded = modulator->amplitude;
	if (rounded < -modulator->amplitude)
		rounded = -modulator->amplitude;
	*sample = (int16_t)rounded;
	advance_time(modulator);
	return SIPPYMODEM_QPSK_OK;
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
	const struct sippymodem_qpsk_channel *channel, uint64_t period,
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
	uint64_t target = QPSK_ACQUISITION_SYMBOLS * whole_samples +
		(QPSK_ACQUISITION_SYMBOLS * remainder_units + step - 1u) /
			step +
		candidates;

	if (count < 2 || count > SIPPYMODEM_QPSK_MAX_MATCHED_FILTER_SAMPLES ||
	    candidates == 0 ||
	    candidates > SIPPYMODEM_QPSK_MAX_TIMING_CANDIDATES ||
	    target > UINT32_MAX)
		return false;
	*filter_count = (size_t)count;
	*candidate_count = (size_t)candidates;
	*timing_step = timing_gcd;
	*acquisition_samples = (uint32_t)target;
	return true;
}

static void build_matched_filter(
	struct sippymodem_qpsk_demodulator *demodulator)
{
	double delay = (double)demodulator->channel.pulse_span_symbols / 2.0;

	for (size_t tap = 0; tap < demodulator->matched_filter_count; ++tap) {
		double symbol_time =
			(double)tap *
				(double)demodulator->channel.symbol_rate_numerator /
			((double)demodulator->channel.sample_rate *
			 (double)demodulator->channel.symbol_rate_denominator);

		(void)sippymodem_root_raised_cosine(
			symbol_time - delay, demodulator->channel.rolloff,
			&demodulator->matched_coefficients[tap]);
	}
}

static void initialize_timing_candidates(
	struct sippymodem_qpsk_demodulator *demodulator)
{
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index)
		demodulator->timing[index].phase_units =
			index * demodulator->timing_step_units;
}

static bool demodulator_is_valid(
	const struct sippymodem_qpsk_demodulator *demodulator)
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
	    demodulator->matched_filter_count != filter_count ||
	    demodulator->timing_candidate_count != candidate_count ||
	    demodulator->acquisition_sample_target != acquisition_samples ||
	    demodulator->acquisition_sample_count >= acquisition_samples ||
	    demodulator->history_index >= filter_count ||
	    demodulator->history_count > filter_count ||
	    demodulator->carrier_phase_step == 0 ||
	    !isfinite(demodulator->line_power) ||
	    demodulator->line_power < 0.0 ||
	    !isfinite(demodulator->symbol_power) ||
	    demodulator->symbol_power < 0.0 ||
	    (demodulator->timing_locked &&
	     (demodulator->locked_phase_units >= period ||
	      !demodulator->has_previous)))
		return false;
	for (size_t index = 0; index < candidate_count; ++index) {
		const struct sippymodem_qpsk_timing_candidate *candidate =
			&demodulator->timing[index];

		if (candidate->phase_units >= period ||
		    !isfinite(candidate->symbol_power_sum) ||
		    candidate->symbol_power_sum < 0.0)
			return false;
	}
	return true;
}

enum sippymodem_qpsk_result sippymodem_qpsk_demodulator_init(
	struct sippymodem_qpsk_demodulator *demodulator,
	const struct sippymodem_qpsk_channel *channel)
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
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	memset(demodulator, 0, sizeof(*demodulator));
	demodulator->channel = *channel;
	demodulator->symbol_period_units = period;
	demodulator->timing_step_units = timing_step;
	demodulator->carrier_phase_step = phase_step(
		channel->sample_rate, channel->carrier_frequency_hz);
	demodulator->matched_filter_count = filter_count;
	demodulator->timing_candidate_count = candidate_count;
	demodulator->acquisition_sample_target = acquisition_samples;
	build_matched_filter(demodulator);
	initialize_timing_candidates(demodulator);
	return SIPPYMODEM_QPSK_OK;
}

enum sippymodem_qpsk_result sippymodem_qpsk_demodulator_reset(
	struct sippymodem_qpsk_demodulator *demodulator)
{
	struct sippymodem_qpsk_channel channel;

	if (!demodulator_is_valid(demodulator))
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	channel = demodulator->channel;
	return sippymodem_qpsk_demodulator_init(demodulator, &channel);
}

static void mix_sample(
	struct sippymodem_qpsk_demodulator *demodulator, int16_t sample)
{
	double radians = (double)demodulator->carrier_phase *
			 (QPSK_TWO_PI / QPSK_PHASE_SCALE);
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
	const struct sippymodem_qpsk_demodulator *demodulator,
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

static bool advance_symbol_phase(uint64_t *phase, uint64_t period,
				 uint64_t step)
{
	if (step < period - *phase) {
		*phase += step;
		return false;
	}
	*phase = step - (period - *phase);
	return true;
}

static void observe_timing_candidate(
	struct sippymodem_qpsk_timing_candidate *candidate,
	double in_phase, double quadrature)
{
	candidate->previous_in_phase = in_phase;
	candidate->previous_quadrature = quadrature;
	candidate->symbol_power_sum +=
		in_phase * in_phase + quadrature * quadrature;
	candidate->symbol_count++;
	candidate->has_previous = true;
}

static void reset_timing_scores(
	struct sippymodem_qpsk_demodulator *demodulator)
{
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct sippymodem_qpsk_timing_candidate *candidate =
			&demodulator->timing[index];

		candidate->symbol_power_sum = 0.0;
		candidate->symbol_count = 0;
		candidate->has_previous = false;
	}
	demodulator->acquisition_sample_count = 0;
}

static void select_timing_candidate(
	struct sippymodem_qpsk_demodulator *demodulator)
{
	struct sippymodem_qpsk_timing_candidate *best = NULL;
	double best_power = 0.0;

	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct sippymodem_qpsk_timing_candidate *candidate =
			&demodulator->timing[index];
		double average;

		if (candidate->symbol_count < QPSK_MIN_ACQUISITION_SYMBOLS)
			continue;
		average = candidate->symbol_power_sum / candidate->symbol_count;
		if (average > best_power) {
			best = candidate;
			best_power = average;
		}
	}
	if (best == NULL || best_power <= 1.0) {
		reset_timing_scores(demodulator);
		return;
	}
	demodulator->locked_phase_units = best->phase_units;
	demodulator->previous_in_phase = best->previous_in_phase;
	demodulator->previous_quadrature = best->previous_quadrature;
	demodulator->symbol_power = best_power;
	demodulator->has_previous = best->has_previous;
	demodulator->timing_locked = true;
	demodulator->acquisition_sample_count = 0;
}

static void acquire_symbol_timing(
	struct sippymodem_qpsk_demodulator *demodulator,
	double in_phase, double quadrature)
{
	for (size_t index = 0; index < demodulator->timing_candidate_count;
	     ++index) {
		struct sippymodem_qpsk_timing_candidate *candidate =
			&demodulator->timing[index];

		if (advance_symbol_phase(
			    &candidate->phase_units,
			    demodulator->symbol_period_units,
			    demodulator->channel.symbol_rate_numerator))
			observe_timing_candidate(candidate, in_phase, quadrature);
	}
	demodulator->acquisition_sample_count++;
	if (demodulator->acquisition_sample_count ==
	    demodulator->acquisition_sample_target)
		select_timing_candidate(demodulator);
}

static uint8_t slice_phase_change(
	double previous_in_phase, double previous_quadrature,
	double in_phase, double quadrature)
{
	double dot = previous_in_phase * in_phase +
		     previous_quadrature * quadrature;
	double cross = previous_in_phase * quadrature -
		       previous_quadrature * in_phase;
	long turns = lround(atan2(cross, dot) / (QPSK_PI / 2.0));

	return (uint8_t)((unsigned long)turns & 3u);
}

static bool recover_symbol(
	struct sippymodem_qpsk_demodulator *demodulator,
	double in_phase, double quadrature, uint8_t *quarter_turns)
{
	if (!advance_symbol_phase(
		    &demodulator->locked_phase_units,
		    demodulator->symbol_period_units,
		    demodulator->channel.symbol_rate_numerator))
		return false;
	*quarter_turns = slice_phase_change(
		demodulator->previous_in_phase,
		demodulator->previous_quadrature, in_phase, quadrature);
	demodulator->previous_in_phase = in_phase;
	demodulator->previous_quadrature = quadrature;
	demodulator->symbol_power +=
		(in_phase * in_phase + quadrature * quadrature -
		 demodulator->symbol_power) /
		64.0;
	return true;
}

enum sippymodem_qpsk_result sippymodem_qpsk_demodulator_sample(
	struct sippymodem_qpsk_demodulator *demodulator, int16_t sample,
	uint8_t *quarter_turns, bool *symbol_ready)
{
	double in_phase;
	double quadrature;

	if (!demodulator_is_valid(demodulator) || quarter_turns == NULL ||
	    symbol_ready == NULL)
		return SIPPYMODEM_QPSK_INVALID_ARGUMENT;
	*quarter_turns = 0;
	*symbol_ready = false;
	mix_sample(demodulator, sample);
	if (demodulator->history_count < demodulator->matched_filter_count)
		return SIPPYMODEM_QPSK_OK;
	matched_filter_sample(demodulator, &in_phase, &quadrature);
	if (!demodulator->timing_locked) {
		acquire_symbol_timing(demodulator, in_phase, quadrature);
		return SIPPYMODEM_QPSK_OK;
	}
	*symbol_ready = recover_symbol(
		demodulator, in_phase, quadrature, quarter_turns);
	return SIPPYMODEM_QPSK_OK;
}

struct sippymodem_qpsk_measurement sippymodem_qpsk_demodulator_measure(
	const struct sippymodem_qpsk_demodulator *demodulator)
{
	if (!demodulator_is_valid(demodulator))
		return (struct sippymodem_qpsk_measurement){ 0 };
	return (struct sippymodem_qpsk_measurement){
		.line_power = demodulator->line_power,
		.symbol_power = demodulator->symbol_power,
		.timing_locked = demodulator->timing_locked,
	};
}

const char *sippymodem_qpsk_result_name(
	enum sippymodem_qpsk_result result)
{
	switch (result) {
	case SIPPYMODEM_QPSK_OK:
		return "ok";
	case SIPPYMODEM_QPSK_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
