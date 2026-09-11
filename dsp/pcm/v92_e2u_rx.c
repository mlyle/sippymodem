// SPDX-License-Identifier: BSD-3-Clause

#include "v92_e2u_rx.h"

#include "memory_region.h"
#include "v92_phase4_record_tx.h"

#include <math.h>
#include <stddef.h>

static bool metrics_are_valid(const struct v92_e2u_rx *receiver)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		receiver->signal.line.demodulator.constellation);

	return isfinite(receiver->correlation) &&
	       isfinite(receiver->observed_energy) &&
	       isfinite(receiver->expected_energy) &&
	       receiver->observed_energy >= 0.0 &&
	       receiver->expected_energy >= 0.0 && bits != 0u &&
	       receiver->bit_errors <= receiver->received_symbols * bits;
}

bool v92_e2u_rx_is_valid(const struct v92_e2u_rx *receiver)
{
	if (receiver == NULL || !v92_upstream_rx_is_valid(&receiver->signal) ||
	    receiver->signal.training_tail_remaining != 0u ||
	    (receiver->expected_symbols != V92_PHASE4_E2U_SYMBOLS &&
	     receiver->expected_symbols != V92_PHASE4_E2U_EXTENDED_SYMBOLS) ||
	    receiver->received_symbols > receiver->expected_symbols ||
	    (v92_upstream_rx_has_failed(&receiver->signal) &&
	     !receiver->failed) ||
	    !metrics_are_valid(receiver))
		return false;
	return !receiver->matched ||
	       (receiver->received_symbols == receiver->expected_symbols &&
		!receiver->failed);
}

bool v92_e2u_rx_is_complete(const struct v92_e2u_rx *receiver)
{
	return v92_e2u_rx_is_valid(receiver) && !receiver->failed &&
	       receiver->matched &&
	       receiver->received_symbols == receiver->expected_symbols;
}

bool v92_e2u_rx_has_failed(const struct v92_e2u_rx *receiver)
{
	return v92_e2u_rx_is_valid(receiver) && receiver->failed;
}

enum v92_e2u_rx_result v92_e2u_rx_init(
	struct v92_e2u_rx *receiver,
	const struct v92_upstream_rx *signal, bool extend_one_symbol)
{
	struct v92_e2u_rx initialized = { 0 };

	if (receiver == NULL || !v92_upstream_rx_is_valid(signal) ||
	    v92_upstream_rx_has_failed(signal) ||
	    signal->training_tail_remaining != 0u)
		return V92_E2U_RX_INVALID_ARGUMENT;
	initialized.signal = *signal;
	initialized.expected_symbols = extend_one_symbol ?
		V92_PHASE4_E2U_EXTENDED_SYMBOLS : V92_PHASE4_E2U_SYMBOLS;
	if (!v92_e2u_rx_is_valid(&initialized))
		return V92_E2U_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_E2U_RX_OK;
}

static uint8_t group_bit_errors(uint8_t group, unsigned int bits)
{
	uint8_t errors = 0u;

	for (unsigned int bit = 0u; bit < bits; ++bit)
		errors += (uint8_t)((group >> bit) & 1u);
	return errors;
}

static bool sequence_matches(const struct v92_e2u_rx *receiver)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		receiver->signal.line.demodulator.constellation);
	double correlation_squared =
		receiver->correlation * receiver->correlation;
	double total_energy =
		receiver->observed_energy * receiver->expected_energy;
	unsigned int bit_count = receiver->expected_symbols * bits;
	bool positive = receiver->correlation > 0.0 && total_energy > 0.0;
	bool strong = positive && correlation_squared * 4.0 >=
		total_energy * 3.0;
	bool weak = positive && correlation_squared * 5.0 >= total_energy &&
		receiver->bit_errors <= bit_count / 6u;

	return strong || weak;
}

static enum v92_e2u_rx_result fail(struct v92_e2u_rx *receiver)
{
	receiver->failed = true;
	return V92_E2U_RX_NUMERIC_FAILURE;
}

enum v92_e2u_rx_result v92_e2u_rx_sample(
	struct v92_e2u_rx *receiver, int16_t sample)
{
	struct v92_e2u_rx next;
	struct v92_upstream_decision decision;
	int16_t expected;
	unsigned int bits;
	enum v92_upstream_rx_result result;

	if (receiver == NULL)
		return V92_E2U_RX_INVALID_ARGUMENT;
	if (!v92_e2u_rx_is_valid(receiver))
		return V92_E2U_RX_INVALID_STATE;
	if (receiver->failed)
		return V92_E2U_RX_FAILED;
	if (receiver->received_symbols == receiver->expected_symbols)
		return receiver->matched ? V92_E2U_RX_COMPLETE :
			V92_E2U_RX_NO_MATCH;
	next = *receiver;
	if (!v92_upstream_rx_expected_level(&next.signal, 0u, &expected))
		return fail(receiver);
	result = v92_upstream_rx_sample_known(
		&next.signal, sample, 0u, &decision);
	if (result == V92_UPSTREAM_RX_FAILED)
		return V92_E2U_RX_FAILED;
	if (result == V92_UPSTREAM_RX_INVALID_STATE)
		return V92_E2U_RX_INVALID_STATE;
	if (result != V92_UPSTREAM_RX_OK)
		return fail(receiver);
	bits = v92_phase4_modulator_bits_per_symbol(
		next.signal.line.demodulator.constellation);
	next.correlation += decision.equalized_level * expected;
	next.observed_energy +=
		decision.equalized_level * decision.equalized_level;
	next.expected_energy += (double)expected * expected;
	next.bit_errors += group_bit_errors(decision.group, bits);
	next.received_symbols++;
	if (next.received_symbols == next.expected_symbols)
		next.matched = sequence_matches(&next);
	if (!v92_e2u_rx_is_valid(&next))
		return fail(receiver);
	*receiver = next;
	if (next.received_symbols != next.expected_symbols)
		return V92_E2U_RX_OK;
	return next.matched ? V92_E2U_RX_COMPLETE : V92_E2U_RX_NO_MATCH;
}

enum v92_e2u_rx_result v92_e2u_rx_search(
	struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, int16_t sample,
	struct v92_control_event *event)
{
	if (receiver == NULL || control == NULL || event == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), control, sizeof(*control)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), event, sizeof(*event)) ||
	    memory_regions_overlap(control, sizeof(*control), event, sizeof(*event)))
		return V92_E2U_RX_INVALID_ARGUMENT;
	if (!v92_e2u_rx_is_valid(receiver) ||
	    !v92_upstream_control_line_rx_is_valid(control) ||
	    receiver->signal.line.law != control->signal.line.law ||
	    receiver->signal.line.renegotiation != control->signal.line.renegotiation ||
	    receiver->signal.line.demodulator.constellation != control->signal.line.demodulator.constellation ||
	    control->signal.training_tail_remaining != 0u)
		return V92_E2U_RX_INVALID_STATE;
	if (receiver->failed || v92_upstream_control_line_rx_has_failed(control))
		return V92_E2U_RX_FAILED;
	if (receiver->matched)
		return V92_E2U_RX_COMPLETE;
	struct v92_e2u_rx next = *receiver;
	struct v92_upstream_control_line_rx line = *control;
	struct v92_control_event emitted = { 0 };
	enum v92_upstream_control_line_rx_result result =
		v92_upstream_control_line_rx_sample(&line, sample, &emitted);
	if (next.received_symbols < next.expected_symbols) {
		enum v92_e2u_rx_result trial = v92_e2u_rx_sample(&next, sample);
		if (trial == V92_E2U_RX_COMPLETE) {
			*receiver = next;
			*event = (struct v92_control_event){ 0 };
			return V92_E2U_RX_COMPLETE;
		}
		if (trial != V92_E2U_RX_OK && trial != V92_E2U_RX_NO_MATCH)
			return fail(receiver);
	}
	if (result != V92_UPSTREAM_CONTROL_LINE_RX_OK && result != V92_UPSTREAM_CONTROL_LINE_RX_EVENT)
		return fail(receiver);
	if (result == V92_UPSTREAM_CONTROL_LINE_RX_EVENT &&
	    v92_e2u_rx_init(&next, &line.signal,
		    receiver->expected_symbols == V92_PHASE4_E2U_EXTENDED_SYMBOLS) != V92_E2U_RX_OK)
		return fail(receiver);
	*receiver = next;
	*control = line;
	*event = emitted;
	return V92_E2U_RX_OK;
}

enum v92_e2u_rx_result v92_e2u_rx_handoff(
	const struct v92_e2u_rx *receiver,
	struct v92_upstream_rx *signal)
{
	if (receiver == NULL || signal == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), signal, sizeof(*signal)))
		return V92_E2U_RX_INVALID_ARGUMENT;
	if (!v92_e2u_rx_is_complete(receiver))
		return V92_E2U_RX_INVALID_STATE;
	*signal = receiver->signal;
	return V92_E2U_RX_OK;
}

const char *v92_e2u_rx_result_name(enum v92_e2u_rx_result result)
{
	switch (result) {
	case V92_E2U_RX_OK:
		return "ok";
	case V92_E2U_RX_COMPLETE:
		return "complete";
	case V92_E2U_RX_NO_MATCH:
		return "no match";
	case V92_E2U_RX_FAILED:
		return "failed";
	case V92_E2U_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_E2U_RX_INVALID_STATE:
		return "invalid state";
	case V92_E2U_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
