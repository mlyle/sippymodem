// SPDX-License-Identifier: BSD-3-Clause

#include "v34_echo_timing.h"

#include "integer_math.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* A weak local return can be hidden by the peer's signal in a raw correlation
 * test. Project the frozen echo prediction through the real receiver's linear
 * filters and test whether shifting it restores the negotiated odd lattice.
 * This never fits an echo response to peer data. A candidate must improve both
 * halves of a window, then win again in the next disjoint window. Strong
 * improvements can qualify sooner; small improvements retain longer windows.
 * Unsupported constellations retain the ordinary echo monitor and retrain
 * behavior. */

void v34_echo_timing_reset(struct v34_echo_timing *timing)
{
	if (timing != NULL) {
		timing->initialized = false;
		timing->prepared = false;
	}
}

static bool configure(struct v34_echo_timing *timing,
	const struct v34_line_receiver *receiver, double scale,
	double rate, double carrier, uint32_t model_delay)
{
	memset(timing, 0, sizeof(*timing));
	timing->scale = scale;
	timing->symbol_rate = rate;
	timing->carrier_frequency = carrier;
	timing->model_delay = model_delay;
	/* Each observation spans two symbols. Keep the 200 ms window for small
	 * improvements and also test 100 ms windows with a stronger margin.
	 * Each path needs four disjoint selection/validation intervals. */
	timing->normal.points = (unsigned)ceil(rate / 10.0);
	timing->fast.points = (unsigned)ceil(rate / 20.0);
	timing->lookahead = (unsigned)ceil(
		V34_ECHO_TIMING_RADIUS * 2.0 * rate / 8000.0) + 4u;
	timing->reference_timing = receiver->timing;
	memset(timing->reference_timing.analytic.history, 0,
	       sizeof(timing->reference_timing.analytic.history));
	memset(timing->reference_timing.input, 0,
	       sizeof(timing->reference_timing.input));
	timing->reference_timing.previous = (struct v34_analytic_sample){ 0 };
	timing->reference_timing.current = (struct v34_analytic_sample){ 0 };
	if (v34_timing_filter_init(&timing->reference_timing.timing_filter) !=
	    V34_TIMING_FILTER_OK)
		return false;
	timing->reference_equalizer = receiver->equalizer.equalizer;
	memset(timing->reference_equalizer.in_phase_delay, 0,
	       sizeof(timing->reference_equalizer.in_phase_delay));
	memset(timing->reference_equalizer.quadrature_delay, 0,
	       sizeof(timing->reference_equalizer.quadrature_delay));
	for (unsigned i = 0; i < V34_ECHO_TIMING_BINS; ++i) {
		struct v34_echo_timing_bin *bin = &timing->bins[i];
		int delta = (int)i - V34_ECHO_TIMING_RADIUS;
		double lag = -delta * 2.0 * rate / 8000.0;
		double integral = floor(lag);
		double f = lag - integral, f2 = f * f, f3 = f2 * f;
		double phase = -2.0 * acos(-1.0) * carrier * delta / 8000.0;

		bin->lag = (int16_t)integral;
		/* Four-point interpolation of the band-limited half-symbol path.
		 * Coefficients sum to one and reproduce integer positions exactly. */
		bin->weight[0] = -0.5 * f + f2 - 0.5 * f3;
		bin->weight[1] = 1.0 - 2.5 * f2 + 1.5 * f3;
		bin->weight[2] = 0.5 * f + 2.0 * f2 - 1.5 * f3;
		bin->weight[3] = -0.5 * f2 + 0.5 * f3;
		bin->cosine = cos(phase);
		bin->sine = sin(phase);
	}
	timing->receiver_epoch = receiver->equalizer.epoch;
	timing->initialized = true;
	return true;
}

bool v34_echo_timing_prepare(struct v34_echo_timing *timing,
	const struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	const struct v34_line_receiver *receiver,
	const struct v34_data_decision_receiver *data,
	int16_t received, int16_t cleaned)
{
	const struct v34_channel_frame_receiver *channel;
	const struct v34_phase3_profile *profile;
	struct v34_receive_timing next;
	struct v34_receive_timing_samples samples = { 0 };
	double scale, rate, carrier;
	uint32_t model_delay;
	int32_t prediction = (int32_t)received - cleaned;

	if (timing == NULL)
		return false;
	if (echo == NULL || receiver == NULL || data == NULL || !receiver->initialized ||
	    (mode != PCM_HOST_ECHO_FILTER && mode != PCM_HOST_ECHO_FILTER_DATA) ||
	    (mode == PCM_HOST_ECHO_FILTER_DATA && echo->filter_suspended) ||
	    (!echo->filter_ready && !echo->retained_delay_samples) ||
	    prediction < INT16_MIN || prediction > INT16_MAX ||
	    cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    received == INT16_MIN || received == INT16_MAX ||
	    receiver->equalizer.feedback_mode != V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE ||
	    receiver->equalizer.epoch > INT64_MAX / 2u - V34_ECHO_TIMING_HISTORY)
		goto unsupported;
	model_delay = echo->retained_delay_samples ? echo->retained_delay_samples :
		pcm_echo_reference_current_delay(&echo->reference);
	channel = &data->data_receiver.channel_receiver;
	profile = &receiver->timing.transmitting_profile;
	if (channel->conditioner.normalizer.nonlinear_parameter != 0 ||
	    !v34_precoder_coefficients_are_zero(channel->receive_precoder.coefficients) ||
	    profile->symbol_rate_denominator == 0 ||
	    profile->carrier_frequency_denominator == 0)
		goto unsupported;
	scale = channel->conditioner.normalizer.normalization_gain *
		V34_TRAINING_FIXED_AMPLITUDE;
	rate = (double)profile->symbol_rate_numerator / profile->symbol_rate_denominator;
	carrier = (double)profile->carrier_frequency_numerator /
		profile->carrier_frequency_denominator;
	if (!isfinite(scale) || scale <= 0 || rate < 2400 || rate > 3430 ||
	    carrier <= 0 || carrier >= 4000)
		goto unsupported;
	if (!timing->initialized || timing->model_delay != model_delay || timing->scale != scale ||
	    timing->symbol_rate != rate || timing->carrier_frequency != carrier ||
	    timing->receiver_epoch != receiver->equalizer.epoch ||
	    timing->reference_equalizer.index != receiver->equalizer.equalizer.index) {
		if (!configure(timing, receiver, scale, rate, carrier, model_delay))
			goto unsupported;
	}
	/* Copy controls, retaining only the reference's signal histories. Its
	 * timing error never feeds the real receiver's clock or carrier loops. */
	next = receiver->timing;
	next.analytic = timing->reference_timing.analytic;
	memcpy(next.input, timing->reference_timing.input, sizeof(next.input));
	next.previous = timing->reference_timing.previous;
	next.current = timing->reference_timing.current;
	next.timing_filter = timing->reference_timing.timing_filter;
	timing->reference_timing = next;
	enum v34_receive_timing_result result = v34_receive_timing_sample(
		&timing->reference_timing, prediction, &samples);
	if (result != V34_RECEIVE_TIMING_OK && result != V34_RECEIVE_TIMING_SAMPLES_READY)
		goto unsupported;
	memcpy(timing->reference_equalizer.in_phase_coefficient,
	       receiver->equalizer.equalizer.in_phase_coefficient,
	       sizeof(timing->reference_equalizer.in_phase_coefficient));
	memcpy(timing->reference_equalizer.quadrature_coefficient,
	       receiver->equalizer.equalizer.quadrature_coefficient,
	       sizeof(timing->reference_equalizer.quadrature_coefficient));
	for (unsigned i = 0; i < samples.count; ++i) {
		struct v34_equalizer_accumulator filtered;
		uint64_t half = 2u * receiver->equalizer.epoch +
			receiver->equalizer.inputs_since_observation + i + 1u;
		struct v34_echo_timing_point *point =
			&timing->history[half % V34_ECHO_TIMING_HISTORY];

		if (v34_equalizer_push(&timing->reference_equalizer, samples.sample[i]) !=
		    V34_EQUALIZER_OK || v34_equalizer_filter(
			&timing->reference_equalizer, &filtered) != V34_EQUALIZER_OK)
			goto unsupported;
		*point = (struct v34_echo_timing_point){
			.epoch = half,
			.real = round_shift_s32_word_to_s16((uint32_t)filtered.in_phase, 14) / scale,
			.imaginary = round_shift_s32_word_to_s16((uint32_t)filtered.quadrature, 14) / scale,
			.cosine = receiver->equalizer.rotate_in_phase / 16384.0,
			.sine = -receiver->equalizer.rotate_quadrature / 16384.0,
		};
		/* A replacement reference awaits its own received observation. */
		if (half % V34_ECHO_TIMING_OBSERVATION_STRIDE == 0)
			timing->observations[(half / V34_ECHO_TIMING_OBSERVATION_STRIDE) %
				V34_ECHO_TIMING_OBSERVATIONS].epoch = 0;
		if (timing->first_half == 0)
			timing->drain_half = timing->first_half = half;
		timing->latest_half = half;
	}
	timing->prepared = true;
	return true;
unsupported:
	v34_echo_timing_reset(timing);
	return false;
}

static double distance(double value)
{
	double difference = value - 2.0 * floor(value * 0.5) - 1.0;
	return difference * difference;
}

static int observe_point(struct v34_echo_timing *timing,
	const struct v34_echo_timing_point *point,
	const struct v34_echo_timing_observation *observation)
{
	unsigned part = echo_shift_part(&timing->normal);
	unsigned fast_part = echo_shift_part(&timing->fast);
	double x = observation->real + point->real * point->cosine -
		point->imaginary * point->sine;
	double y = observation->imaginary + point->real * point->sine +
		point->imaginary * point->cosine;

	for (unsigned i = 0; i < V34_ECHO_TIMING_BINS; ++i) {
		const struct v34_echo_timing_bin *bin = &timing->bins[i];
		int64_t at = (int64_t)point->epoch + bin->lag;
		double real = 0, imaginary = 0;

		for (unsigned j = 0; j < 4; ++j) {
			uint64_t epoch = (uint64_t)(at + (int)j - 1);
			const struct v34_echo_timing_point *reference =
				&timing->history[epoch % V34_ECHO_TIMING_HISTORY];
			if (reference->epoch != epoch) {
				v34_echo_timing_reset(timing);
				return 0;
			}
			real += reference->real * bin->weight[j];
			imaginary += reference->imaginary * bin->weight[j];
		}
		double cosine = point->cosine * bin->cosine - point->sine * bin->sine;
		double sine = point->sine * bin->cosine + point->cosine * bin->sine;
		double cost = distance(x - real * cosine + imaginary * sine) +
			distance(y - real * sine - imaginary * cosine);
		timing->normal.score[part][i] += cost;
		timing->fast.score[fast_part][i] += cost;
	}
	int normal = echo_shift_advance(&timing->normal, 0.95);
	int fast = echo_shift_advance(&timing->fast, 0.75);
	/* Prefer the longer evidence if both paths qualify on this sample. */
	return normal != 0 ? normal : fast;
}

int v34_echo_timing_observe(struct v34_echo_timing *timing,
	struct pcm_host_echo *echo,
	const struct v34_line_receiver *receiver)
{
	if (timing == NULL || !timing->initialized || !timing->prepared)
		return 0;
	timing->prepared = false;
	if (echo == NULL || receiver == NULL || timing->reference_equalizer.index !=
	    receiver->equalizer.equalizer.index || timing->latest_half !=
	    2u * receiver->equalizer.epoch + receiver->equalizer.inputs_since_observation) {
		v34_echo_timing_reset(timing);
		return 0;
	}
	timing->receiver_epoch = receiver->equalizer.epoch;
	const struct v34_adaptive_equalizer_observation *observation =
		&receiver->latest_observation;
	if (observation->epoch != 0 && (observation->epoch & 1u) == 0) {
		uint64_t half = 2u * observation->epoch;
		struct v34_echo_timing_point *point =
			&timing->history[half % V34_ECHO_TIMING_HISTORY];
		if (point->epoch == half) {
			struct v34_echo_timing_observation *received = &timing->observations[
				(half / V34_ECHO_TIMING_OBSERVATION_STRIDE) % V34_ECHO_TIMING_OBSERVATIONS];

			*received = (struct v34_echo_timing_observation){
				.epoch = half,
				.real = observation->decision.in_phase / timing->scale,
				.imaginary = observation->decision.quadrature / timing->scale,
			};
		}
	}
	/* At most two new half-symbols are available per host sample. Leave the
	 * media path alone while future reference hypotheses acquire evidence. */
	while (timing->latest_half >= timing->drain_half + timing->lookahead) {
		uint64_t half = timing->drain_half++;
		const struct v34_echo_timing_point *point =
			&timing->history[half % V34_ECHO_TIMING_HISTORY];
		const struct v34_echo_timing_observation *received = &timing->observations[
			(half / V34_ECHO_TIMING_OBSERVATION_STRIDE) % V34_ECHO_TIMING_OBSERVATIONS];
		if (half < timing->first_half + 2u * timing->lookahead ||
		    point->epoch != half || half == 0 ||
		    half % V34_ECHO_TIMING_OBSERVATION_STRIDE != 0 || received->epoch != half)
			continue;
		int delta = observe_point(timing, point, received);
		if (delta != 0) {
			bool applied = pcm_host_echo_retime(echo, delta) == PCM_HOST_ECHO_OK;
			v34_echo_timing_reset(timing);
			return applied ? delta : 0;
		}
		if (!timing->initialized)
			return 0;
	}
	return 0;
}
