// SPDX-License-Identifier: BSD-3-Clause

#include "v92_e2u_rx.h"
#include "packed_bits.h"

#include "integer_math.h"
#include "memory_region.h"
#include "v92_phase4_record_tx.h"

#include <math.h>
#include <stddef.h>

static bool metrics_are_valid(const struct v92_e2u_rx *receiver)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		receiver->signal.line.demodulator.constellation);

	return isfinite(receiver->carrier_energy) &&
	       isfinite(receiver->carrier_peak) &&
	       receiver->carrier_energy >= 0.0 &&
	       receiver->carrier_peak >= 0.0 &&
	       receiver->carrier_peak <= receiver->carrier_energy &&
	       receiver->carrier_tap < receiver->signal.line.equalizer.tap_count &&
	       isfinite(receiver->correlation) &&
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
	double lu = receiver->signal.line.demodulator.lu;
	unsigned count = receiver->received_symbols;

	/* Exclude one impulse from raw carrier evidence. The learned FIR may
	 * move its strongest input away from its initial decision delay;
	 * following samples and padding cannot supply marker evidence. Keep the
	 * acquired-LU floor 18 dB below training. */
	if (count < 2u || (receiver->carrier_energy - receiver->carrier_peak) * 64.0 <
	    (count - 1u) * lu * lu)
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
	/* Freeze one input delay for the whole marker, retaining the nominal
	 * delay when equal taps provide no reason to move it. */
	if (next.received_symbols == 0u) {
		const struct qam_equalizer *equalizer = &next.signal.line.equalizer;
		next.carrier_tap = (uint8_t)equalizer->decision_delay;
		double largest = hypot(equalizer->coefficients[next.carrier_tap].in_phase,
				       equalizer->coefficients[next.carrier_tap].quadrature);

		for (unsigned tap = 0u; tap < equalizer->tap_count; ++tap) {
			double magnitude = hypot(equalizer->coefficients[tap].in_phase,
					 equalizer->coefficients[tap].quadrature);
			if (magnitude > largest) {
				largest = magnitude;
				next.carrier_tap = (uint8_t)tap;
			}
		}
	}
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
	double input = next.signal.line.equalizer.history[next.carrier_tap].in_phase;
	next.carrier_energy += input * input;
	next.carrier_peak = fmax(next.carrier_peak, input * input);
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

bool v92_e2u_search_is_valid(const struct v92_e2u_search *search)
{
	if (search == NULL)
		return false;
	if (search->frame_bits) {
		unsigned bits = v92_phase4_modulator_bits_per_symbol(search->coder.constellation);
		if (!v92_phase4_modulator_is_valid(&search->coder) || !bits ||
		    (search->frame_bits < V92_CPU_MIN_FRAME_BITS &&
		     search->frame_bits != V92_SUVU_FRAME_BITS) ||
		    search->frame_bits > V92_CPU_MAX_FRAME_BITS ||
		    search->frame_bits % (bits * V92_CPU_SYMBOLS_PER_FRAME) || search->bit_index % bits ||
		    search->bit_index >= search->frame_bits)
			return false;
	}
	return !search->candidate_active ||
		(search->frame_bits && v92_e2u_rx_is_valid(&search->repeated) &&
		 !search->repeated.failed &&
		 search->repeated.received_symbols < search->repeated.expected_symbols);
}

static bool advance_repeat(struct v92_e2u_search *search, unsigned bits, int16_t lu)
{
	if (!search->frame_bits)
		return true;
	unsigned group = packed_msb_read_lsb_field(search->frame, search->bit_index, bits);
	int16_t ignored;
	search->coder.lu = lu;
	if (v92_phase4_modulator_next(&search->coder, group, &ignored) != V92_PHASE4_MODULATOR_OK)
		return false;
	search->bit_index = (search->bit_index + bits) % search->frame_bits;
	return true;
}

static bool retain_repeat(struct v92_e2u_search *search,
	const struct v92_upstream_control_line_rx *before,
	const struct v92_upstream_control_line_rx *after,
	const struct v92_control_event *event, unsigned bits, int16_t sample)
{
	if (event->type == V92_CONTROL_EVENT_CPUS) {
		search->frame_bits = 0;
		search->bit_index = 0;
		search->candidate_active = false;
	}
	if (event->type != V92_CONTROL_EVENT_CPU && event->type != V92_CONTROL_EVENT_SUVU)
		return true;
	search->frame_bits = 0;
	search->bit_index = 0;
	search->candidate_active = false;
	const struct v92_control_rx *record = &before->control;
	/* A valid control record fixes its coding clock without confirming CPd.
	 * The downstream owner still guards E2u against our acknowledgement;
	 * E2u itself can confirm the exchange if the last repeat was damaged. */
	bool status = event->type == V92_CONTROL_EVENT_SUVU;
	if ((status && (record->stage != V92_CONTROL_RX_RECEIVE_STATUS ||
	     record->frame_bit_count != V92_SUVU_FRAME_BITS)) ||
	    (!status && (!event->record.cpu.parameters.data_rate_number ||
	     record->stage != V92_CONTROL_RX_RECEIVE_PARAMETERS)) ||
	    record->frame_bit_count > V92_CPU_MAX_FRAME_BITS ||
	    record->bit_index + bits != record->frame_bit_count ||
	    !after->control.record_boundary)
		return true;
	/* The parser cleared its candidate after the CRC passed. Retain its
	 * original wire prefix plus this sample's final group, not a re-encoded
	 * record whose reserved/duplicate fields may differ. */
	struct v92_upstream_rx signal = before->signal;
	struct v92_upstream_decision decision;
	if (v92_upstream_rx_sample(&signal, sample, &decision) != V92_UPSTREAM_RX_OK)
		return false;
	memset(search->frame, 0, sizeof(search->frame));
	memcpy(search->frame, record->frame, (record->frame_bit_count + 7u) / 8u);
	for (unsigned bit = 0; bit < bits; ++bit)
		packed_msb_set_bit(search->frame, record->bit_index + bit,
			(decision.group >> bit) & 1u);
	search->frame_bits = (uint16_t)record->frame_bit_count;
	search->bit_index = 0;
	const struct v92_phase4_demodulator *from = &after->signal.line.demodulator;
	search->coder = (struct v92_phase4_modulator){
		.scrambler = from->descrambler, .constellation = from->constellation,
		.lu = from->lu, .previous_encoded_sign = from->previous_encoded_sign,
	};
	return true;
}

static enum v92_e2u_rx_result search_sample(
	struct v92_e2u_search *search, struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, bool eligible, int16_t sample,
	struct v92_control_event *event)
{
	if (search != NULL &&
	    ((receiver != NULL && memory_regions_overlap(search, sizeof(*search), receiver, sizeof(*receiver))) ||
	     (control != NULL && memory_regions_overlap(search, sizeof(*search), control, sizeof(*control))) ||
	     (event != NULL && memory_regions_overlap(search, sizeof(*search), event, sizeof(*event)))))
		return V92_E2U_RX_INVALID_ARGUMENT;
	if (receiver == NULL || control == NULL || event == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), control, sizeof(*control)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), event, sizeof(*event)) ||
	    memory_regions_overlap(control, sizeof(*control), event, sizeof(*event)))
		return V92_E2U_RX_INVALID_ARGUMENT;
	if ((search != NULL && !v92_e2u_search_is_valid(search)) ||
	    !v92_e2u_rx_is_valid(receiver) ||
	    !v92_upstream_control_line_rx_is_valid(control) ||
	    receiver->signal.line.law != control->signal.line.law ||
	    receiver->signal.line.renegotiation != control->signal.line.renegotiation ||
	    receiver->signal.line.output_bias != control->signal.line.output_bias ||
	    receiver->signal.line.demodulator.constellation != control->signal.line.demodulator.constellation ||
	    control->signal.training_tail_remaining != 0u)
		return V92_E2U_RX_INVALID_STATE;
	if (search != NULL && search->frame_bits &&
	    search->coder.constellation != control->signal.line.demodulator.constellation)
		return V92_E2U_RX_INVALID_STATE;
	if (search != NULL && search->candidate_active &&
	    (search->repeated.expected_symbols != receiver->expected_symbols ||
	     search->repeated.signal.line.law != control->signal.line.law ||
	     search->repeated.signal.line.demodulator.constellation != control->signal.line.demodulator.constellation ||
	     search->repeated.signal.line.polarity != control->signal.line.polarity ||
	     search->repeated.signal.line.renegotiation != control->signal.line.renegotiation))
		return V92_E2U_RX_INVALID_STATE;
	if (receiver->failed || control->signal.failed)
		return V92_E2U_RX_FAILED;
	if (receiver->matched)
		return V92_E2U_RX_COMPLETE;
	struct v92_e2u_rx next = *receiver;
	struct v92_upstream_control_line_rx line = *control;
	struct v92_e2u_search repeat;
	if (search != NULL)
		repeat = *search;
	struct v92_control_event emitted = { 0 };
	bool extended = receiver->expected_symbols == V92_PHASE4_E2U_EXTENDED_SYMBOLS;
	/* V.92 9.6.2.1.4 finishes CPu before E2u; SUVu repeats also
	 * end on control frame boundaries. Once repetition is confirmed,
	 * a damaged header must not erase the next boundary.
	 * An aligned record still owns its full length; never search its
	 * zero-filled interior at the old repetition period. After CPu selection,
	 * a partial record off the twelve-symbol alignment cannot own that
	 * boundary. Keep parsing it, but allow a competing marker search. */
	/* An accepted CPu fixes its wire extent before a second intact repeat
	 * confirms the clock. At that retained period an idle parser cannot
	 * erase E2u just because the preceding repeat lost its sync/header.
	 * Partial headers still require the confirmed-clock rule below. */
	unsigned bits = v92_phase4_modulator_bits_per_symbol(
		control->signal.line.demodulator.constellation);
	bool selected_cpu_boundary = control->retained_cpu_bits &&
		control->repeat_symbols * bits == control->retained_cpu_bits &&
		!control->repeat_remaining &&
		control->control.stage == V92_CONTROL_RX_SEARCH_SYNC;
	bool boundary = selected_cpu_boundary || control->control.record_boundary ||
		(control->repeat_confirmed && !control->repeat_remaining &&
		 (control->control.stage == V92_CONTROL_RX_SEARCH_SYNC ||
		  (control->retained_cpu_bits &&
		   control->control.bit_index % control->control.alignment_bits != 0u)));
	if (eligible && boundary &&
	    v92_e2u_rx_init(&next, &control->signal, extended) != V92_E2U_RX_OK)
		return fail(receiver);
	if (search != NULL) {
		if (!eligible || boundary)
			repeat.candidate_active = false;
		const struct v92_phase4_demodulator *observed = &control->signal.line.demodulator;
		/* Identical coding memories make this trial identical to the ordinary one. */
		if (eligible && boundary && repeat.frame_bits && !repeat.bit_index &&
		    control->repeat_confirmed && control->repeat_symbols * bits == repeat.frame_bits &&
		    (repeat.coder.scrambler.history != observed->descrambler.history ||
		     repeat.coder.previous_encoded_sign != observed->previous_encoded_sign)) {
			if (v92_e2u_rx_init(&repeat.repeated, &control->signal, extended) != V92_E2U_RX_OK)
				return fail(receiver);
			repeat.repeated.signal.line.demodulator.descrambler = repeat.coder.scrambler;
			repeat.repeated.signal.line.demodulator.previous_encoded_sign = repeat.coder.previous_encoded_sign;
			repeat.candidate_active = true;
		}
		if (!advance_repeat(&repeat, bits, control->signal.line.demodulator.lu))
			return fail(receiver);
	}

	enum v92_upstream_control_line_rx_result result =
		v92_upstream_control_line_rx_sample(&line, sample, &emitted);
	if (eligible && (boundary || next.received_symbols != 0u) &&
	    next.received_symbols < next.expected_symbols) {
		enum v92_e2u_rx_result trial = v92_e2u_rx_sample(&next, sample);
		if (trial == V92_E2U_RX_COMPLETE) {
			if (search != NULL) {
				repeat.candidate_active = false;
				*search = repeat;
			}
			*receiver = next;
			*event = (struct v92_control_event){ 0 };
			return V92_E2U_RX_COMPLETE;
		}
		if (trial != V92_E2U_RX_OK && trial != V92_E2U_RX_NO_MATCH)
			return fail(receiver);
	}
	if (search != NULL && repeat.candidate_active) {
		enum v92_e2u_rx_result trial = v92_e2u_rx_sample(&repeat.repeated, sample);
		if (trial == V92_E2U_RX_COMPLETE) {
			repeat.candidate_active = false;
			*receiver = repeat.repeated;
			*search = repeat;
			*event = (struct v92_control_event){ 0 };
			return V92_E2U_RX_COMPLETE;
		}
		if (trial == V92_E2U_RX_NO_MATCH)
			repeat.candidate_active = false;
		else if (trial != V92_E2U_RX_OK)
			return fail(receiver);
	}
	if (result != V92_UPSTREAM_CONTROL_LINE_RX_OK && result != V92_UPSTREAM_CONTROL_LINE_RX_EVENT)
		return fail(receiver);
	if ((!eligible || line.control.record_boundary) &&
	    v92_e2u_rx_init(&next, &line.signal, extended) != V92_E2U_RX_OK)
		return fail(receiver);
	if (search != NULL) {
		if (line.control.record_boundary)
			repeat.candidate_active = false;
		if (!retain_repeat(&repeat, control, &line, &emitted, bits, sample))
			return fail(receiver);
		*search = repeat;
	}
	*receiver = next;
	*control = line;
	*event = emitted;
	return V92_E2U_RX_OK;
}

enum v92_e2u_rx_result v92_e2u_rx_search(
	struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, bool eligible, int16_t sample,
	struct v92_control_event *event)
{
	return search_sample(NULL, receiver, control, eligible, sample, event);
}

enum v92_e2u_rx_result v92_e2u_search_sample(
	struct v92_e2u_search *search, struct v92_e2u_rx *receiver,
	struct v92_upstream_control_line_rx *control, bool eligible, int16_t sample,
	struct v92_control_event *event)
{
	if (search == NULL)
		return V92_E2U_RX_INVALID_ARGUMENT;
	return search_sample(search, receiver, control, eligible, sample, event);
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
