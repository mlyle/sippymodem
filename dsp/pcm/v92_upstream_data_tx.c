// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_data_tx.h"

#include "memory_region.h"
#include "packed_bits.h"

#include <string.h>

enum {
	RM_SYMBOLS = 384, RM_PRIME_SYMBOLS = 24,
	FINISH_SYMBOLS = V92_PCM_MAPPING_FRAME_SYMBOLS + V92_B1U_SYMBOLS,
};

bool v92_upstream_data_tx_is_valid(
	const struct v92_upstream_data_tx *transmitter)
{
	if (transmitter == NULL ||
	    !v92_pcm_frame_tx_is_valid(&transmitter->encoder) ||
	    transmitter->upstream_level <= 0 ||
	    transmitter->b1u_symbols_remaining > V92_B1U_SYMBOLS ||
	    transmitter->finish_symbols_remaining > FINISH_SYMBOLS ||
	    (transmitter->finish_symbols_remaining != 0u &&
	     (transmitter->b1u_symbols_remaining != 0u || transmitter->rm_symbols_remaining != 0u ||
	      transmitter->finish_symbols_remaining % V92_PCM_MAPPING_FRAME_SYMBOLS != transmitter->queue_count)) ||
	    transmitter->rm_symbols_remaining > RM_SYMBOLS + RM_PRIME_SYMBOLS +
		    V92_PCM_MAPPING_FRAME_SYMBOLS - 1u ||
	    (transmitter->rm_symbols_remaining != 0u &&
	     (transmitter->b1u_symbols_remaining != 0u ||
	      transmitter->rm_symbols_remaining % V92_PCM_MAPPING_FRAME_SYMBOLS !=
		      transmitter->queue_count)) ||
	    transmitter->queue_count >= V92_PCM_MAPPING_FRAME_SYMBOLS ||
	    (transmitter->b1u_symbols_remaining != 0u &&
	     transmitter->b1u_symbols_remaining % V92_PCM_MAPPING_FRAME_SYMBOLS !=
		     transmitter->queue_count))
		return false;
	for (size_t index = 0u;
	     index < (size_t)V92_PCM_MAPPING_FRAME_SYMBOLS -
		     transmitter->queue_count; ++index) {
		if (transmitter->queue[index] != 0)
			return false;
	}
	return true;
}

enum v92_upstream_data_tx_result v92_upstream_data_tx_init(
	struct v92_upstream_data_tx *transmitter,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level)
{
	struct v92_upstream_data_tx initialized = {
		.upstream_level = upstream_level,
		.b1u_symbols_remaining = V92_B1U_SYMBOLS,
	};

	if (transmitter == NULL || mapping == NULL)
		return V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (upstream_level <= 0)
		return V92_UPSTREAM_DATA_TX_OUT_OF_RANGE;
	if (v92_pcm_frame_tx_init(&initialized.encoder, mapping) !=
	    V92_PCM_FRAME_OK)
		return V92_UPSTREAM_DATA_TX_INVALID_PARAMETERS;
	*transmitter = initialized;
	return V92_UPSTREAM_DATA_TX_OK;
}

bool v92_upstream_data_tx_is_online(
	const struct v92_upstream_data_tx *transmitter)
{
	return v92_upstream_data_tx_is_valid(transmitter) &&
	       transmitter->b1u_symbols_remaining == 0u &&
	       transmitter->rm_symbols_remaining == 0u &&
	       transmitter->finish_symbols_remaining == 0u;
}

enum v92_upstream_data_tx_result v92_upstream_data_tx_start_rm(
	struct v92_upstream_data_tx *transmitter)
{
	if (transmitter == NULL)
		return V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v92_upstream_data_tx_is_online(transmitter))
		return V92_UPSTREAM_DATA_TX_INVALID_STATE;
	transmitter->rm_symbols_remaining = RM_SYMBOLS + RM_PRIME_SYMBOLS +
		transmitter->queue_count;
	return V92_UPSTREAM_DATA_TX_OK;
}

enum v92_upstream_data_tx_result v92_upstream_data_tx_reset_control(
	struct v92_upstream_data_tx *transmitter)
{
	if (transmitter == NULL)
		return V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v92_upstream_data_tx_is_online(transmitter) ||
	    transmitter->queue_count != 0u)
		return V92_UPSTREAM_DATA_TX_INVALID_STATE;
	if (v92_pcm_frame_tx_reset_control(&transmitter->encoder) != V92_PCM_FRAME_OK)
		return V92_UPSTREAM_DATA_TX_INVALID_STATE;
	return V92_UPSTREAM_DATA_TX_OK;
}

enum v92_upstream_data_tx_result v92_upstream_data_tx_start_e2u(
	struct v92_upstream_data_tx *transmitter)
{
	if (transmitter == NULL)
		return V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!v92_upstream_data_tx_is_online(transmitter) || transmitter->queue_count != 0u)
		return V92_UPSTREAM_DATA_TX_INVALID_STATE;
	transmitter->finish_symbols_remaining = FINISH_SYMBOLS;
	return V92_UPSTREAM_DATA_TX_OK;
}

static enum v92_upstream_data_tx_result generate_frame(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context)
{
	struct v92_pcm_frame_tx encoder = transmitter->encoder;
	struct v92_pcm_frame frame;
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	size_t count = encoder.mapping.data_bits_per_frame;

	/* B1u is exactly 48 complete known-one frames. Fetching the first data
	 * frame does not reset or reseed any of the active coding histories. */
	if (transmitter->rm_symbols_remaining != 0u) {
		if (v92_pcm_frame_encode_rm(&encoder,
			transmitter->rm_symbols_remaining <= RM_PRIME_SYMBOLS, &frame) != V92_PCM_FRAME_OK)
			return V92_UPSTREAM_DATA_TX_NUMERIC_FAILURE;
	} else {
		if (transmitter->finish_symbols_remaining != 0u) {
			memset(bits, transmitter->finish_symbols_remaining <= V92_B1U_SYMBOLS, count);
		} else if (transmitter->b1u_symbols_remaining != 0u || source == NULL) {
			memset(bits, 1, count);
		} else {
			memset(bits, 2, count);
			if (!source(context, bits, count) ||
			    !unpacked_bits_are_binary(bits, count))
				return V92_UPSTREAM_DATA_TX_SOURCE_FAILURE;
		}
		if (v92_pcm_frame_encode_owned(&encoder, bits, count, &frame) != V92_PCM_FRAME_OK)
			return V92_UPSTREAM_DATA_TX_NUMERIC_FAILURE;
	}
	for (size_t index = 0u; index < V92_PCM_MAPPING_FRAME_SYMBOLS; ++index) {
		if (v92_pcm_frame_scale(
			    encoder.mapping.cpd.four_times_gain_q0_16, transmitter->upstream_level,
			    frame.v[index], &samples[index]) != V92_PCM_FRAME_OK)
			return V92_UPSTREAM_DATA_TX_NUMERIC_FAILURE;
	}
	transmitter->encoder.state = encoder.state;
	memcpy(transmitter->queue, samples, sizeof(samples));
	transmitter->queue_count = V92_PCM_MAPPING_FRAME_SYMBOLS;
	return V92_UPSTREAM_DATA_TX_OK;
}

static enum v92_upstream_data_tx_result next_sample(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context, int16_t *sample,
	bool owned)
{
	if (transmitter == NULL || sample == NULL ||
	    (!owned && !v92_upstream_data_tx_is_valid(transmitter)) ||
	    memory_regions_overlap(
		    transmitter, sizeof(*transmitter), sample, sizeof(*sample)))
		return V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (transmitter->queue_count == 0u) {
		enum v92_upstream_data_tx_result result =
			generate_frame(transmitter, source, context);

		if (result != V92_UPSTREAM_DATA_TX_OK)
			return result;
	}
	bool rm_complete = transmitter->rm_symbols_remaining == 1u;
	bool fb1u_complete = transmitter->finish_symbols_remaining == 1u;

	if (rm_complete && v92_pcm_frame_tx_reset_control(&transmitter->encoder) != V92_PCM_FRAME_OK)
		return V92_UPSTREAM_DATA_TX_NUMERIC_FAILURE;
	size_t index = V92_PCM_MAPPING_FRAME_SYMBOLS - transmitter->queue_count;
	*sample = transmitter->queue[index];
	transmitter->queue[index] = 0;
	transmitter->queue_count--;
	if (transmitter->b1u_symbols_remaining != 0u)
		transmitter->b1u_symbols_remaining--;
	if (transmitter->rm_symbols_remaining != 0u)
		transmitter->rm_symbols_remaining--;
	if (transmitter->finish_symbols_remaining != 0u)
		transmitter->finish_symbols_remaining--;
	return rm_complete ? V92_UPSTREAM_DATA_TX_RM_COMPLETE :
		fb1u_complete ? V92_UPSTREAM_DATA_TX_FB1U_COMPLETE : V92_UPSTREAM_DATA_TX_OK;
}

enum v92_upstream_data_tx_result v92_upstream_data_tx_next(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context, int16_t *sample)
{
	return next_sample(transmitter, source, context, sample, false);
}

enum v92_upstream_data_tx_result v92_upstream_data_tx_next_owned(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context, int16_t *sample)
{
	return next_sample(transmitter, source, context, sample, true);
}
