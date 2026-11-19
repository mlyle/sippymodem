// SPDX-License-Identifier: BSD-3-Clause

#include "v92_echo_timing.h"

#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert((unsigned)V92_ECHO_TIMING_HISTORY > (unsigned)ECHO_TIMING_BINS,
	       "retain both sides of the echo shift search");
_Static_assert((V92_ECHO_TIMING_HISTORY & (V92_ECHO_TIMING_HISTORY - 1)) == 0,
	       "echo history is a power-of-two ring");
_Static_assert(V92_PCM_MAPPING_FRAME_SYMBOLS % V92_PCM_MAPPING_PHASES == 0,
	       "receive frame wraps must preserve the constellation phase");

_Static_assert(V92_ECHO_TIMING_HISTORY % V92_ECHO_TIMING_STRIDE == 0 &&
	       ECHO_TIMING_RADIUS % V92_ECHO_TIMING_STRIDE == 0,
	       "delayed scoring must preserve the observation stride");

void v92_echo_timing_reset(struct v92_echo_timing *timing)
{
	if (timing != NULL)
		timing->initialized = false;
}

static bool same_inverse(const struct v92_residual *a, const struct v92_residual *b)
{
	if (a->active != b->active || a->count != b->count || a->offset != b->offset)
		return false;
	for (unsigned j = 0; j < a->count; j++)
		if (a->coefficient[j] != b->coefficient[j] || a->delay[j] != b->delay[j])
			return false;
	return true;
}

static void configure(struct v92_echo_timing *timing,
	const struct v92_upstream_data_line *line, uint32_t delay)
{
	memset(timing, 0, sizeof(*timing));
	timing->gain = line->calibration.gain;
	timing->offset = line->calibration.offset;
	timing->polarity = line->polarity;
	timing->inverse = line->residual;
	memset(timing->inverse.history, 0, sizeof(timing->inverse.history));
	timing->inverse.cursor = 0;
	timing->model_delay = delay;
	timing->initialized = true;
	/* Two selection/validation pairs, each half spanning 128 physical inputs. */
	timing->window.points = V92_ECHO_TIMING_WINDOW_SAMPLES / V92_ECHO_TIMING_STRIDE;
}

static inline void score_shifts(struct v92_echo_timing *timing,
	const struct v92_cpd_constellation *points, unsigned center,
	unsigned part, double x, double half_width, double step,
	double inverse_step, const struct pam_distance_range *ranges)
{
	const unsigned mask = V92_ECHO_TIMING_HISTORY - 1;

	for (unsigned i = 0; i < ECHO_TIMING_BINS; i++) {
		unsigned shifted = (center + V92_ECHO_TIMING_HISTORY +
			ECHO_TIMING_RADIUS - i) & mask;
		timing->window.score[part][i] += pam_distance_squared(
			points->positive_linear, points->count, step, inverse_step,
			half_width, x - timing->prediction[shifted], ranges);
	}
}

int v92_echo_timing_observe(
	struct v92_echo_timing *timing, struct pcm_host_echo *echo,
	const struct v92_upstream_data_rx *receiver, struct v92_cpd_distances *distances,
	enum pcm_host_echo_mode mode, uint8_t wire,
	int16_t received, int16_t cleaned, bool available)
{
	if (timing == NULL)
		return 0;
	uint32_t delay = pcm_host_echo_active_delay_owned(echo, mode);
	if (!delay || receiver == NULL || !available || receiver->failed ||
	    receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    receiver->line.received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES ||
	    receiver->line.delay_remaining || receiver->input.count || receiver->model.valid || receiver->model.scalar_gain ||
	    received == INT16_MIN || received == INT16_MAX ||
	    cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    receiver->line.input_samples == UINT64_MAX) {
		v92_echo_timing_reset(timing);
		return 0;
	}
	const struct v92_upstream_data_line *line = &receiver->line;
	if (!timing->initialized || timing->gain != line->calibration.gain ||
	    timing->offset != line->calibration.offset || timing->polarity != line->polarity ||
	    !same_inverse(&timing->inverse, &line->residual) ||
	    timing->expected_sample != line->input_samples || timing->model_delay != delay) {
		configure(timing, line, delay);
		v92_cpd_distances_prepare(distances, &receiver->decoder.mapping.cpd);
	}
	timing->expected_sample = line->input_samples + 1;
	double projected = ((int32_t)received - cleaned) * line->polarity;
	double corrected = (int32_t)cleaned * line->polarity;
	if (line->residual.active) {
		corrected = v92_residual_output(&line->residual, corrected);
		projected = v92_residual_project(&timing->inverse, projected);
		v92_residual_retain(&timing->inverse, projected);
	}
	/* Match the actual inverse's physical bounds and rounding before calibration. */
	if (!isfinite(projected) || !isfinite(corrected) ||
	    corrected * line->polarity < INT16_MIN || corrected * line->polarity > INT16_MAX) {
		v92_echo_timing_reset(timing);
		return 0;
	}
	/* Match each input's slow data correction without restarting the timing
	 * window on every bias update. Older observations already carry their
	 * own correction; the acquired gain and physical inverse remain fixed. */
	double observed = (lrint(corrected * line->polarity) * line->polarity -
		timing->offset - line->tracking.bias) * timing->gain;
	double midpoint = 0, half_width = 0;
	if (line->host.gain > 0) {
		struct pcm_gain_interval cell;
		if (!pcm_gain_observed_cell(&line->host.bounds,
		    receiver->decoder.mapping.law, wire, received,
		    line->polarity, false, &cell)) {
			v92_echo_timing_reset(timing);
			return 0;
		}
		/* Move the actual wire cell by known echo/residual subtraction.
		 * Held-out input verifies, but never refits, the host gain. */
		double lower = cell.lower * line->host.gain;
		double upper = cell.upper * line->host.gain;
		midpoint = ((lower + upper) * .5 - (int32_t)received * line->polarity) * timing->gain;
		half_width = (upper - lower) * .5 * timing->gain;
	}
	projected *= timing->gain;
	if (!isfinite(observed) || !isfinite(projected) ||
	    fabs(observed) > 65536. || fabs(projected) > 65536.) {
		v92_echo_timing_reset(timing);
		return 0;
	}
	const unsigned mask = V92_ECHO_TIMING_HISTORY - 1;
	unsigned at = (unsigned)timing->samples & mask;
	timing->prediction[at] = (float)projected;
	/* Scoring consumes the last input of each stride, delayed by radius.
	 * Earlier inputs may replace this slot until that last input arrives;
	 * every physical prediction is still needed by the shift hypotheses. */
	unsigned observation = at / V92_ECHO_TIMING_STRIDE;
	timing->observation[observation] = (float)(observed + midpoint);
	timing->half_width[observation] = (float)half_width;
	++timing->samples;
	if (timing->samples < ECHO_TIMING_BINS || timing->samples % V92_ECHO_TIMING_STRIDE)
		return 0;
	/* Wait for both sides of each hypothesis. Initial inverse-history error
	 * has also contracted through more than 300 inputs before any scoring. */
	unsigned center = (at + V92_ECHO_TIMING_HISTORY - ECHO_TIMING_RADIUS) & mask;
	observation = center / V92_ECHO_TIMING_STRIDE;
	double x = timing->observation[observation] + timing->prediction[center];
	/* Supported input advances the receiver phase once per physical sample.
	 * Read the center's phase from that clock, independent of ring wrapping:
	 * the power-of-two history length is not a multiple of the phase cycle. */
	unsigned phase = (line->symbol_count + V92_PCM_MAPPING_PHASES -
		ECHO_TIMING_RADIUS % V92_PCM_MAPPING_PHASES) % V92_PCM_MAPPING_PHASES;
	unsigned set = receiver->decoder.mapping.cpd.frame_constellation[phase];
	const struct v92_cpd_distance *entry = &distances->set[set];
	unsigned part = echo_shift_part(&timing->window);
	/* Alphabet geometry is fixed across this window's hypotheses. Keep the
	 * uniform kernel independent of the nonuniform cache. */
	if (entry->step > 0)
		score_shifts(timing, &entry->points, center, part, x,
			timing->half_width[observation], entry->step,
			entry->inverse_step, NULL);
	else
		score_shifts(timing, &entry->points, center, part, x,
			timing->half_width[observation], 0, 0, entry->range);
	int shift = echo_shift_advance(&timing->window, .5);
	if (shift != 0) {
		bool applied = pcm_host_echo_retime(echo, shift) == PCM_HOST_ECHO_OK;
		v92_echo_timing_reset(timing);
		return applied ? shift : 0;
	}
	return 0;
}
