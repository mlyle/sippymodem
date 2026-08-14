// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_downstream_rx.h"

#include <stddef.h>
#include <string.h>

static bool stage_is_valid(enum pcm_downstream_rx_stage stage)
{
	return stage == PCM_DOWNSTREAM_RX_TRN2D ||
	       stage == PCM_DOWNSTREAM_RX_MP_SEARCH ||
	       stage == PCM_DOWNSTREAM_RX_SUVD_SEARCH ||
	       stage == PCM_DOWNSTREAM_RX_ED_SEARCH ||
	       stage == PCM_DOWNSTREAM_RX_COMPLETE;
}

static bool protocol_is_valid(enum pcm_protocol protocol)
{
	return protocol == PCM_PROTOCOL_V90 || protocol == PCM_PROTOCOL_V92;
}

static bool mp_is_fresh(const struct v90_mp_receiver *receiver)
{
	return v90_mp_receiver_is_valid(receiver) &&
	       receiver->stage == V90_MP_SEARCH_SYNC &&
	       receiver->sync_one_count == 0;
}

static bool suvd_is_fresh(const struct v92_suvd_receiver *receiver)
{
	return v92_suvd_receiver_is_valid(receiver) &&
	       receiver->stage == V92_SUVD_SEARCH_SYNC &&
	       receiver->bit_index == 0u && receiver->sync_one_count == 0u;
}

static bool control_is_valid(const struct pcm_downstream_rx *receiver)
{
	uint8_t frame_bits = receiver->stream.decoder.mapping.frame_bits;

	if (receiver->protocol == PCM_PROTOCOL_V90)
		return v90_mp_receiver_is_valid(&receiver->control.mp) &&
		       receiver->control.mp.training_frame_bits == frame_bits;
	return v92_suvd_receiver_is_valid(&receiver->control.suvd) &&
	       receiver->control.suvd.training_frame_bits == frame_bits;
}

static bool control_is_fresh(const struct pcm_downstream_rx *receiver)
{
	return receiver->protocol == PCM_PROTOCOL_V90 ?
		       mp_is_fresh(&receiver->control.mp) :
		       suvd_is_fresh(&receiver->control.suvd);
}

static bool stage_matches_protocol(const struct pcm_downstream_rx *receiver)
{
	if (receiver->stage == PCM_DOWNSTREAM_RX_TRN2D)
		return true;
	if (receiver->protocol == PCM_PROTOCOL_V92)
		return receiver->stage == PCM_DOWNSTREAM_RX_SUVD_SEARCH;
	return receiver->stage == PCM_DOWNSTREAM_RX_MP_SEARCH ||
	       receiver->stage == PCM_DOWNSTREAM_RX_ED_SEARCH ||
	       receiver->stage == PCM_DOWNSTREAM_RX_COMPLETE;
}

bool pcm_downstream_rx_is_valid(
	const struct pcm_downstream_rx *receiver)
{
	size_t ed_bits;
	uint64_t training_bits;

	if (receiver == NULL || !protocol_is_valid(receiver->protocol) ||
	    !stage_is_valid(receiver->stage) ||
	    !stage_matches_protocol(receiver) ||
	    !v90_pcm_stream_rx_is_valid(&receiver->stream) ||
	    receiver->stream.decoder.mapping.kind != V90_CP_TRAINING ||
	    !control_is_valid(receiver))
		return false;
	training_bits = (uint64_t)V90_PHASE4_TRN2D_MINIMUM_FRAMES *
			receiver->stream.decoder.mapping.frame_bits;
	ed_bits = receiver->ed_frames *
		  (size_t)receiver->stream.decoder.mapping.frame_bits;
	if (receiver->trn2d_bit_errors > training_bits)
		return false;
	if (receiver->stage == PCM_DOWNSTREAM_RX_TRN2D) {
		return receiver->stream.received_symbols <
			       V90_PHASE4_TRN2D_MINIMUM_SYMBOLS &&
		       control_is_fresh(receiver) &&
		       receiver->ed_frames == 0 &&
		       receiver->ed_bit_errors == 0 &&
		       !receiver->ed_requested &&
		       !receiver->mp_prime_boundary;
	}
	if (receiver->stream.received_symbols <
	    V90_PHASE4_TRN2D_MINIMUM_SYMBOLS)
		return false;
	if (receiver->protocol == PCM_PROTOCOL_V92)
		return receiver->ed_frames == 0 &&
		       receiver->ed_bit_errors == 0 &&
		       !receiver->ed_requested &&
		       !receiver->mp_prime_boundary;
	if (receiver->ed_bit_errors > ed_bits ||
	    (receiver->mp_prime_boundary &&
	     (receiver->stage != PCM_DOWNSTREAM_RX_MP_SEARCH ||
	      receiver->ed_requested || receiver->stream.pending_count != 0 ||
	      !mp_is_fresh(&receiver->control.mp))))
		return false;
	if (receiver->stage == PCM_DOWNSTREAM_RX_MP_SEARCH)
		return receiver->ed_frames == 0 &&
		       receiver->ed_bit_errors == 0;
	if (!receiver->ed_requested || receiver->mp_prime_boundary ||
	    receiver->control.mp.stage != V90_MP_SEARCH_SYNC)
		return false;
	if (receiver->stage == PCM_DOWNSTREAM_RX_ED_SEARCH)
		return receiver->ed_frames < V90_PHASE4_ED_FRAMES;
	return receiver->ed_frames == V90_PHASE4_ED_FRAMES;
}

enum pcm_downstream_rx_result pcm_downstream_rx_init(
	struct pcm_downstream_rx *receiver,
	enum pcm_protocol protocol,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct pcm_downstream_rx initialized = { 0 };
	enum v90_pcm_stream_rx_result stream_result;
	bool control_initialized;

	if (receiver == NULL || training_mapping == NULL ||
	    !protocol_is_valid(protocol))
		return PCM_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(training_mapping) ||
	    training_mapping->kind != V90_CP_TRAINING)
		return PCM_DOWNSTREAM_RX_INVALID_PARAMETERS;
	stream_result = v90_pcm_stream_rx_init(
		&initialized.stream, training_mapping, decision_plane);
	if (stream_result == V90_PCM_STREAM_RX_INVALID_ARGUMENT)
		return PCM_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (stream_result == V90_PCM_STREAM_RX_INVALID_PARAMETERS)
		return PCM_DOWNSTREAM_RX_INVALID_PARAMETERS;
	if (stream_result != V90_PCM_STREAM_RX_OK)
		return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
	initialized.protocol = protocol;
	if (protocol == PCM_PROTOCOL_V90) {
		control_initialized = v90_mp_receiver_init(
			&initialized.control.mp,
			training_mapping->frame_bits) == V90_MP_RECEIVER_OK;
	} else {
		control_initialized = v92_suvd_receiver_init(
			&initialized.control.suvd,
			training_mapping->frame_bits) == V92_SUVD_RECEIVER_OK;
	}
	if (!control_initialized)
		return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
	initialized.stage = PCM_DOWNSTREAM_RX_TRN2D;
	if (!pcm_downstream_rx_is_valid(&initialized))
		return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return PCM_DOWNSTREAM_RX_OK;
}

bool pcm_downstream_rx_is_fresh(
	const struct pcm_downstream_rx *receiver)
{
	if (!pcm_downstream_rx_is_valid(receiver) ||
	    receiver->stage != PCM_DOWNSTREAM_RX_TRN2D ||
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
	return control_is_fresh(receiver);
}

static void begin_ed_search(struct pcm_downstream_rx *receiver)
{
	receiver->stage = PCM_DOWNSTREAM_RX_ED_SEARCH;
	receiver->ed_frames = 0;
	receiver->ed_bit_errors = 0;
	receiver->mp_prime_boundary = false;
}

enum pcm_downstream_rx_result pcm_downstream_rx_expect_ed(
	struct pcm_downstream_rx *receiver)
{
	struct pcm_downstream_rx next;

	if (receiver == NULL)
		return PCM_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!pcm_downstream_rx_is_valid(receiver) ||
	    receiver->protocol != PCM_PROTOCOL_V90 ||
	    receiver->stage == PCM_DOWNSTREAM_RX_TRN2D ||
	    receiver->stage == PCM_DOWNSTREAM_RX_COMPLETE)
		return PCM_DOWNSTREAM_RX_INVALID_STATE;
	if (receiver->ed_requested)
		return PCM_DOWNSTREAM_RX_OK;
	next = *receiver;
	next.ed_requested = true;
	if (next.mp_prime_boundary)
		begin_ed_search(&next);
	if (!pcm_downstream_rx_is_valid(&next))
		return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
	*receiver = next;
	return PCM_DOWNSTREAM_RX_OK;
}

bool pcm_downstream_rx_is_complete(
	const struct pcm_downstream_rx *receiver)
{
	return pcm_downstream_rx_is_valid(receiver) &&
	       receiver->stage == PCM_DOWNSTREAM_RX_COMPLETE &&
	       receiver->ed_bit_errors == 0;
}

bool pcm_downstream_rx_has_failed(
	const struct pcm_downstream_rx *receiver)
{
	return pcm_downstream_rx_is_valid(receiver) &&
	       receiver->stage == PCM_DOWNSTREAM_RX_COMPLETE &&
	       receiver->ed_bit_errors != 0;
}

static enum pcm_downstream_rx_result feed_mp_frame(
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
	struct pcm_downstream_event *event)
{
	struct v90_mp_event emitted = { 0 };
	bool aligned = false;

	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		struct v90_mp_event current;

		if (v90_mp_receive_bit(
			    &receiver->control.mp, frame->bits[bit], &current) !=
		    V90_MP_RECEIVER_OK)
			return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
		if (current.type == V90_MP_EVENT_NONE)
			continue;
		if (emitted.type != V90_MP_EVENT_NONE)
			return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
		emitted = current;
		aligned = bit + 1u == frame->bit_count;
	}
	if (emitted.type == V90_MP_EVENT_NONE)
		return PCM_DOWNSTREAM_RX_BUFFERING;
	if (!aligned && emitted.type != V90_MP_EVENT_REJECTED) {
		emitted = (struct v90_mp_event){
			.type = V90_MP_EVENT_REJECTED,
			.frame_result = V90_MP_INVALID_FRAME,
		};
	}
	event->mp = emitted;
	if (emitted.type == V90_MP_EVENT_REJECTED)
		return PCM_DOWNSTREAM_RX_MP_REJECTED;
	return v90_mp_record_acknowledges_cp(&emitted.record) ?
		       PCM_DOWNSTREAM_RX_MP_PRIME :
		       PCM_DOWNSTREAM_RX_MP;
}

static enum pcm_downstream_rx_result feed_suvd_frame(
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
	struct pcm_downstream_event *event)
{
	struct v92_suvd_event emitted = { 0 };
	bool aligned = false;

	for (size_t bit = 0u; bit < frame->bit_count; ++bit) {
		struct v92_suvd_event current;

		if (v92_suvd_receiver_receive_bit(
			    &receiver->control.suvd, frame->bits[bit],
			    &current) != V92_SUVD_RECEIVER_OK)
			return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
		if (current.type == V92_SUVD_EVENT_NONE)
			continue;
		if (emitted.type != V92_SUVD_EVENT_NONE)
			return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
		emitted = current;
		aligned = bit + 1u == frame->bit_count;
	}
	if (emitted.type == V92_SUVD_EVENT_NONE)
		return PCM_DOWNSTREAM_RX_BUFFERING;
	if (emitted.type == V92_SUVD_EVENT_RECORD && !aligned) {
		emitted = (struct v92_suvd_event){
			.type = V92_SUVD_EVENT_REJECTED,
			.frame_result = V92_SUVD_INVALID_FRAME,
		};
	}
	event->suvd = emitted;
	return emitted.type == V92_SUVD_EVENT_RECORD ?
		       PCM_DOWNSTREAM_RX_SUVD :
		       PCM_DOWNSTREAM_RX_SUVD_REJECTED;
}

static enum pcm_downstream_rx_result map_stream_failure(
	enum v90_pcm_stream_rx_result result)
{
	switch (result) {
	case V90_PCM_STREAM_RX_INVALID_ARGUMENT:
		return PCM_DOWNSTREAM_RX_INVALID_ARGUMENT;
	case V90_PCM_STREAM_RX_INVALID_PARAMETERS:
		return PCM_DOWNSTREAM_RX_INVALID_PARAMETERS;
	case V90_PCM_STREAM_RX_INVALID_STATE:
		return PCM_DOWNSTREAM_RX_INVALID_STATE;
	case V90_PCM_STREAM_RX_INVALID_SYMBOL:
		return PCM_DOWNSTREAM_RX_INVALID_SYMBOL;
	case V90_PCM_STREAM_RX_OUT_OF_RANGE:
		return PCM_DOWNSTREAM_RX_OUT_OF_RANGE;
	case V90_PCM_STREAM_RX_OK:
	case V90_PCM_STREAM_RX_BUFFERING:
	case V90_PCM_STREAM_RX_FRAME_READY:
	case V90_PCM_STREAM_RX_INTERNAL_ERROR:
		return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
	}
	return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
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

static struct pcm_trn2d_frame classify_trn2d(
	const struct v90_pcm_stream_frame *frame)
{
	struct pcm_trn2d_frame classified = {
		.bit_count = frame->bit_count,
		.rule_count = frame->rule_count,
		.training_bit_errors = nonone_bits(frame),
	};

	memcpy(classified.bits, frame->bits, frame->bit_count);
	memcpy(classified.rules, frame->rules,
	       frame->rule_count * sizeof(frame->rules[0]));
	return classified;
}

static enum pcm_downstream_rx_result receive_mp_frame(
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
	struct pcm_downstream_event *event)
{
	enum pcm_downstream_rx_result result =
		feed_mp_frame(receiver, frame, event);

	if (result == PCM_DOWNSTREAM_RX_MP_PRIME) {
		if (receiver->ed_requested)
			begin_ed_search(receiver);
		else
			receiver->mp_prime_boundary = true;
	}
	return result;
}

static enum pcm_downstream_rx_result receive_ed_frame(
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_stream_frame *frame,
	struct pcm_downstream_event *event)
{
	enum pcm_downstream_rx_result result =
		feed_mp_frame(receiver, frame, event);

	/* A repeated MP-prime begins with the 17-one prefix and its following
	 * zero within at most two training frames. Keep that candidate intact
	 * and restart the Ed window; it may be the record already accepted by
	 * the sender's spectral lookahead. */
	if (result != PCM_DOWNSTREAM_RX_BUFFERING ||
	    receiver->control.mp.stage != V90_MP_SEARCH_SYNC) {
		receiver->stage = PCM_DOWNSTREAM_RX_MP_SEARCH;
		receiver->ed_frames = 0;
		receiver->ed_bit_errors = 0;
		if (result == PCM_DOWNSTREAM_RX_MP_PRIME)
			begin_ed_search(receiver);
		return result;
	}
	receiver->ed_frames++;
	receiver->ed_bit_errors += nonzero_bits(frame);
	if (receiver->ed_frames < V90_PHASE4_ED_FRAMES)
		return PCM_DOWNSTREAM_RX_BUFFERING;
	receiver->stage = PCM_DOWNSTREAM_RX_COMPLETE;
	event->ed = (struct pcm_downstream_ed_event){
		.bit_count = (uint8_t)(V90_PHASE4_ED_FRAMES *
				       frame->bit_count),
		.bit_errors = receiver->ed_bit_errors,
	};
	return receiver->ed_bit_errors == 0 ?
		       PCM_DOWNSTREAM_RX_ED :
		       PCM_DOWNSTREAM_RX_ED_REJECTED;
}

enum pcm_downstream_rx_result pcm_downstream_rx_receive(
	struct pcm_downstream_rx *receiver, uint8_t network_octet,
	struct pcm_downstream_event *event)
{
	struct pcm_downstream_rx next;
	struct pcm_downstream_event generated = { 0 };
	struct v90_pcm_stream_frame frame;
	enum pcm_downstream_rx_result result;
	enum v90_pcm_stream_rx_result stream_result;

	if (receiver == NULL || event == NULL)
		return PCM_DOWNSTREAM_RX_INVALID_ARGUMENT;
	if (!pcm_downstream_rx_is_valid(receiver))
		return PCM_DOWNSTREAM_RX_INVALID_STATE;
	if (receiver->stage == PCM_DOWNSTREAM_RX_COMPLETE)
		return PCM_DOWNSTREAM_RX_INVALID_STATE;
	next = *receiver;
	if (next.stage == PCM_DOWNSTREAM_RX_MP_SEARCH)
		next.mp_prime_boundary = false;
	stream_result = v90_pcm_stream_rx_receive_octet(
		&next.stream, network_octet, &frame);
	if (stream_result == V90_PCM_STREAM_RX_BUFFERING) {
		if (!pcm_downstream_rx_is_valid(&next))
			return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
		*receiver = next;
		return PCM_DOWNSTREAM_RX_BUFFERING;
	}
	if (stream_result != V90_PCM_STREAM_RX_FRAME_READY)
		return map_stream_failure(stream_result);

	if (next.stage == PCM_DOWNSTREAM_RX_TRN2D) {
		generated.trn2d = classify_trn2d(&frame);
		next.trn2d_bit_errors +=
			generated.trn2d.training_bit_errors;
		if (next.stream.received_symbols ==
		    V90_PHASE4_TRN2D_MINIMUM_SYMBOLS) {
			next.stage = next.protocol == PCM_PROTOCOL_V90 ?
				PCM_DOWNSTREAM_RX_MP_SEARCH :
				PCM_DOWNSTREAM_RX_SUVD_SEARCH;
		}
		result = PCM_DOWNSTREAM_RX_TRN2D_FRAME;
	} else {
		if (next.stage == PCM_DOWNSTREAM_RX_ED_SEARCH)
			result = receive_ed_frame(&next, &frame, &generated);
		else if (next.stage == PCM_DOWNSTREAM_RX_SUVD_SEARCH)
			result = feed_suvd_frame(&next, &frame, &generated);
		else
			result = receive_mp_frame(&next, &frame, &generated);
		if (result == PCM_DOWNSTREAM_RX_INTERNAL_ERROR)
			return result;
	}
	if (!pcm_downstream_rx_is_valid(&next))
		return PCM_DOWNSTREAM_RX_INTERNAL_ERROR;
	*receiver = next;
	if (result != PCM_DOWNSTREAM_RX_BUFFERING)
		*event = generated;
	return result;
}

const char *pcm_downstream_rx_result_name(
	enum pcm_downstream_rx_result result)
{
	switch (result) {
	case PCM_DOWNSTREAM_RX_OK:
		return "ok";
	case PCM_DOWNSTREAM_RX_BUFFERING:
		return "buffering";
	case PCM_DOWNSTREAM_RX_TRN2D_FRAME:
		return "TRN2d frame";
	case PCM_DOWNSTREAM_RX_MP:
		return "MP";
	case PCM_DOWNSTREAM_RX_MP_PRIME:
		return "MP prime";
	case PCM_DOWNSTREAM_RX_MP_REJECTED:
		return "MP rejected";
	case PCM_DOWNSTREAM_RX_SUVD:
		return "SUVd";
	case PCM_DOWNSTREAM_RX_SUVD_REJECTED:
		return "SUVd rejected";
	case PCM_DOWNSTREAM_RX_ED:
		return "Ed";
	case PCM_DOWNSTREAM_RX_ED_REJECTED:
		return "Ed rejected";
	case PCM_DOWNSTREAM_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_DOWNSTREAM_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case PCM_DOWNSTREAM_RX_INVALID_STATE:
		return "invalid state";
	case PCM_DOWNSTREAM_RX_INVALID_SYMBOL:
		return "invalid symbol";
	case PCM_DOWNSTREAM_RX_OUT_OF_RANGE:
		return "out of range";
	case PCM_DOWNSTREAM_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
