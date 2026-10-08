// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_cpt_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool stage_is_valid(enum v92_pcm_phase3_cpt_rx_stage stage)
{
	return stage >= V92_PCM_PHASE3_CPT_RX_TRAINING &&
	       stage <= V92_PCM_PHASE3_CPT_RX_COMPLETE;
}

static bool coding_is_valid(const struct pcm_binary_sign *coding)
{
	return pcm_binary_sign_is_valid(coding) &&
	       coding->scrambler.polynomial == V34_POLYNOMIAL_GPA;
}

static bool boundary_is_valid(
	const struct v92_pcm_phase3_cpt_rx *receiver)
{
	uint8_t ones = 0u;

	if (receiver->boundary_count > V92_CPT_SYMBOLS_PER_FRAME ||
	    receiver->boundary_ones > receiver->boundary_count)
		return false;
	for (size_t index = 0; index < V92_CPT_SYMBOLS_PER_FRAME; ++index) {
		if (receiver->boundary_bits[index] > 1u ||
		    (index >= receiver->boundary_count &&
		     receiver->boundary_bits[index] != 0u))
			return false;
		if (index < receiver->boundary_count)
			ones += receiver->boundary_bits[index];
	}
	return receiver->boundary_ones == ones;
}

bool v92_pcm_phase3_cpt_rx_is_valid(
	const struct v92_pcm_phase3_cpt_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !v92_phase3_sync_is_valid(&receiver->sync) ||
	    !coding_is_valid(&receiver->coding) ||
	    !v92_cpt_receiver_is_valid(&receiver->records) ||
	    !boundary_is_valid(receiver) ||
	    (receiver->matching_records != 0u &&
	     !v92_cpt_parameters_are_valid(&receiver->parameters)))
		return false;
	switch (receiver->stage) {
	case V92_PCM_PHASE3_CPT_RX_TRAINING:
		return receiver->sync.stage == V92_PHASE3_SYNC_TRAINING &&
		       receiver->records.wire.stage == PCM_CONTROL_SEARCH_SYNC &&
		       receiver->matching_records == 0u &&
		       receiver->boundary_count == 0u;
	case V92_PCM_PHASE3_CPT_RX_SEARCHING:
		return receiver->sync.stage == V92_PHASE3_SYNC_SEARCHING &&
		       receiver->records.wire.stage == PCM_CONTROL_SEARCH_SYNC &&
		       receiver->matching_records == 0u &&
		       receiver->boundary_count == 0u;
	case V92_PCM_PHASE3_CPT_RX_RECORDS:
		return receiver->sync.stage == V92_PHASE3_SYNC_COMPLETE &&
		       receiver->boundary_count == 0u;
	case V92_PCM_PHASE3_CPT_RX_E1U_CANDIDATE:
		return receiver->sync.stage == V92_PHASE3_SYNC_COMPLETE &&
		       receiver->records.wire.stage == PCM_CONTROL_SEARCH_SYNC &&
		       receiver->matching_records != 0u &&
		       receiver->boundary_count < V92_CPT_SYMBOLS_PER_FRAME &&
		       receiver->boundary_ones <= 1u;
	case V92_PCM_PHASE3_CPT_RX_COMPLETE:
		return receiver->sync.stage == V92_PHASE3_SYNC_COMPLETE &&
		       receiver->records.wire.stage == PCM_CONTROL_SEARCH_SYNC &&
		       receiver->matching_records != 0u &&
		       receiver->boundary_count == V92_CPT_SYMBOLS_PER_FRAME &&
		       receiver->boundary_ones <= 1u;
	}
	return false;
}

enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_init(
	struct v92_pcm_phase3_cpt_rx *receiver)
{
	struct v92_pcm_phase3_cpt_rx initialized = {
		.stage = V92_PCM_PHASE3_CPT_RX_TRAINING,
	};

	if (receiver == NULL ||
	    v92_phase3_sync_init(&initialized.sync) != V92_PHASE3_SYNC_OK ||
	    pcm_binary_sign_init(
		    &initialized.coding, V34_POLYNOMIAL_GPA) !=
		    PCM_BINARY_SIGN_OK ||
	    v92_cpt_receiver_init(&initialized.records) != V92_CPT_OK ||
	    !v92_pcm_phase3_cpt_rx_is_valid(&initialized))
		return V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT;
	*receiver = initialized;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

enum v92_pcm_phase3_cpt_rx_result
v92_pcm_phase3_cpt_rx_init_from_training(
	struct v92_pcm_phase3_cpt_rx *receiver,
	const struct v92_phase3_sync *training)
{
	struct v92_pcm_phase3_cpt_rx initialized;

	if (receiver == NULL || !v92_phase3_sync_is_valid(training) ||
	    training->stage != V92_PHASE3_SYNC_TRAINING ||
	    training->trained_symbols < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS)
		return V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT;
	if (v92_pcm_phase3_cpt_rx_init(&initialized) !=
	    V92_PCM_PHASE3_CPT_RX_OK)
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	initialized.sync = *training;
	if (v92_pcm_phase3_cpt_rx_begin(&initialized) !=
			V92_PCM_PHASE3_CPT_RX_OK ||
	    !v92_pcm_phase3_cpt_rx_is_valid(&initialized))
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

bool v92_pcm_phase3_cpt_rx_is_complete(
	const struct v92_pcm_phase3_cpt_rx *receiver)
{
	return v92_pcm_phase3_cpt_rx_is_valid(receiver) &&
	       receiver->stage == V92_PCM_PHASE3_CPT_RX_COMPLETE;
}

enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_train(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t sign)
{
	struct v92_pcm_phase3_cpt_rx next;

	if (receiver == NULL || sign > 1u)
		return V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_cpt_rx_is_valid(receiver) ||
	    receiver->stage != V92_PCM_PHASE3_CPT_RX_TRAINING)
		return V92_PCM_PHASE3_CPT_RX_INVALID_STATE;
	next = *receiver;
	if (v92_phase3_sync_train(&next.sync, sign) != V92_PHASE3_SYNC_OK ||
	    !v92_pcm_phase3_cpt_rx_is_valid(&next))
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	*receiver = next;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_begin(
	struct v92_pcm_phase3_cpt_rx *receiver)
{
	struct v92_pcm_phase3_cpt_rx next;

	if (receiver == NULL)
		return V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_cpt_rx_is_valid(receiver) ||
	    receiver->stage != V92_PCM_PHASE3_CPT_RX_TRAINING)
		return V92_PCM_PHASE3_CPT_RX_INVALID_STATE;
	next = *receiver;
	if (v92_phase3_sync_begin(&next.sync) != V92_PHASE3_SYNC_OK)
		return V92_PCM_PHASE3_CPT_RX_INVALID_STATE;
	next.stage = V92_PCM_PHASE3_CPT_RX_SEARCHING;
	if (!v92_pcm_phase3_cpt_rx_is_valid(&next))
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	*receiver = next;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

static enum v92_pcm_phase3_cpt_rx_result receive_search_sign(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t sign)
{
	enum v92_phase3_sync_result result =
		v92_phase3_sync_receive(&receiver->sync, sign);

	if (result == V92_PHASE3_SYNC_OK)
		return V92_PCM_PHASE3_CPT_RX_OK;
	if (result != V92_PHASE3_SYNC_SYNCHRONIZED ||
	    !v92_phase3_sync_handoff(&receiver->sync, &receiver->coding) ||
	    v92_cpt_receiver_begin_after_sync(&receiver->records) != V92_CPT_OK)
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	receiver->stage = V92_PCM_PHASE3_CPT_RX_RECORDS;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

static void begin_boundary(struct v92_pcm_phase3_cpt_rx *receiver)
{
	memset(receiver->boundary_bits, 0, sizeof(receiver->boundary_bits));
	receiver->boundary_count = 0u;
	receiver->boundary_ones = 0u;
	receiver->stage = V92_PCM_PHASE3_CPT_RX_E1U_CANDIDATE;
}

static bool parameters_match(
	const struct v92_cpt_parameters *first,
	const struct v92_cpt_parameters *second)
{
	/* Both records came through the codec's CRC and canonical-constellation
	 * checks. Compare fields, not struct padding or regenerated wire CRCs. */
	if (first->law != second->law ||
	    first->trn1d_ratio_q3_13 != second->trn1d_ratio_q3_13 ||
	    first->shaping_a1_q1_6 != second->shaping_a1_q1_6 ||
	    first->shaping_a2_q1_6 != second->shaping_a2_q1_6 ||
	    first->shaping_b1_q1_6 != second->shaping_b1_q1_6 ||
	    first->shaping_b2_q1_6 != second->shaping_b2_q1_6 ||
	    first->selected_rate_index != second->selected_rate_index ||
	    first->sign_redundancy != second->sign_redundancy ||
	    first->spectral_lookahead != second->spectral_lookahead ||
	    first->constellation_count != second->constellation_count ||
	    first->acknowledges_peer != second->acknowledges_peer ||
	    first->codec_constellations_differ != second->codec_constellations_differ ||
	    memcmp(first->frame_constellation, second->frame_constellation,
		   sizeof(first->frame_constellation)) != 0)
		return false;
	for (size_t index = 0; index < V92_CPT_MAX_CONSTELLATIONS; ++index) {
		if (memcmp(first->transmit_constellations[index].ucode_mask,
			   second->transmit_constellations[index].ucode_mask,
			   PCM_CONSTELLATION_MASK_BYTES) != 0 ||
		    memcmp(first->codec_constellations[index].ucode_mask,
			   second->codec_constellations[index].ucode_mask,
			   PCM_CONSTELLATION_MASK_BYTES) != 0)
			return false;
	}
	return true;
}

static enum v92_pcm_phase3_cpt_rx_result accept_frame(
	struct v92_pcm_phase3_cpt_rx *receiver,
	const struct v92_cpt_receive_event *record,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	event->observed_bits = record->observed_bits;
	event->parameters = record->parameters;
	if (receiver->matching_records == 0u) {
		receiver->parameters = record->parameters;
		receiver->matching_records = 1u;
		event->type = V92_PCM_PHASE3_CPT_RX_PARAMETERS;
	} else {
		if (parameters_match(&receiver->parameters, &record->parameters)) {
			if (receiver->matching_records != UINT32_MAX)
				receiver->matching_records++;
			event->type = V92_PCM_PHASE3_CPT_RX_REPEAT;
		} else {
			event->type = V92_PCM_PHASE3_CPT_RX_REJECTED;
			event->frame_result = V92_CPT_INVALID_FRAME;
		}
	}
	begin_boundary(receiver);
	return V92_PCM_PHASE3_CPT_RX_OK;
}

static enum v92_pcm_phase3_cpt_rx_result receive_record_bit(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t bit,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	struct v92_cpt_receive_event record;

	if (v92_cpt_receiver_receive_bit(
		    &receiver->records, bit, &record) != V92_CPT_OK)
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	switch (record.type) {
	case V92_CPT_RECEIVE_NO_EVENT:
		return V92_PCM_PHASE3_CPT_RX_OK;
	case V92_CPT_RECEIVE_FRAME:
		return accept_frame(receiver, &record, event);
	case V92_CPT_RECEIVE_REJECTED:
		event->type = V92_PCM_PHASE3_CPT_RX_REJECTED;
		event->frame_result = record.frame_result;
		event->observed_bits = record.observed_bits;
		return V92_PCM_PHASE3_CPT_RX_OK;
	}
	return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
}

static enum v92_pcm_phase3_cpt_rx_result replay_boundary(
	struct v92_pcm_phase3_cpt_rx *receiver)
{
	for (size_t index = 0; index < receiver->boundary_count; ++index) {
		struct v92_pcm_phase3_cpt_rx_event ignored = { 0 };

		if (receive_record_bit(
			    receiver, receiver->boundary_bits[index], &ignored) !=
			    V92_PCM_PHASE3_CPT_RX_OK ||
		    ignored.type != V92_PCM_PHASE3_CPT_RX_NO_EVENT)
			return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	}
	memset(receiver->boundary_bits, 0, sizeof(receiver->boundary_bits));
	receiver->boundary_count = 0u;
	receiver->boundary_ones = 0u;
	receiver->stage = V92_PCM_PHASE3_CPT_RX_RECORDS;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

static enum v92_pcm_phase3_cpt_rx_result receive_boundary_bit(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t bit,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	receiver->boundary_bits[receiver->boundary_count++] = bit;
	receiver->boundary_ones += bit;
	if (receiver->boundary_ones > 1u)
		return replay_boundary(receiver);
	if (receiver->boundary_count < V92_CPT_SYMBOLS_PER_FRAME)
		return V92_PCM_PHASE3_CPT_RX_OK;
	receiver->stage = V92_PCM_PHASE3_CPT_RX_COMPLETE;
	event->type = V92_PCM_PHASE3_CPT_RX_E1U;
	event->observed_bits = V92_CPT_SYMBOLS_PER_FRAME;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

static enum v92_pcm_phase3_cpt_rx_result receive_data_sign(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t sign,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	uint8_t bit;

	if (pcm_binary_sign_decode(&receiver->coding, sign, &bit) !=
	    PCM_BINARY_SIGN_OK)
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	if (receiver->stage == V92_PCM_PHASE3_CPT_RX_E1U_CANDIDATE)
		return receive_boundary_bit(receiver, bit, event);
	return receive_record_bit(receiver, bit, event);
}

enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_receive(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t sign,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	struct v92_pcm_phase3_cpt_rx next;
	struct v92_pcm_phase3_cpt_rx_event emitted = { 0 };
	enum v92_pcm_phase3_cpt_rx_result result;

	if (receiver == NULL || sign > 1u || event == NULL)
		return V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT;
	if (!v92_pcm_phase3_cpt_rx_is_valid(receiver) ||
	    receiver->stage == V92_PCM_PHASE3_CPT_RX_TRAINING)
		return V92_PCM_PHASE3_CPT_RX_INVALID_STATE;
	if (receiver->stage == V92_PCM_PHASE3_CPT_RX_COMPLETE)
		return V92_PCM_PHASE3_CPT_RX_DONE;
	next = *receiver;
	result = next.stage == V92_PCM_PHASE3_CPT_RX_SEARCHING ?
			 receive_search_sign(&next, sign) :
			 receive_data_sign(&next, sign, &emitted);
	if (result != V92_PCM_PHASE3_CPT_RX_OK ||
	    !v92_pcm_phase3_cpt_rx_is_valid(&next))
		return result != V92_PCM_PHASE3_CPT_RX_OK ?
			       result : V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	*receiver = next;
	*event = emitted;
	return V92_PCM_PHASE3_CPT_RX_OK;
}

bool v92_pcm_phase3_cpt_rx_parameters(
	const struct v92_pcm_phase3_cpt_rx *receiver,
	struct v92_cpt_parameters *parameters)
{
	if (!v92_pcm_phase3_cpt_rx_is_valid(receiver) ||
	    receiver->matching_records == 0u || parameters == NULL)
		return false;
	*parameters = receiver->parameters;
	return true;
}

bool v92_pcm_phase3_cpt_rx_previous_sign(
	const struct v92_pcm_phase3_cpt_rx *receiver,
	uint8_t *previous_sign)
{
	if (!v92_pcm_phase3_cpt_rx_is_complete(receiver) ||
	    previous_sign == NULL)
		return false;
	*previous_sign = receiver->coding.previous_sign;
	return true;
}
