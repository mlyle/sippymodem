// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_data_rx.h"

#include "memory_region.h"
#include "sample_math.h"
#include "v92_data_quality.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Each input crosses at most 128 codec boundaries. A sweep counts both
 * cell solves and transitions; include its terminal check, begin/final
 * stages, and all eight slower refinements for both input hypotheses. */
enum {
	B1_WORK_SAMPLES_PER_TAP = 6,
	FIT_SWEEP_STEPS = (2 * (V92_OBSERVATION_TRAINING - 1) * 128 + 1023) / 1024 + 1,
	FIT_HOLD_SAMPLES = 2 * ((FIT_SWEEP_STEPS + 6) * V92_MODEL_SWEEP_SAMPLES +
		V92_OBSERVATION_INTERVALS * V92_MODEL_REFINE_SAMPLES),
	/* Retained validation adds the difference between scoring credit and
	 * decoded symbols. Three work quanta cover the native 40-sample block
	 * boundary and the fitting stage's reserved credit. */
	B1_WORK_SAMPLES = B1_WORK_SAMPLES_PER_TAP * V92_OBSERVATION_TAPS,
	FIT_QUEUE_SAMPLES = FIT_HOLD_SAMPLES +
		(V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES - V92_B1U_ACQUISITION_FRAMES) *
		(B1_WORK_SAMPLES - V92_PCM_MAPPING_FRAME_SYMBOLS) + 3 * B1_WORK_SAMPLES,
};
_Static_assert((int)V92_UPSTREAM_INPUT_QUEUE >= (int)FIT_QUEUE_SAMPLES,
	"fitting and independent B1 validation must fit in retained input");
_Static_assert(sizeof(((struct v92_upstream_model *)0)->fit) <=
	sizeof(((struct v92_upstream_model *)0)->alternate), "fit must reuse the inactive decoder");

static bool calibrated_gain_is_valid(const struct v92_upstream_data_line *line)
{
	return isfinite(line->calibration.gain) &&
	       isfinite(line->calibration.offset) &&
	       fabs(line->calibration.offset) <= V92_DATA_MAX_RMS_ERROR &&
	       isfinite(line->calibration.minimum) && isfinite(line->calibration.maximum) &&
	       line->calibration.minimum >= 1.0 / 32768.0 && line->calibration.maximum <= 32768.0 &&
	       line->calibration.minimum <= line->plan.gain && line->calibration.maximum >= line->plan.gain &&
	       line->calibration.gain >= line->calibration.minimum &&
	       line->calibration.gain <= line->calibration.maximum;
}

bool v92_upstream_data_rx_is_valid(
	const struct v92_upstream_data_rx *receiver)
{
	if (receiver == NULL || receiver->input.head >= V92_UPSTREAM_INPUT_QUEUE ||
		receiver->input.count > V92_UPSTREAM_INPUT_QUEUE || receiver->input.catchup > 80 ||
		receiver->input.frame_credit >= B1_WORK_SAMPLES ||
		receiver->input.count > UINT64_MAX - receiver->line.input_samples ||
		!v92_pcm_frame_rx_is_valid(&receiver->decoder) ||
	    !v92_upstream_plan_is_valid(&receiver->line.plan) ||
	    !isfinite(receiver->line.output_bias) ||
	    receiver->decoder.observation != receiver->line.plan.observation ||
	    (receiver->line.polarity != -1 && receiver->line.polarity != 1) ||
	    receiver->line.symbol_count >= V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    receiver->line.received_frames >
		    receiver->line.input_samples / V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    receiver->decoder.pending_frames !=
		    (receiver->line.received_frames < V92_PCM_RX_DELAY_FRAMES ?
			     receiver->line.received_frames : V92_PCM_RX_DELAY_FRAMES) ||
	    !v92_upstream_model_is_valid(receiver) ||
	    !v92_residual_is_valid(&receiver->line.residual))
		return false;
	if (receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION) {
		const struct v92_upstream_data_line *line = &receiver->line;

		if ((line->residual.active && line->received_frames < V92_B1U_ACQUISITION_FRAMES) ||
		    line->adaptation != 0.0 || line->output_bias != 0.0 ||
		    !memory_region_is_zero(&line->equalizer, sizeof(line->equalizer)) ||
		    !isfinite(line->host.gain) || line->host.gain < 0.0 ||
		    !isfinite(line->host.lower) || !isfinite(line->host.upper) ||
		    line->host.lower < 0.0 || line->host.lower > line->host.gain ||
		    line->host.upper < line->host.gain ||
		    (line->host.gain == 0.0 && line->host.upper != 0.0) ||
		    (line->received_frames < V92_B1U_ACQUISITION_FRAMES && line->host.gain != 0.0) ||
		    (!memory_region_is_zero(&line->host.fit, sizeof(line->host.fit)) &&
		     (line->received_frames >= V92_B1U_ACQUISITION_FRAMES ||
		      !pcm_gain_is_valid(&line->host.fit))) ||
		    !calibrated_gain_is_valid(line) ||
		    !isfinite(line->calibration.correlation) ||
		    !isfinite(line->calibration.energy) ||
		    !isfinite(line->calibration.raw_sum) || !isfinite(line->calibration.target_sum) ||
		    line->calibration.samples > V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS ||
		    line->calibration.samples % V92_PCM_MAPPING_FRAME_SYMBOLS != 0u ||
		    line->calibration.samples > line->received_frames * V92_PCM_MAPPING_FRAME_SYMBOLS ||
		    fabs(line->calibration.raw_sum) > line->calibration.samples * 32768.0 ||
		    fabs(line->calibration.target_sum) > line->calibration.samples * 32768.0 ||
		    line->calibration.energy < 0.0 ||
		    line->calibration.energy > line->calibration.samples * 1073741824.0 ||
		    fabs(line->calibration.correlation) > line->calibration.samples * 1073741824.0 ||
		    (line->calibration.samples == 0u &&
		     (line->calibration.correlation != 0.0 || line->calibration.energy != 0.0)) ||
		    line->delay_remaining > line->plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY ||
		    (line->delay_remaining != 0u && (line->received_frames != 0u || line->symbol_count != 0u)))
			return false;
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			if (line->raw[i] < -32768 || line->raw[i] > 32768 ||
			    line->correction[i] < -65536 || line->correction[i] > 65536 ||
			    (!line->residual.active && line->correction[i] != 0) ||
			    (line->received[i] && !line->wire_known[i]) ||
			    (!line->wire_known[i] && line->wire[i] != 0) ||
			    (!line->wire_known[i] && line->host_samples[i] != 0) ||
			    (i >= line->symbol_count && (line->raw[i] != 0 || line->samples[i] != 0 || line->correction[i] != 0 ||
				line->wire[i] != 0 || line->host_samples[i] != 0 ||
				line->wire_known[i] || line->received[i])))
				return false;
		return true;
	}
	if (receiver->line.residual.active ||
	    !memory_region_is_zero(receiver->line.correction, sizeof(receiver->line.correction)) ||
	    !memory_region_is_zero(&receiver->line.calibration, sizeof(receiver->line.calibration)) ||
	    !memory_region_is_zero(&receiver->line.host, sizeof(receiver->line.host)) ||
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
	struct v92_upstream_data_line line;

	if (receiver == NULL || handoff == NULL || mapping == NULL || plan == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), plan, sizeof(*plan)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   handoff, sizeof(*handoff)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   mapping, sizeof(*mapping)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!v92_upstream_rx_is_valid(handoff) || handoff->failed ||
	    !v92_upstream_plan_is_valid(plan) ||
	    handoff->training_tail_remaining != 0u || mapping->law != handoff->line.law)
		return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	line = (struct v92_upstream_data_line){
		.plan = *plan,
		.input_samples = handoff->line.input_samples,
		.polarity = handoff->line.polarity,
	};
	if (plan->observation == V92_PCM_FRAME_CONSTELLATION) {
		int16_t lu = handoff->line.demodulator.lu;
		struct g711_interval levels;

		if (!g711_quantization_interval(mapping->law, lu, &levels))
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;

		line.calibration.gain = plan->gain;
		pcm_gain_init(&line.host.fit);
		/* Divide LU by each cell edge first. At an edge the ratio is exactly
		 * one; rounding gain * LU first can otherwise exclude nominal gain. */
		line.calibration.minimum = fmax(1.0 / 32768.0,
			plan->gain * ((double)lu / levels.upper));
		/* LU is positive even when its PCMU cell also contains zero. */
		line.calibration.maximum = fmin(32768.0,
			plan->gain * ((double)lu / fmax(1.0, levels.lower)));
		line.delay_remaining = plan->delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
		if (!calibrated_gain_is_valid(&line))
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	} else {
		/* Training uses known symbols and a sparse constellation. Dense
		 * data decisions near a companding boundary must not drive that
		 * fast acquisition loop: even a quantized LU estimate can make it
		 * chase the wrong point. Retain the trained FIR and any slower
		 * caller-selected step, but bound decision-directed tracking. */
		line.equalizer = handoff->line.equalizer;
		line.output_bias = handoff->line.output_bias;
		line.adaptation = fmin(handoff->line.adaptation, 0.002);
	}
	/* The decoder validates the mapping and commits only on success. Preflight
	 * the line first; zero counters and a fresh model need no speculative histories. */
	enum v92_pcm_frame_result result = plan->observation == V92_PCM_FRAME_CONSTELLATION ?
		v92_pcm_frame_rx_init_constellation(&receiver->decoder, mapping) :
		v92_pcm_frame_rx_init(&receiver->decoder, mapping, handoff->line.demodulator.lu);
	if (result != V92_PCM_FRAME_OK)
		return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	const size_t line_offset = offsetof(struct v92_upstream_data_rx, line);
	memset((char *)receiver + line_offset, 0, sizeof(*receiver) - line_offset);
	receiver->line = line;
	v92_upstream_model_init(receiver);
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
	if (receiver->line.symbol_count != 0u || receiver->line.received_frames < V92_B1U_FRAMES)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	bool paired = v92_upstream_model_has_alternate(receiver);

	/* Preflight validated every active decoder. Reset only clears coding
	 * fields, with no further failure point or change to delayed payload. */
	(void)v92_pcm_frame_rx_reset_control(&receiver->decoder);
	if (paired)
		(void)v92_pcm_frame_rx_reset_control(&receiver->model.alternate);
	return V92_UPSTREAM_DATA_RX_OK;
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_start_b1u(
	struct v92_upstream_data_rx *receiver,
	const struct v92_pcm_mapping *mapping,
	const struct v92_upstream_plan *plan)
{
	if (receiver == NULL || mapping == NULL || plan == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), mapping, sizeof(*mapping)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), plan, sizeof(*plan)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!v92_upstream_data_rx_is_valid(receiver))
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (receiver->failed)
		return V92_UPSTREAM_DATA_RX_FAILED;
	if (receiver->line.symbol_count != 0u || receiver->line.received_frames < V92_B1U_FRAMES)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (!v92_pcm_mapping_is_valid(mapping) || !v92_upstream_plan_is_valid(plan) ||
	    mapping->law != receiver->decoder.mapping.law || mapping->cpd.extend_e2u ||
	    plan->observation != receiver->line.plan.observation || plan->delay != receiver->line.plan.delay)
		return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	struct v92_upstream_data_line line = receiver->line;
	line.plan = *plan;
	line.received_frames = 0;
	if (plan->observation == V92_PCM_FRAME_CONSTELLATION) {
		double ratio = plan->gain / receiver->line.plan.gain;
		line.calibration.gain *= ratio;
		/* The old bounds contain the nominal gain. Preserve that invariant
		 * exactly: multiplying by a rounded ratio can miss it by one ULP. */
		line.calibration.minimum = fmin(plan->gain, line.calibration.minimum * ratio);
		line.calibration.maximum = fmax(plan->gain, line.calibration.maximum * ratio);
		line.calibration.correlation = line.calibration.energy = 0.0;
		line.calibration.raw_sum = line.calibration.target_sum = 0.0;
		line.calibration.samples = 0u;
		pcm_gain_init(&line.host.fit);
		line.host.gain = line.host.lower = line.host.upper = 0.0;
		if (!calibrated_gain_is_valid(&line))
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
		if (v92_pcm_frame_rx_init_constellation(&receiver->decoder, mapping) != V92_PCM_FRAME_OK)
			return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	} else if (v92_pcm_frame_rx_init(&receiver->decoder, mapping, receiver->decoder.upstream_level) != V92_PCM_FRAME_OK) {
		return V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
	}
	/* Decoder init commits only on success and rebased gain is validated.
	 * Reacquire the mapping-dependent model, retaining the warm physical line. */
	receiver->line = line;
	v92_upstream_model_init(receiver);
	return V92_UPSTREAM_DATA_RX_OK;
}

static enum v92_upstream_data_rx_result fail(
	struct v92_upstream_data_rx *receiver, enum v92_upstream_data_rx_result result)
{
	receiver->failed = true;
	return result;
}

static void observe_host_gain(struct v92_upstream_data_line *line)
{
	if (line->plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    line->received_frames >= V92_B1U_ACQUISITION_FRAMES ||
	    line->host.fit.upper == 0.0)
		return;
	for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
		/* Clipping occurred before polarity correction. Missing media or
		 * non-scalar conditioning must not supply a quality normalization. */
		if (!line->wire_known[i] || !line->received[i] ||
		    !pcm_gain_observe(&line->host.fit, line->wire[i] * line->polarity,
			    line->host_samples[i])) {
			memset(&line->host, 0, sizeof(line->host));
			return;
		}
	}
	if (line->received_frames + 1u == V92_B1U_ACQUISITION_FRAMES) {
		if (pcm_gain_estimate(&line->host.fit, &line->host.gain)) {
			line->host.lower = line->host.fit.lower;
			line->host.upper = line->host.fit.upper;
		}
		memset(&line->host.fit, 0, sizeof(line->host.fit));
	}
}

static void fit_offset(struct v92_upstream_data_line *line)
{
	if (line->received_frames + 1u != V92_B1U_ACQUISITION_FRAMES ||
	    line->host.gain <= 0.0 || line->calibration.samples == 0u)
		return;
	double mean = line->calibration.raw_sum / line->calibration.samples;
	double target = line->calibration.target_sum / line->calibration.samples;
	double energy = line->calibration.energy - mean * line->calibration.raw_sum;
	double correlation = line->calibration.correlation - mean * line->calibration.target_sum;
	if (energy <= 0.0 || correlation <= 0.0)
		return;
	double gain = fmax(line->calibration.minimum,
		fmin(line->calibration.maximum, correlation / energy));
	double offset = mean - target / gain;
	/* Complete wire/host evidence identifies the conditioning independently.
	 * Bound the extra analogue degree of freedom by the existing quality
	 * ceiling; constant or badly damaged input must not be fitted into B1u.
	 * Later frames must independently pass both decoding and residual checks. */
	if (isfinite(offset) && fabs(offset) <= V92_DATA_MAX_RMS_ERROR) {
		line->calibration.gain = gain;
		line->calibration.offset = offset;
	}
}

static bool paired_b1(const struct v92_upstream_data_rx *receiver)
{
	return receiver->model.valid == 3 &&
		receiver->line.received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES;
}

static enum v92_upstream_data_rx_result receive_frame(
	struct v92_upstream_data_rx *receiver, struct v92_upstream_data_line *line,
	struct v92_upstream_data_frame *frame)
{
	bool budgeted = receiver->input.frame_limited && paired_b1(receiver);
	if (budgeted && !receiver->input.frame_budget) return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	struct v92_pcm_frame_rx decoder = receiver->decoder;
	struct v92_upstream_model model;
	const size_t model_core = offsetof(struct v92_upstream_model, alternate);
	bool alternate_active = line->received_frames <= V92_B1U_ACQUISITION_FRAMES ||
		v92_upstream_model_has_alternate(receiver);
	struct v92_pcm_frame_decision decision;
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS];
	struct v92_upstream_data_frame generated = { 0 };
	enum v92_pcm_frame_result result;
	bool b1u = line->received_frames < V92_B1U_FRAMES;
	bool raw = line->plan.observation == V92_PCM_FRAME_CONSTELLATION;
	/* Keep the rare alternate transactional without copying it on
	 * every ordinary data frame. Acquisition may first create it here. */
	memcpy(&model, &receiver->model, model_core);
	if (alternate_active)
		model.alternate = receiver->model.alternate;
	result = v92_upstream_model_decode(
		&model, &decoder, line, generated.bits, &decision, predictions);

	if (result != V92_PCM_FRAME_OK)
		return fail(receiver, result == V92_PCM_FRAME_NO_PATH ?
			V92_UPSTREAM_DATA_RX_NO_PATH :
			V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	v92_upstream_model_observe(&model, &decision, line);
	if (raw && line->received_frames < V92_B1U_ACQUISITION_FRAMES &&
	    (model.acquiring || line->received_frames >=
		V92_B1U_ACQUISITION_FRAMES - V92_PCM_RX_DELAY_FRAMES)) {
		/* TRN2u's sparse, quantized levels leave a small scalar uncertainty
		 * in the inverse. Outside explicit codec-model acquisition, wait
		 * for initial coding decisions to settle: early wrong predictions
		 * must not bias the fit used by every later decision. Retain the
		 * original training-cell bounds and freeze before held-out B1u;
		 * quiet/noisy input must not be scaled into valid training. */
		for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			double sample = line->raw[symbol];

			line->calibration.correlation += sample * decision.predicted_samples[symbol];
			line->calibration.energy += sample * sample;
			line->calibration.raw_sum += sample;
			line->calibration.target_sum += decision.predicted_samples[symbol];
		}
		line->calibration.samples += V92_PCM_MAPPING_FRAME_SYMBOLS;
		if (!isfinite(line->calibration.correlation) || !isfinite(line->calibration.energy))
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
		if (line->calibration.energy > 0.0 && line->calibration.correlation > 0.0)
			line->calibration.gain = fmax(line->calibration.minimum,
				fmin(line->calibration.maximum, line->calibration.correlation / line->calibration.energy));
	}
	/* Combine known training inside the original LU cell. An early biased
	 * frame must not exclude a level supported by later B1u observations. */
	if (b1u && !raw) {
		if (v92_pcm_frame_rx_refine_level(&decoder, &model.level, &decision.frame,
			    line->samples) != V92_PCM_FRAME_OK)
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
		if (line->received_frames + 1u == V92_B1U_FRAMES)
			memset(&model.level, 0, sizeof(model.level));
	}
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		struct qam_point target = { .in_phase = decision.predicted_samples[symbol] };

		if (raw) {
			/* Use the decoder's channel prediction in original input
			 * units. G.711 conversion preceded any host conditioning: a
			 * second quantization here would invent different wire cells.
			 * Retain actual companding noise and unclipped prediction error. */
			double error = line->raw[symbol] - line->calibration.offset - line->correction[symbol] -
				predictions[symbol] / line->calibration.gain;

			generated.squared_error += error * error;
			if (!isfinite(generated.squared_error))
				return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
			continue;
		}
		if (b1u) {
			int16_t sample;

			if (v92_pcm_frame_scale(decoder.mapping.cpd.four_times_gain_q0_16,
		decoder.upstream_level,
		decision.frame.v[symbol], &sample) != V92_PCM_FRAME_OK)
				return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
			target.in_phase = g711_quantize(decoder.mapping.law, sample);
		}
		/* Preserve the original equalizer residual, including excursions
		 * clipped only for the frame decoder's fixed-point input. */
		double observed = line->observations[symbol].output.in_phase + line->output_bias;
		double error = observed - target.in_phase;

		generated.squared_error += error * error;
		if (!isfinite(generated.squared_error))
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
		/* Unusable decisions must not train an unrelated signal into an
		 * apparently clean constellation before quality recovery runs.
		 * Also reject targets farther away than silence: nonzero codec
		 * silence can otherwise amplify normalized updates. Retain every
		 * observation and its full residual; known B1u and nearby data
		 * decisions keep their tracking step. */
		double adaptation = line->adaptation;
		if (!b1u && (fabs(error) > V92_DATA_MAX_RMS_ERROR ||
		    fabs(error) > fabs(observed)))
			adaptation = 0.0;
		target.in_phase -= line->output_bias;
		if (qam_equalizer_adapt_observation(&line->equalizer,
				&line->observations[symbol], &target, adaptation) !=
		    QAM_EQUALIZER_OK)
			return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	}
	generated.rm = decision.rm;
	generated.uncertain = decision.uncertain;
	if (decision.bits_ready) {
		generated.bit_count = decoder.mapping.data_bits_per_frame;
		generated.b1u = line->received_frames - V92_PCM_RX_DELAY_FRAMES <
			V92_B1U_FRAMES;
		generated.ambiguous = decision.ambiguous;
		generated.b1u_mismatches = decision.b1u_mismatches;
	}
	observe_host_gain(line);
	if (raw && !model.acquiring)
		fit_offset(line);
	v92_upstream_model_fit(&model, line, &decoder);
	line->received_frames++;
	line->symbol_count = 0u;
	memset(line->observations, 0, sizeof(line->observations));
	memset(line->samples, 0, sizeof(line->samples));
	memset(line->correction, 0, sizeof(line->correction));
	memset(line->wire, 0, sizeof(line->wire));
	memset(line->host_samples, 0, sizeof(line->host_samples));
	memset(line->wire_known, 0, sizeof(line->wire_known));
	memset(line->received, 0, sizeof(line->received));
	receiver->decoder = decoder;
	memcpy(&receiver->model, &model, model_core);
	if (alternate_active)
		memcpy(&receiver->model.alternate, &model.alternate,
			sizeof(model) - model_core);
	receiver->line = *line;
	if (budgeted) receiver->input.frame_budget--;
	*frame = generated;
	return V92_UPSTREAM_DATA_RX_FRAME;
}

static enum v92_upstream_data_rx_result decode_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame, uint8_t code, bool wire_known,
	int16_t host_sample, bool available, bool owned)
{
	struct v92_upstream_data_line next, *line;
	struct qam_point input;
	bool ready;

	if (receiver == NULL || frame == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver),
				   frame, sizeof(*frame)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if ((!owned && !v92_upstream_data_rx_is_valid(receiver)) ||
	    receiver->line.input_samples == UINT64_MAX)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (receiver->failed)
		return V92_UPSTREAM_DATA_RX_FAILED;
	if ((receiver->model.valid || receiver->model.scalar_gain) && !wire_known)
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	/* An equalizer observation is transactional, and a handed-off FIR is
	 * always ready. Only the final frame decode needs a private line. */
	bool final_sample = receiver->line.symbol_count + 1u ==
		V92_PCM_MAPPING_FRAME_SYMBOLS;
	bool direct = !final_sample &&
		(receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION ||
		 receiver->line.equalizer.fill > receiver->line.equalizer.decision_delay);
	line = direct ? &receiver->line : &next;
	if (!direct)
		next = receiver->line;
	if (line->plan.observation == V92_PCM_FRAME_CONSTELLATION) {
		++line->input_samples;
		if (line->delay_remaining != 0u) {
			--line->delay_remaining;
			return V92_UPSTREAM_DATA_RX_OK;
		}
		int32_t raw = (int32_t)line->polarity * sample, correction;
		if (!v92_residual_sample(&line->residual, raw, line->polarity,
		    wire_known && available, &correction))
			return fail(receiver, V92_UPSTREAM_DATA_RX_NO_PATH);
		line->raw[line->symbol_count] = raw;
		line->correction[line->symbol_count] = correction;
		line->wire[line->symbol_count] = wire_known ? line->polarity *
			g711_decode(receiver->decoder.mapping.law, code) : 0;
		line->wire_known[line->symbol_count] = wire_known;
		line->host_samples[line->symbol_count] = wire_known ? host_sample : 0;
		line->received[line->symbol_count] = wire_known && available;
		line->samples[line->symbol_count] = sample_round_s16(
			(line->raw[line->symbol_count] - line->calibration.offset -
			 line->correction[line->symbol_count]) * line->calibration.gain);
		if (++line->symbol_count == V92_PCM_MAPPING_FRAME_SYMBOLS)
			return receive_frame(receiver, line, frame);
		return V92_UPSTREAM_DATA_RX_OK;
	}
	input = (struct qam_point){ .in_phase = line->polarity * (double)sample };
	if (qam_equalizer_observe(&line->equalizer, &input,
				  &line->observations[line->symbol_count], &ready) !=
		    QAM_EQUALIZER_OK || !ready)
		return fail(receiver, V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE);
	line->samples[line->symbol_count] = sample_round_s16(
		line->observations[line->symbol_count].output.in_phase + line->output_bias);
	line->symbol_count++;
	line->input_samples++;
	if (final_sample)
		return receive_frame(receiver, line, frame);
	if (!direct)
		receiver->line = next;
	return V92_UPSTREAM_DATA_RX_OK;
}

void v92_upstream_data_rx_begin_block_owned(struct v92_upstream_data_rx *receiver,
	unsigned samples)
{
	/* Dense codec cells add work beyond the fitted tap count. Carry credit
	 * across all native block sizes, including 40-sample fragments. */
	unsigned frame_work = B1_WORK_SAMPLES_PER_TAP *
		v92_upstream_model_taps(&receiver->decoder.mapping);
	uint64_t credit = (uint64_t)receiver->input.frame_credit + samples;
	receiver->input.frame_limited = true;
	receiver->input.frame_budget = (unsigned)(credit / frame_work);
	receiver->input.frame_credit = (unsigned)(credit % frame_work);
	/* Leave room for a fitting stage that may complete inside this block. */
	if (receiver->model.fitting && receiver->input.frame_budget > 1)
		receiver->input.frame_budget--;
}

static bool frame_blocked(const struct v92_upstream_data_rx *receiver)
{
	return receiver->input.frame_limited && !receiver->input.frame_budget &&
		receiver->line.symbol_count + 1 == V92_PCM_MAPPING_FRAME_SYMBOLS && paired_b1(receiver);
}

/* Consume only a previously accepted physical input. Failure preserves the
 * queued input and the frame transaction; ordinary recovery owns disposal. */
static enum v92_upstream_data_rx_result decode_pending(
	struct v92_upstream_data_rx *receiver, struct v92_upstream_data_frame *frame)
{
	if (frame_blocked(receiver)) return V92_UPSTREAM_DATA_RX_OK;
	struct v92_upstream_input *input = &receiver->input.samples[receiver->input.head];
	if (!(input->flags & 1) && (receiver->model.valid || receiver->model.scalar_gain))
		return fail(receiver, V92_UPSTREAM_DATA_RX_NO_PATH);
	enum v92_upstream_data_rx_result result = decode_sample(receiver, input->sample, frame,
		input->code, input->flags & 1, input->host, input->flags & 2, true);
	if (result != V92_UPSTREAM_DATA_RX_OK && result != V92_UPSTREAM_DATA_RX_FRAME) return result;
	memset(input, 0, sizeof(*input));
	receiver->input.head = (receiver->input.head + 1) % V92_UPSTREAM_INPUT_QUEUE;
	receiver->input.count--;
	if (!receiver->input.count) receiver->input.head = receiver->input.catchup = 0;
	return result;
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_drain_owned(
	struct v92_upstream_data_rx *receiver, struct v92_upstream_data_frame *frame)
{
	if (!receiver || !frame || memory_regions_overlap(receiver, sizeof(*receiver), frame, sizeof(*frame)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (receiver->failed) return V92_UPSTREAM_DATA_RX_FAILED;
	if (!receiver->input.count) return V92_UPSTREAM_DATA_RX_OK;
	if (receiver->model.fitting) {
		v92_upstream_model_progress(receiver);
		if (receiver->model.fitting) return V92_UPSTREAM_DATA_RX_OK;
	}
	return decode_pending(receiver, frame);
}

static enum v92_upstream_data_rx_result receive_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame, uint8_t code, bool wire_known,
	int16_t host_sample, bool available, bool owned)
{
	if (!receiver || !frame || memory_regions_overlap(receiver, sizeof(*receiver), frame, sizeof(*frame)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if ((!owned && !v92_upstream_data_rx_is_valid(receiver)) ||
		receiver->line.input_samples == UINT64_MAX ||
		receiver->input.count >= UINT64_MAX - receiver->line.input_samples)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (receiver->failed) return V92_UPSTREAM_DATA_RX_FAILED;
	if ((receiver->model.valid || receiver->model.scalar_gain) && !wire_known)
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!receiver->model.fitting && !receiver->input.count && !frame_blocked(receiver))
		return decode_sample(receiver, sample, frame, code, wire_known, host_sample, available, true);
	if (receiver->input.count == V92_UPSTREAM_INPUT_QUEUE)
		return fail(receiver, V92_UPSTREAM_DATA_RX_NO_PATH);
	unsigned tail = (receiver->input.head + receiver->input.count) % V92_UPSTREAM_INPUT_QUEUE;
	receiver->input.samples[tail] = (struct v92_upstream_input){sample, host_sample, code,
		(wire_known ? 1 : 0) | (available ? 2 : 0)};
	receiver->input.count++;
	if (receiver->model.fitting) {
		v92_upstream_model_progress(receiver);
		if (receiver->model.fitting) return V92_UPSTREAM_DATA_RX_OK;
	}
	enum v92_upstream_data_rx_result result = V92_UPSTREAM_DATA_RX_OK;
	/* Repay latency after B1u validation, accepting at most one extra decode
	 * per four physical inputs on a selected channel. This limits catch-up
	 * acceleration to 25%; paired decoders retain their slower cadence.
	 * No call emits two frames. */
	unsigned catchup = receiver->line.received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES ?
		0 : receiver->model.valid != 3 ? 4 : 80;
	if (receiver->input.catchup < catchup) receiver->input.catchup++;
	for (unsigned i = 0; i < 2; i++) {
		result = decode_pending(receiver, frame);
		if (result != V92_UPSTREAM_DATA_RX_OK && result != V92_UPSTREAM_DATA_RX_FRAME) return result;
		if (!receiver->input.count) break;
		if (result == V92_UPSTREAM_DATA_RX_FRAME || receiver->model.fitting ||
			!catchup || receiver->input.catchup < catchup ||
			receiver->line.symbol_count + 1 == V92_PCM_MAPPING_FRAME_SYMBOLS || i) break;
		const struct v92_upstream_input *next = &receiver->input.samples[receiver->input.head];
		if ((!(next->flags & 1) && (receiver->model.valid || receiver->model.scalar_gain)) ||
			(receiver->line.residual.active && (next->flags & 3) != 3)) break;
		receiver->input.catchup = 0;
	}
	return result;
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame)
{
	return receive_sample(receiver, sample, frame, 0u, false, 0, false, false);
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_sample_owned(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame)
{
	return receive_sample(receiver, sample, frame, 0u, false, 0, false, true);
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_octet(
	struct v92_upstream_data_rx *receiver, uint8_t code, int16_t sample,
	bool available, struct v92_upstream_data_frame *frame)
{
	return receive_sample(receiver, sample, frame, code, true, sample, available, false);
}

enum v92_upstream_data_rx_result v92_upstream_data_rx_octet_owned(
	struct v92_upstream_data_rx *receiver, uint8_t code, int16_t sample,
	int16_t host_sample, bool available, struct v92_upstream_data_frame *frame)
{
	return receive_sample(receiver, sample, frame, code, true, host_sample, available, true);
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
