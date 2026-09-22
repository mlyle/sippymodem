// SPDX-License-Identifier: BSD-3-Clause

#include "v92_control_rx.h"

#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	CONTROL_SYNC_BITS = 17,
	CONTROL_MARKER_BIT = 18,
};

_Static_assert(
	(size_t)V92_CPD_MAX_FRAME_BYTES >= (size_t)V92_CPU_MAX_FRAME_BYTES,
	"shared control receiver buffer must hold either parameter record");

static bool stage_is_valid(enum v92_control_rx_stage stage)
{
	return stage == V92_CONTROL_RX_SEARCH_SYNC ||
	       stage == V92_CONTROL_RX_READ_MARKER ||
	       stage == V92_CONTROL_RX_RECEIVE_STATUS ||
	       stage == V92_CONTROL_RX_RECEIVE_PARAMETERS;
}

static bool configuration_is_valid(
	const struct v92_control_rx *receiver)
{
	if (receiver->direction == V92_CONTROL_DOWNSTREAM) {
		return receiver->alignment_bits >=
			       V92_CPD_DOWNSTREAM_FRAME_MIN_BITS &&
		       receiver->alignment_bits <=
			       V92_CPD_DOWNSTREAM_DATA_MAX_BITS;
	}
	return receiver->direction == V92_CONTROL_UPSTREAM &&
	       v92_control_upstream_alignment_is_valid(receiver->alignment_bits);
}

static size_t maximum_frame_bits(
	const struct v92_control_rx *receiver)
{
	return receiver->direction == V92_CONTROL_DOWNSTREAM ?
		       V92_CPD_MAX_FRAME_BITS : V92_CPU_MAX_FRAME_BITS;
}

static size_t status_frame_bits(
	const struct v92_control_rx *receiver)
{
	if (receiver->direction == V92_CONTROL_DOWNSTREAM)
		return v92_suvd_frame_bits(receiver->alignment_bits);
	return v92_suvu_frame_bits(receiver->alignment_bits);
}

bool v92_control_rx_is_valid(
	const struct v92_control_rx *receiver)
{
	size_t inferred = 0u;
	size_t status_bits;

	if (receiver == NULL || !stage_is_valid(receiver->stage) ||
	    (receiver->record_boundary && receiver->stage != V92_CONTROL_RX_SEARCH_SYNC) ||
	    !configuration_is_valid(receiver) ||
	    receiver->sync_one_count > CONTROL_SYNC_BITS ||
	    receiver->bit_index > maximum_frame_bits(receiver) ||
	    !packed_msb_tail_is_zero(
		    receiver->frame, receiver->bit_index,
		    sizeof(receiver->frame)))
		return false;
	if (receiver->stage == V92_CONTROL_RX_SEARCH_SYNC)
		return receiver->frame_bit_count == 0u &&
		       receiver->bit_index == 0u;
	if (!pcm_control_frame_prefix_is_valid(receiver->frame) ||
	    receiver->sync_one_count != pcm_control_frame_trailing_ones(
		    receiver->frame, receiver->bit_index))
		return false;
	if (receiver->stage == V92_CONTROL_RX_READ_MARKER)
		return receiver->frame_bit_count == 0u &&
		       receiver->bit_index == CONTROL_MARKER_BIT;
	if (receiver->bit_index <= CONTROL_MARKER_BIT)
		return false;
	if (receiver->stage == V92_CONTROL_RX_RECEIVE_STATUS) {
		status_bits = status_frame_bits(receiver);
		return receiver->frame_bit_count == status_bits &&
		       receiver->bit_index < status_bits &&
		       packed_msb_bit(
			       receiver->frame, CONTROL_MARKER_BIT) != 0u;
	}
	if (packed_msb_bit(receiver->frame, CONTROL_MARKER_BIT) != 0u)
		return false;
	if (receiver->direction == V92_CONTROL_DOWNSTREAM) {
		enum v92_cpd_result probe = v92_cpd_probe_frame_bits(
			receiver->frame, sizeof(receiver->frame),
			receiver->bit_index, receiver->alignment_bits,
			&inferred);

		if (receiver->frame_bit_count == 0u)
			return probe == V92_CPD_INCOMPLETE;
		return probe == V92_CPD_OK &&
		       inferred == receiver->frame_bit_count &&
		       receiver->bit_index < receiver->frame_bit_count;
	} else {
		enum v92_cpu_result probe = v92_cpu_probe_frame_bits(
			receiver->frame, sizeof(receiver->frame),
			receiver->bit_index,
			receiver->alignment_bits, &inferred);

		if (receiver->frame_bit_count == 0u)
			return probe == V92_CPU_INCOMPLETE;
		return probe == V92_CPU_OK &&
		       inferred == receiver->frame_bit_count &&
		       receiver->bit_index < receiver->frame_bit_count;
	}
}

enum v92_control_rx_result v92_control_rx_init_downstream(
	struct v92_control_rx *receiver, uint8_t mapping_frame_bits)
{
	struct v92_control_rx initialized = {
		.alignment_bits = mapping_frame_bits,
		.direction = V92_CONTROL_DOWNSTREAM,
		.stage = V92_CONTROL_RX_SEARCH_SYNC,
	};

	if (receiver == NULL)
		return V92_CONTROL_RX_INVALID_ARGUMENT;
	if (mapping_frame_bits < V92_CPD_DOWNSTREAM_FRAME_MIN_BITS ||
	    mapping_frame_bits > V92_CPD_DOWNSTREAM_DATA_MAX_BITS)
		return V92_CONTROL_RX_INVALID_PARAMETERS;
	if (!v92_control_rx_is_valid(&initialized))
		return V92_CONTROL_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V92_CONTROL_RX_OK;
}

enum v92_control_rx_result v92_control_rx_init_upstream(
	struct v92_control_rx *receiver,
	uint8_t mapping_frame_bits)
{
	struct v92_control_rx initialized = {
		.alignment_bits = mapping_frame_bits,
		.direction = V92_CONTROL_UPSTREAM,
		.stage = V92_CONTROL_RX_SEARCH_SYNC,
	};

	if (receiver == NULL)
		return V92_CONTROL_RX_INVALID_ARGUMENT;
	if (!v92_control_upstream_alignment_is_valid(mapping_frame_bits))
		return V92_CONTROL_RX_INVALID_PARAMETERS;
	if (!v92_control_rx_is_valid(&initialized))
		return V92_CONTROL_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V92_CONTROL_RX_OK;
}

static void clear_candidate(struct v92_control_rx *receiver)
{
	memset(receiver->frame, 0, sizeof(receiver->frame));
	receiver->frame_bit_count = 0u;
	receiver->bit_index = 0u;
	receiver->sync_one_count = 0u;
	receiver->stage = V92_CONTROL_RX_SEARCH_SYNC;
}

/* A rejected candidate may end inside the next record's sync prefix. */
static void finish_candidate(struct v92_control_rx *receiver, bool accepted)
{
	uint8_t sync_one_count = accepted ? 0u : receiver->sync_one_count;

	clear_candidate(receiver);
	receiver->sync_one_count = sync_one_count;
}

static void begin_candidate(struct v92_control_rx *receiver)
{
	clear_candidate(receiver);
	pcm_control_frame_write_prefix(receiver->frame);
	receiver->bit_index = CONTROL_MARKER_BIT;
	receiver->stage = V92_CONTROL_RX_READ_MARKER;
}

static void reject_cpd(
	struct v92_control_rx *receiver,
	struct v92_control_event *event,
	enum v92_cpd_result result)
{
	event->type = V92_CONTROL_EVENT_CPD_REJECTED;
	event->record.cpd.frame_result = result;
	finish_candidate(receiver, false);
}

static void reject_cpu(
	struct v92_control_rx *receiver,
	struct v92_control_event *event,
	enum v92_cpu_result result)
{
	/* Table 24 uses type 2, transmitted LSB first in bits 19:20. */
	if (packed_msb_read_lsb_field(receiver->frame, 19u, 2u) == 2u) {
		event->type = V92_CONTROL_EVENT_CPUS_REJECTED;
		event->record.cpus.frame_result = result;
	} else {
		event->type = V92_CONTROL_EVENT_CPU_REJECTED;
		event->record.cpu.frame_result = result;
	}
	finish_candidate(receiver, false);
}

static void finish_suvd(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	struct v92_suvd_parameters parameters;
	enum v92_suvd_result result = v92_suvd_decode(
		receiver->frame, sizeof(receiver->frame),
		receiver->frame_bit_count, receiver->alignment_bits,
		&parameters);

	event->record.suvd.frame_result = result;
	if (result == V92_SUVD_OK) {
		event->type = V92_CONTROL_EVENT_SUVD;
		event->record.suvd.parameters = parameters;
	} else {
		event->type = V92_CONTROL_EVENT_SUVD_REJECTED;
	}
	finish_candidate(receiver, result == V92_SUVD_OK);
}

static void finish_suvu(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	struct v92_suvu_parameters parameters;
	enum v92_suvu_result result = v92_suvu_decode(
		receiver->frame, sizeof(receiver->frame),
		receiver->frame_bit_count, receiver->alignment_bits,
		&parameters);

	event->record.suvu.frame_result = result;
	if (result == V92_SUVU_OK) {
		event->type = V92_CONTROL_EVENT_SUVU;
		event->record.suvu.parameters = parameters;
	} else {
		event->type = V92_CONTROL_EVENT_SUVU_REJECTED;
	}
	finish_candidate(receiver, result == V92_SUVU_OK);
}

static void finish_cpd(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	struct v92_cpd_parameters parameters;
	enum v92_cpd_result result = v92_cpd_decode(
		receiver->frame, sizeof(receiver->frame),
		receiver->frame_bit_count, receiver->alignment_bits,
		&parameters);

	event->record.cpd.frame_result = result;
	if (result == V92_CPD_OK) {
		event->type = V92_CONTROL_EVENT_CPD;
		event->record.cpd.parameters = parameters;
	} else {
		event->type = V92_CONTROL_EVENT_CPD_REJECTED;
	}
	finish_candidate(receiver, result == V92_CPD_OK);
}

static void finish_cpu(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	struct v92_cpu_parameters parameters;
	enum v92_cpu_result result;

	if (packed_msb_read_lsb_field(receiver->frame, 19u, 2u) == 2u) {
		struct v92_cpus_parameters short_parameters;

		result = v92_cpus_decode(
			receiver->frame, sizeof(receiver->frame),
			receiver->frame_bit_count, receiver->alignment_bits,
			&short_parameters);

		if (result != V92_CPU_OK) {
			reject_cpu(receiver, event, result);
			return;
		}
		event->type = V92_CONTROL_EVENT_CPUS;
		event->record.cpus.frame_result = result;
		event->record.cpus.parameters = short_parameters;
		clear_candidate(receiver);
		return;
	}
	result = v92_cpu_decode(
		receiver->frame, sizeof(receiver->frame),
		receiver->frame_bit_count,
		receiver->alignment_bits, &parameters);

	event->record.cpu.frame_result = result;
	if (result == V92_CPU_OK) {
		event->type = V92_CONTROL_EVENT_CPU;
		event->record.cpu.parameters = parameters;
	} else {
		event->type = V92_CONTROL_EVENT_CPU_REJECTED;
	}
	finish_candidate(receiver, result == V92_CPU_OK);
}

static void finish_status(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	if (receiver->direction == V92_CONTROL_DOWNSTREAM)
		finish_suvd(receiver, event);
	else
		finish_suvu(receiver, event);
}

static void finish_parameters(
	struct v92_control_rx *receiver,
	struct v92_control_event *event)
{
	if (receiver->direction == V92_CONTROL_DOWNSTREAM)
		finish_cpd(receiver, event);
	else
		finish_cpu(receiver, event);
}

static bool receive_valid_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	size_t frame_bits = 0u;
	enum v92_cpd_result probe;

	if (pcm_control_frame_sync_bit(&receiver->sync_one_count, bit != 0u)) {
		begin_candidate(receiver);
		return true;
	}
	if (receiver->stage == V92_CONTROL_RX_SEARCH_SYNC)
		return true;
	packed_msb_set_bit(receiver->frame, receiver->bit_index, bit != 0u);
	receiver->bit_index++;
	if (receiver->stage == V92_CONTROL_RX_READ_MARKER) {
		if (bit != 0u) {
			receiver->frame_bit_count = status_frame_bits(receiver);
			receiver->stage = V92_CONTROL_RX_RECEIVE_STATUS;
		} else {
			receiver->stage = V92_CONTROL_RX_RECEIVE_PARAMETERS;
		}
		return true;
	}
	if (receiver->stage == V92_CONTROL_RX_RECEIVE_PARAMETERS &&
	    receiver->frame_bit_count == 0u) {
		if (receiver->direction == V92_CONTROL_DOWNSTREAM) {
			probe = v92_cpd_probe_frame_bits(
				receiver->frame, sizeof(receiver->frame),
				receiver->bit_index, receiver->alignment_bits,
				&frame_bits);
			if (probe == V92_CPD_OK)
				receiver->frame_bit_count = frame_bits;
			else if (probe != V92_CPD_INCOMPLETE) {
				if (probe != V92_CPD_INVALID_FRAME)
					return false;
				reject_cpd(receiver, event, probe);
				return true;
			}
		} else {
			enum v92_cpu_result cpu_probe =
				v92_cpu_probe_frame_bits(
					receiver->frame, sizeof(receiver->frame),
					receiver->bit_index,
					receiver->alignment_bits,
					&frame_bits);

			if (cpu_probe == V92_CPU_OK)
				receiver->frame_bit_count = frame_bits;
			else if (cpu_probe != V92_CPU_INCOMPLETE) {
				if (cpu_probe != V92_CPU_INVALID_FRAME)
					return false;
				reject_cpu(receiver, event, cpu_probe);
				return true;
			}
		}
	}
	if (receiver->frame_bit_count == 0u)
		return receiver->bit_index < maximum_frame_bits(receiver);
	if (receiver->bit_index == receiver->frame_bit_count) {
		if (receiver->stage == V92_CONTROL_RX_RECEIVE_STATUS)
			finish_status(receiver, event);
		else
			finish_parameters(receiver, event);
		receiver->record_boundary = true;
	}
	return true;
}

enum v92_control_rx_result v92_control_rx_receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	struct v92_control_rx next;
	struct v92_control_event emitted = { 0 };

	if (receiver == NULL || event == NULL || bit > 1u)
		return V92_CONTROL_RX_INVALID_ARGUMENT;
	if (!v92_control_rx_is_valid(receiver))
		return V92_CONTROL_RX_INVALID_STATE;
	next = *receiver;
	next.record_boundary = false;
	if (!receive_valid_bit(&next, bit, &emitted))
		return V92_CONTROL_RX_INTERNAL_ERROR;
	if (!v92_control_rx_is_valid(&next))
		return V92_CONTROL_RX_INTERNAL_ERROR;
	*receiver = next;
	*event = emitted;
	return V92_CONTROL_RX_OK;
}

const char *v92_control_rx_result_name(
	enum v92_control_rx_result result)
{
	switch (result) {
	case V92_CONTROL_RX_OK:
		return "ok";
	case V92_CONTROL_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_CONTROL_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_CONTROL_RX_INVALID_STATE:
		return "invalid state";
	case V92_CONTROL_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
