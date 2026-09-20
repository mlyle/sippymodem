// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_digital_rx.h"

#include "memory_region.h"

#include <math.h>
#include <stddef.h>

static bool stage_is_valid(enum v92_phase4_digital_rx_stage stage)
{
	return stage == V92_PHASE4_DIGITAL_RX_TRN2U ||
	       stage == V92_PHASE4_DIGITAL_RX_CONTROL ||
	       stage == V92_PHASE4_DIGITAL_RX_RESUMED_CONTROL;
}

bool v92_phase4_digital_rx_is_valid(
	const struct v92_phase4_digital_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !v92_upstream_channel_is_valid(&receiver->channel))
		return false;
	if (receiver->stage == V92_PHASE4_DIGITAL_RX_RESUMED_CONTROL)
		return receiver->channel.symbols == 0u && !receiver->channel_distorted &&
			v92_upstream_control_line_rx_is_valid(&receiver->signal.control) &&
			receiver->signal.control.signal.line.renegotiation &&
			receiver->signal.control.signal.training_tail_remaining == 0u;
	uint32_t symbols = receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
		receiver->signal.trn2u.trained_symbols :
		receiver->signal.control.signal.line.training_symbols -
			receiver->signal.control.signal.training_tail_remaining;

	if (receiver->channel.symbols != symbols ||
	    (receiver->channel_distorted && symbols <= V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS))
		return false;
	return receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
		       v92_pcm_phase4_trn2u_line_rx_is_valid(
			       &receiver->signal.trn2u) :
		       v92_upstream_control_line_rx_is_valid(
			       &receiver->signal.control);
}

/* The enclosing receive state, including its active child, is validated first. */
static bool state_has_failed(
	const struct v92_phase4_digital_rx *receiver)
{
	return receiver->failed ||
	       (receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
			receiver->signal.trn2u.failed :
			receiver->signal.control.signal.failed);
}

bool v92_phase4_digital_rx_has_failed(
	const struct v92_phase4_digital_rx *receiver)
{
	return v92_phase4_digital_rx_is_valid(receiver) &&
	       state_has_failed(receiver);
}

bool v92_phase4_digital_rx_is_receiving_control(
	const struct v92_phase4_digital_rx *receiver)
{
	return v92_phase4_digital_rx_is_valid(receiver) &&
	       !state_has_failed(receiver) &&
	       receiver->stage != V92_PHASE4_DIGITAL_RX_TRN2U;
}

enum v92_phase4_digital_rx_result v92_phase4_digital_rx_resume(
	struct v92_phase4_digital_rx *receiver,
	const struct v92_upstream_control_line_rx *line)
{
	struct v92_phase4_digital_rx initialized = { .stage = V92_PHASE4_DIGITAL_RX_RESUMED_CONTROL };

	if (receiver == NULL || line == NULL)
		return V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT;
	initialized.signal.control = *line;
	if (!v92_phase4_digital_rx_is_valid(&initialized) ||
	    state_has_failed(&initialized))
		return V92_PHASE4_DIGITAL_RX_INVALID_STATE;
	*receiver = initialized;
	return V92_PHASE4_DIGITAL_RX_OK;
}

enum v92_phase4_digital_rx_result v92_phase4_digital_rx_init(
	struct v92_phase4_digital_rx *receiver,
	const struct v92_pcm_phase3_cpt_line_handoff *handoff,
	enum v92_constellation_size constellation, enum g711_law law)
{
	struct v92_phase4_digital_rx initialized = { 0 };
	enum v92_pcm_phase4_trn2u_line_rx_result result;

	if (receiver == NULL ||
	    !v92_pcm_phase3_cpt_line_handoff_is_valid(handoff) ||
	    v92_phase4_modulator_bits_per_symbol(constellation) == 0u)
		return V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT;
	result = v92_pcm_phase4_trn2u_line_rx_init(
		&initialized.signal.trn2u, handoff, constellation, law);
	if (result == V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT)
		return V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT;
	if (result != V92_PCM_PHASE4_TRN2U_LINE_RX_OK)
		return V92_PHASE4_DIGITAL_RX_NUMERIC_FAILURE;
	initialized.stage = V92_PHASE4_DIGITAL_RX_TRN2U;
	if (!v92_phase4_digital_rx_is_valid(&initialized))
		return V92_PHASE4_DIGITAL_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_PHASE4_DIGITAL_RX_OK;
}

enum v92_phase4_digital_rx_result v92_phase4_digital_rx_init_after_ru(
	struct v92_phase4_digital_rx *receiver, const struct v92_ru_line_rx *ru)
{
	struct v92_phase4_digital_rx initialized = { 0 };

	if (receiver == NULL || ru == NULL)
		return V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT;
	if (v92_pcm_phase4_trn2u_line_rx_init_after_ru(&initialized.signal.trn2u, ru) !=
	    V92_PCM_PHASE4_TRN2U_LINE_RX_OK)
		return V92_PHASE4_DIGITAL_RX_INVALID_STATE;
	if (!v92_phase4_digital_rx_is_valid(&initialized))
		return V92_PHASE4_DIGITAL_RX_NUMERIC_FAILURE;
	*receiver = initialized;
	return V92_PHASE4_DIGITAL_RX_OK;
}

bool v92_phase4_digital_rx_is_renegotiating(
	const struct v92_phase4_digital_rx *receiver)
{
	return v92_phase4_digital_rx_is_valid(receiver) &&
		(receiver->stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
		 receiver->signal.trn2u.renegotiation :
		 receiver->signal.control.signal.line.renegotiation);
}

static enum v92_phase4_digital_rx_result fail(
	struct v92_phase4_digital_rx *receiver)
{
	receiver->failed = true;
	return V92_PHASE4_DIGITAL_RX_NUMERIC_FAILURE;
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool observe_channel(
	struct v92_phase4_digital_rx *receiver, int16_t expected,
	const struct qam_equalizer *equalizer,
	const struct v92_phase4_demodulator *demodulator, enum g711_law law)
{
	double raw = equalizer->history[equalizer->decision_delay].in_phase;

	if (!isfinite(raw) || raw < -32768.0 || raw > 32768.0 || trunc(raw) != raw)
		return false;
	/* The acquisition FIR targets G.711 cells; the channel inverse needs
	 * the linear training waveform. Using cell centres in the inverse
	 * biases its gain, especially when LU lies near a companding boundary. */
	for (uint8_t magnitude = 0u; magnitude < demodulator->constellation / 2u; ++magnitude) {
		int16_t linear;

		if (v92_phase4_modulator_level(demodulator->constellation,
			    demodulator->lu, magnitude, expected < 0, &linear) != V92_PHASE4_MODULATOR_OK)
			return false;
		if (quantize(law, linear) != expected)
			continue;
		if (!v92_upstream_channel_sample(&receiver->channel, linear, (int32_t)raw))
			return false;
		/* Host DC/echo conditioning can move a sample within its original
		 * cell without changing the direct mapping's G.711 point placement. */
		if (receiver->channel.symbols > V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS &&
		    quantize(law, (int16_t)fmin(raw, INT16_MAX)) != expected)
			receiver->channel_distorted = true;
		return true;
	}
	return false;
}

static enum v92_phase4_digital_rx_result receive_training(
	struct v92_phase4_digital_rx *receiver,
	struct v92_phase4_digital_rx *next, int16_t sample)
{
	int16_t expected = next->signal.trn2u.expected[next->signal.trn2u.expected_index];
	enum v92_pcm_phase4_trn2u_line_rx_result result =
		v92_pcm_phase4_trn2u_line_rx_sample(
			&next->signal.trn2u, sample);

	if ((result == V92_PCM_PHASE4_TRN2U_LINE_RX_OK ||
	     result == V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED) &&
	    !observe_channel(next, expected, &next->signal.trn2u.equalizer,
		    &next->signal.trn2u.demodulator, next->signal.trn2u.law))
		return fail(receiver);
	if (result == V92_PCM_PHASE4_TRN2U_LINE_RX_OK)
		return V92_PHASE4_DIGITAL_RX_OK;
	if (result == V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED) {
		struct v92_pcm_phase4_trn2u_line_handoff handoff;
		struct v92_upstream_control_line_rx control;

		if (v92_pcm_phase4_trn2u_line_rx_handoff(
			    &next->signal.trn2u, &handoff) !=
			    V92_PCM_PHASE4_TRN2U_LINE_RX_OK ||
		    v92_upstream_control_line_rx_init(
			    &control, &handoff) !=
			    V92_UPSTREAM_CONTROL_LINE_RX_OK)
			return fail(receiver);
		next->signal =
			(union v92_phase4_digital_receive){ .control = control };
		next->stage = V92_PHASE4_DIGITAL_RX_CONTROL;
		return V92_PHASE4_DIGITAL_RX_ALIGNED;
	}
	if (result == V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE)
		return V92_PHASE4_DIGITAL_RX_INVALID_STATE;
	if (result == V92_PCM_PHASE4_TRN2U_LINE_RX_FAILED)
		return V92_PHASE4_DIGITAL_RX_FAILED;
	return fail(receiver);
}

static enum v92_phase4_digital_rx_result receive_control(
	struct v92_phase4_digital_rx *receiver,
	struct v92_phase4_digital_rx *next, int16_t sample,
	struct v92_control_event *event)
{
	const struct v92_upstream_rx *signal = &next->signal.control.signal;
	uint8_t remaining = signal->training_tail_remaining;
	int16_t expected = remaining ? signal->line.pending_training_targets[
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY - remaining] : 0;
	enum v92_upstream_control_line_rx_result result =
		v92_upstream_control_line_rx_sample(
			&next->signal.control, sample, event);

	if (remaining &&
	    (result == V92_UPSTREAM_CONTROL_LINE_RX_OK || result == V92_UPSTREAM_CONTROL_LINE_RX_EVENT) &&
	    !observe_channel(next, expected, &signal->line.equalizer,
		    &signal->line.demodulator, signal->line.law))
		return fail(receiver);
	switch (result) {
	case V92_UPSTREAM_CONTROL_LINE_RX_OK:
		return V92_PHASE4_DIGITAL_RX_OK;
	case V92_UPSTREAM_CONTROL_LINE_RX_EVENT:
		return V92_PHASE4_DIGITAL_RX_EVENT;
	case V92_UPSTREAM_CONTROL_LINE_RX_FAILED:
		return V92_PHASE4_DIGITAL_RX_FAILED;
	case V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE:
		return V92_PHASE4_DIGITAL_RX_INVALID_STATE;
	case V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT:
	case V92_UPSTREAM_CONTROL_LINE_RX_INTERNAL_ERROR:
		break;
	}
	return fail(receiver);
}

enum v92_phase4_digital_rx_result v92_phase4_digital_rx_sample(
	struct v92_phase4_digital_rx *receiver, int16_t sample,
	struct v92_control_event *event)
{
	struct v92_phase4_digital_rx next;
	struct v92_control_event emitted = { 0 };
	enum v92_phase4_digital_rx_result result;

	if (receiver == NULL || event == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), event, sizeof(*event)))
		return V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT;
	if (!v92_phase4_digital_rx_is_valid(receiver))
		return V92_PHASE4_DIGITAL_RX_INVALID_STATE;
	if (state_has_failed(receiver))
		return V92_PHASE4_DIGITAL_RX_FAILED;
	next = *receiver;
	result = next.stage == V92_PHASE4_DIGITAL_RX_TRN2U ?
			 receive_training(receiver, &next, sample) :
			 receive_control(receiver, &next, sample, &emitted);
	if (result != V92_PHASE4_DIGITAL_RX_OK &&
	    result != V92_PHASE4_DIGITAL_RX_ALIGNED &&
	    result != V92_PHASE4_DIGITAL_RX_EVENT)
		return result;
	if (!v92_phase4_digital_rx_is_valid(&next))
		return fail(receiver);
	*receiver = next;
	*event = emitted;
	return result;
}

const char *v92_phase4_digital_rx_result_name(
	enum v92_phase4_digital_rx_result result)
{
	switch (result) {
	case V92_PHASE4_DIGITAL_RX_OK:
		return "ok";
	case V92_PHASE4_DIGITAL_RX_ALIGNED:
		return "aligned";
	case V92_PHASE4_DIGITAL_RX_EVENT:
		return "event";
	case V92_PHASE4_DIGITAL_RX_FAILED:
		return "failed";
	case V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_DIGITAL_RX_INVALID_STATE:
		return "invalid state";
	case V92_PHASE4_DIGITAL_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
