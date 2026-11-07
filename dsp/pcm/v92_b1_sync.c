// SPDX-License-Identifier: BSD-3-Clause
#include "v92_b1_sync.h"

#include "memory_region.h"
#include "integer_math.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum { OFF, COLLECT, SEARCH, REPLAY };
enum { FIRST_FRAME = 2, SCORE_FRAMES = 4 };

static void clear(struct v92_b1_sync *sync)
{
	memset(sync, 0, sizeof(*sync));
}

bool v92_b1_sync_init(struct v92_b1_sync *sync,
	const struct v92_upstream_rx *handoff,
	const struct v92_upstream_data_rx *receiver)
{
	if (!sync || !v92_upstream_rx_is_valid(handoff) || handoff->failed ||
	    !v92_upstream_data_rx_is_valid(receiver) || receiver->failed ||
	    handoff->training_tail_remaining || receiver->line.received_frames ||
	    receiver->line.symbol_count || receiver->input.count ||
	    receiver->line.input_samples != handoff->line.input_samples ||
	    handoff->line.law != receiver->decoder.mapping.law ||
	    handoff->line.polarity != receiver->line.polarity ||
	    memory_regions_overlap(sync, sizeof(*sync), handoff, sizeof(*handoff)) ||
	    memory_regions_overlap(sync, sizeof(*sync), receiver, sizeof(*receiver)))
		return false;
	clear(sync);
	if (receiver->line.plan.observation == V92_PCM_FRAME_CONSTELLATION) {
		sync->handoff = *handoff;
		sync->seed = receiver->model.seed;
		sync->stage = COLLECT;
	}
	return true;
}

bool v92_b1_sync_is_valid(const struct v92_b1_sync *sync,
	const struct v92_upstream_data_rx *receiver)
{
	if (!sync || !receiver || sync->stage > REPLAY)
		return false;
	if (sync->stage == OFF)
		return memory_region_is_zero(sync, sizeof(*sync));
	if (!v92_upstream_rx_is_valid(&sync->handoff) || sync->handoff.failed ||
	    sync->handoff.training_tail_remaining || !v92_upstream_seed_is_valid(&sync->seed) ||
	    receiver->line.plan.observation != V92_PCM_FRAME_CONSTELLATION ||
	    sync->handoff.line.law != receiver->decoder.mapping.law ||
	    sync->handoff.line.polarity != receiver->line.polarity ||
	    sync->head >= V92_UPSTREAM_INPUT_QUEUE || sync->count > V92_UPSTREAM_INPUT_QUEUE ||
	    sync->candidate > V92_B1_SYNC_SHIFT / V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    sync->shift > V92_B1_SYNC_SHIFT || sync->shift % V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    !isfinite(sync->first_score) || sync->first_score < 0. ||
	    !isfinite(sync->best_score) || sync->best_score < 0.)
		return false;
	if (sync->stage != REPLAY && sync->head)
		return false;
	if (sync->stage == COLLECT && (sync->candidate || sync->shift ||
	    sync->first_score || sync->best_score ||
	    !memory_region_is_zero(sync->digits, sizeof(sync->digits))))
		return false;
	if (sync->stage != COLLECT) {
		if (!sync->count || receiver->failed ||
		    (sync->stage == REPLAY && !sync->shift))
			return false;
		for (unsigned f = 0; f < V92_B1_SYNC_WORDS; ++f)
			for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
				if (sync->digits[f][i] >= receiver->decoder.mapping.cpd.modulus[i])
					return false;
	}
	for (unsigned i = 0; i < V92_UPSTREAM_INPUT_QUEUE; ++i) {
		unsigned relative = (i + V92_UPSTREAM_INPUT_QUEUE - sync->head) % V92_UPSTREAM_INPUT_QUEUE;
		if (relative < sync->count ? sync->input[i].flags != 3 :
		    !memory_region_is_zero(&sync->input[i], sizeof(sync->input[i])))
			return false;
	}
	return true;
}

static bool known_words(struct v92_b1_sync *sync, const struct v92_pcm_mapping *mapping)
{
	struct v34_scrambler scrambler;
	uint8_t differential = 0, sign;
	if (v34_scrambler_init(&scrambler, V34_POLYNOMIAL_GPA) != V34_FRAMING_OK)
		return false;
	for (unsigned f = 0; f < V92_B1_SYNC_WORDS; ++f) {
		uint8_t ones[V92_PCM_MAPPING_MAX_BITS], scrambled[V92_PCM_MAPPING_MAX_BITS];
		memset(ones, 1, mapping->data_bits_per_frame);
		if (v34_scramble_unpacked_bits(&scrambler, ones, mapping->data_bits_per_frame,
			scrambled) != V34_FRAMING_OK ||
		    v92_pcm_mapping_encode_owned(mapping, scrambled, mapping->data_bits_per_frame,
			&differential, sync->digits[f], &sign) != V92_PCM_MAPPING_OK)
			return false;
	}
	return true;
}

/* GPA/mixed-radix words constrain a set of possible u(n), not one transmitter
 * waveform. Exclude fourth symbols, whose cosets additionally depend on Y0.
 * Keep the receiver's existing one-frame control-prefix convention. This is
 * a cheap candidate selector; it cannot certify B1u or fit the channel. */
static double score(const struct v92_b1_sync *sync,
	const struct v92_upstream_data_rx *receiver, unsigned shift)
{
	const struct v92_pcm_mapping *mapping = &receiver->decoder.mapping;
	unsigned delay = receiver->line.plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	double total = 0.;
	for (unsigned f = FIRST_FRAME; f < FIRST_FRAME + SCORE_FRAMES; ++f)
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			if (i % 4 == 3)
				continue;
			const struct v92_cpd_constellation *c = &mapping->cpd.constellation[
				mapping->cpd.frame_constellation[i % V92_PCM_MAPPING_PHASES]];
			double input = sync->input[delay + shift + f * V92_PCM_MAPPING_FRAME_SYMBOLS + i].sample *
				(double)receiver->line.polarity * receiver->line.plan.gain;
			double best = DBL_MAX;
			unsigned modulus = mapping->cpd.modulus[i];
			int first = -(int)c->count;
			first += nonnegative_remainder_s32(sync->digits[f - 1][i] - first, modulus);
			/* Visit the same signed class as the frame coder, in order.
			 * Other classes cannot satisfy this known mixed-radix digit. */
			for (int eta = first; eta < c->count; eta += (int)modulus) {
				double error = input - v92_cpd_signed_point(c, eta);
				best = fmin(best, error * error);
			}
			total += best;
		}
	return total;
}

static enum v92_upstream_data_rx_result fail(struct v92_b1_sync *sync,
	struct v92_upstream_data_rx *receiver)
{
	clear(sync);
	receiver->failed = true;
	return V92_UPSTREAM_DATA_RX_NO_PATH;
}

static bool restart(struct v92_b1_sync *sync, struct v92_upstream_data_rx *receiver)
{
	struct v92_pcm_mapping mapping = receiver->decoder.mapping;
	struct v92_upstream_plan plan = receiver->line.plan;
	struct v92_upstream_rx handoff = sync->handoff;
	if (handoff.line.input_samples > UINT64_MAX - sync->shift)
		return false;
	handoff.line.input_samples += sync->shift;
	if (v92_upstream_data_rx_init(receiver, &handoff, &mapping, &plan) != V92_UPSTREAM_DATA_RX_OK)
		return false;
	(void)v92_upstream_model_seed(receiver, &sync->seed);
	memset(sync->input, 0, sync->shift * sizeof(*sync->input));
	sync->head = sync->shift;
	sync->count -= sync->shift;
	sync->stage = REPLAY;
	return true;
}

enum v92_upstream_data_rx_result v92_b1_sync_octet_owned(
	struct v92_b1_sync *sync, struct v92_upstream_data_rx *receiver,
	uint8_t code, int16_t sample, int16_t host, bool available,
	struct v92_upstream_data_frame *frame)
{
	if (!sync || !receiver || !frame ||
	    memory_regions_overlap(sync, sizeof(*sync), receiver, sizeof(*receiver)) ||
	    memory_regions_overlap(sync, sizeof(*sync), frame, sizeof(*frame)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), frame, sizeof(*frame)))
		return V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (receiver->failed)
		return V92_UPSTREAM_DATA_RX_FAILED;
	if (receiver->line.input_samples == UINT64_MAX ||
	    receiver->input.count >= UINT64_MAX - receiver->line.input_samples)
		return V92_UPSTREAM_DATA_RX_INVALID_STATE;
	if (sync->stage != OFF) {
		if (!available || sync->count == V92_UPSTREAM_INPUT_QUEUE) {
			if (sync->stage != COLLECT)
				return fail(sync, receiver);
			clear(sync);
		} else {
			unsigned tail = (sync->head + sync->count) % V92_UPSTREAM_INPUT_QUEUE;
			sync->input[tail] = (struct v92_upstream_input){sample, host, code, 3};
			++sync->count;
		}
	}
	if (sync->stage == SEARCH) {
		unsigned required = receiver->line.plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY +
			V92_B1_SYNC_SHIFT + (FIRST_FRAME + SCORE_FRAMES) * V92_PCM_MAPPING_FRAME_SYMBOLS;
		if (sync->count < required)
			return V92_UPSTREAM_DATA_RX_OK;
		unsigned shift = sync->candidate * V92_PCM_MAPPING_FRAME_SYMBOLS;
		double value = score(sync, receiver, shift);
		if (!shift) sync->first_score = value;
		if (value < sync->best_score) {
			sync->best_score = value;
			sync->shift = shift;
		}
		if (shift < V92_B1_SYNC_SHIFT) {
			++sync->candidate;
			return V92_UPSTREAM_DATA_RX_OK;
		}
		/* A factor-two RMS improvement chooses one retry, not readiness. */
		if (!sync->shift || sync->best_score * 4 >= sync->first_score || !restart(sync, receiver))
			return fail(sync, receiver);
		return V92_UPSTREAM_DATA_RX_OK;
	}
	if (sync->stage == REPLAY) {
		enum v92_upstream_data_rx_result result = V92_UPSTREAM_DATA_RX_OK;
		for (unsigned work = 0; work < 2 && sync->count; ++work) {
			struct v92_upstream_input *input = &sync->input[sync->head];
			result = v92_upstream_data_rx_octet_owned(receiver, input->code,
				input->sample, input->host, true, frame);
			if (result != V92_UPSTREAM_DATA_RX_OK && result != V92_UPSTREAM_DATA_RX_FRAME) {
				clear(sync);
				return result;
			}
			memset(input, 0, sizeof(*input));
			sync->head = (sync->head + 1) % V92_UPSTREAM_INPUT_QUEUE;
			--sync->count;
			if (result == V92_UPSTREAM_DATA_RX_FRAME)
				break;
		}
		if (!sync->count) clear(sync);
		return result;
	}
	enum v92_upstream_data_rx_result result = v92_upstream_data_rx_octet_owned(
		receiver, code, sample, host, available, frame);
	if (sync->stage == COLLECT && result == V92_UPSTREAM_DATA_RX_NO_PATH &&
	    receiver->line.received_frames < V92_B1U_ACQUISITION_FRAMES &&
	    !receiver->input.count && known_words(sync, &receiver->decoder.mapping)) {
		receiver->failed = false;
		sync->best_score = DBL_MAX;
		sync->stage = SEARCH;
		return V92_UPSTREAM_DATA_RX_OK;
	}
	if (sync->stage == COLLECT && (receiver->line.received_frames >= V92_B1U_ACQUISITION_FRAMES ||
	    (result != V92_UPSTREAM_DATA_RX_OK && result != V92_UPSTREAM_DATA_RX_FRAME)))
		clear(sync);
	return result;
}
