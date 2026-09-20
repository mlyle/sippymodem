// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_model.h"
#include "v92_upstream_data_rx.h"
#include "integer_math.h"

#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(V92_OBSERVATION_TRAINING ==
	V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	"model acquisition must end before held-out B1u decisions");

static void close_acquisition(struct v92_upstream_model *model)
{
	model->acquiring = false;
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

void v92_upstream_model_observe(struct v92_upstream_model *model,
	const struct v92_pcm_frame *frame, const struct v92_upstream_data_line *line)
{
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
 * decoder coordinates by only the independently subtracted echo. Clipping
 * loses that affine contract; a changed host gain must not become an echo
 * correction. Unqualified acquisition retains the linear receiver, while
 * loss of this contract after fitting takes the ordinary no-path recovery. */
static bool observation_offset(const struct v92_upstream_data_line *line,
	int32_t raw, int16_t wire, int16_t host, double *offset)
{
	double sample = raw * line->calibration.gain;
	int32_t physical = raw * line->polarity;
	/* Test containment using the acquired rounding interval, not the
	 * chosen point estimate. Do not narrow it using held-out data. */
	struct pcm_gain gain = { .lower = line->host.lower, .upper = line->host.upper };
	if (host == INT16_MIN || host == INT16_MAX ||
	    physical <= INT16_MIN || physical >= INT16_MAX ||
	    sample <= INT16_MIN || sample >= INT16_MAX ||
	    !pcm_gain_observe(&gain, wire * line->polarity, host))
		return false;
	/* Rounding the calibrated sample does not shift the codec interval.
	 * In particular, no echo subtraction must leave exactly zero offset. */
	*offset = (raw - (int32_t)host * line->polarity) * line->calibration.gain;
	return isfinite(*offset);
}

void v92_upstream_model_fit(struct v92_upstream_model *model,
	const struct v92_upstream_data_line *line, const struct v92_pcm_frame_rx *decoder)
{
	int16_t samples[V92_OBSERVATION_TRAINING];
	double offsets[V92_OBSERVATION_TRAINING];

	if (!model->acquiring || line->received_frames + 1u != V92_B1U_ACQUISITION_FRAMES)
		return;
	if (line->host.gain <= 0.0) {
		close_acquisition(model);
		return;
	}
	for (size_t i = 0u; i < V92_OBSERVATION_TRAINING; ++i) {
		if (!observation_offset(line, model->raw[i], model->wire[i], model->host[i], offsets + i)) {
			close_acquisition(model);
			return;
		}
		samples[i] = (int16_t)fmax(INT16_MIN, fmin(INT16_MAX,
			round(model->raw[i] * line->calibration.gain)));
	}
	const struct v92_cpd_filter *feedback = &decoder->mapping.cpd.filter[V92_CPD_PRECODER_FEEDBACK];
	const struct v92_observation_training training = {
		.input = model->x, .linear = model->u, .sample = samples, .levels = model->wire,
		.offsets = offsets, .count = V92_OBSERVATION_TRAINING,
		.taps = feedback->count >= 3 && feedback->coefficient[2] ? 4 : 3,
		.host_gain = line->host.gain * line->calibration.gain, .law = decoder->mapping.law,
	};
	for (unsigned candidate = 0u; candidate < 2u; ++candidate) {
		struct v92_observation *fit = &model->model[candidate];
		int32_t x[V92_OBSERVATION_TRAINING];

		memcpy(x, model->x, sizeof(x));
		if (candidate)
			for (size_t i = 0u; i < V92_OBSERVATION_TRAINING; ++i)
				x[i] = round_shift_s64_to_s32_saturating(
					(int64_t)x[i] * model->coefficient, 15u);
		struct v92_observation_training candidate_training = training;
		candidate_training.input = x;
		if (v92_observation_fit(fit, &candidate_training)) {
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
	} else
		v92_observation_fit_scalar(&model->scalar_gain, &training);
	/* Later B1u decisions and payload validate, but never train, these fits. */
	close_acquisition(model);
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
		double offset = line->raw[i] - (int32_t)host * line->polarity;
		double lower = line->polarity > 0 ? cell.lower : -(double)cell.upper;
		double upper = line->polarity > 0 ? cell.upper : -(double)cell.lower;

		cells[i] = (struct v92_pcm_frame_cell){
			.lower = (lower * gain + offset) * line->calibration.gain,
			.upper = (upper * gain + offset) * line->calibration.gain,
		};
	}
	return true;
}

enum v92_pcm_frame_result v92_upstream_model_decode(struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision,
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS])
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
			.offsets = offsets,
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
