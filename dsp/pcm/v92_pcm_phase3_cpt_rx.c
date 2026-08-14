// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_phase3_cpt_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

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

static bool parameters_are_valid(
	const struct v92_pcm_phase3_cpt_rx *receiver)
{
	uint8_t canonical[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t canonical_bits = 0u;

	if (receiver->matching_records == 0u)
		return receiver->parameter_frame_bits == 0u &&
		       bytes_are_zero(
			       receiver->parameter_frame,
			       sizeof(receiver->parameter_frame));
	return v92_cpt_encode(
		       &receiver->parameters, canonical, sizeof(canonical),
		       &canonical_bits) == V92_CPT_OK &&
	       canonical_bits == receiver->parameter_frame_bits &&
	       memcmp(canonical, receiver->parameter_frame,
		      sizeof(canonical)) == 0;
}

bool v92_pcm_phase3_cpt_rx_is_valid(
	const struct v92_pcm_phase3_cpt_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !v92_phase3_sync_is_valid(&receiver->sync) ||
	    !coding_is_valid(&receiver->coding) ||
	    !v92_cpt_receiver_is_valid(&receiver->records) ||
	    !boundary_is_valid(receiver) || !parameters_are_valid(receiver))
		return false;
	switch (receiver->stage) {
	case V92_PCM_PHASE3_CPT_RX_TRAINING:
		return receiver->sync.stage == V92_PHASE3_SYNC_TRAINING &&
		       receiver->records.stage == V92_CPT_RECEIVER_SEARCH_SYNC &&
		       receiver->matching_records == 0u &&
		       receiver->boundary_count == 0u;
	case V92_PCM_PHASE3_CPT_RX_SEARCHING:
		return receiver->sync.stage == V92_PHASE3_SYNC_SEARCHING &&
		       receiver->records.stage == V92_CPT_RECEIVER_SEARCH_SYNC &&
		       receiver->matching_records == 0u &&
		       receiver->boundary_count == 0u;
	case V92_PCM_PHASE3_CPT_RX_RECORDS:
		return receiver->sync.stage == V92_PHASE3_SYNC_COMPLETE &&
		       receiver->boundary_count == 0u;
	case V92_PCM_PHASE3_CPT_RX_E1U_CANDIDATE:
		return receiver->sync.stage == V92_PHASE3_SYNC_COMPLETE &&
		       receiver->records.stage == V92_CPT_RECEIVER_SEARCH_SYNC &&
		       receiver->matching_records != 0u &&
		       receiver->boundary_count < V92_CPT_SYMBOLS_PER_FRAME &&
		       receiver->boundary_ones <= 1u;
	case V92_PCM_PHASE3_CPT_RX_COMPLETE:
		return receiver->sync.stage == V92_PHASE3_SYNC_COMPLETE &&
		       receiver->records.stage == V92_CPT_RECEIVER_SEARCH_SYNC &&
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

static bool canonicalize(
	const struct v92_cpt_parameters *parameters, uint8_t *frame,
	size_t *frame_bits)
{
	memset(frame, 0, V92_CPT_MAX_FRAME_BYTES);
	return v92_cpt_encode(
		       parameters, frame, V92_CPT_MAX_FRAME_BYTES,
		       frame_bits) == V92_CPT_OK;
}

static enum v92_pcm_phase3_cpt_rx_result accept_frame(
	struct v92_pcm_phase3_cpt_rx *receiver,
	const struct v92_cpt_receive_event *record,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	uint8_t canonical[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bits;
	bool matches;

	if (!canonicalize(&record->parameters, canonical, &frame_bits))
		return V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR;
	event->observed_bits = record->observed_bits;
	event->parameters = record->parameters;
	if (receiver->matching_records == 0u) {
		receiver->parameters = record->parameters;
		memcpy(receiver->parameter_frame, canonical, sizeof(canonical));
		receiver->parameter_frame_bits = frame_bits;
		receiver->matching_records = 1u;
		event->type = V92_PCM_PHASE3_CPT_RX_PARAMETERS;
	} else {
		matches = frame_bits == receiver->parameter_frame_bits &&
			  memcmp(canonical, receiver->parameter_frame,
				 sizeof(canonical)) == 0;
		if (matches) {
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

const char *v92_pcm_phase3_cpt_rx_result_name(
	enum v92_pcm_phase3_cpt_rx_result result)
{
	switch (result) {
	case V92_PCM_PHASE3_CPT_RX_OK:
		return "ok";
	case V92_PCM_PHASE3_CPT_RX_DONE:
		return "done";
	case V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_PHASE3_CPT_RX_INVALID_STATE:
		return "invalid state";
	case V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
