// SPDX-License-Identifier: BSD-3-Clause

#include "v92_b1_echo.h"
#include "pam_distance.h"
#include "gram_fit.h"
#include "sample_math.h"
#include "v92_upstream_data_tx.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* Select a delay on acquisition only. Each later 200-input window must
 * independently prefer it to every other delay. These local guards do not
 * supply a statistical false-trigger bound for correlated channel samples. */
enum {
	RADIUS = V92_B1_ECHO_RADIUS, BINS = V92_B1_ECHO_BINS,
	B1_SAMPLES = V92_B1_ECHO_SAMPLES,
	FIT_FIRST = 24, FIT_END = 128, FIT_READY = FIT_END + 16,
	FIT_SAMPLES = FIT_END - FIT_FIRST, HELD = 200,
	SCORE_END = FIT_READY + 2 * HELD, CONSTELLATION_MODEL = 3,
};
enum { IDLE, ACTIVE, FINISHED, COMMITTED };
_Static_assert(SCORE_END + 24 <= B1_SAMPLES, "holdout exceeds known B1u");

static bool find_alignment(struct v92_b1_echo_fit *fit, const int16_t *ideal, const int16_t *host)
{
	double sx = 0., xx = 0.;
	for (unsigned i = FIT_FIRST; i < FIT_END; ++i) {
		sx += ideal[i];
		xx += (double)ideal[i] * ideal[i];
	}
	xx -= sx * sx / FIT_SAMPLES;
	if (xx <= 0.)
		return false;
	for (int offset = -24; offset <= 16; ++offset) {
		double sy = 0., yy = 0., xy = 0.;
		for (unsigned i = FIT_FIRST; i < FIT_END; ++i) {
			double y = host[(int)i + offset];
			sy += y;
			yy += y * y;
			xy += ideal[i] * y;
		}
		yy -= sy * sy / FIT_SAMPLES;
		xy -= sx * sy / FIT_SAMPLES;
		double correlation = yy > 0. ? xy * xy / (xx * yy) : 0.;
		if (correlation > fit->correlation_squared) {
			fit->correlation_squared = correlation;
			fit->alignment = offset;
		}
	}
	return fit->correlation_squared >= .98;
}

static bool linear_fit(const int16_t *x, const int16_t *y, double *beta)
{
	double moments[GRAM_FIT_MOMENTS] = {0};
	double rhs[GRAM_FIT_COLUMNS] = {0};
	for (unsigned i = FIT_FIRST; i < FIT_END; ++i) {
		double row[] = {1., x[i], x[i-1]};
		gram_accumulate(moments, rhs, row, 3, y[i]);
	}
	return gram_fit(moments, rhs, 3, beta);
}

static int16_t quantize(enum g711_law law, double x)
{
	x = x < -32768. ? -32768. : x > 32767. ? 32767. : x;
	return g711_quantize(law, (int16_t)lrint(x));
}

struct cell {
	int16_t x;
	unsigned count;
	double sum;
};

static bool tandem(const int16_t *x, const int16_t *y, double postcursor,
	enum g711_law law, int16_t *target)
{
	struct cell cells[FIT_SAMPLES];
	unsigned used = 0;
	double sx = 0., sy = 0., xx = 0., xy = 0.;
	for (unsigned i = FIT_FIRST; i < FIT_END; ++i) {
		double r = y[i] - postcursor * x[i-1];
		sx += x[i];
		sy += r;
		xx += (double)x[i] * x[i];
		xy += r * x[i];
		unsigned j = 0;
		for (; j < used && cells[j].x != x[i]; ++j)
			;
		if (j == used) {
			cells[j] = (struct cell){ .x = x[i] };
			++used;
		}
		++cells[j].count;
		cells[j].sum += r;
	}
	double variance = xx - sx * sx / FIT_SAMPLES;
	if (variance <= 0.)
		return false;
	double gain = (xy - sx * sy / FIT_SAMPLES) / variance;
	double dc = (sy - gain * sx) / FIT_SAMPLES;
	if (!isfinite(gain) || !isfinite(dc) || fabs(gain) > 32768. || fabs(dc) > 1e9)
		return false;
	double best = DBL_MAX, best_gain = gain, best_dc = dc;
	for (int g = -16; g <= 16; ++g) {
		double selected_gain = gain * (1. + g * .001);
		double base[FIT_SAMPLES];
		for (unsigned i = 0; i < used; ++i)
			base[i] = selected_gain * cells[i].x;
		for (int c = -12; c <= 12; ++c) {
			double selected_dc = dc + c * 4.;
			double error = 0.;
			for (unsigned i = 0; i < used; ++i) {
				double q = quantize(law, base[i] + selected_dc);
				error += (cells[i].count * q - 2. * cells[i].sum) * q;
			}
			if (error < best) {
				best = error;
				best_gain = selected_gain;
				best_dc = selected_dc;
			}
		}
	}
	for (unsigned i = 0; i < B1_SAMPLES; ++i)
		target[i] = quantize(law, best_gain * x[i] + best_dc);
	return true;
}

static const double *prediction_row(const struct v92_b1_echo_fit *fit,
	const double *prediction, unsigned i)
{
	return prediction + (int)i + fit->alignment;
}

/* Score a raw-coordinate B1u against the mapping's signed alphabet. The
 * precoder may select different representatives; no prescribed waveform is
 * used as a reference. Geometry and coordinates stay frozen with the copy. */
static double raw_cost(const struct v92_b1_echo *recovery, unsigned i,
	int16_t raw, double prediction)
{
	const struct v92_pcm_mapping *mapping = &recovery->receiver.decoder.mapping;
	unsigned phase = mapping->cpd.frame_constellation[i % V92_PCM_MAPPING_PHASES];
	const struct v92_cpd_constellation *points = &mapping->cpd.constellation[phase];
	double gain = recovery->receiver.line.plan.gain;
	return pam_distance_squared(points->positive_linear, points->count,
		recovery->step[phase], recovery->inverse_step[phase], 0.,
		(raw - prediction) * gain, recovery->ranges[phase]) / (gain * gain);
}

static bool fit_raw(struct v92_b1_echo *recovery)
{
	struct v92_b1_echo_fit *fit = &recovery->fit;
	*fit = (struct v92_b1_echo_fit){ .model = CONSTELLATION_MODEL };
	double cost[BINS] = {0};
	for (unsigned i = FIT_FIRST; i < FIT_END; ++i)
		for (unsigned j = 0; j < BINS; ++j)
			cost[j] += raw_cost(recovery, i, recovery->raw[i],
				recovery->prediction[i + 2 * RADIUS - j]);
	unsigned chosen = 0;
	for (unsigned j = 1; j < BINS; ++j)
		if (cost[j] < cost[chosen])
			chosen = j;
	unsigned runner = chosen ? 0 : 1;
	for (unsigned j = 0; j < BINS; ++j)
		if (j != chosen && cost[j] < cost[runner])
			runner = j;
	fit->shift = (int)chosen - RADIUS;
	fit->runner = (int)runner - RADIUS;
	return isfinite(cost[chosen]);
}

static bool fit_delay(struct v92_b1_echo_fit *fit, const int16_t *ideal, const int16_t *host,
	const double *prediction, enum g711_law law)
{
	*fit = (struct v92_b1_echo_fit){ 0 };
	if (!find_alignment(fit, ideal, host))
		return false;
	int16_t y[FIT_END];
	for (unsigned i = FIT_FIRST; i < FIT_END; ++i)
		y[i] = host[(int)i + fit->alignment];
	double beta[3], quant_beta[3];
	int16_t codec[B1_SAMPLES], post_codec[B1_SAMPLES];
	if (!linear_fit(ideal, y, beta) || !tandem(ideal, y, 0., law, codec) ||
		!tandem(ideal, y, beta[2], law, post_codec) || !linear_fit(post_codec, y, quant_beta))
		return false;
	double targets[3][B1_SAMPLES];
	double costs[3][BINS] = {{0}};
	for (unsigned i = 0; i < B1_SAMPLES; ++i) {
		unsigned previous = i ? i - 1 : B1_SAMPLES - 1;
		targets[0][i] = codec[i];
		targets[1][i] = quant_beta[0] + quant_beta[1]*post_codec[i] + quant_beta[2]*post_codec[previous];
		targets[2][i] = beta[0] + beta[1]*ideal[i] + beta[2]*ideal[previous];
	}
	for (unsigned i = FIT_FIRST; i < FIT_END; ++i) {
		const double *p = prediction_row(fit, prediction, i);
		for (unsigned m = 0; m < 3; ++m) {
			double residual = y[i] - targets[m][i];
			for (unsigned j = 0; j < BINS; ++j) {
				double e = residual - p[2 * RADIUS - j];
				costs[m][j] += e * e;
			}
		}
	}
	double best = DBL_MAX;
	for (unsigned m = 0; m < 3; ++m) {
		for (unsigned j = 0; j < BINS; ++j) {
			double cost = costs[m][j] * (m ? 1.03 : 1.);
			if (cost < best) {
				best = cost;
				fit->model = m;
				fit->shift = (int)j - RADIUS;
			}
		}
	}
	unsigned chosen = fit->shift + RADIUS;
	unsigned runner = chosen ? 0 : 1;
	for (unsigned j = 0; j < BINS; ++j)
		if (j != chosen && costs[fit->model][j] < costs[fit->model][runner])
			runner = j;
	fit->runner = (int)runner - RADIUS;
	for (unsigned i = 0; i < B1_SAMPLES; ++i)
		fit->target[i] = targets[fit->model][i];
	return isfinite(best);
}

static void observe_delay(const struct v92_b1_echo *recovery, struct v92_b1_echo_evidence *ev,
	const double *prediction, int16_t raw, unsigned i)
{
	const struct v92_b1_echo_fit *fit = &recovery->fit;
	const double *p = prediction_row(fit, prediction, i);
	if (fit->model == CONSTELLATION_MODEL) {
		double error[BINS];
		for (unsigned j = 0; j < BINS; ++j) {
			error[j] = raw_cost(recovery, i, raw, p[2 * RADIUS - j]);
			ev->score[j] += error[j];
		}
		double good = error[fit->shift + RADIUS];
		double old = error[RADIUS] - good, runner = error[fit->runner + RADIUS] - good;
		ev->old_sum += old;
		ev->old_square += old * old;
		ev->runner_sum += runner;
		ev->runner_square += runner * runner;
		++ev->count;
		return;
	}

	double r = raw - fit->target[i];
	double good = r - p[RADIUS - fit->shift];
	good *= good;
	double old = r - p[RADIUS];
	double runner = r - p[RADIUS - fit->runner];
	old = old*old - good;
	runner = runner*runner - good;
	ev->old_sum += old;
	ev->old_square += old*old;
	ev->runner_sum += runner;
	ev->runner_square += runner*runner;
	for (unsigned j = 0; j < BINS; ++j) {
		double error = r - p[2*RADIUS-j];
		ev->score[j] += error*error;
	}
	++ev->count;
}

static double evidence_z(double sum, double square, unsigned n)
{
	return sum / sqrt(fmax(1e-18*n, square - sum*sum/n));
}

static bool confirm_delay(const struct v92_b1_echo_fit *fit, const struct v92_b1_echo_evidence *ev,
	double *old_z, double *runner_z)
{
	if (ev->count != HELD)
		return false;
	unsigned selected = fit->shift + RADIUS;
	*old_z = evidence_z(ev->old_sum, ev->old_square, ev->count);
	*runner_z = evidence_z(ev->runner_sum, ev->runner_square, ev->count);
	for (unsigned i = 0; i < BINS; ++i)
		if (i != selected && ev->score[i] <= ev->score[selected])
			return false;
	return (!fit->shift || *old_z > 1.5) && *runner_z > 1.5;
}

void v92_b1_echo_reset(struct v92_b1_echo *recovery)
{
	if (recovery->stage == IDLE)
		return;
	/* Keep block credit when changing epoch inside the block. */
	unsigned budget = recovery->budget;
	memset(recovery, 0, offsetof(struct v92_b1_echo, receiver));
	recovery->budget = budget;
}

void v92_b1_echo_begin_block_owned(struct v92_b1_echo *recovery, unsigned samples)
{
	recovery->budget = (unsigned)(((uint64_t)samples * 13 + 9) / 10);
}

static const float *frozen_coefficients(const struct pcm_host_echo *echo)
{
	return echo->retained_delay_samples ? echo->retained_coefficients : echo->coefficients;
}

static bool start(struct v92_b1_echo *recovery, const struct pcm_host_echo *echo,
	const struct v92_upstream_data_rx *receiver)
{
	uint32_t delay = pcm_host_echo_active_delay_owned(echo, PCM_HOST_ECHO_FILTER);
	if ((receiver->line.plan.observation != V92_PCM_FRAME_FILTER_OUTPUT &&
	     receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION) ||
	    receiver->failed || receiver->line.received_frames || receiver->line.symbol_count ||
	    receiver->line.delay_remaining || receiver->input.count || delay <= RADIUS)
		return false;
	v92_b1_echo_reset(recovery);
	recovery->clock = receiver->line.input_samples;
	recovery->delay = delay;
	recovery->retained = echo->retained_delay_samples != 0;
	memcpy(recovery->coefficients, frozen_coefficients(echo), sizeof(recovery->coefficients));
	/* All receive state is pointer-free. Disable the primary's frame-work
	 * credit in this isolated replay; the explicit replay budget below owns
	 * its bounded work. Primary credit is restored on publication. */
	recovery->receiver = *receiver;
	recovery->receiver.input.frame_limited = false;
	recovery->receiver.input.frame_budget = recovery->receiver.input.frame_credit = 0;
	if (receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION) {
		for (unsigned c = 0; c < V92_CPD_MAX_CONSTELLATIONS; ++c) {
			const struct v92_cpd_constellation *points = &receiver->decoder.mapping.cpd.constellation[c];
			pam_distance_prepare(points->positive_linear, points->count,
				&recovery->step[c], &recovery->inverse_step[c], recovery->ranges[c]);
		}
	} else {
		struct v92_upstream_data_tx tx;
		if (v92_upstream_data_tx_init(&tx, &receiver->decoder.mapping,
		    receiver->decoder.upstream_level) != V92_UPSTREAM_DATA_TX_OK)
			return false;
		for (unsigned i = 0; i < B1_SAMPLES; ++i)
			if (v92_upstream_data_tx_next(&tx, NULL, NULL, &recovery->ideal[i]) != V92_UPSTREAM_DATA_TX_OK)
				return false;
	}

	recovery->stage = ACTIVE;
	return true;
}

static bool model_unchanged(const struct v92_b1_echo *recovery, const struct pcm_host_echo *echo)
{
	return echo != NULL && recovery->delay == pcm_host_echo_active_delay_owned(echo, PCM_HOST_ECHO_FILTER) &&
	       recovery->retained == (echo->retained_delay_samples != 0) &&
	       !memcmp(recovery->coefficients, frozen_coefficients(echo), sizeof(recovery->coefficients));
}

static bool predict_at(struct v92_b1_echo *recovery, const struct pcm_host_echo *echo,
	unsigned now, int target, double *prediction)
{
	int64_t delay = (int64_t)recovery->delay + now - target;
	return delay > 0 && delay <= PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES &&
	       pcm_host_echo_project_owned(echo, (uint32_t)delay, prediction) == PCM_HOST_ECHO_OK;
}

static bool capture(struct v92_b1_echo *recovery, const struct pcm_host_echo *echo,
	unsigned i, uint8_t code, int16_t raw, int16_t cleaned)
{
	recovery->raw[i] = raw;
	recovery->cleaned[i] = cleaned;
	recovery->code[i] = code;
	/* Capture the initial shifted prediction prefix over 80 inputs. Future
	 * received times use existing past TX: the frozen delay exceeds radius.
	 * One new far-end prediction then extends the range on every input. */
	for (unsigned work = 0; work < 8 && recovery->prefix < 2 * RADIUS; ++work, ++recovery->prefix)
		if (!predict_at(recovery, echo, i, (int)recovery->prefix - RADIUS,
			    &recovery->prediction[recovery->prefix]))
			return false;
	if (!predict_at(recovery, echo, i, (int)i + RADIUS, &recovery->prediction[2 * RADIUS + i]))
		return false;
	if (recovery->prefix > i + RADIUS &&
	    sample_round_s16(raw - recovery->prediction[i + RADIUS]) != cleaned)
		return false;
	return true;
}

static bool replay_one(struct v92_b1_echo *recovery, unsigned i)
{
	struct v92_upstream_data_frame frame;
	int16_t cleaned = sample_round_s16(recovery->raw[i] -
		recovery->prediction[(int)i + RADIUS - recovery->fit.shift]);
	enum v92_upstream_data_rx_result result = v92_upstream_data_rx_octet_owned(
		&recovery->receiver, recovery->code[i], cleaned, recovery->raw[i], true, &frame);
	if (result == V92_UPSTREAM_DATA_RX_OK)
		return true;
	return result == V92_UPSTREAM_DATA_RX_FRAME &&
	       v92_b1_quality_observe_owned(&recovery->quality, recovery->receiver.line.received_frames,
		       recovery->receiver.line.host.gain, &frame) && !recovery->quality.failed;
}

bool v92_b1_echo_observe_owned(struct v92_b1_echo *recovery,
	const struct pcm_host_echo *echo, const struct v92_upstream_data_rx *receiver,
	uint8_t code, int16_t raw, int16_t cleaned, bool available)
{
	/* A new zero-frame handoff can begin another bounded attempt. A declined
	 * attempt at this exact clock is not retried on each subsequent input. */
	if (recovery->stage == FINISHED && !receiver->line.received_frames &&
	    !receiver->line.symbol_count && receiver->line.input_samples != recovery->clock)
		v92_b1_echo_reset(recovery);
	if (recovery->stage == FINISHED || recovery->stage == COMMITTED)
		return false;
	if (recovery->stage == IDLE && !start(recovery, echo, receiver))
		goto decline;
	if (!available || raw == INT16_MIN || raw == INT16_MAX ||
	    recovery->count >= V92_B1_ECHO_INPUTS || receiver->failed || receiver->input.count ||
	    receiver->line.input_samples != recovery->clock + recovery->count ||
	    receiver->line.plan.observation != recovery->receiver.line.plan.observation ||
	    receiver->line.plan.gain != recovery->receiver.line.plan.gain ||
	    receiver->line.plan.delay != recovery->receiver.line.plan.delay ||
	    receiver->line.polarity != recovery->receiver.line.polarity || !model_unchanged(recovery, echo))
		goto decline;
	unsigned i = recovery->count++;
	if (!capture(recovery, echo, i, code, raw, cleaned))
		goto decline;
	if (recovery->count == FIT_READY) {
		for (unsigned k = 0; k < FIT_READY; ++k)
			if (sample_round_s16(recovery->raw[k] - recovery->prediction[k + RADIUS]) != recovery->cleaned[k])
				goto decline;
		if (!(receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION ?
		    fit_raw(recovery) :
		    fit_delay(&recovery->fit, recovery->ideal, recovery->raw, recovery->prediction,
			    receiver->decoder.mapping.law)) || !recovery->fit.shift)
			goto decline;
	}
	if (i >= FIT_READY && i < SCORE_END) {
		unsigned h = (i - FIT_READY) / HELD;
		observe_delay(recovery, &recovery->held[h], recovery->prediction, raw,
			(unsigned)((int)i - recovery->fit.alignment));
		if (recovery->held[h].count == HELD) {
			double old_z, runner_z;
			if (!confirm_delay(&recovery->fit, &recovery->held[h], &old_z, &runner_z))
				goto decline;
		}
	}
	if (recovery->count < FIT_READY)
		return false;
	/* Start in the fit's own block. Waiting for the next packet can prevent
	 * catching up by the native delayed B1u verdict at 696 physical inputs. */
	for (unsigned work = 0; work < 2 && recovery->replayed < recovery->count && recovery->budget; ++work) {
		--recovery->budget;
		if (!replay_one(recovery, recovery->replayed++))
			goto decline;
	}
	if (recovery->count != V92_B1_ECHO_INPUTS)
		return false;
	recovery->stage = FINISHED;
	return recovery->replayed == recovery->count &&
	       recovery->held[0].count == HELD && recovery->held[1].count == HELD &&
	       !recovery->receiver.input.count &&
	       v92_b1_quality_is_ready(&recovery->quality, recovery->receiver.line.received_frames);

decline:
	recovery->stage = FINISHED;
	return false;
}

bool v92_b1_echo_accept_owned(struct v92_b1_echo *recovery, struct pcm_host_echo *echo)
{
	if (recovery->stage != FINISHED || recovery->count != V92_B1_ECHO_INPUTS ||
	    recovery->replayed != recovery->count || recovery->held[0].count != HELD ||
	    recovery->held[1].count != HELD || recovery->receiver.failed || recovery->receiver.input.count ||
	    !v92_b1_quality_is_ready(&recovery->quality, recovery->receiver.line.received_frames) ||
	    !model_unchanged(recovery, echo) ||
	    pcm_host_echo_retime(echo, recovery->fit.shift) != PCM_HOST_ECHO_OK)
		return false;
	recovery->stage = COMMITTED;
	return true;
}
