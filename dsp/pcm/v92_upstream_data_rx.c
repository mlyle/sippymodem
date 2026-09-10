// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_data_rx.h"

#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

bool v92_upstream_data_rx_is_valid(
	const struct v92_upstream_data_rx *receiver)
{
	if (receiver == NULL || !v92_pcm_frame_rx_is_valid(&receiver->decoder) ||
	    !v92_upstream_plan_is_valid(&receiver->line.plan) ||
	    receiver->decoder.observation != receiver->line.plan.observation ||
	    (receiver->line.polarity != -1 && receiver->line.polarity != 1) ||
	    receiver->line.symbol_count >= V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    receiver->line.received_frames >
		    receiver->line.input_samples / V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    receiver->decoder.pending_frames !=
		    (receiver->line.received_frames < V92_PCM_RX_DELAY_FRAMES ?
			     receiver->line.received_frames : V92_PCM_RX_DELAY_FRAMES))
		return false;
	if (receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION) {
		const struct v92_upstream_data_line *line = &receiver->line;

		if (line->adaptation != 0.0 ||
		    !memory_region_is_zero(&line->equalizer, sizeof(line->equalizer)) ||
		    !isfinite(line->calibration.gain) ||
		    !isfinite(line->calibration.minimum) ||
		    !isfinite(line->calibration.maximum) ||
		    line->calibration.minimum < 1.0 / 32768.0 ||
		    line->calibration.maximum > 32768.0 ||
		    line->calibration.minimum > line->plan.gain ||
		    line->calibration.maximum < line->plan.gain ||
		    line->calibration.gain < line->calibration.minimum ||
		    line->calibration.gain > line->calibration.maximum ||
		    !isfinite(line->calibration.correlation) ||
		    !isfinite(line->calibration.energy) ||
		    line->calibration.energy < 0.0 ||
		    (line->received_frames == 0u &&
		     (line->calibration.correlation != 0.0 || line->calibration.energy != 0.0)) ||
		    line->delay_remaining > line->plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY ||
		    (line->delay_remaining != 0u && (line->received_frames != 0u || line->symbol_count != 0u)))
			return false;
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			if (line->raw[i] < -32768 || line->raw[i] > 32768 ||
			    (i >= line->symbol_count && (line->raw[i] != 0 || line->samples[i] != 0)))
				return false;
		return true;
	}
	if (!memory_region_is_zero(&receiver->line.calibration, sizeof(receiver->line.calibration)) ||
	    !qam_equalizer_is_handoff_ready(&receiver->line.equalizer) ||
	    receiver->line.equalizer.tap_count != V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS ||
	    receiver->line.equalizer.decision_delay != V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY ||
	    !qam_adaptation_is_valid(receiver->line.adaptation) || receiver->line.delay_remaining != 0u)
		return false;
	for (size_t index = 0u; index < V92_PCM_MAPPING_FRAME_SYMBOLS; ++index) {
		const struct qam_equalizer_observation *observation =
			&receiver->line.observations[index];

		if (index < receiver->line.symbol_count) {
			if (!qam_equalizer_observation_is_pending(observation) ||
			    observation->tap_count != receiver->line.equalizer.tap_count ||
			    observation->decision_delay !=
				    receiver->line.equalizer.decision_delay)
				return false;
		} else if (observation->pending) {
			return false;
		}
	}
	return true;
}

bool v92_upstream_data_rx_b1u_complete(
	const struct v92_upstream_data_rx *receiver)
{
	return v92_upstream_data_rx_is_valid(receiver) && !receiver->failed &&
	       receiver->line.received_frames >= V92_B1U_FRAMES;
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_init(
	struct v92_upstream_data_rx *receiver,
	const struct v92_upstream_rx *handoff,
	const struct v92_pcm_mapping *mapping,
	const struct v92_upstream_plan *plan)
{
	struct v92_upstream_data_rx initialized = { 0 };

	if (receiver == NULL || handoff == NULL || mapping == NULL || plan == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), plan, sizeof(*plan)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   handoff, sizeof(*handoff)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   mapping, sizeof(*mapping)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!v92_upstream_rx_is_valid(handoff) || handoff->failed ||
	    !v92_upstream_plan_is_valid(plan) ||
	    handoff->training_tail_remaining != 0u ||
	    !v92_pcm_mapping_is_valid(mapping) || mapping->law != handoff->line.law)
		return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	initialized.line = (struct v92_upstream_data_line){
		.plan = *plan,
		.input_samples = handoff->line.input_samples,
		.polarity = handoff->line.polarity,
	};
	if (plan->observation == V92_PCM_FRAME_CONSTELLATION) {
		int16_t lu = handoff->line.demodulator.lu;
		struct g711_interval levels;

		if (!g711_quantization_interval(mapping->law, lu, &levels))
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;

		initialized.line.calibration.gain = plan->gain;
		initialized.line.calibration.minimum = fmax(1.0 / 32768.0,
			plan->gain * lu / levels.upper);
		/* LU is positive even when its PCMU cell also contains zero. */
		initialized.line.calibration.maximum = fmin(32768.0,
			plan->gain * lu / fmax(1.0, levels.lower));
		initialized.line.delay_remaining = plan->delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
		if (v92_pcm_frame_rx_init_constellation(&initialized.decoder, mapping) != V92_PCM_FRAME_OK)
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	} else {
		/* Training uses known symbols and a sparse constellation. Dense
		 * data decisions near a companding boundary must not drive that
		 * fast acquisition loop: even a quantized LU estimate can make it
		 * chase the wrong point. Retain the trained FIR and any slower
		 * caller-selected step, but bound decision-directed tracking. */
		initialized.line.equalizer = handoff->line.equalizer;
		initialized.line.adaptation = fmin(handoff->line.adaptation, 0.002);
		if (v92_pcm_frame_rx_init(&initialized.decoder, mapping,
			    handoff->line.demodulator.lu) != V92_PCM_FRAME_OK)
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	}
	if (!v92_upstream_data_rx_is_valid(&initialized))
		return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	*receiver = initialized;
	return V92_UPSTREAM_DATA_RX_OK;
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_reset_control(
	struct v92_upstream_data_rx *receiver)
{
	if (receiver == NULL)
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!v92_upstream_data_rx_is_valid(receiver))
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (receiver->failed)
		return V92_UPSTREAM_DATA_RX_FAILED;
	if (receiver->line.symbol_count != 0u ||
	    receiver->line.received_frames < V92_B1U_FRAMES ||
	    v92_pcm_frame_rx_reset_control(&receiver->decoder) != V92_PCM_FRAME_OK)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	return V92_UPSTREAM_DATA_RX_OK;
}

static int16_t rounded_sample(double level)
{
	if (level >= INT16_MAX)
		return INT16_MAX;
	if (level <= INT16_MIN)
		return INT16_MIN;
	return (int16_t)lround(level);
}

static enum v92_upstream_data_rx_result fail(
	struct v92_upstream_data_rx *receiver, enum v92_upstream_data_rx_result result)
{
	receiver->failed = true;
	return result;
}

static enum v92_upstream_data_rx_result receive_frame(
	struct v92_upstream_data_rx *receiver, struct v92_upstream_data_line *line,
	struct v92_upstream_data_frame *frame)
{
	struct v92_pcm_frame_rx decoder = receiver->decoder;
	struct v92_pcm_frame_decision decision;
	struct v92_upstream_data_frame generated = { 0 };
	bool b1u = line->received_frames < V92_B1U_FRAMES;
	bool raw = line->plan.observation == V92_PCM_FRAME_CONSTELLATION;
	enum v92_pcm_frame_result result = v92_pcm_frame_decode(
		&decoder, line->samples, b1u, generated.bits,
		decoder.mapping.data_bits_per_frame, &decision);

	if (result != V92_PCM_FRAME_OK)
		return fail(receiver, result == V92_PCM_FRAME_NO_PATH ?
			V92_UPSTREAM_DATA_RX_NO_PATH :
			V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	if (raw && line->received_frames < V92_B1U_ACQUISITION_FRAMES) {
		/* TRN2u's sparse, quantized levels leave a small scalar uncertainty
		 * in the inverse. Fit only known B1u coordinates, and never outside
		 * the original training cell: quiet/noisy input must not be scaled
		 * into apparently valid training. Freeze it before the decoder tests
		 * the remaining B1u with ordinary, unconstrained decisions. */
		for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			double sample = line->raw[symbol];

			line->calibration.correlation += sample * decision.predicted_samples[symbol];
			line->calibration.energy += sample * sample;
		}
		if (!isfinite(line->calibration.correlation) || !isfinite(line->calibration.energy))
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
		if (line->calibration.energy > 0.0 && line->calibration.correlation > 0.0)
			line->calibration.gain = fmax(line->calibration.minimum,
				fmin(line->calibration.maximum, line->calibration.correlation / line->calibration.energy));
	}
	/* LU came from a quantized training waveform. A tiny level error can
	 * put a dense CPd point on the opposite side of a much wider G.711 cell.
	 * Fit the known B1u waveform inside that initial uncertainty, not by
	 * weakening the residual ceiling or adapting level from unknown data. */
	if (b1u && !raw) {
		struct g711_interval levels = { decoder.minimum_level, decoder.maximum_level };

		if (line->received_frames == 0u &&
		    !g711_quantization_interval(decoder.mapping.law, decoder.upstream_level, &levels))
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
		if (levels.lower < 1)
			levels.lower = 1;

		if (v92_pcm_frame_rx_refine_level(&decoder, &decision.frame,
			    line->samples, levels.lower, levels.upper) != V92_PCM_FRAME_OK)
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	}
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		struct qam_point target = { .in_phase = decision.predicted_samples[symbol] };

		if (raw) {
			/* Use the decoder's linear observation model in original input
			 * units. G.711 conversion preceded any host conditioning: a
			 * second quantization here would invent different wire cells.
			 * Retain actual companding noise and unclipped prediction error. */
			double error = line->raw[symbol] - target.in_phase / line->calibration.gain;

			generated.squared_error += error * error;
			if (!isfinite(generated.squared_error))
				return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
			continue;
		}
		if (b1u) {
			int16_t sample;

			if (v92_pcm_frame_scale(&decoder.mapping, decoder.upstream_level,
					decision.frame.v[symbol], &sample) != V92_PCM_FRAME_OK)
				return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
			target.in_phase = quantize(decoder.mapping.law, sample);
		}
		/* Preserve the original equalizer residual, including excursions
		 * clipped only for the frame decoder's fixed-point input. */
		double error = line->observations[symbol].output.in_phase - target.in_phase;

		generated.squared_error += error * error;
		if (!isfinite(generated.squared_error))
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
		if (qam_equalizer_adapt_observation(&line->equalizer,
				&line->observations[symbol], &target, line->adaptation) !=
		    QAM_EQUALIZER_OK)
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	}
	if (decision.bits_ready) {
		generated.bit_count = decoder.mapping.data_bits_per_frame;
		generated.b1u = line->received_frames - V92_PCM_RX_DELAY_FRAMES <
			V92_B1U_FRAMES;
		generated.ambiguous = decision.ambiguous;
	}
	line->received_frames++;
	line->symbol_count = 0u;
	memset(line->observations, 0, sizeof(line->observations));
	memset(line->samples, 0, sizeof(line->samples));
	receiver->decoder = decoder;
	receiver->line = *line;
	*frame = generated;
	return V92_UPSTREAM_DATA_RX_FRAME;
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame)
{
	struct v92_upstream_data_line next;
	struct qam_point input;
	bool ready;

	if (receiver == NULL || frame == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   frame, sizeof(*frame)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!v92_upstream_data_rx_is_valid(receiver) ||
	    receiver->line.input_samples == UINT64_MAX)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (receiver->failed)
		return V92_UPSTREAM_DATA_RX_FAILED;
	next = receiver->line;
	if (next.plan.observation == V92_PCM_FRAME_CONSTELLATION) {
		++next.input_samples;
		if (next.delay_remaining != 0u) {
			--next.delay_remaining;
			receiver->line = next;
			return V92_UPSTREAM_DATA_RX_OK;
		}
		next.raw[next.symbol_count] = (int32_t)next.polarity * sample;
		next.samples[next.symbol_count] = rounded_sample(next.raw[next.symbol_count] * next.calibration.gain);
		if (++next.symbol_count == V92_PCM_MAPPING_FRAME_SYMBOLS)
			return receive_frame(receiver, &next, frame);
		receiver->line = next;
		return V92_UPSTREAM_DATA_RX_OK;
	}
	input = (struct qam_point){ .in_phase = next.polarity * (double)sample };
	if (qam_equalizer_observe(&next.equalizer, &input,
				  &next.observations[next.symbol_count], &ready) !=
		    QAM_EQUALIZER_OK || !ready)
		return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	next.samples[next.symbol_count] = rounded_sample(
		next.observations[next.symbol_count].output.in_phase);
	next.symbol_count++;
	next.input_samples++;
	if (next.symbol_count == V92_PCM_MAPPING_FRAME_SYMBOLS)
		return receive_frame(receiver, &next, frame);
	receiver->line = next;
	return V92_UPSTREAM_DATA_RX_OK;
}

const char *v92_upstream_data_rx_result_name(
	enum v92_upstream_data_rx_result result)
{
	switch (result) {
	case V92_UPSTREAM_DATA_RX_OK:
		return "ok";
	case V92_UPSTREAM_DATA_RX_FRAME:
		return "frame";
	case V92_UPSTREAM_DATA_RX_FAILED:
		return "failed";
	case V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_UPSTREAM_DATA_RX_INVALID_STATE:
		return "invalid state";
	case V92_UPSTREAM_DATA_RX_NO_PATH:
		return "no path";
	case V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
