// SPDX-License-Identifier: BSD-3-Clause
#include "receive_clock.h"
#include "rtp_receiver.h"

#include <string.h>

bool receive_clock_init(struct receive_clock *clock, size_t frame_samples,
	unsigned sample_rate, size_t reserve_samples, size_t maximum_burst)
{
	struct receive_clock next = { 0 };
	if (!clock || !reserve_samples || reserve_samples < frame_samples ||
	    frame_clock_init(&next.frames, frame_samples, sample_rate, 1) != FRAME_CLOCK_OK ||
	    next.frames.interval_ns % 4 != 0 ||
	    !frame_pacer_init(&next.ceiling, next.frames.interval_ns / 4 * 3, maximum_burst))
		return false;
	next.nominal_interval_ns = next.frames.interval_ns;
	next.reserve_samples = reserve_samples;
	next.frame_samples = frame_samples;
	next.sample_rate = sample_rate;
	*clock = next;
	return true;
}

void receive_clock_reset(struct receive_clock *clock)
{
	if (!clock) return;
	frame_clock_stop(&clock->frames);
	clock->frames.interval_ns = clock->nominal_interval_ns;
	frame_pacer_reset(&clock->ceiling);
	clock->last_time_ns = clock->observation_elapsed_ns = 0;
	clock->integral = clock->phase_sum = 0;
	memset(clock->phase, 0, sizeof(clock->phase));
	clock->phase_cursor = 0;
	clock->source_valid = clock->catching_up = clock->failed = false;
}

uint64_t receive_clock_wait_ns(const struct receive_clock *clock, uint64_t now_ns)
{
	if (!clock || !clock->nominal_interval_ns || clock->failed) return UINT64_MAX;
	if (now_ns < clock->last_time_ns) now_ns = clock->last_time_ns;
	uint64_t wait = clock->frames.running ? frame_clock_wait_ns(&clock->frames, now_ns) : 0;
	uint64_t pacing = frame_pacer_wait_ns(&clock->ceiling, now_ns);
	return wait > pacing ? wait : pacing;
}

enum frame_clock_result receive_clock_take(struct receive_clock *clock, uint64_t now_ns)
{
	if (!clock || !clock->nominal_interval_ns) return FRAME_CLOCK_INVALID_ARGUMENT;
	if (clock->failed) return FRAME_CLOCK_OVERRUN;
	if (now_ns < clock->last_time_ns) now_ns = clock->last_time_ns;
	clock->last_time_ns = now_ns;
	if (!clock->frames.running) frame_clock_start(&clock->frames, now_ns);
	if (receive_clock_wait_ns(clock, now_ns) != 0) return FRAME_CLOCK_NOT_DUE;
	clock->catching_up = now_ns - clock->frames.next_deadline_ns >= clock->frames.interval_ns;
	size_t frames;
	enum frame_clock_result result = frame_clock_take_due(&clock->frames, now_ns, &frames);
	if (result == FRAME_CLOCK_OK) {
		(void)frame_pacer_take(&clock->ceiling, now_ns);
		uint64_t interval = clock->frames.interval_ns;
		clock->observation_elapsed_ns = interval > UINT64_MAX - clock->observation_elapsed_ns ?
			UINT64_MAX : clock->observation_elapsed_ns + interval;
	}
	else if (result == FRAME_CLOCK_OVERRUN) clock->failed = true;
	return result;
}

bool receive_clock_observe(struct receive_clock *clock, const struct rtp_playout_status *status)
{
	if (!clock || !status || !clock->frames.running || clock->failed) return false;
	if (!status->stream_active) return true;
	bool new_source = !clock->source_valid || clock->ssrc != status->ssrc;
	if (!new_source && clock->source_timestamp == status->source_timestamp) return true;
	clock->source_valid = true;
	clock->ssrc = status->ssrc;
	clock->source_timestamp = status->source_timestamp;
	double elapsed = clock->observation_elapsed_ns / 1e9;
	clock->observation_elapsed_ns = 0;
	/* The retained deadline already drains an owner's scheduling backlog.
	 * Treating that queued media as oscillator drift corrects it twice and
	 * can empty the receive reserve after the deadline has caught up. */
	if (clock->catching_up && !new_source) return true;
	uint64_t old = clock->frames.interval_ns;
	double rate = 1;
	if (new_source) {
		clock->integral = clock->phase_sum = 0;
		memset(clock->phase, 0, sizeof(clock->phase));
		clock->phase_cursor = 0;
	} else {
		/* Observe after the whole local block, following the last accepted
		 * packet. Account for both block sizes: packet batching is not drift. */
		double target = clock->reserve_samples;
		if (status->source_packet_samples)
			target += (double)status->source_packet_samples - clock->frame_samples;
		double error = ((double)status->clock_lead_samples - target) / clock->sample_rate;
		double period = old / 1e9;
		if (status->source_packet_samples > clock->frame_samples)
			period *= (double)status->source_packet_samples / clock->frame_samples;
		/* An observation can span several local frames, but a media outage
		 * must not accumulate a large frequency-integrator correction. */
		if (elapsed > period) elapsed = period;
		clock->phase_sum -= clock->phase[clock->phase_cursor];
		clock->phase[clock->phase_cursor] = error;
		clock->phase_sum += error;
		clock->phase_cursor = (clock->phase_cursor + 1) % 4;
		error = clock->phase_sum / 4;
		/* Queue error obeys d(error)/dt = source_rate - output_rate.
		 * Kp=2*w, Ki=w*w gives a critically damped loop: w=.025/s
		 * for oscillator drift, up to w=10/s outside a 20 ms phase band.
		 * Limit the fast pole to 1/(8 observation periods) for larger
		 * packets. Four-observation averaging attenuates packet jitter.
		 * Clamp pull to [3/4, 4/3]; no waveform sample is resampled. */
		double excess = error > .020 ? error - .020 : error < -.020 ? error + .020 : 0;
		double fast = period * 10 <= .125 ? 10 : .125 / period;
		clock->integral += elapsed * (error * .000625 + excess * fast * fast);
		if (clock->integral > 1.0 / 3) clock->integral = 1.0 / 3;
		if (clock->integral < -.25) clock->integral = -.25;
		rate += clock->integral + error * .05 + excess * 2 * fast;
		if (rate > 4.0 / 3) rate = 4.0 / 3;
		if (rate < .75) rate = .75;
	}
	double desired = clock->nominal_interval_ns / rate + .5;
	if (desired >= (double)UINT64_MAX) goto overflow;
	uint64_t interval = (uint64_t)desired;
	uint64_t anchor = clock->frames.next_deadline_ns - old;
	if (interval > UINT64_MAX - anchor) goto overflow;
	clock->frames.interval_ns = interval;
	clock->frames.next_deadline_ns = anchor + interval;
	return true;

overflow:
	clock->failed = true;
	frame_clock_stop(&clock->frames);
	return false;
}
