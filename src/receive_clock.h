// SPDX-License-Identifier: BSD-3-Clause
#ifndef RECEIVE_CLOCK_H
#define RECEIVE_CLOCK_H

#include "frame_clock.h"
#include "frame_pacer.h"

struct rtp_playout_status;

/* One media owner's paced clock. Accepted RTP progress disciplines frequency;
 * individual packet arrivals never directly release a transmit burst. */
struct receive_clock {
	struct frame_clock frames;
	struct frame_pacer ceiling;
	uint64_t nominal_interval_ns, last_time_ns, observation_elapsed_ns;
	size_t reserve_samples, frame_samples;
	unsigned sample_rate;
	double integral, phase[4], phase_sum;
	unsigned phase_cursor;
	uint32_t ssrc, source_timestamp;
	bool source_valid, catching_up, failed;
};

bool receive_clock_init(struct receive_clock *clock, size_t frame_samples,
	unsigned sample_rate, size_t reserve_samples, size_t maximum_burst);
void receive_clock_reset(struct receive_clock *clock);
/* Each OK consumes exactly one frame. Repeated passes retain the same burst
 * bound; backward time cannot renew credit. Overflow latches failure. */
enum frame_clock_result receive_clock_take(struct receive_clock *clock, uint64_t now_ns);
uint64_t receive_clock_wait_ns(const struct receive_clock *clock, uint64_t now_ns);
/* Call after each processed frame. Only new accepted source time updates the
 * loop; missing input holds the last rate. A new SSRC resets frequency, not
 * pacing credit. False means the next deadline is unrepresentable. */
bool receive_clock_observe(struct receive_clock *clock, const struct rtp_playout_status *status);

#endif
