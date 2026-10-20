// SPDX-License-Identifier: BSD-3-Clause

#include "v92_training_echo.h"

#include "gram_fit.h"
#include "memory_region.h"
#include "sample_math.h"
#include "v92_upstream_channel.h"

#include <math.h>
#include <string.h>

enum {
	FIT_START = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS,
	FIT_SAMPLES = 1024,
	FIT_END = FIT_START + FIT_SAMPLES,
};

static bool clear(struct v92_training_echo *timing, struct pcm_host_echo *echo)
{
	if (timing->view && pcm_host_echo_reset_timing(echo) != PCM_HOST_ECHO_OK)
		return false;
	memset(timing, 0, sizeof(*timing));
	return true;
}

static bool target(const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	int32_t *sample, uint32_t *symbol)
{
	if (receiver == NULL || receiver->failed || receiver->renegotiation ||
	    (receiver->polarity != 1 && receiver->polarity != -1))
		return false;
	struct v92_pcm_phase4_trn2u reference = receiver->reference;
	int16_t generated;
	*symbol = reference.emitted_symbols;
	if (v92_pcm_phase4_trn2u_next(&reference, &generated) != V92_PCM_PHASE4_TRN2U_OK)
		return false;
	*sample = (int32_t)generated * receiver->polarity;
	return true;
}

static bool fit(const struct v92_training_echo *timing, const unsigned *lags,
	unsigned count, double *coefficient, double *error)
{
	double normal[GRAM_FIT_MOMENTS] = { 0 }, cross[4] = { 0 };
	unsigned columns[4];
	for (unsigned i = 0; i < count; ++i) columns[i] = lags[i];
	columns[count] = V92_TRAINING_ECHO_LAGS;
	gram_select(timing->normal, timing->cross, columns, count + 1u, normal, cross);
	if (!gram_fit(normal, cross, count + 1u, coefficient))
		return false;
	*error = timing->energy;
	for (unsigned i = 0; i <= count; ++i)
		*error -= coefficient[i] * cross[i];
	return isfinite(*error);
}

static bool choose(struct v92_training_echo *timing)
{
	/* Fit three causal taps plus DC. The same small solver serves every
	 * prefix; no dense channel inverse or second timing monitor is needed. */
	for (unsigned step = 0; step < 3; ++step) {
		bool found = false;
		unsigned best = 0;
		double best_score = 0;
		for (unsigned lag = 0; lag < V92_TRAINING_ECHO_LAGS; ++lag) {
			bool used = false;
			unsigned candidate[3] = { 0 };
			for (unsigned i = 0; i < step; ++i) {
				candidate[i] = timing->lag[i];
				used |= candidate[i] == lag;
			}
			if (used)
				continue;
			candidate[step] = lag;
			double coefficient[4] = { 0 }, error;
			if (!fit(timing, candidate, step + 1u, coefficient, &error))
				continue;
			if (!found || error < best_score) {
				found = true;
				best = lag;
				best_score = error;
				memcpy(timing->coefficient, coefficient, sizeof(coefficient));
			}
		}
		if (!found)
			return false;
		timing->lag[step] = (uint8_t)best;
	}
	return true;
}

static void collect(struct v92_training_echo *timing, int16_t received)
{
	double feature[V92_TRAINING_ECHO_COLUMNS];
	for (unsigned i = 0; i < V92_TRAINING_ECHO_LAGS; ++i)
		feature[i] = timing->history[i];
	feature[V92_TRAINING_ECHO_LAGS] = 1;
	/* At most 1024 integer products, each at most 2^30: all sums are
	 * exact in binary64. Symmetry therefore loses neither bits nor order. */
	gram_accumulate(timing->normal, timing->cross, feature,
		V92_TRAINING_ECHO_COLUMNS, received);
	timing->energy += (double)received * received;
	if (++timing->count == FIT_SAMPLES)
		timing->ready = choose(timing);
}

enum v92_training_echo_result v92_training_echo_observe(
	struct v92_training_echo *timing, struct pcm_host_echo *echo,
	const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	enum pcm_host_echo_mode mode, int16_t received, int16_t cleaned,
	bool available, int *delay_change)
{
	if (timing == NULL || echo == NULL || delay_change == NULL ||
	    (mode != PCM_HOST_ECHO_BYPASS && mode != PCM_HOST_ECHO_TRAIN &&
	     mode != PCM_HOST_ECHO_FILTER && mode != PCM_HOST_ECHO_FILTER_DATA) ||
	    memory_regions_overlap(timing, sizeof(*timing), echo, sizeof(*echo)) ||
	    memory_regions_overlap(timing, sizeof(*timing), delay_change, sizeof(*delay_change)) ||
	    memory_regions_overlap(echo, sizeof(*echo), delay_change, sizeof(*delay_change)) ||
	    (receiver != NULL &&
	     (memory_regions_overlap(receiver, sizeof(*receiver), timing, sizeof(*timing)) ||
	      memory_regions_overlap(receiver, sizeof(*receiver), echo, sizeof(*echo)) ||
	      memory_regions_overlap(receiver, sizeof(*receiver), delay_change, sizeof(*delay_change)))))
		return V92_TRAINING_ECHO_ERROR;
	if (timing->fill > V92_TRAINING_ECHO_LAGS || timing->count > FIT_SAMPLES ||
	    (!timing->active && (timing->ready || timing->view || timing->fill || timing->count)) ||
	    (timing->view && !timing->ready) ||
	    (timing->count && timing->fill != V92_TRAINING_ECHO_LAGS) ||
	    (timing->ready && (timing->count != FIT_SAMPLES ||
	     timing->lag[0] >= V92_TRAINING_ECHO_LAGS ||
	     timing->lag[1] >= V92_TRAINING_ECHO_LAGS ||
	     timing->lag[2] >= V92_TRAINING_ECHO_LAGS)))
		return V92_TRAINING_ECHO_ERROR;
	int32_t sample;
	uint32_t symbol;
	*delay_change = 0;
	if (!available || mode != PCM_HOST_ECHO_FILTER || received == INT16_MIN ||
	    received == INT16_MAX || cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    !target(receiver, &sample, &symbol)) {
		return (!timing->active && !timing->view) || clear(timing, echo) ?
			V92_TRAINING_ECHO_UNOBSERVED : V92_TRAINING_ECHO_ERROR;
	}
	if (timing->active && symbol != timing->last_symbol + 1u && !clear(timing, echo))
		return V92_TRAINING_ECHO_ERROR;
	timing->active = true;
	timing->last_symbol = symbol;
	for (unsigned i = V92_TRAINING_ECHO_LAGS - 1u; i != 0; --i)
		timing->history[i] = timing->history[i - 1u];
	timing->history[0] = sample;
	if (timing->fill < V92_TRAINING_ECHO_LAGS)
		++timing->fill;
	if (symbol >= FIT_START && symbol < FIT_END && timing->fill == V92_TRAINING_ECHO_LAGS)
		collect(timing, received);
	bool view = timing->ready && symbol >= FIT_END;
	int32_t monitored = 0, after = 0;
	if (view) {
		double peer = timing->coefficient[3];
		for (unsigned i = 0; i < 3; ++i)
			peer += timing->coefficient[i] * timing->history[timing->lag[i]];
		double projected = received - peer;
		view = isfinite(projected) && projected > INT16_MIN && projected < INT16_MAX;
		if (view) {
			monitored = sample_round_s16(projected);
			after = monitored - ((int32_t)received - cleaned);
			view = after > INT16_MIN && after < INT16_MAX;
		}
	}
	if (view != timing->view) {
		if (pcm_host_echo_reset_timing(echo) != PCM_HOST_ECHO_OK)
			return V92_TRAINING_ECHO_ERROR;
		timing->view = view;
	}
	if (!view)
		return V92_TRAINING_ECHO_UNOBSERVED;
	if (pcm_host_echo_observe_timing(echo, mode, (int16_t)monitored, (int16_t)after,
		    delay_change) != PCM_HOST_ECHO_OK)
		return V92_TRAINING_ECHO_ERROR;
	return *delay_change ? V92_TRAINING_ECHO_RETIMED : V92_TRAINING_ECHO_OBSERVED;
}
