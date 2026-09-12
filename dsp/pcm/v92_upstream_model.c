// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_model.h"
#include "v92_upstream_data_rx.h"
#include "integer_math.h"
#include "pcm_gain.h"

#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(V92_OBSERVATION_TRAINING ==
	V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	"model acquisition must end before held-out B1u decisions");

static void close_acquisition(struct v92_upstream_model *model)
{
	model->acquiring = false;
	memset(&model->known, 0, sizeof(model->known));
	memset(model->x, 0, sizeof(model->x));
	memset(model->raw, 0, sizeof(model->raw));
	memset(model->u, 0, sizeof(model->u));
	memset(model->wire, 0, sizeof(model->wire));
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
	if (model->valid > 3u || model->primary > 1u || model->coefficient < 0)
		return false;
	if (!model->acquiring && !model->valid)
		return model->primary == 0u && model->cost[0] == 0u && model->cost[1] == 0u &&
		       (!v92_upstream_model_has_alternate(receiver) || alternate_is_valid(receiver));
	if (receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    model->coefficient == 0)
		return false;
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
}

bool v92_upstream_model_observe(struct v92_upstream_model *model,
	const struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line)
{
	if (!model->acquiring || line->received_frames >= V92_B1U_ACQUISITION_FRAMES)
		return true;
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
		if (!line->wire_known[i] || !line->received[i]) {
			close_acquisition(model);
			return true;
		}
	}
	struct v92_pcm_frame_tx known = { .mapping = decoder->mapping, .state = model->known };
	struct v92_pcm_frame frame;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	size_t offset = line->received_frames * V92_PCM_MAPPING_FRAME_SYMBOLS;

	if (line->received_frames == 0u &&
	    v92_pcm_frame_tx_init(&known, &decoder->mapping) != V92_PCM_FRAME_OK)
		return false;
	memset(bits, 1, decoder->mapping.data_bits_per_frame);
	if (v92_pcm_frame_encode(&known, bits, decoder->mapping.data_bits_per_frame,
			&frame) != V92_PCM_FRAME_OK)
		return false;
	model->known = known.state;
	memcpy(model->x + offset, frame.x, sizeof(frame.x));
	memcpy(model->u + offset, frame.u, sizeof(frame.u));
	memcpy(model->raw + offset, line->raw, sizeof(line->raw));
	memcpy(model->wire + offset, line->wire, sizeof(line->wire));
	return true;
}

void v92_upstream_model_fit(struct v92_upstream_model *model,
	const struct v92_upstream_data_line *line, const struct v92_pcm_frame_rx *decoder)
{
	struct pcm_gain host;
	int16_t samples[V92_OBSERVATION_TRAINING];
	double gain;

	if (!model->acquiring || line->received_frames + 1u != V92_B1U_ACQUISITION_FRAMES)
		return;
	pcm_gain_init(&host);
	for (size_t i = 0u; i < V92_OBSERVATION_TRAINING; ++i) {
		/* Host clipping preceded polarity correction. Undo polarity for the
		 * host-gain estimate, but retain it in fitted channel coordinates. */
		if (!pcm_gain_observe(&host, model->wire[i] * line->polarity,
				model->raw[i] * line->polarity)) {
			close_acquisition(model);
			return;
		}
		samples[i] = (int16_t)fmax(INT16_MIN, fmin(INT16_MAX,
			round(model->raw[i] * line->calibration.gain)));
	}
	if (!pcm_gain_estimate(&host, &gain)) {
		close_acquisition(model);
		return;
	}
	for (unsigned candidate = 0u; candidate < 2u; ++candidate) {
		struct v92_observation *fit = &model->model[candidate];
		int32_t x[V92_OBSERVATION_TRAINING];

		memcpy(x, model->x, sizeof(x));
		if (candidate)
			for (size_t i = 0u; i < V92_OBSERVATION_TRAINING; ++i)
				x[i] = round_shift_s64_to_s32_saturating(
					(int64_t)x[i] * model->coefficient, 15u);
		const struct v92_observation_training training = {
			.input = x, .linear = model->u, .sample = samples, .levels = model->wire,
			.count = V92_OBSERVATION_TRAINING, .host_gain = gain * line->calibration.gain,
			.law = decoder->mapping.law,
		};
		if (v92_observation_fit(fit, &training)) {
			fit->input_coefficient = candidate ? model->coefficient : 0;
			model->valid |= (uint8_t)(1u << candidate);
		} else {
			memset(fit, 0, sizeof(*fit));
		}
	}
	if (model->valid) {
		model->primary = (model->valid & 1u) ? 0u : 1u;
		if (model->valid == 3u)
			model->alternate = *decoder;
	}
	/* Later B1u decisions and payload validate, but never train, these fits. */
	close_acquisition(model);
}

enum v92_pcm_frame_result v92_upstream_model_decode(struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision,
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	bool b1u = line->received_frames < V92_B1U_FRAMES;
	size_t count = decoder->mapping.data_bits_per_frame;

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
		enum v92_pcm_frame_result result = v92_pcm_frame_decode(decoder,
			line->samples, b1u, bits, count, decision);

		if (result != V92_PCM_FRAME_OK)
			return result;
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			predictions[i] = decision->predicted_samples[i];
		if (validate) {
			uint8_t checked[V92_PCM_MAPPING_MAX_BITS];
			struct v92_pcm_frame_decision validation;
			bool failed = v92_pcm_frame_decode(&model->alternate, line->samples,
				false, checked, count, &validation) != V92_PCM_FRAME_OK;

			if (!failed) {
				failed = !validation.bits_ready || validation.ambiguous;
				if (validation.bits_ready)
					for (size_t i = 0u; i < count; ++i)
						failed |= checked[i] != 1u;
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
			.model = &model->model[i], .levels = line->wire, .received = line->received,
		};
		enum v92_pcm_frame_result result = v92_pcm_frame_decode_observed(branch[i],
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
	if ((model->valid & (1u << other)) && gap <= UINT64_C(3000000) &&
	    decisions[other].rm != decision->rm)
		decision->rm = V92_PCM_FRAME_RM_NONE;
	memcpy(predictions, likelihood[chosen].predicted_samples,
		sizeof(likelihood[chosen].predicted_samples));
	if (decision->bits_ready) {
		memcpy(bits, output[chosen], count);
		if ((model->valid & (1u << other)) && gap <= UINT64_C(3000000) &&
		    (!decisions[other].bits_ready || decisions[other].ambiguous ||
		     memcmp(output[chosen], output[other], count)))
			decision->ambiguous = true;
	}
	if (chosen != primary) {
		struct v92_pcm_frame_rx old = *decoder;

		*decoder = model->alternate;
		model->alternate = old;
		model->primary = chosen;
	}
	/* Relative evidence bounds growth over arbitrarily long calls. */
	uint64_t minimum = model->cost[chosen];

	for (unsigned i = 0u; i < 2u; ++i)
		model->cost[i] = (model->valid & (1u << i)) ? model->cost[i] - minimum : 0u;
	return V92_PCM_FRAME_OK;
}
