// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_rx.h"

#include <stddef.h>
#include <string.h>

static bool stage_is_valid(enum v90_downstream_rx_stage stage)
{
	return stage == V90_DOWNSTREAM_RX_TRN2D ||
	       stage == V90_DOWNSTREAM_RX_MP_SEARCH ||
	       stage == V90_DOWNSTREAM_RX_ED_SEARCH ||
	       stage == V90_DOWNSTREAM_RX_COMPLETE;
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
	size_t ed_bits;
	uint64_t training_bits;

	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    !v90_pcm_stream_rx_is_valid(&receiver->stream) ||
	    receiver->stream.decoder.mapping.kind != V90_CP_TRAINING ||
	    !v90_mp_receiver_is_valid(&receiver->mp) ||
	    receiver->mp.training_frame_bits !=
		    receiver->stream.decoder.mapping.frame_bits)
		return false;
	training_bits = (uint64_t)V90_PHASE4_TRN2D_MINIMUM_FRAMES *
			receiver->stream.decoder.mapping.frame_bits;
	ed_bits = receiver->ed_frames *
		  (size_t)receiver->stream.decoder.mapping.frame_bits;
	if (receiver->trn2d_bit_errors > training_bits ||
	    receiver->ed_bit_errors > ed_bits ||
	    (receiver->mp_prime_boundary &&
	     (receiver->stage != V90_DOWNSTREAM_RX_MP_SEARCH ||
	      receiver->ed_requested || receiver->stream.pending_count != 0 ||
	      !mp_is_fresh(&receiver->mp))))
		return false;
	if (receiver->stage == V90_DOWNSTREAM_RX_TRN2D) {
		return receiver->stream.received_symbols <
			       V90_PHASE4_TRN2D_MINIMUM_SYMBOLS &&
		       mp_is_fresh(&receiver->mp) &&
		       receiver->ed_frames == 0 &&
		       receiver->ed_bit_errors == 0 &&
		       !receiver->ed_requested &&
		       !receiver->mp_prime_boundary;
	}
	if (receiver->stream.received_symbols <
	    V90_PHASE4_TRN2D_MINIMUM_SYMBOLS)
		return false;
	if (receiver->stage == V90_DOWNSTREAM_RX_MP_SEARCH)
		return receiver->ed_frames == 0 &&
		       receiver->ed_bit_errors == 0;
	if (!receiver->ed_requested || receiver->mp_prime_boundary ||
	    receiver->mp.stage != V90_MP_SEARCH_SYNC)
		return false;
	if (receiver->stage == V90_DOWNSTREAM_RX_ED_SEARCH)
		return receiver->ed_frames < V90_PHASE4_ED_FRAMES;
	return receiver->ed_frames == V90_PHASE4_ED_FRAMES;
}

enum v90_downstream_rx_result v90_downstream_rx_init(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_downstream_rx initialized = { 0 };
	enum v90_pcm_stream_rx_result stream_result;
	enum v90_mp_receiver_result mp_result;

	if (receiver == NULL || training_mapping == NULL)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING)
		return V90_DOWNSTREAM_RX_INVALID_PARAMETERS;
	stream_result = v90_pcm_stream_rx_init(
		&initialized.stream, training_mapping, decision_plane);
	if (stream_result == V90_PCM_STREAM_RX_INVALID_ARGUMENT)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (stream_result == V90_PCM_STREAM_RX_INVALID_PARAMETERS)
		return V90_DOWNSTREAM_RX_INVALID_PARAMETERS;
	if (stream_result != V90_PCM_STREAM_RX_OK)
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
	    receiver->trn2d_bit_errors != 0)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (receiver->stream.decoder.signs
			    .previous_shaping_signs[interval] != 0)
			return false;
	}
	return true;
}

static void begin_ed_search(struct v90_downstream_rx *receiver)
{
	receiver->stage = V90_DOWNSTREAM_RX_ED_SEARCH;
	receiver->ed_frames = 0;
	receiver->ed_bit_errors = 0;
	receiver->mp_prime_boundary = false;
}

enum v90_downstream_rx_result v90_downstream_rx_expect_ed(
	struct v90_downstream_rx *receiver)
{
	struct v90_downstream_rx next;

	if (receiver == NULL)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!v90_downstream_rx_is_valid(receiver) ||
	    receiver->stage == V90_DOWNSTREAM_RX_TRN2D ||
	    receiver->stage == V90_DOWNSTREAM_RX_COMPLETE)
		return V90_DOWNSTREAM_RX_INVALID_STATE;
	if (receiver->ed_requested)
		return V90_DOWNSTREAM_RX_OK;
	next = *receiver;
	next.ed_requested = true;
	if (next.mp_prime_boundary)
		begin_ed_search(&next);
	if (!v90_downstream_rx_is_valid(&next))
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	*receiver = next;
	return V90_DOWNSTREAM_RX_OK;
}

bool v90_downstream_rx_is_complete(
	const struct v90_downstream_rx *receiver)
{
	return v90_downstream_rx_is_valid(receiver) &&
	       receiver->stage == V90_DOWNSTREAM_RX_COMPLETE &&
	       receiver->ed_bit_errors == 0;
}

bool v90_downstream_rx_has_failed(
	const struct v90_downstream_rx *receiver)
{
	return v90_downstream_rx_is_valid(receiver) &&
	       receiver->stage == V90_DOWNSTREAM_RX_COMPLETE &&
	       receiver->ed_bit_errors != 0;
}

static enum v90_downstream_rx_result feed_mp_frame(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
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
	return v90_mp_record_acknowledges_cp(&emitted.record) ?
		       V90_DOWNSTREAM_RX_MP_PRIME :
		       V90_DOWNSTREAM_RX_MP;
}

static enum v90_downstream_rx_result map_stream_failure(
	enum v90_pcm_stream_rx_result result)
{
	switch (result) {
	case V90_PCM_STREAM_RX_INVALID_ARGUMENT:
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	case V90_PCM_STREAM_RX_INVALID_PARAMETERS:
		return V90_DOWNSTREAM_RX_INVALID_PARAMETERS;
	case V90_PCM_STREAM_RX_INVALID_STATE:
		return V90_DOWNSTREAM_RX_INVALID_STATE;
	case V90_PCM_STREAM_RX_INVALID_SYMBOL:
		return V90_DOWNSTREAM_RX_INVALID_SYMBOL;
	case V90_PCM_STREAM_RX_OUT_OF_RANGE:
		return V90_DOWNSTREAM_RX_OUT_OF_RANGE;
	case V90_PCM_STREAM_RX_OK:
	case V90_PCM_STREAM_RX_BUFFERING:
	case V90_PCM_STREAM_RX_FRAME_READY:
	case V90_PCM_STREAM_RX_INTERNAL_ERROR:
		return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
	}
	return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
}

static uint8_t nonzero_bits(const struct v90_pcm_stream_frame *frame)
{
	uint8_t count = 0;

	for (size_t bit = 0; bit < frame->bit_count; ++bit)
		count += frame->bits[bit] != 0;
	return count;
}

static uint8_t nonone_bits(const struct v90_pcm_stream_frame *frame)
{
	uint8_t count = 0;

	for (size_t bit = 0; bit < frame->bit_count; ++bit)
		count += frame->bits[bit] != 1;
	return count;
}

static struct v90_trn2d_frame classify_trn2d(
	const struct v90_pcm_stream_frame *frame)
{
	struct v90_trn2d_frame classified = {
		.bit_count = frame->bit_count,
		.rule_count = frame->rule_count,
		.training_bit_errors = nonone_bits(frame),
	};

	memcpy(classified.bits, frame->bits, frame->bit_count);
	memcpy(classified.rules, frame->rules,
	       frame->rule_count * sizeof(frame->rules[0]));
	return classified;
}

static enum v90_downstream_rx_result receive_mp_frame(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
	struct v90_downstream_event *event)
{
	enum v90_downstream_rx_result result =
		feed_mp_frame(receiver, frame, event);

	if (result == V90_DOWNSTREAM_RX_MP_PRIME) {
		if (receiver->ed_requested)
			begin_ed_search(receiver);
		else
			receiver->mp_prime_boundary = true;
	}
	return result;
}

static enum v90_downstream_rx_result receive_ed_frame(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
	struct v90_downstream_event *event)
{
	enum v90_downstream_rx_result result =
		feed_mp_frame(receiver, frame, event);

	/* A repeated MP-prime begins with the 17-one prefix and its following
	 * zero within at most two training frames. Keep that candidate intact
	 * and restart the Ed window; it may be the record already accepted by
	 * the sender's spectral lookahead. */
	if (result != V90_DOWNSTREAM_RX_BUFFERING ||
	    receiver->mp.stage != V90_MP_SEARCH_SYNC) {
		receiver->stage = V90_DOWNSTREAM_RX_MP_SEARCH;
		receiver->ed_frames = 0;
		receiver->ed_bit_errors = 0;
		if (result == V90_DOWNSTREAM_RX_MP_PRIME)
			begin_ed_search(receiver);
		return result;
	}
	receiver->ed_frames++;
	receiver->ed_bit_errors += nonzero_bits(frame);
	if (receiver->ed_frames < V90_PHASE4_ED_FRAMES)
		return V90_DOWNSTREAM_RX_BUFFERING;
	receiver->stage = V90_DOWNSTREAM_RX_COMPLETE;
	event->ed = (struct v90_downstream_ed_event){
		.bit_count = (uint8_t)(V90_PHASE4_ED_FRAMES *
				       frame->bit_count),
		.bit_errors = receiver->ed_bit_errors,
	};
	return receiver->ed_bit_errors == 0 ?
		       V90_DOWNSTREAM_RX_ED :
		       V90_DOWNSTREAM_RX_ED_REJECTED;
}

enum v90_downstream_rx_result v90_downstream_rx_receive(
	struct v90_downstream_rx *receiver, uint8_t network_octet,
	struct v90_downstream_event *event)
{
	struct v90_downstream_rx next;
	struct v90_downstream_event generated = { 0 };
	struct v90_pcm_stream_frame frame;
	enum v90_downstream_rx_result result;
	enum v90_pcm_stream_rx_result stream_result;

	if (receiver == NULL || event == NULL)
		return V90_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!v90_downstream_rx_is_valid(receiver))
		return V90_DOWNSTREAM_RX_INVALID_STATE;
	if (receiver->stage == V90_DOWNSTREAM_RX_COMPLETE)
		return V90_DOWNSTREAM_RX_INVALID_STATE;
	next = *receiver;
	if (next.stage == V90_DOWNSTREAM_RX_MP_SEARCH)
		next.mp_prime_boundary = false;
	stream_result = v90_pcm_stream_rx_receive_octet(
		&next.stream, network_octet, &frame);
	if (stream_result == V90_PCM_STREAM_RX_BUFFERING) {
		if (!v90_downstream_rx_is_valid(&next))
			return V90_DOWNSTREAM_RX_INTERNAL_ERROR;
		*receiver = next;
		return V90_DOWNSTREAM_RX_BUFFERING;
	}
	if (stream_result != V90_PCM_STREAM_RX_FRAME_READY)
		return map_stream_failure(stream_result);

	if (next.stage == V90_DOWNSTREAM_RX_TRN2D) {
		generated.trn2d = classify_trn2d(&frame);
		next.trn2d_bit_errors +=
			generated.trn2d.training_bit_errors;
		if (next.stream.received_symbols ==
		    V90_PHASE4_TRN2D_MINIMUM_SYMBOLS)
			next.stage = V90_DOWNSTREAM_RX_MP_SEARCH;
		result = V90_DOWNSTREAM_RX_TRN2D_FRAME;
	} else {
		result = next.stage == V90_DOWNSTREAM_RX_ED_SEARCH ?
				 receive_ed_frame(&next, &frame, &generated) :
				 receive_mp_frame(&next, &frame, &generated);
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
	case V90_DOWNSTREAM_RX_ED:
		return "Ed";
	case V90_DOWNSTREAM_RX_ED_REJECTED:
		return "Ed rejected";
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
