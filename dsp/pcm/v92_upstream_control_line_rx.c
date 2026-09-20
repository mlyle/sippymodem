// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_control_line_rx.h"

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
	       receiver->cpu_repeat_symbols <= V92_CPU_MAX_FRAME_BITS /
		       (expected_alignment / V92_CPU_SYMBOLS_PER_FRAME) &&
	       receiver->cpu_repeat_symbols % V92_CPU_SYMBOLS_PER_FRAME == 0u &&
	       receiver->cpu_repeat_remaining <= receiver->cpu_repeat_symbols &&
	       (!receiver->cpu_repeat_confirmed || receiver->cpu_repeat_symbols != 0u);
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
	       v92_upstream_rx_has_failed(&receiver->signal);
}

enum v92_upstream_control_line_rx_result
v92_upstream_control_line_rx_init(
	struct v92_upstream_control_line_rx *receiver,
	const struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct v92_upstream_control_line_rx initialized = { 0 };

	if (receiver == NULL ||
	    !v92_pcm_phase4_trn2u_line_handoff_is_valid(handoff))
		return V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT;
	if (v92_upstream_rx_init(&initialized.signal, handoff) !=
		    V92_UPSTREAM_RX_OK ||
	    v92_control_rx_init_upstream(
		    &initialized.control,
		    initialized.signal.line.demodulator.constellation ==
			    V92_CONSTELLATION_4_POINT ?
			    V92_CONTROL_UPSTREAM_4_POINT_BITS :
			    V92_CONTROL_UPSTREAM_8_POINT_BITS) !=
		    V92_CONTROL_RX_OK ||
	    !v92_upstream_control_line_rx_is_valid(&initialized))
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

static void track_cpu_repetition(struct v92_upstream_control_line_rx *receiver,
	const struct v92_control_event *event, unsigned bits_per_symbol)
{
	if (receiver->cpu_repeat_symbols) {
		if (!receiver->cpu_repeat_remaining)
			receiver->cpu_repeat_remaining = receiver->cpu_repeat_symbols;
		--receiver->cpu_repeat_remaining;
	}
	/* A rejected status cannot replace a confirmed CPu frame epoch with
	 * an off-frame boundary. Keep the clock for the real CPu end/E2u. */
	if (event->type == V92_CONTROL_EVENT_SUVU_REJECTED &&
	    receiver->control.record_boundary && receiver->cpu_repeat_confirmed &&
	    receiver->cpu_repeat_remaining % V92_CPU_SYMBOLS_PER_FRAME != 0u) {
		receiver->control.record_boundary = false;
		return;
	}
	if (event->type == V92_CONTROL_EVENT_SUVU ||
	    (event->type == V92_CONTROL_EVENT_SUVU_REJECTED && receiver->control.record_boundary)) {
		receiver->cpu_repeat_symbols = 0u;
		receiver->cpu_repeat_remaining = 0u;
		receiver->cpu_repeat_confirmed = false;
	} else if (event->type == V92_CONTROL_EVENT_CPU || event->type == V92_CONTROL_EVENT_CPUS) {
		size_t bits = event->type == V92_CONTROL_EVENT_CPU ?
			v92_cpu_frame_bits(&event->record.cpu.parameters, receiver->control.alignment_bits) :
			v92_cpus_frame_bits(receiver->control.alignment_bits);
		uint16_t symbols = (uint16_t)(bits / bits_per_symbol);
		receiver->cpu_repeat_confirmed = symbols == receiver->cpu_repeat_symbols &&
			receiver->cpu_repeat_remaining == 0u;
		receiver->cpu_repeat_symbols = symbols;
		receiver->cpu_repeat_remaining = 0u;
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
	if (v92_upstream_control_line_rx_has_failed(receiver))
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
	track_cpu_repetition(&next, &emitted, bits);
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
