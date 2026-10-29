// SPDX-License-Identifier: BSD-3-Clause
#include "rtp_transmit.h"
#include "frame_pacer.h"
#include "integer_math.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CLOCK_POINTS = 32, CLOCK_POINT_SPAN_NS = 10000000 };
struct clock_point { uint64_t ordinal, time_ns; };
struct packet_slot { size_t size; uint64_t ordinal; };
struct transmit_clock {
	struct clock_point points[CLOCK_POINTS];
	uint64_t frame_ns, reserve_ns, ordinal, last_time_ns, next_deadline_ns;
	uint64_t origin_ns, origin_ordinal, epochs;
	double interval_ns, phase_ns;
	unsigned head, count;
	bool started;
};
struct rtp_transmit {
	struct packet_slot packets[RTP_TRANSMIT_CAPACITY];
	struct transmit_clock clock;
	struct frame_pacer ceiling;
	uint64_t accepted, sent, canceled, last_due_ns;
	unsigned head, count, empty_admissions;
	bool credit_limited, last_due_valid;
	size_t packet_bytes;
	uint8_t storage[];
};

static int compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double median(double *values, unsigned count)
{
	qsort(values, count, sizeof(*values), compare_double);
	return count % 2 ? values[count / 2] :
		(values[count / 2 - 1] + values[count / 2]) * .5;
}

static bool add_signed(uint64_t base, double offset, uint64_t *out)
{
	if (!isfinite(offset) || offset < -(double)INT64_MAX || offset >= (double)INT64_MAX)
		return false;
	/* The fitted timer is quantized by less than one nanosecond. */
	int64_t delta = (int64_t)offset;
	if (delta >= 0) {
		if ((uint64_t)delta > UINT64_MAX - base)
			return false;
		*out = base + (uint64_t)delta;
	} else {
		uint64_t magnitude = (uint64_t)(-(delta + 1)) + 1;
		if (magnitude > base)
			return false;
		*out = base - magnitude;
	}
	return true;
}

static void reset_clock(struct transmit_clock *clock)
{
	clock->ordinal = 0;
	clock->count = clock->head = 0;
	clock->interval_ns = (double)clock->frame_ns;
	clock->phase_ns = 0.;
	clock->epochs = add_u64_saturating(clock->epochs, 1);
}

static bool deadline_at(const struct transmit_clock *clock, uint64_t ordinal, uint64_t *deadline)
{
	double delta = ordinal >= clock->origin_ordinal ?
		(double)(ordinal - clock->origin_ordinal) : -(double)(clock->origin_ordinal - ordinal);
	double offset = clock->phase_ns + clock->interval_ns * delta + clock->reserve_ns;
	if (offset < 0. && -offset > (double)clock->origin_ns) {
		*deadline = 0;
		return true;
	}
	return add_signed(clock->origin_ns, offset, deadline);
}

/* Median pairwise slope rejects isolated delayed arrivals. The upper-quartile
 * phase keeps regularly coalesced batches from consuming the reserve. Sample
 * at most once per 10 ms of media, independently of packet duration. Rebase
 * differences within the bounded window rather than fitting absolute clocks. */
static bool observe(struct transmit_clock *clock, uint64_t now_ns, uint64_t *deadline)
{
	unsigned last = (clock->head + clock->count + CLOCK_POINTS - 1) % CLOCK_POINTS;
	uint64_t step = (CLOCK_POINT_SPAN_NS + clock->frame_ns - 1) / clock->frame_ns;
	if (!clock->count || clock->ordinal - clock->points[last].ordinal >= step) {
		unsigned at = (clock->head + clock->count) % CLOCK_POINTS;
		if (clock->count == CLOCK_POINTS) {
			at = clock->head;
			clock->head = (clock->head + 1) % CLOCK_POINTS;
		} else {
			clock->count++;
		}
		clock->points[at] = (struct clock_point){ clock->ordinal, now_ns };
		const struct clock_point *first = &clock->points[clock->head];
		clock->origin_ns = first->time_ns;
		clock->origin_ordinal = first->ordinal;
		clock->phase_ns = 0.;
		if (clock->count >= 4) {
			double slopes[CLOCK_POINTS * (CLOCK_POINTS - 1) / 2];
			unsigned used = 0;
			for (unsigned i = 0; i < clock->count; i++)
				for (unsigned j = i + 1; j < clock->count; j++) {
					const struct clock_point *a = &clock->points[(clock->head + i) % CLOCK_POINTS];
					const struct clock_point *b = &clock->points[(clock->head + j) % CLOCK_POINTS];
					slopes[used++] = (double)(b->time_ns - a->time_ns) /
						(double)(b->ordinal - a->ordinal);
				}
			clock->interval_ns = median(slopes, used);
			if (clock->interval_ns < clock->frame_ns * .75)
				clock->interval_ns = clock->frame_ns * .75;
			if (clock->interval_ns > clock->frame_ns * 4.)
				clock->interval_ns = clock->frame_ns * 4.;
			double offsets[CLOCK_POINTS];
			for (unsigned i = 0; i < clock->count; i++) {
				const struct clock_point *point = &clock->points[(clock->head + i) % CLOCK_POINTS];
				offsets[i] = (double)(point->time_ns - first->time_ns) -
					clock->interval_ns * (double)(point->ordinal - first->ordinal);
			}
			qsort(offsets, clock->count, sizeof(*offsets), compare_double);
			clock->phase_ns = offsets[3 * clock->count / 4];
		}
	}
	if (!deadline_at(clock, clock->ordinal, deadline))
		return false;
	clock->ordinal++;
	return true;
}

struct rtp_transmit *rtp_transmit_create(uint64_t frame_ns, uint64_t reserve_ns, size_t packet_bytes)
{
	if (frame_ns < 1000000 || frame_ns > 1000000000 || frame_ns % 4 ||
	    reserve_ns > frame_ns * (RTP_TRANSMIT_CAPACITY / 2) || !packet_bytes ||
	    packet_bytes > (SIZE_MAX - sizeof(struct rtp_transmit)) / RTP_TRANSMIT_CAPACITY)
		return NULL;
	struct rtp_transmit *transmit = calloc(1, sizeof(*transmit) + RTP_TRANSMIT_CAPACITY * packet_bytes);
	if (!transmit)
		return NULL;
	transmit->packet_bytes = packet_bytes;
	transmit->clock.frame_ns = frame_ns;
	transmit->clock.reserve_ns = reserve_ns;
	transmit->clock.interval_ns = (double)frame_ns;
	if (!frame_pacer_init(&transmit->ceiling, frame_ns / 4 * 3, 4)) {
		free(transmit);
		return NULL;
	}
	return transmit;
}

void rtp_transmit_destroy(struct rtp_transmit *transmit)
{
	free(transmit);
}

enum rtp_transmit_result rtp_transmit_queue(struct rtp_transmit *transmit,
	const void *packet, size_t size, uint64_t now_ns)
{
	if (!transmit || !packet || !size || size > transmit->packet_bytes ||
	    transmit->clock.ordinal == UINT64_MAX || transmit->accepted == UINT64_MAX ||
	    (transmit->clock.started && now_ns < transmit->clock.last_time_ns))
		return RTP_TRANSMIT_INVALID;
	if (transmit->count == RTP_TRANSMIT_CAPACITY)
		return RTP_TRANSMIT_FULL;
	struct transmit_clock next = transmit->clock;
	bool reset = !next.started || (!transmit->count && now_ns > next.next_deadline_ns);
	if (reset)
		reset_clock(&next);
	uint64_t deadline;
	if (!observe(&next, now_ns, &deadline))
		return RTP_TRANSMIT_INVALID;

	unsigned at = (transmit->head + transmit->count) % RTP_TRANSMIT_CAPACITY;
	transmit->packets[at] = (struct packet_slot){ size, next.ordinal - 1 };
	memcpy(transmit->storage + at * transmit->packet_bytes, packet, size);
	unsigned empty = reset ? 1 : transmit->count ? 0 :
		transmit->empty_admissions < 4 ? transmit->empty_admissions + 1 : 4;
	/* One-credit feedback cannot fund a reserve. Preserve its progress under
	 * the same rate ceiling; retain this decision until explicit clear. */
	transmit->credit_limited |= empty >= 4 && next.interval_ns > next.frame_ns * 1.5;
	transmit->empty_admissions = empty;
	if (reset)
		transmit->last_due_valid = false;
	next.started = true;
	next.last_time_ns = now_ns;
	transmit->clock = next;
	transmit->count++;
	transmit->accepted++;
	return RTP_TRANSMIT_OK;
}

static bool due_at(const struct rtp_transmit *transmit, uint64_t now_ns, uint64_t *due)
{
	if (transmit->credit_limited) {
		*due = now_ns;
		return true;
	}
	if (!deadline_at(&transmit->clock, transmit->packets[transmit->head].ordinal, due))
		return false;
	if (transmit->last_due_valid) {
		uint64_t period = (uint64_t)transmit->clock.interval_ns;
		uint64_t step = transmit->clock.frame_ns / 20;
		uint64_t next = add_u64_saturating(transmit->last_due_ns, period);
		uint64_t low = next > step ? next - step : 0;
		uint64_t high = add_u64_saturating(next, step);
		/* Fit phase may move while packets wait. Slew it by at most 5% of a
		 * nominal frame per send instead of recreating an arrival-time gap. */
		if (*due < low) *due = low;
		if (*due > high) *due = high;
	}
	return true;
}

uint64_t rtp_transmit_wait_ns(const struct rtp_transmit *transmit, uint64_t now_ns)
{
	if (!transmit || !transmit->count)
		return UINT64_MAX;
	uint64_t due;
	if (!due_at(transmit, now_ns, &due))
		return UINT64_MAX;
	uint64_t wait = due > now_ns ? due - now_ns : 0;
	uint64_t ceiling = frame_pacer_wait_ns(&transmit->ceiling, now_ns);
	return wait > ceiling ? wait : ceiling;
}

const uint8_t *rtp_transmit_peek(const struct rtp_transmit *transmit, uint64_t now_ns, size_t *size)
{
	if (size) *size = 0;
	if (!size || rtp_transmit_wait_ns(transmit, now_ns) != 0)
		return NULL;
	*size = transmit->packets[transmit->head].size;
	return transmit->storage + transmit->head * transmit->packet_bytes;
}

bool rtp_transmit_sent(struct rtp_transmit *transmit, uint64_t now_ns)
{
	if (rtp_transmit_wait_ns(transmit, now_ns) != 0)
		return false;
	uint64_t due;
	if (!due_at(transmit, now_ns, &due) || !frame_pacer_take(&transmit->ceiling, now_ns))
		return false;
	uint64_t interval = (uint64_t)transmit->clock.interval_ns;
	transmit->last_due_ns = due;
	transmit->last_due_valid = true;
	transmit->clock.next_deadline_ns = add_u64_saturating(due, interval);
	transmit->head = (transmit->head + 1) % RTP_TRANSMIT_CAPACITY;
	transmit->count--;
	transmit->sent++;
	return true;
}

size_t rtp_transmit_clear(struct rtp_transmit *transmit)
{
	if (!transmit)
		return 0;
	size_t canceled = transmit->count;
	transmit->canceled = add_u64_saturating(transmit->canceled, canceled);
	transmit->head = transmit->count = transmit->empty_admissions = 0;
	transmit->credit_limited = transmit->last_due_valid = false;
	transmit->clock.count = transmit->clock.head = 0;
	transmit->clock.ordinal = transmit->clock.next_deadline_ns = 0;
	transmit->clock.started = false;
	frame_pacer_reset(&transmit->ceiling);
	return canceled;
}

struct rtp_transmit_status rtp_transmit_get_status(const struct rtp_transmit *transmit)
{
	if (!transmit)
		return (struct rtp_transmit_status){ 0 };
	return (struct rtp_transmit_status){
		.accepted = transmit->accepted, .sent = transmit->sent, .canceled = transmit->canceled,
		.clock_epochs = transmit->clock.epochs,
		.interval_ns = (uint64_t)transmit->clock.interval_ns,
		.queued = transmit->count, .credit_limited = transmit->credit_limited,
	};
}
