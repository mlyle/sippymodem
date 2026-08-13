// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_rx.h"

#include <stddef.h>
#include <string.h>

static bool stage_is_valid(enum v90_downstream_rx_stage stage)
{
	return stage == V90_DOWNSTREAM_RX_TRN2D ||
	       stage == V90_DOWNSTREAM_RX_MP_SEARCH;
}

static bool mp_is_fresh(const struct v90_mp_receiver *receiver)
{
	return v90_mp_receiver_is_valid(receiver) &&
	       receiver->stage == V90_MP_SEARCH_SYNC &&
	       receiver->sync_one_count == 0;
}

bool v90_downstream_rx_is_valid(
	const struct v90_downstream_rx *receiver)
{
	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !v90_trn2d_rx_is_valid(&receiver->stream) ||
	    !v90_mp_receiver_is_valid(&receiver->mp) ||
	    receiver->mp.training_frame_bits !=
		    receiver->stream.decoder.mapping.frame_bits)
		return false;
	if (receiver->stage == V90_DOWNSTREAM_RX_TRN2D)
		return receiver->stream.received_symbols <
			       V90_PHASE4_TRN2D_MINIMUM_SYMBOLS &&
		       mp_is_fresh(&receiver->mp);
	return receiver->stream.received_symbols >=
	       V90_PHASE4_TRN2D_MINIMUM_SYMBOLS;
}

enum v90_downstream_rx_result v90_downstream_rx_init(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_downstream_rx initialized = { 0 };
	enum v90_trn2d_rx_result stream_result;
	enum v90_mp_receiver_result mp_result;

	if (receiver == NULL || training_mapping == NULL)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	stream_result = v90_trn2d_rx_init(
		&initialized.stream, training_mapping, decision_plane);
	if (stream_result == V90_TRN2D_RX_INVALID_ARGUMENT)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (stream_result == V90_TRN2D_RX_INVALID_PARAMETERS)
		return V90_DOWNSTREAM_RX_INVALID_PARAMETERS;
	if (stream_result != V90_TRN2D_RX_OK)
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	mp_result = v90_mp_receiver_init(
		&initialized.mp, training_mapping->frame_bits);
	if (mp_result == V90_MP_RECEIVER_INVALID_PARAMETERS)
		return V90_DOWNSTREAM_RX_INVALID_PARAMETERS;
	if (mp_result != V90_MP_RECEIVER_OK)
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	initialized.stage = V90_DOWNSTREAM_RX_TRN2D;
	if (!v90_downstream_rx_is_valid(&initialized))
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V90_DOWNSTREAM_RX_OK;
}

bool v90_downstream_rx_is_fresh(
	const struct v90_downstream_rx *receiver)
{
	if (!v90_downstream_rx_is_valid(receiver) ||
	    receiver->stage != V90_DOWNSTREAM_RX_TRN2D ||
	    receiver->stream.descrambler.history != 0 ||
	    receiver->stream.decoder.signs.previous_odd != 0 ||
	    receiver->stream.decoder.signs.previous_pcm_sign != 0 ||
	    receiver->stream.decoder.signs.trellis_state ||
	    receiver->stream.pending_count != 0 ||
	    receiver->stream.received_symbols != 0 ||
	    receiver->stream.received_frames != 0 ||
	    receiver->stream.received_bits != 0 ||
	    receiver->stream.training_bit_errors != 0)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (receiver->stream.decoder.signs
			    .previous_shaping_signs[interval] != 0)
			return false;
	}
	return true;
}

static bool record_acknowledges_cp(const struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       record->parameters.type0.acknowledges_cp :
		       record->parameters.type1.common.acknowledges_cp;
}

static enum v90_downstream_rx_result feed_mp_frame(
	struct v90_downstream_rx *receiver,
	const struct v90_trn2d_frame *frame,
	struct v90_downstream_event *event)
{
	struct v90_mp_event emitted = { 0 };
	bool aligned = false;

	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		struct v90_mp_event current;

		if (v90_mp_receive_bit(
			    &receiver->mp, frame->bits[bit], &current) !=
		    V90_MP_RECEIVER_OK)
			return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
		if (current.type == V90_MP_EVENT_NONE)
			continue;
		if (emitted.type != V90_MP_EVENT_NONE)
			return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
		emitted = current;
		aligned = bit + 1u == frame->bit_count;
	}
	if (emitted.type == V90_MP_EVENT_NONE)
		return V90_DOWNSTREAM_RX_BUFFERING;
	if (!aligned && emitted.type != V90_MP_EVENT_REJECTED) {
		emitted = (struct v90_mp_event){
			.type = V90_MP_EVENT_REJECTED,
			.frame_result = V90_MP_INVALID_FRAME,
		};
	}
	event->mp = emitted;
	if (emitted.type == V90_MP_EVENT_REJECTED)
		return V90_DOWNSTREAM_RX_MP_REJECTED;
	return record_acknowledges_cp(&emitted.record) ?
		       V90_DOWNSTREAM_RX_MP_PRIME :
		       V90_DOWNSTREAM_RX_MP;
}

static enum v90_downstream_rx_result map_stream_failure(
	enum v90_trn2d_rx_result result)
{
	switch (result) {
	case V90_TRN2D_RX_INVALID_ARGUMENT:
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	case V90_TRN2D_RX_INVALID_PARAMETERS:
		return V90_DOWNSTREAM_RX_INVALID_PARAMETERS;
	case V90_TRN2D_RX_INVALID_STATE:
		return V90_DOWNSTREAM_RX_INVALID_STATE;
	case V90_TRN2D_RX_INVALID_SYMBOL:
		return V90_DOWNSTREAM_RX_INVALID_SYMBOL;
	case V90_TRN2D_RX_OUT_OF_RANGE:
		return V90_DOWNSTREAM_RX_OUT_OF_RANGE;
	case V90_TRN2D_RX_OK:
	case V90_TRN2D_RX_BUFFERING:
	case V90_TRN2D_RX_FRAME_READY:
	case V90_TRN2D_RX_INTERNAL_ERROR:
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	}
	return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
}

enum v90_downstream_rx_result v90_downstream_rx_receive(
	struct v90_downstream_rx *receiver, uint8_t network_octet,
	struct v90_downstream_event *event)
{
	struct v90_downstream_rx next;
	struct v90_downstream_event generated = { 0 };
	struct v90_trn2d_frame frame;
	enum v90_downstream_rx_result result;
	enum v90_trn2d_rx_result stream_result;

	if (receiver == NULL || event == NULL)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!v90_downstream_rx_is_valid(receiver))
		return V90_DOWNSTREAM_RX_INVALID_STATE;
	next = *receiver;
	stream_result = v90_trn2d_rx_receive_octet(
		&next.stream, network_octet, &frame);
	if (stream_result == V90_TRN2D_RX_BUFFERING) {
		if (!v90_downstream_rx_is_valid(&next))
			return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
		*receiver = next;
		return V90_DOWNSTREAM_RX_BUFFERING;
	}
	if (stream_result != V90_TRN2D_RX_FRAME_READY)
		return map_stream_failure(stream_result);

	if (next.stage == V90_DOWNSTREAM_RX_TRN2D) {
		generated.trn2d = frame;
		if (next.stream.received_symbols ==
		    V90_PHASE4_TRN2D_MINIMUM_SYMBOLS)
			next.stage = V90_DOWNSTREAM_RX_MP_SEARCH;
		result = V90_DOWNSTREAM_RX_TRN2D_FRAME;
	} else {
		/* The lower owner computes departures from TRN2d ones for every
		 * decoded frame. Once MP search begins, remove that policy-only
		 * classification while retaining its complete coding state. */
		if (next.stream.training_bit_errors <
		    frame.training_bit_errors)
			return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
		next.stream.training_bit_errors -=
			frame.training_bit_errors;
		result = feed_mp_frame(&next, &frame, &generated);
		if (result == V90_DOWNSTREAM_RX_INTERNAL_ERROR)
			return result;
	}
	if (!v90_downstream_rx_is_valid(&next))
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	*receiver = next;
	if (result != V90_DOWNSTREAM_RX_BUFFERING)
		*event = generated;
	return result;
}

const char *v90_downstream_rx_result_name(
	enum v90_downstream_rx_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_RX_OK:
		return "ok";
	case V90_DOWNSTREAM_RX_BUFFERING:
		return "buffering";
	case V90_DOWNSTREAM_RX_TRN2D_FRAME:
		return "TRN2d frame";
	case V90_DOWNSTREAM_RX_MP:
		return "MP";
	case V90_DOWNSTREAM_RX_MP_PRIME:
		return "MP prime";
	case V90_DOWNSTREAM_RX_MP_REJECTED:
		return "MP rejected";
	case V90_DOWNSTREAM_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DOWNSTREAM_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_DOWNSTREAM_RX_INVALID_STATE:
		return "invalid state";
	case V90_DOWNSTREAM_RX_INVALID_SYMBOL:
		return "invalid symbol";
	case V90_DOWNSTREAM_RX_OUT_OF_RANGE:
		return "out of range";
	case V90_DOWNSTREAM_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
