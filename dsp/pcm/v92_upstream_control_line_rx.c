// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_control_line_rx.h"

#include "packed_bits.h"

#include <stddef.h>
#include <stdint.h>

static bool line_state_is_valid(
	const struct v92_upstream_control_line_rx *receiver)
{
	unsigned int expected_alignment =
		receiver->signal.line.demodulator.constellation ==
				V92_CONSTELLATION_4_POINT ?
			V92_CONTROL_UPSTREAM_4_POINT_BITS :
			V92_CONTROL_UPSTREAM_8_POINT_BITS;

	return v92_upstream_rx_is_valid(&receiver->signal) &&
	       v92_control_rx_is_valid(&receiver->control) &&
	       receiver->control.direction == V92_CONTROL_UPSTREAM &&
	       receiver->control.alignment_bits == expected_alignment &&
	       (!receiver->retained_cpu_bits ||
		(receiver->retained_cpu_bits >= V92_CPU_MIN_FRAME_BITS &&
		 receiver->retained_cpu_bits <= V92_CPU_MAX_FRAME_BITS &&
		 receiver->retained_cpu_bits % expected_alignment == 0u)) &&
	       receiver->repeat_symbols <= V92_CPU_MAX_FRAME_BITS /
		       (expected_alignment / V92_CPU_SYMBOLS_PER_FRAME) &&
	       receiver->repeat_symbols % V92_CPU_SYMBOLS_PER_FRAME == 0u &&
	       receiver->repeat_remaining <= receiver->repeat_symbols &&
	       (!receiver->repeat_confirmed || receiver->repeat_symbols != 0u);
}

bool v92_upstream_control_line_rx_is_valid(
	const struct v92_upstream_control_line_rx *receiver)
{
	return receiver != NULL && line_state_is_valid(receiver);
}

bool v92_upstream_control_line_rx_has_failed(
	const struct v92_upstream_control_line_rx *receiver)
{
	return v92_upstream_control_line_rx_is_valid(receiver) &&
	       receiver->signal.failed;
}

enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_init(
	struct v92_upstream_control_line_rx *receiver,
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct v92_upstream_control_line_rx initialized = { 0 };

	if (receiver == NULL ||
	    v92_upstream_rx_init(&initialized.signal, handoff) !=
		    V92_UPSTREAM_RX_OK)
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT;
	if (v92_control_rx_init_upstream(
		    &initialized.control,
		    initialized.signal.line.demodulator.constellation ==
			    V92_CONSTELLATION_4_POINT ?
			    V92_CONTROL_UPSTREAM_4_POINT_BITS :
			    V92_CONTROL_UPSTREAM_8_POINT_BITS) !=
		    V92_CONTROL_RX_OK)
		return V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V92_UPSTREAM_CONTROL_LINE_RX_OK;
}

static enum v92_upstream_control_line_rx_result fail(
	struct v92_upstream_control_line_rx *receiver)
{
	receiver->signal.failed = true;
	return V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR;
}

static bool receive_group(
	struct v92_control_rx *control, uint8_t group, unsigned int bits,
	struct v92_control_event *event)
{
	struct v92_control_event emitted = { 0 };

	for (unsigned int bit = 0u; bit < bits; ++bit) {
		struct v92_control_event current;

		if (v92_control_rx_receive_bit(
			    control, (uint8_t)((group >> bit) & 1u),
			    &current) != V92_CONTROL_RX_OK)
			return false;
		if (current.type == V92_CONTROL_EVENT_NONE)
			continue;
		if (emitted.type != V92_CONTROL_EVENT_NONE)
			return false;
		emitted = current;
	}
	*event = emitted;
	return true;
}

static void track_repetition(struct v92_upstream_control_line_rx *receiver,
	const struct v92_control_event *event, unsigned bits_per_symbol, size_t frame_bits)
{
	if (receiver->repeat_symbols) {
		if (!receiver->repeat_remaining)
			receiver->repeat_remaining = receiver->repeat_symbols;
		--receiver->repeat_remaining;
	}
	/* A rejected status cannot replace a confirmed record period with
	 * an interior boundary, even on a twelve-symbol frame. Keep the clock
	 * for the real record end/E2u. */
	if (event->type == V92_CONTROL_EVENT_SUVU_REJECTED &&
	    receiver->control.record_boundary && receiver->repeat_confirmed &&
	    receiver->repeat_remaining != 0u) {
		receiver->control.record_boundary = false;
		return;
	}
	if (event->type == V92_CONTROL_EVENT_SUVU ||
	    event->type == V92_CONTROL_EVENT_CPU || event->type == V92_CONTROL_EVENT_CPUS) {
		/* The decoded CPu canonicalizes duplicate/unused constellation rows.
		 * Only the parsed wire extent is a valid repetition clock. */
		uint16_t symbols = (uint16_t)(frame_bits / bits_per_symbol);
		receiver->repeat_confirmed = symbols == receiver->repeat_symbols &&
			receiver->repeat_remaining == 0u;
		receiver->repeat_symbols = symbols;
		receiver->repeat_remaining = 0u;
	}
}

enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_sample(
	struct v92_upstream_control_line_rx *receiver, int16_t sample,
	struct v92_control_event *event)
{
	struct v92_upstream_control_line_rx next;
	struct v92_control_event emitted = { 0 };
	struct v92_upstream_decision decision;
	enum v92_upstream_rx_result signal_result;
	unsigned int bits;

	if (!v92_upstream_control_line_rx_is_valid(receiver) || event == NULL)
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT;
	if (receiver->signal.failed)
		return V92_UPSTREAM_CONTROL_LINE_RX_FAILED;
	next = *receiver;
	signal_result = v92_upstream_rx_sample(
		&next.signal, sample, &decision);
	if (signal_result == V92_UPSTREAM_RX_FAILED)
		return V92_UPSTREAM_CONTROL_LINE_RX_FAILED;
	if (signal_result == V92_UPSTREAM_RX_INVALID_STATE)
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE;
	if (signal_result != V92_UPSTREAM_RX_OK)
		return fail(receiver);
	bits = v92_phase4_modulator_bits_per_symbol(
		next.signal.line.demodulator.constellation);
	if (bits == 0u || !receive_group(
				  &next.control, decision.group, bits, &emitted))
		return fail(receiver);
	/* CPu selection is fixed for this exchange once its owner accepts it.
	 * A damaged row-count header must not hold the parser past the next
	 * confirmed repeat boundary and swallow E2u. Discard the bad candidate;
	 * never substitute its length or accept its unverified parameters. Short
	 * CPus and SUVu records retain their ordinary framing. */
	if (next.retained_cpu_bits &&
	    next.control.stage == V92_CONTROL_RX_RECEIVE_PARAMETERS &&
	    next.control.frame_bit_count >= V92_CPU_MIN_FRAME_BITS &&
	    next.control.frame_bit_count != next.retained_cpu_bits &&
	    /* Table 23 bits 21..25: zero rate starts an independent cleardown. */
	    packed_msb_read_lsb_field(next.control.frame, 21u, 5u) != 0u) {
		if (v92_control_rx_init_upstream(&next.control,
			    receiver->control.alignment_bits) != V92_CONTROL_RX_OK)
			return fail(receiver);
		emitted = (struct v92_control_event){
			.type = V92_CONTROL_EVENT_CPU_REJECTED,
			.record.cpu.frame_result = V92_CPU_INVALID_FRAME,
		};
	}
	track_repetition(&next, &emitted, bits, receiver->control.frame_bit_count);
	if (!v92_upstream_control_line_rx_is_valid(&next))
		return fail(receiver);
	*receiver = next;
	*event = emitted;
	return emitted.type == V92_CONTROL_EVENT_NONE ?
		       V92_UPSTREAM_CONTROL_LINE_RX_OK :
		       V92_UPSTREAM_CONTROL_LINE_RX_EVENT;
}

const char *v92_upstream_control_line_rx_result_name(
	enum v92_upstream_control_line_rx_result result)
{
	switch (result) {
	case V92_UPSTREAM_CONTROL_LINE_RX_OK:
		return "ok";
	case V92_UPSTREAM_CONTROL_LINE_RX_EVENT:
		return "event";
	case V92_UPSTREAM_CONTROL_LINE_RX_FAILED:
		return "failed";
	case V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE:
		return "invalid state";
	case V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
