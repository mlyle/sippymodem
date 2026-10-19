// SPDX-License-Identifier: BSD-3-Clause

#include "v92_e2u_rx.h"

#include "integer_math.h"
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
	       receiver->expected_energy >= 0.0 &&
	       isfinite(receiver->outlier.squared_error) &&
	       isfinite(receiver->outlier.correlation) &&
	       isfinite(receiver->outlier.observed_energy) &&
	       isfinite(receiver->outlier.expected_energy) &&
	       receiver->outlier.squared_error >= 0.0 &&
	       receiver->outlier.observed_energy >= 0.0 &&
	       receiver->outlier.expected_energy >= 0.0 &&
	       receiver->outlier.observed_energy <= receiver->observed_energy &&
	       receiver->outlier.expected_energy <= receiver->expected_energy && bits != 0u &&
	       receiver->bit_errors <= receiver->received_symbols * bits;
}

bool v92_e2u_rx_is_valid(const struct v92_e2u_rx *receiver)
{
	if (receiver == NULL || !v92_upstream_rx_is_valid(&receiver->signal) ||
	    receiver->signal.training_tail_remaining != 0u ||
	    (receiver->expected_symbols != V92_PHASE4_E2U_SYMBOLS &&
	     receiver->expected_symbols != V92_PHASE4_E2U_EXTENDED_SYMBOLS) ||
	    receiver->received_symbols > receiver->expected_symbols ||
	    (receiver->signal.failed && !receiver->failed) ||
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
	    signal->failed ||
	    signal->training_tail_remaining != 0u)
		return V92_E2U_RX_INVALID_ARGUMENT;
	initialized.signal = *signal;
	initialized.expected_symbols = extend_one_symbol ?
		V92_PHASE4_E2U_EXTENDED_SYMBOLS : V92_PHASE4_E2U_SYMBOLS;
	*receiver = initialized;
	return V92_E2U_RX_OK;
}

static bool sequence_matches(const struct v92_e2u_rx *receiver)
{
	const struct qam_equalizer *equalizer = &receiver->signal.line.equalizer;
	double lu = receiver->signal.line.demodulator.lu;
	double input_energy = 0.0, peak_energy = 0.0;
	unsigned count = receiver->received_symbols;
	unsigned begin = equalizer->decision_delay;

	/* Measure only the raw samples corresponding to this marker. FIR
	 * padding and following B1u do not supply evidence about E2u carrier.
	 * Keep the acquired-LU floor 18 dB below training, excluding the
	 * largest sample so one impulse cannot make quiet input a carrier. */
	if (count < 2u || begin > equalizer->tap_count ||
	    count > equalizer->tap_count - begin)
		return false;
	for (unsigned tap = begin; tap < begin + count; ++tap) {
		double input = equalizer->history[tap].in_phase;
		input_energy += input * input;
		peak_energy = fmax(peak_energy, input * input);
	}
	if ((input_energy - peak_energy) * 64.0 < (count - 1u) * lu * lu)
		return false;

	double correlation_squared =
		receiver->correlation * receiver->correlation;
	double total_energy =
		receiver->observed_energy * receiver->expected_energy;
	bool positive = receiver->correlation > 0.0 && total_energy > 0.0;
	bool strong = positive && correlation_squared * 4.0 >=
		total_energy * 3.0;
	/* Repeated control boundaries test many short E2u candidates. A few
	 * decoded errors plus weak correlation also fit ordinary control bits.
	 * Require exact decoded zeros when the waveform alone is insufficient;
	 * an impulse that preserves the coded decisions remains acceptable. */
	bool weak = positive && correlation_squared * 5.0 >= total_energy &&
		receiver->bit_errors == 0u;

	if (strong || weak)
		return true;
	/* One dominant impulse can reverse an outer point while the rest of
	 * E2u remains a strong known-sequence match. Restrict this additional
	 * qualification to one decoded error and one residual carrying over
	 * four times the remaining error energy. Keep the ordinary strong
	 * correlation requirement on every retained sample. */
	if (receiver->bit_errors != 1u)
		return false;
	double remaining_error = fmax(0.0, receiver->observed_energy +
		receiver->expected_energy - 2.0 * receiver->correlation -
		receiver->outlier.squared_error);
	if (receiver->outlier.squared_error <= 4.0 * remaining_error)
		return false;
	double correlation = receiver->correlation - receiver->outlier.correlation;
	double observed = receiver->observed_energy - receiver->outlier.observed_energy;
	double expected = receiver->expected_energy - receiver->outlier.expected_energy;
	return correlation > 0.0 && observed > 0.0 && expected > 0.0 &&
		correlation * correlation * 4.0 >= observed * expected * 3.0;
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
	double residual = decision.equalized_level - expected;
	if (residual * residual > next.outlier.squared_error) {
		next.outlier.squared_error = residual * residual;
		next.outlier.correlation = decision.equalized_level * expected;
		next.outlier.observed_energy = decision.equalized_level * decision.equalized_level;
		next.outlier.expected_energy = (double)expected * expected;
	}
	next.bit_errors += bit_count_u8(
		(uint8_t)(decision.group & ((1u << bits) - 1u)));
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
	struct v92_upstream_control_line_rx *control, bool eligible, int16_t sample,
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
	    receiver->signal.line.output_bias != control->signal.line.output_bias ||
	    receiver->signal.line.demodulator.constellation != control->signal.line.demodulator.constellation ||
	    control->signal.training_tail_remaining != 0u)
		return V92_E2U_RX_INVALID_STATE;
	if (receiver->failed || control->signal.failed)
		return V92_E2U_RX_FAILED;
	if (receiver->matched)
		return V92_E2U_RX_COMPLETE;
	struct v92_e2u_rx next = *receiver;
	struct v92_upstream_control_line_rx line = *control;
	struct v92_control_event emitted = { 0 };
	bool extended = receiver->expected_symbols == V92_PHASE4_E2U_EXTENDED_SYMBOLS;
	/* V.92 9.6.2.1.4 finishes CPu before E2u; SUVu repeats also
	 * end on control frame boundaries. Once repetition is confirmed,
	 * a damaged header must not erase the next boundary.
	 * An aligned record still owns its full length; never search its
	 * zero-filled interior at the old repetition period. After CPu selection,
	 * a partial record off the twelve-symbol alignment cannot own that
	 * boundary. Keep parsing it, but allow a competing marker search. */
	bool boundary = control->control.record_boundary ||
		(control->repeat_confirmed && !control->repeat_remaining &&
		 (control->control.stage == V92_CONTROL_RX_SEARCH_SYNC ||
		  (control->retained_cpu_bits &&
		   control->control.bit_index % control->control.alignment_bits != 0u)));
	if (eligible && boundary &&
	    v92_e2u_rx_init(&next, &control->signal, extended) != V92_E2U_RX_OK)
		return fail(receiver);
	enum v92_upstream_control_line_rx_result result =
		v92_upstream_control_line_rx_sample(&line, sample, &emitted);
	if (eligible && (boundary || next.received_symbols != 0u) &&
	    next.received_symbols < next.expected_symbols) {
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
	if ((!eligible || line.control.record_boundary) &&
	    v92_e2u_rx_init(&next, &line.signal, extended) != V92_E2U_RX_OK)
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
