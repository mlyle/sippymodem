// SPDX-License-Identifier: BSD-3-Clause

#include "v92_echo_timing.h"

#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert((unsigned)V92_ECHO_TIMING_HISTORY > (unsigned)ECHO_TIMING_BINS,
	       "retain both sides of the echo shift search");
_Static_assert((V92_ECHO_TIMING_HISTORY & (V92_ECHO_TIMING_HISTORY - 1)) == 0,
	       "echo history is a power-of-two ring");

void v92_echo_timing_reset(struct v92_echo_timing *timing)
{
	if (timing != NULL)
		timing->initialized = false;
}

static bool same_inverse(const struct v92_residual *a, const struct v92_residual *b)
{
	if (a->active != b->active || a->count != b->count)
		return false;
	for (unsigned j = 0; j < a->count; j++)
		if (a->coefficient[j] != b->coefficient[j] || a->delay[j] != b->delay[j])
			return false;
	return true;
}

static void configure(struct v92_echo_timing *timing,
	const struct v92_upstream_data_line *line,
	const struct v92_pcm_mapping *mapping, uint32_t delay)
{
	memset(timing, 0, sizeof(*timing));
	timing->gain = line->calibration.gain;
	timing->offset = line->calibration.offset;
	timing->channel_offset = line->residual.offset;
	timing->polarity = line->polarity;
	timing->inverse = line->residual;
	memset(timing->inverse.history, 0, sizeof(timing->inverse.history));
	timing->inverse.cursor = 0;
	timing->inverse.offset = 0;
	timing->model_delay = delay;
	timing->initialized = true;
	/* Two selection/validation pairs, each half spanning 128 physical inputs. */
	timing->window.points = V92_ECHO_TIMING_WINDOW_SAMPLES / V92_ECHO_TIMING_STRIDE;
	for (unsigned j = 0; j < V92_CPD_MAX_CONSTELLATIONS; j++) {
		const struct v92_cpd_constellation *points = &mapping->cpd.constellation[j];
		if (points->count < 2)
			continue;
		double step = points->positive_linear[1] - points->positive_linear[0];
		for (unsigned i = 2; i < points->count; i++)
			if (points->positive_linear[i] - points->positive_linear[i - 1] != step) {
				step = 0;
				break;
			}
		timing->step[j] = step;
		timing->inverse_step[j] = step > 0 ? 1. / step : 0;
	}
}

/* Distance from a translated input cell to the nearest signed CPd point.
 * Uniform alphabets need no table search or per-candidate division. The
 * interior index is bounded by the point count before conversion to integer. */
static double distance(const struct v92_cpd_constellation *points,
	double step, double inverse_step, double half_width, double x)
{
	x = fabs(x);
	double first = points->positive_linear[0];
	double last = points->positive_linear[points->count - 1];
	double error;
	if (x <= first) {
		error = first - x;
	} else if (x >= last) {
		error = x - last;
	} else if (step > 0) {
		unsigned lower = (unsigned)((x - first) * inverse_step);
		error = x - (first + lower * step);
		if (error > step * .5)
			error -= step;
		error = fabs(error);
	} else {
		unsigned low = 0, high = points->count - 1;
		while (high - low > 1) {
			unsigned middle = low + (high - low) / 2;
			if (points->positive_linear[middle] < x)
				low = middle;
			else
				high = middle;
		}
		error = fmin(fabs(x - points->positive_linear[low]),
			     fabs(x - points->positive_linear[high]));
	}
	error -= half_width;
	return error > 0 ? error * error : 0;
}

int v92_echo_timing_observe(
	struct v92_echo_timing *timing, struct pcm_host_echo *echo,
	const struct v92_upstream_data_rx *receiver,
	enum pcm_host_echo_mode mode, uint8_t wire,
	int16_t received, int16_t cleaned, bool available)
{
	if (timing == NULL)
		return 0;
	if (echo == NULL || receiver == NULL || !available || receiver->failed ||
	    receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    receiver->line.received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES ||
	    receiver->line.delay_remaining || receiver->input.count || receiver->model.valid || receiver->model.scalar_gain ||
	    (!echo->filter_ready && !echo->retained_delay_samples) ||
	    (mode != PCM_HOST_ECHO_FILTER && mode != PCM_HOST_ECHO_FILTER_DATA) ||
	    (mode == PCM_HOST_ECHO_FILTER_DATA && echo->filter_suspended) ||
	    received == INT16_MIN || received == INT16_MAX ||
	    cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    receiver->line.input_samples == UINT64_MAX) {
		v92_echo_timing_reset(timing);
		return 0;
	}
	const struct v92_upstream_data_line *line = &receiver->line;
	uint32_t delay = echo->retained_delay_samples ? echo->retained_delay_samples :
		pcm_host_echo_window_delay_samples(echo);
	if (!timing->initialized || timing->gain != line->calibration.gain ||
	    timing->offset != line->calibration.offset || timing->polarity != line->polarity ||
	    timing->channel_offset != line->residual.offset ||
	    !same_inverse(&timing->inverse, &line->residual) ||
	    timing->expected_sample != line->input_samples || timing->model_delay != delay)
		configure(timing, line, &receiver->decoder.mapping, delay);
	timing->expected_sample = line->input_samples + 1;
	double projected = ((int32_t)received - cleaned) * line->polarity;
	double corrected = (int32_t)cleaned * line->polarity;
	if (line->residual.active) {
		corrected = v92_residual_output(&line->residual, corrected);
		projected = v92_residual_output(&timing->inverse, projected);
		v92_residual_retain(&timing->inverse, projected);
	}
	/* Match the actual inverse's physical bounds and rounding before calibration. */
	if (!isfinite(projected) || !isfinite(corrected) ||
	    corrected * line->polarity < INT16_MIN || corrected * line->polarity > INT16_MAX) {
		v92_echo_timing_reset(timing);
		return 0;
	}
	double observed = (lrint(corrected * line->polarity) * line->polarity - timing->offset) * timing->gain;
	double midpoint = 0, half_width = 0;
	if (line->host.gain > 0) {
		struct g711_interval cell;
		int16_t decoded = g711_decode(receiver->decoder.mapping.law, wire);
		if (!pcm_gain_contains(&line->host.bounds, decoded, received) ||
		    !g711_quantization_interval(receiver->decoder.mapping.law, decoded, &cell)) {
			v92_echo_timing_reset(timing);
			return 0;
		}
		/* Move the actual wire cell by known echo/residual subtraction.
		 * Held-out input verifies, but never refits, the host gain. */
		double lower = (line->polarity > 0 ? cell.lower : -(double)cell.upper) * line->host.gain;
		double upper = (line->polarity > 0 ? cell.upper : -(double)cell.lower) * line->host.gain;
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
	timing->observation[at] = (float)(observed + midpoint);
	timing->half_width[at] = (float)half_width;
	timing->phase[at] = line->symbol_count % V92_PCM_MAPPING_PHASES;
	++timing->samples;
	if (timing->samples < ECHO_TIMING_BINS || timing->samples % V92_ECHO_TIMING_STRIDE)
		return 0;
	/* Wait for both sides of each hypothesis. Initial inverse-history error
	 * has also contracted through more than 300 inputs before any scoring. */
	unsigned center = (at + V92_ECHO_TIMING_HISTORY - ECHO_TIMING_RADIUS) & mask;
	double x = timing->observation[center] + timing->prediction[center];
	unsigned set = receiver->decoder.mapping.cpd.frame_constellation[timing->phase[center]];
	const struct v92_cpd_constellation *points = &receiver->decoder.mapping.cpd.constellation[set];
	unsigned part = echo_shift_part(&timing->window);
	for (unsigned i = 0; i < ECHO_TIMING_BINS; i++) {
		unsigned shifted = (center + V92_ECHO_TIMING_HISTORY + ECHO_TIMING_RADIUS - i) & mask;
		timing->window.score[part][i] += distance(points, timing->step[set],
			timing->inverse_step[set], timing->half_width[center], x - timing->prediction[shifted]);
	}
	int shift = echo_shift_advance(&timing->window, .5);
	if (shift != 0) {
		bool applied = pcm_host_echo_retime(echo, shift) == PCM_HOST_ECHO_OK;
		v92_echo_timing_reset(timing);
		return applied ? shift : 0;
	}
	return 0;
}
