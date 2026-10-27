// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_model.h"
#include "v92_upstream_data_rx.h"
#include "integer_math.h"
#include "sample_math.h"

#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(V92_OBSERVATION_TRAINING ==
	V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	"model acquisition must end before held-out B1u decisions");

bool v92_upstream_seed_is_valid(const struct v92_upstream_seed *seed)
{
	return seed && v92_residual_is_valid(&seed->residual) &&
		(seed->residual.active ? isfinite(seed->gain) && seed->gain >= 1. / 32768. &&
		 seed->gain <= 32768. : seed->gain == 0.);
}

bool v92_upstream_model_seed(struct v92_upstream_data_rx *receiver,
	const struct v92_upstream_seed *seed)
{
	if (!v92_upstream_seed_is_valid(seed) || !seed->residual.active ||
	    !v92_upstream_data_rx_is_valid(receiver) || receiver->failed ||
	    !receiver->model.residual_acquiring || receiver->line.received_frames ||
	    receiver->line.symbol_count || seed->gain < receiver->line.calibration.minimum ||
	    seed->gain > receiver->line.calibration.maximum)
		return false;
	receiver->model.seed = *seed;
	memset(receiver->model.seed.residual.history, 0, sizeof(seed->residual.history));
	receiver->model.seed.residual.cursor = 0;
	return true;
}

static void close_acquisition(struct v92_upstream_model *model)
{
	if (model->residual_acquiring)
		memset(&model->alternate, 0, sizeof(model->alternate));
	memset(&model->seed, 0, sizeof(model->seed));
	model->acquiring = false;
	model->fitting = false;
	model->residual_acquiring = false;
	memset(model->x, 0, sizeof(model->x));
	memset(model->raw, 0, sizeof(model->raw));
	memset(model->u, 0, sizeof(model->u));
	memset(model->wire, 0, sizeof(model->wire));
	memset(model->host, 0, sizeof(model->host));
}

bool v92_upstream_model_has_alternate(const struct v92_upstream_data_rx *receiver)
{
	if (receiver->line.plan.observation == V92_PCM_FRAME_FILTER_OUTPUT)
		return receiver->line.received_frames > V92_B1U_ACQUISITION_FRAMES &&
		       receiver->line.received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES;
	return (receiver->model.valid & (1u << (receiver->model.primary ^ 1u))) != 0u;
}

static bool alternate_is_valid(const struct v92_upstream_data_rx *receiver)
{
	return v92_pcm_frame_rx_is_valid(&receiver->model.alternate) &&
	       v92_pcm_mapping_equal(&receiver->model.alternate.mapping, &receiver->decoder.mapping) &&
	       receiver->model.alternate.observation == receiver->decoder.observation &&
	       receiver->model.alternate.pending_frames == receiver->decoder.pending_frames;
}

bool v92_upstream_model_is_valid(const struct v92_upstream_data_rx *receiver)
{
	const struct v92_upstream_model *model = &receiver->model;

	if (!v92_upstream_seed_is_valid(&model->seed) ||
	    (model->seed.residual.active && (!model->residual_acquiring ||
	     model->seed.gain < receiver->line.calibration.minimum ||
	     model->seed.gain > receiver->line.calibration.maximum)))
		return false;
	if (model->residual_acquiring && (model->acquiring || model->valid || model->scalar_gain ||
	    receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    receiver->line.received_frames >= V92_B1U_ACQUISITION_FRAMES || receiver->line.residual.active))
		return false;
	if (receiver->line.residual.active && (model->acquiring || model->valid || model->scalar_gain))
		return false;
	if (model->residual_acquiring && receiver->line.received_frames >= 4 && model->alternate.path_count &&
	    (!v92_pcm_frame_rx_is_valid(&model->alternate) ||
	     !v92_pcm_mapping_equal(&model->alternate.mapping, &receiver->decoder.mapping) ||
	     model->alternate.observation != V92_PCM_FRAME_CONSTELLATION ||
	     (model->alternate.noise.frames != receiver->line.received_frames &&
	      model->alternate.noise.frames + 1u != receiver->line.received_frames)))
		return false;

	if (receiver->line.plan.observation == V92_PCM_FRAME_FILTER_OUTPUT) {
		const struct v92_pcm_level_fit *fit = &model->level;
		unsigned frames = receiver->line.received_frames < V92_B1U_FRAMES ?
			(unsigned)receiver->line.received_frames : 0u;
		if (fit->frames != frames || (frames &&
		    (fit->minimum <= 0 || fit->minimum > receiver->decoder.upstream_level ||
		     fit->maximum < receiver->decoder.upstream_level ||
		     fit->maximum - fit->minimum >= V92_PCM_LEVEL_FIT_POINTS)))
			return false;
	}
	if (model->valid > 3u || model->primary > 1u || model->coefficient < 0 ||
	    !isfinite(model->scalar_gain) || model->scalar_gain < 0. ||
	    (model->scalar_gain && (model->acquiring || model->valid || model->coefficient == 0 ||
		 receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
		 receiver->line.received_frames < V92_B1U_ACQUISITION_FRAMES || receiver->line.host.gain <= 0.)))
		return false;
	if (!model->acquiring && !model->valid)
		return model->primary == 0u && model->cost[0] == 0u && model->cost[1] == 0u &&
		       (!v92_upstream_model_has_alternate(receiver) || alternate_is_valid(receiver));
	if (receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    model->coefficient == 0)
		return false;
	if (model->fitting)
		return model->acquiring && !model->residual_acquiring && (model->valid == 0 || model->valid == 1) &&
			model->fit.candidate < 2 && model->fit.clock < V92_MODEL_REFINE_SAMPLES &&
			model->fit.work.stage <= V92_FIT_VALIDATE && receiver->line.received_frames == V92_B1U_ACQUISITION_FRAMES;
	if (model->acquiring)
		return model->valid == 0u && model->primary == 0u &&
		       model->cost[0] == 0u && model->cost[1] == 0u &&
		       receiver->line.received_frames < V92_B1U_ACQUISITION_FRAMES;
	if (!(model->valid & (1u << model->primary)) || model->cost[model->primary] != 0u ||
	    receiver->line.received_frames < V92_B1U_ACQUISITION_FRAMES)
		return false;
	for (unsigned i = 0u; i < 2u; ++i) {
		if (!(model->valid & (1u << i)))
			continue;
		if (!v92_observation_is_valid(&model->model[i]) ||
		    model->model[i].law != receiver->decoder.mapping.law ||
		    model->model[i].input_coefficient != (i ? model->coefficient : 0))
			return false;
	}
	if (model->valid & (1u << (model->primary ^ 1u)))
		return alternate_is_valid(receiver);
	return true;
}

void v92_upstream_model_init(struct v92_upstream_data_rx *receiver)
{
	struct v92_upstream_model *model = &receiver->model;
	const struct v92_cpd_parameters *cpd = &receiver->decoder.mapping.cpd;
	const struct v92_cpd_filter *filter = &cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD];
	unsigned delay = receiver->line.plan.delay;
	unsigned history = cpd->filter[V92_CPD_PRECODER_FEEDBACK].count;

	memset(model, 0, sizeof(*model));
	memset(&receiver->line.residual, 0, sizeof(receiver->line.residual));
	model->residual_acquiring = receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION;
	if (receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    delay >= filter->count || filter->coefficient[delay] <= 0)
		return;
	for (unsigned i = 0u; i < filter->count; ++i)
		if (i != delay && filter->coefficient[i])
			return;
	for (unsigned i = 0u; i < cpd->filter[V92_CPD_PREFILTER_FEEDBACK].count; ++i)
		if (cpd->filter[V92_CPD_PREFILTER_FEEDBACK].coefficient[i])
			return;
	if (filter->count > history + 1u)
		history = filter->count - 1u;
	if (history < 2u)
		return;
	model->coefficient = filter->coefficient[delay];
	model->acquiring = true;
	model->residual_acquiring = false;
}

void v92_upstream_model_observe(struct v92_upstream_model *model,
	const struct v92_pcm_frame_decision *decision, const struct v92_upstream_data_line *line)
{
	const struct v92_pcm_frame *frame = &decision->frame;
	if (model->residual_acquiring) {
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++)
			if (!line->wire_known[i] || !line->received[i]) {
				close_acquisition(model);
				return;
			}
		size_t offset = line->received_frames * V92_PCM_MAPPING_FRAME_SYMBOLS;
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) {
			model->x[offset + i] = decision->predicted_samples[i];
			model->raw[offset + i] = line->raw[i];
		}
		return;
	}
	if (!model->acquiring || line->received_frames >= V92_B1U_ACQUISITION_FRAMES)
		return;
	size_t offset = line->received_frames * V92_PCM_MAPPING_FRAME_SYMBOLS;

	/* V.92 6.4.2 permits any representative in the encoded residue class.
	 * Known B1u bits constrain that class, not the peer's power-selection
	 * policy. Train from the received points and reconstructed filter state,
	 * never a locally encoded waveform. The held-out half of B1u still
	 * validates the resulting fit with unconstrained bit decisions. */
	memcpy(model->x + offset, frame->x, sizeof(frame->x));
	memcpy(model->u + offset, frame->u, sizeof(frame->u));
	memcpy(model->raw + offset, line->raw, sizeof(line->raw));
	memcpy(model->wire + offset, line->wire, sizeof(line->wire));
	memcpy(model->host + offset, line->host_samples, sizeof(line->host_samples));
}

/* Echo subtraction is independent of the decoded candidate. Identify gain
 * from the unmodified host sample, then translate the actual wire cell into
 * decoder coordinates by the subtracted echo and frozen B1u offset. Clipping
 * loses that affine contract; a changed host gain must not become an echo
 * correction. Unqualified acquisition retains the linear receiver, while
 * loss of this contract after fitting takes the ordinary no-path recovery. */
static bool observation_offset(const struct v92_upstream_data_line *line,
	int32_t raw, int16_t wire, int16_t host, double *offset)
{
	double sample = (raw - line->calibration.offset) * line->calibration.gain;
	int32_t physical = raw * line->polarity;
	/* Test containment using the acquired rounding interval, not the
	 * chosen point estimate. Do not narrow it using held-out data. */
	struct pcm_gain gain = { .lower = line->host.lower, .upper = line->host.upper };
	if (host == INT16_MIN || host == INT16_MAX ||
	    physical <= INT16_MIN || physical >= INT16_MAX ||
	    sample <= INT16_MIN || sample >= INT16_MAX ||
	    !pcm_gain_observe(&gain, wire * line->polarity, host))
		return false;
	/* Rounding the calibrated sample does not shift the codec interval. */
	*offset = (raw - (int32_t)host * line->polarity - line->calibration.offset) * line->calibration.gain;
	return isfinite(*offset);
}

/* Compare only ordinary-symbol residue classes: the fourth symbol also
 * depends on trellis parity. Shared scrambling and mapping identify the
 * classes without assuming the peer chooses our precoder representatives.
 * A single leading frame can remain at the control/data transition. This
 * selects acquisition context only; receive timing and payload stay intact. */
static double residual_prefix_score(const struct v92_pcm_mapping *mapping,
	const int32_t *raw, double gain, unsigned prefix)
{
	struct v34_scrambler scrambler;
	uint8_t differential = 0;
	double score = 0.;
	if (v34_scrambler_init(&scrambler, V34_POLYNOMIAL_GPA) != V34_FRAMING_OK)
		return INFINITY;
	for (unsigned frame = prefix; frame < 4; frame++) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], digits[V92_PCM_MAPPING_FRAME_SYMBOLS], sign;
		memset(bits, 1, mapping->data_bits_per_frame);
		if (v34_scramble_unpacked_bits(&scrambler, bits, mapping->data_bits_per_frame, bits) != V34_FRAMING_OK ||
		    v92_pcm_mapping_encode_owned(mapping, bits, mapping->data_bits_per_frame,
			&differential, digits, &sign) != V92_PCM_MAPPING_OK)
			return INFINITY;
		if (frame == 0) continue;
		for (unsigned symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; symbol++) {
			if (symbol % 4 == 3) continue;
			const struct v92_cpd_constellation *points = &mapping->cpd.constellation[
				mapping->cpd.frame_constellation[symbol % V92_PCM_MAPPING_PHASES]];
			int modulus = mapping->cpd.modulus[symbol];
			int first = -(int)points->count;
			first += ((int)digits[symbol] - first) % modulus;
			double sample = raw[frame * V92_PCM_MAPPING_FRAME_SYMBOLS + symbol] * gain;
			double distance = INFINITY;
			for (int eta = first; eta < points->count; eta += modulus) {
				double point = eta < 0 ? -points->positive_linear[-eta - 1] : points->positive_linear[eta];
				distance = fmin(distance, (sample - point) * (sample - point));
			}
			score += distance;
		}
	}
	return score;
}

static bool residual_coding_is_known(const struct v92_upstream_model *model, unsigned states)
{
	return !model->seed.residual.active || states <= V92_PCM_RX_PATHS;
}

static void acquire_residual_points(struct v92_upstream_model *model,
	const struct v92_upstream_data_line *line, const struct v92_pcm_frame_rx *decoder)
{
	unsigned first = (unsigned)line->received_frames;
	unsigned states = v34_trellis_state_count(decoder->paths[0].trellis.selection);
	bool known = residual_coding_is_known(model, states);

	if (first < 3) return;
	if (first == 3) {
		/* A physical correction does not fix the coding epoch. Small
		 * trellises can acquire known B1u across all initial histories;
		 * retain control-prefix selection for that candidate too. */
		first = 0;
		if (known) {
			double direct = residual_prefix_score(&decoder->mapping, model->raw, line->calibration.gain, 0);
			double delayed = residual_prefix_score(&decoder->mapping, model->raw, line->calibration.gain, 1);
			first = delayed < direct;
		}
		if (v92_pcm_frame_rx_init_constellation(&model->alternate, &decoder->mapping) != V92_PCM_FRAME_OK)
			return;
		/* Keep every 16/32-state hypothesis in the existing beam. A wrong
		 * history must not become a physical channel fit. The 64-state
		 * case keeps its preceding acquisition behavior. */
		if (states <= V92_PCM_RX_PATHS) {
			for (unsigned i = 1; i < states; i++) {
				model->alternate.paths[i] = model->alternate.paths[0];
				model->alternate.paths[i].trellis.state = i;
			}
			model->alternate.path_count = (uint8_t)states;
		}
	}
	if (!model->alternate.path_count) return;
	/* Catch up four frames once, then one frame per invocation. Existing
	 * acquisition scratch and the otherwise idle alternate own this work. */
	for (unsigned frame = first; frame <= line->received_frames; frame++) {
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame_decision decision;
		size_t offset = frame * V92_PCM_MAPPING_FRAME_SYMBOLS;
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) {
			int32_t correction = 0;
			if (!v92_residual_sample(&model->seed.residual, model->raw[offset + i],
			    1, true, &correction)) {
				memset(&model->alternate, 0, sizeof(model->alternate));
				return;
			}
			double gain = model->seed.residual.active ? model->seed.gain : line->calibration.gain;
			samples[i] = sample_round_s16((model->raw[offset + i] - correction) * gain);
		}
		enum v92_pcm_frame_result result = known ?
			v92_pcm_frame_acquire_model(&model->alternate, samples, bits,
				decoder->mapping.data_bits_per_frame, &decision) :
			v92_pcm_frame_decode(&model->alternate, samples, true, bits,
				decoder->mapping.data_bits_per_frame, &decision);
		if (result != V92_PCM_FRAME_OK) {
			memset(&model->alternate, 0, sizeof(model->alternate));
			return;
		}
		memcpy(model->u + offset, decision.predicted_samples, sizeof(decision.predicted_samples));
	}
}

static bool fit_residual(struct v92_upstream_model *model, struct v92_upstream_data_line *line)
{
	bool selected = false;
	double best = INFINITY;
	(void)v92_residual_fit(&line->residual, model->x, model->raw,
		line->calibration.minimum, line->calibration.maximum, &line->calibration.gain, &best);
	if (model->alternate.path_count) {
		struct v92_residual candidate;
		int32_t points[V92_OBSERVATION_TRAINING];
		double gain, score;
		for (unsigned i = 0; i < V92_OBSERVATION_TRAINING; i++) points[i] = model->u[i];
		/* A constant point translation has the same physical fit after a
		 * DC shift. Use the known B1 coding history to resolve that exact
		 * ambiguity, rather than roundoff between equivalent fit scores.
		 * A retained physical seed also has known coding for the 16/32-state
		 * acquisitions above. Independent later B1 decisions still qualify
		 * the selected fit. */
		if (residual_coding_is_known(model, v34_trellis_state_count(
			    model->alternate.paths[0].trellis.selection)) && isfinite(best) &&
		    v92_residual_translate(&line->residual, model->x, points,
			model->raw, line->calibration.gain)) {
			selected = true;
		} else if (v92_residual_fit(&candidate, points, model->raw, line->calibration.minimum,
		    line->calibration.maximum, &gain, &score) && score < best) {
			line->residual = candidate;
			line->calibration.gain = gain;
			selected = true;
		}
	}
	if (line->residual.active) line->calibration.offset = 0.;
	return selected;
}

void v92_upstream_model_fit(struct v92_upstream_model *model,
	struct v92_upstream_data_line *line, struct v92_pcm_frame_rx *decoder)
{
	if (model->residual_acquiring) {
		acquire_residual_points(model, line, decoder);
		if (line->received_frames + 1u == V92_B1U_ACQUISITION_FRAMES) {
			if (line->host.gain > 0. && fit_residual(model, line)) {
				/* Carry the selected acquisition's coding history into
				 * independent decoding. Old erroneous decisions must not
				 * poison the first validation bits through the descrambler.
				 * Retain the physical-frame quality clock. */
				struct v92_pcm_frame_noise noise = decoder->noise;
				*decoder = model->alternate;
				decoder->noise = noise;
			}
			close_acquisition(model);
		}
		return;
	}

	if (!model->acquiring || line->received_frames + 1u != V92_B1U_ACQUISITION_FRAMES)
		return;
	if (line->host.gain <= 0.) { close_acquisition(model); return; }
	memset(&model->alternate, 0, sizeof(model->alternate));
	for (size_t i = 0; i < V92_OBSERVATION_TRAINING; i++) {
		if (!observation_offset(line, model->raw[i], model->wire[i], model->host[i], model->fit.offsets + i)) {
			close_acquisition(model); return;
		}
		model->fit.samples[i] = sample_round_s16((model->raw[i] - line->calibration.offset) * line->calibration.gain);
	}
	model->fitting = true;
}

void v92_upstream_model_progress(struct v92_upstream_data_rx *receiver)
{
	struct v92_upstream_model *model = &receiver->model;
	if (!model->fitting) return;
	if (model->fit.clock) { model->fit.clock--; return; }
	unsigned candidate = model->fit.candidate;
	struct v92_observation_training training = {
		.input = model->fit.input, .linear = model->u, .sample = model->fit.samples, .levels = model->wire,
		.offsets = model->fit.offsets, .count = V92_OBSERVATION_TRAINING,
		.taps = v92_upstream_model_taps(&receiver->decoder.mapping),
		.host_gain = receiver->line.host.gain * receiver->line.calibration.gain,
		.law = receiver->decoder.mapping.law,
	};
	enum v92_observation_fit_result result = V92_OBSERVATION_FIT_MORE;
	unsigned stage = model->fit.work.stage;
	if (!stage) {
		for (size_t i = 0; i < V92_OBSERVATION_TRAINING; i++)
			model->fit.input[i] = candidate ? round_shift_s64_to_s32_saturating(
				(int64_t)model->x[i] * model->coefficient, 15u) : model->x[i];
		if (!v92_observation_fit_begin(&model->fit.work, &training)) result = V92_OBSERVATION_FIT_DECLINED;
	} else result = v92_observation_fit_step(&model->fit.work, &training, &model->model[candidate]);
	if (result == V92_OBSERVATION_FIT_READY) {
		model->model[candidate].input_coefficient = candidate ? model->coefficient : 0;
		v92_observation_geometry_reset(&receiver->geometry[candidate]);
		model->valid |= (uint8_t)(1u << candidate);
	} else if (result == V92_OBSERVATION_FIT_DECLINED)
		memset(&model->model[candidate], 0, sizeof(model->model[candidate]));
	if (result != V92_OBSERVATION_FIT_MORE) {
		model->fit.candidate++;
		if (model->fit.candidate == 2) {
			if (model->valid) model->primary = (model->valid & 1u) ? 0 : 1;
			else v92_observation_fit_scalar(&model->scalar_gain, &training);
			memset(&model->alternate, 0, sizeof(model->alternate));
			if (model->valid == 3) model->alternate = receiver->decoder;
			close_acquisition(model);
			return;
		}
	}
	model->fit.clock = stage == V92_FIT_REFINE ? V92_MODEL_REFINE_SAMPLES - 1 : V92_MODEL_SWEEP_SAMPLES - 1;
}

/* Once every held-out B1u bit and its physical quality have qualified, a
 * decisively separated frozen channel need not retain a competing decoder.
 * Keep uncertain channels paired. This margin is sixteen times the decoder's
 * ambiguity boundary; it is model-selection evidence, not a measured BER. */
bool v92_upstream_model_finish_b1u(struct v92_upstream_data_rx *receiver)
{
	if (!receiver || receiver->line.received_frames != V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES ||
		receiver->model.fitting || receiver->model.valid != 3) return false;
	struct v92_upstream_model *model = &receiver->model;
	unsigned other = model->primary ^ 1u;
	if (model->cost[other] < UINT64_C(16) * V92_PCM_RX_AMBIGUITY_COST) return false;
	model->valid = (uint8_t)(1u << model->primary);
	model->cost[other] = 0;
	memset(&model->alternate, 0, sizeof(model->alternate));
	return true;
}

static bool wire_cells(const struct v92_upstream_data_line *line, enum g711_law law,
	struct v92_pcm_frame_cell cells[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	if (line->plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    line->received_frames < V92_B1U_ACQUISITION_FRAMES - V92_PCM_RX_DELAY_FRAMES)
		return false;
	bool acquiring = line->received_frames < V92_B1U_ACQUISITION_FRAMES;
	struct pcm_gain bounds = acquiring ? line->host.fit : (struct pcm_gain){
		.lower = line->host.lower, .upper = line->host.upper,
	};
	double gain = line->host.gain;

	/* Only actual wire/host pairs identify post-codec conditioning. During
	 * acquisition use the independent accumulated fit; held-out B1u and
	 * payload may verify its frozen bounds but must never refit the gain.
	 * Let initial coding decisions settle before interval evidence changes
	 * the ranking used by the independently bounded analogue gain fit. */
	if (acquiring && !pcm_gain_estimate(&bounds, &gain))
		return false;
	if (gain <= 0.0)
		return false;
	for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
		struct g711_interval cell;
		int32_t raw = line->raw[i] * line->polarity;
		int16_t host = line->host_samples[i];
		struct pcm_gain checked = bounds;

		if (!line->wire_known[i] || !line->received[i] ||
		    raw <= INT16_MIN || raw >= INT16_MAX ||
		    host == INT16_MIN || host == INT16_MAX ||
		    !pcm_gain_observe(&checked, line->wire[i] * line->polarity, host) ||
		    !g711_quantization_interval(law, line->wire[i] * line->polarity, &cell))
			return false;
		double offset = line->raw[i] - (int32_t)host * line->polarity -
			line->calibration.offset - line->correction[i];
		double lower = line->polarity > 0 ? cell.lower : -(double)cell.upper;
		double upper = line->polarity > 0 ? cell.upper : -(double)cell.lower;

		cells[i] = (struct v92_pcm_frame_cell){
			.lower = (lower * gain + offset) * line->calibration.gain,
			.upper = (upper * gain + offset) * line->calibration.gain,
		};
	}
	return true;
}

/* A noisy acquisition word can exhaust the unconstrained decoder before
 * its channel fit is ready. The retained known-B1 candidate may bridge that
 * word, but never validation or payload, missing media, or seeded unknown
 * coding. A seeded 16/32-state candidate also searches known B1u across all
 * initial histories. The independent second half still owns readiness. */
static bool recover_acquisition(const struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision)
{
	if (!model->residual_acquiring ||
	    (model->seed.residual.active &&
	     v34_trellis_state_count(decoder->paths[0].trellis.selection) > V92_PCM_RX_PATHS) ||
	    !model->alternate.path_count || line->received_frames < 3u ||
	    line->received_frames >= V92_B1U_ACQUISITION_FRAMES)
		return false;
	for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++)
		if (!line->wire_known[i] || !line->received[i]) return false;
	struct v92_pcm_frame_rx recovered = model->alternate;
	unsigned pending = line->received_frames < V92_PCM_RX_DELAY_FRAMES ?
		(unsigned)line->received_frames : V92_PCM_RX_DELAY_FRAMES;

	if (recovered.pending_frames != pending) {
		if (recovered.pending_frames + 1u != pending) return false;
		/* Known acquisition may have skipped one control-prefix frame.
		 * Preserve its place in traceback as nondata, never invented bits.
		 * This slot ages out before independent B1u validation begins. */
		for (unsigned i = 0; i < recovered.path_count; i++) {
			struct v92_pcm_frame_rx_path *path = &recovered.paths[i];

			memmove(path->pending + 1, path->pending,
				recovered.pending_frames * sizeof(path->pending[0]));
			memset(path->pending[0], 0, sizeof(path->pending[0]));
			path->ambiguous_frames <<= 1;
			path->nondata_frames = (uint16_t)((path->nondata_frames << 1) | 1u);
			path->b1u_code_mismatches = (uint16_t)((path->b1u_code_mismatches << 1) | 1u);
		}
		recovered.pending_frames++;
	}
	if (v92_pcm_frame_acquire_model(&recovered, line->samples, bits,
	    decoder->mapping.data_bits_per_frame, decision) != V92_PCM_FRAME_OK)
		return false;
	*decoder = recovered;
	return true;
}

enum v92_pcm_frame_result v92_upstream_model_decode(struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision,
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS],
	struct v92_observation_geometry geometry[2])
{
	bool b1u = line->received_frames < V92_B1U_FRAMES;
	size_t count = decoder->mapping.data_bits_per_frame;
	/* A model cannot be fitted without every actual wire observation. Decline
	 * it before imposing a known-bit decision on this frame, so missing media
	 * or sample-only input retains ordinary linear coding acquisition. */
	if (model->acquiring)
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			if (!line->wire_known[i] || !line->received[i]) {
				close_acquisition(model);
				break;
			}

	double offsets[V92_PCM_MAPPING_FRAME_SYMBOLS] = { 0 };
	if (model->valid || model->scalar_gain)
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++)
			if (line->received[i] && !observation_offset(line,
				line->raw[i], line->wire[i], line->host_samples[i], offsets + i))
				return V92_PCM_FRAME_NO_PATH;

	if (!model->valid) {
		bool validate = decoder->observation == V92_PCM_FRAME_FILTER_OUTPUT &&
			line->received_frames >= V92_B1U_ACQUISITION_FRAMES &&
			line->received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES;

		/* Known-one decisions retain the primary training and calibration
		 * path, but cannot prove that the peer sent ones: a wrong late bit
		 * can fit below the aggregate residual ceiling. Decode the remaining
		 * B1u independently at the acquired level, without imposing its bits
		 * or changing primary histories, predictions, or data decisions. */
		if (validate && line->received_frames == V92_B1U_ACQUISITION_FRAMES)
			model->alternate = *decoder;
		struct v92_pcm_frame_cell cells[V92_PCM_MAPPING_FRAME_SYMBOLS];
		enum v92_pcm_frame_result result;
		if (model->acquiring) {
			result = v92_pcm_frame_acquire_model(decoder, line->samples, bits, count, decision);
		} else if (wire_cells(line, decoder->mapping.law, cells)) {
			struct v92_pcm_frame_channel channel = { .cells = cells };
			struct v92_pcm_frame_likelihood likelihood;

			result = v92_pcm_frame_decode_observed(decoder, line->samples,
				b1u, bits, count, decision, &channel, &likelihood);
		} else {
			result = v92_pcm_frame_decode(decoder, line->samples, b1u, bits, count, decision);
		}

		if (result == V92_PCM_FRAME_NO_PATH &&
		    recover_acquisition(model, decoder, line, bits, decision))
			result = V92_PCM_FRAME_OK;
		if (result != V92_PCM_FRAME_OK)
			return result;
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			predictions[i] = model->scalar_gain ? v92_observation_predict_scalar(
				decoder->mapping.law, model->scalar_gain, line->host.gain * line->calibration.gain,
				decision->predicted_samples[i], offsets[i]) : decision->predicted_samples[i];
			if (!isfinite(predictions[i])) return V92_PCM_FRAME_NO_PATH;
		}
		if (validate) {
			uint8_t checked[V92_PCM_MAPPING_MAX_BITS];
			struct v92_pcm_frame_decision validation;
			bool failed = v92_pcm_frame_decode(&model->alternate, line->samples,
				false, checked, count, &validation) != V92_PCM_FRAME_OK;

			if (!failed) {
				failed = !validation.bits_ready || validation.ambiguous;
				/* The known primary cannot certify B1u. Keep the
				 * independent decoder's two complete-window explanations. */
				decision->b1u_mismatches = validation.b1u_mismatches;
				failed |= validation.b1u_mismatches == V92_B1U_BOTH_MISMATCH;
			}
			if (decision->bits_ready)
				decision->ambiguous |= failed;
			if (line->received_frames + 1u == V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES)
				memset(&model->alternate, 0, sizeof(model->alternate));
		}
		return result;
	}
	struct v92_pcm_frame_likelihood likelihood[2];
	struct v92_pcm_frame_decision decisions[2];
	uint8_t output[2][V92_PCM_MAPPING_MAX_BITS];
	unsigned primary = model->primary;
	struct v92_pcm_frame_rx *branch[2];

	branch[primary] = decoder;
	branch[primary ^ 1u] = &model->alternate;
	for (unsigned i = 0u; i < 2u; ++i) {
		if (!(model->valid & (1u << i)))
			continue;
		struct v92_pcm_frame_channel channel = {
			.model = &model->model[i], .geometry = geometry ? &geometry[i] : NULL,
			.levels = line->wire, .received = line->received,
			.offsets = offsets,
		};
		enum v92_pcm_frame_result result = v92_pcm_frame_decode_observed_owned(branch[i],
			line->samples, b1u, output[i], count, &decisions[i], &channel, &likelihood[i]);

		if (result != V92_PCM_FRAME_OK || likelihood[i].cost > UINT64_MAX - model->cost[i])
			model->valid &= (uint8_t)~(1u << i);
		else
			model->cost[i] += likelihood[i].cost;
	}
	if (!model->valid)
		return V92_PCM_FRAME_NO_PATH;
	unsigned chosen = (model->valid & 1u) &&
		(!(model->valid & 2u) || model->cost[0] <= model->cost[1]) ? 0u : 1u;
	unsigned other = chosen ^ 1u;
	uint64_t gap = model->valid & (1u << other) ?
		model->cost[other] - model->cost[chosen] : UINT64_MAX;

	*decision = decisions[chosen];
	if ((model->valid & (1u << other)) && gap <= V92_PCM_RX_AMBIGUITY_COST &&
	    decisions[other].rm != decision->rm)
		decision->rm = V92_PCM_FRAME_RM_NONE;
	memcpy(predictions, likelihood[chosen].predicted_samples,
		sizeof(likelihood[chosen].predicted_samples));
	if (decision->bits_ready) {
		memcpy(bits, output[chosen], count);
		if ((model->valid & (1u << other)) && gap <= V92_PCM_RX_AMBIGUITY_COST &&
		    (!decisions[other].bits_ready || decisions[other].ambiguous ||
		     memcmp(output[chosen], output[other], count)))
			decision->ambiguous = true;
		if ((model->valid & (1u << other)) && gap <= V92_PCM_RX_AMBIGUITY_COST)
			decision->b1u_mismatches |= decisions[other].b1u_mismatches;
	}
	if (chosen != primary) {
		unsigned char *current = (unsigned char *)decoder;
		unsigned char *alternate = (unsigned char *)&model->alternate;
		unsigned char old[16];

		for (size_t i = 0; i < sizeof(*decoder); i += sizeof(old)) {
			size_t count = sizeof(*decoder) - i;

			if (count > sizeof(old))
				count = sizeof(old);
			memcpy(old, current + i, count);
			memcpy(current + i, alternate + i, count);
			memcpy(alternate + i, old, count);
		}
		model->primary = chosen;
	}
	/* Relative evidence bounds growth over arbitrarily long calls. */
	uint64_t minimum = model->cost[chosen];

	for (unsigned i = 0u; i < 2u; ++i)
		model->cost[i] = (model->valid & (1u << i)) ? model->cost[i] - minimum : 0u;
	return V92_PCM_FRAME_OK;
}
