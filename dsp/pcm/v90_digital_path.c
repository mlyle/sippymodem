// SPDX-License-Identifier: BSD-3-Clause

#include "v90_digital_path.h"

#include "g711.h"
#include "memory_region.h"

#include <stddef.h>
#include <stdint.h>

static bool data_mapping_matches(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init(&expected, parameters) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(mapping, &expected);
}

static bool downstream_is_valid(const struct v90_digital_path *path)
{
	if (path->downstream_stage ==
	    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING) {
		const struct pcm_downstream_tx *training =
			&path->downstream.training;

		return pcm_downstream_tx_is_valid(training) &&
		       training->encoder.mapping.kind == V90_CP_TRAINING &&
		       training->encoder.mapping.law == path->law &&
		       (path->failed || !pcm_downstream_tx_is_complete(training));
	}
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_DATA) {
		const struct v90_downstream_data_stream *data =
			&path->downstream.data;

		return path->data_mapping_ready &&
		       v90_downstream_data_stream_is_valid(data) &&
		       v90_pcm_mapping_equal(
			       &data->transmitter.encoder.mapping,
			       &path->data_mapping);
	}
	return false;
}

static bool upstream_is_valid(const struct v90_digital_path *path)
{
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP) {
		const struct v90_upstream_rx *startup = &path->upstream.startup;
		bool mapping_state_is_valid =
			(startup->data_cp_received == path->data_mapping_ready &&
			 (!startup->data_cp_received ||
			  data_mapping_matches(
				  &path->data_mapping,
				  &startup->data_parameters))) ||
			(path->failed && startup->data_cp_received &&
			 !path->data_mapping_ready);

		return v90_upstream_rx_is_valid(startup) &&
		       startup->training_mapping.law == path->law &&
		       mapping_state_is_valid &&
		       (path->failed || !v90_upstream_rx_is_complete(startup));
	}
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA)
		return path->data_mapping_ready &&
		       v34_data_line_receiver_is_valid(&path->upstream.data) &&
		       (path->failed ||
			!v34_data_line_receiver_has_failed(
				&path->upstream.data));
	return false;
}

bool v90_digital_path_is_valid(const struct v90_digital_path *path)
{
	if (path == NULL || !g711_law_is_valid(path->law) ||
	    path->enabled_downstream_rate_mask == 0 ||
	    (path->enabled_downstream_rate_mask &
	     ~(uint32_t)V90_JD_RATE_CAPABILITY_MASK) != 0 ||
	    (path->data_mapping_ready &&
	     (!v90_pcm_mapping_is_valid(&path->data_mapping) ||
	      path->data_mapping.kind != V90_CP_DATA ||
	      path->data_mapping.law != path->law)))
		return false;
	return downstream_is_valid(path) && upstream_is_valid(path);
}

enum v90_digital_path_result v90_digital_path_init(
	struct v90_digital_path *path,
	const struct v90_phase4_digital_handoff *handoff,
	uint32_t enabled_downstream_rate_mask)
{
	struct v90_digital_path initialized = {
		.downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_TRAINING,
		.upstream_stage = V90_DIGITAL_PATH_UPSTREAM_STARTUP,
	};
	const struct v90_pcm_mapping *training;

	if (path == NULL || handoff == NULL ||
	    enabled_downstream_rate_mask == 0 ||
	    (enabled_downstream_rate_mask &
	     ~(uint32_t)V90_JD_RATE_CAPABILITY_MASK) != 0 ||
	    !v90_phase4_digital_transmit_handoff_is_valid(
		    &handoff->transmit) ||
	    !v90_phase4_cp_line_rx_is_valid(&handoff->receive) ||
	    v90_phase4_cp_line_rx_has_failed(&handoff->receive) ||
	    v90_phase4_cp_line_rx_has_pending_record(&handoff->receive))
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	initialized.downstream.training = handoff->transmit.transmitter;
	initialized.enabled_downstream_rate_mask =
		enabled_downstream_rate_mask;
	training = &initialized.downstream.training.encoder.mapping;
	initialized.law = training->law;
	if (v90_upstream_rx_init(
		    &initialized.upstream.startup, &handoff->receive,
		    training,
		    &initialized.downstream.training.control.mp) !=
	    V90_UPSTREAM_RX_OK)
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	if (!v90_digital_path_is_valid(&initialized))
		return V90_DIGITAL_PATH_INTERNAL_ERROR;
	*path = initialized;
	return V90_DIGITAL_PATH_OK;
}

bool v90_digital_path_has_failed(const struct v90_digital_path *path)
{
	return v90_digital_path_is_valid(path) && path->failed;
}

bool v90_digital_path_downstream_is_online(
	const struct v90_digital_path *path)
{
	return v90_digital_path_is_valid(path) && !path->failed &&
	       path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_DATA &&
	       v90_downstream_data_stream_is_online(&path->downstream.data);
}

bool v90_digital_path_upstream_is_online(
	const struct v90_digital_path *path)
{
	return v90_digital_path_is_valid(path) && !path->failed &&
	       path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA;
}

bool v90_digital_path_is_online(const struct v90_digital_path *path)
{
	return v90_digital_path_downstream_is_online(path) &&
	       v90_digital_path_upstream_is_online(path);
}

static enum v90_digital_path_result stream_result(
	enum v90_downstream_data_stream_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_DATA_STREAM_OK:
		return V90_DIGITAL_PATH_OK;
	case V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE:
		return V90_DIGITAL_PATH_SOURCE_FAILURE;
	case V90_DOWNSTREAM_DATA_STREAM_INVALID_PARAMETERS:
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	case V90_DOWNSTREAM_DATA_STREAM_OUT_OF_RANGE:
		return V90_DIGITAL_PATH_OUT_OF_RANGE;
	case V90_DOWNSTREAM_DATA_STREAM_NUMERIC_FAILURE:
		return V90_DIGITAL_PATH_NUMERIC_FAILURE;
	case V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT:
	case V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE:
	case V90_DOWNSTREAM_DATA_STREAM_INTERNAL_ERROR:
		break;
	}
	return V90_DIGITAL_PATH_INTERNAL_ERROR;
}

static enum v90_digital_path_result emit_downstream(
	struct v90_digital_path *path,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *octet)
{
	if (path->downstream_stage ==
	    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING) {
		struct pcm_downstream_tx training = path->downstream.training;
		struct pcm_downstream_symbol symbol;
		enum pcm_downstream_tx_result result =
			pcm_downstream_tx_next(&training, &symbol);

		if (result == PCM_DOWNSTREAM_TX_OK) {
			path->downstream.training = training;
			*octet = symbol.pcm.octet;
			return V90_DIGITAL_PATH_OK;
		}
		if (result != PCM_DOWNSTREAM_TX_COMPLETE)
			return result == PCM_DOWNSTREAM_TX_OUT_OF_RANGE ?
				       V90_DIGITAL_PATH_OUT_OF_RANGE :
			       result == PCM_DOWNSTREAM_TX_NUMERIC_FAILURE ?
				       V90_DIGITAL_PATH_NUMERIC_FAILURE :
				       V90_DIGITAL_PATH_INTERNAL_ERROR;
		if (!path->data_mapping_ready)
			return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
		{
			struct v90_downstream_data_stream data;
			struct v90_pcm_symbol pcm;
			enum v90_downstream_data_stream_result data_result =
				v90_downstream_data_stream_start_after_ed(
					&data, &training,
					&path->data_mapping);

			if (data_result == V90_DOWNSTREAM_DATA_STREAM_OK)
				data_result = v90_downstream_data_stream_next(
					&data, source, source_context, &pcm);
			if (data_result != V90_DOWNSTREAM_DATA_STREAM_OK)
				return stream_result(data_result);
			path->downstream =
				(union v90_digital_path_downstream){ 0 };
			path->downstream.data = data;
			path->downstream_stage =
				V90_DIGITAL_PATH_DOWNSTREAM_DATA;
			*octet = pcm.octet;
			return V90_DIGITAL_PATH_OK;
		}
	}
	{
		struct v90_pcm_symbol pcm;
		enum v90_downstream_data_stream_result result =
			v90_downstream_data_stream_next(
				&path->downstream.data, source,
				source_context, &pcm);

		if (result != V90_DOWNSTREAM_DATA_STREAM_OK)
			return stream_result(result);
		*octet = pcm.octet;
		return V90_DIGITAL_PATH_OK;
	}
}

static enum v90_digital_path_result request_mp_prime(
	struct v90_digital_path *path,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping mapping;

	if (path->downstream_stage !=
		    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING ||
	    path->data_mapping_ready)
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	if (v90_pcm_mapping_init(&mapping, parameters) !=
	    V90_PCM_MAPPING_OK)
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	if ((path->enabled_downstream_rate_mask &
	     (UINT32_C(1) << (parameters->selected_rate_index - 1u))) == 0)
		return V90_DIGITAL_PATH_NO_COMMON_RATE;
	/* The upstream owner has already committed the accepted CP. Publish its
	 * derived mapping before making the fallible downstream request so even
	 * an internal failure leaves one valid, diagnosable owner. */
	path->data_mapping = mapping;
	path->data_mapping_ready = true;
	if (pcm_downstream_tx_request_mp_prime(
		    &path->downstream.training) != PCM_DOWNSTREAM_TX_OK)
		return V90_DIGITAL_PATH_INTERNAL_ERROR;
	return V90_DIGITAL_PATH_OK;
}

static enum v90_digital_path_result request_ed(
	struct v90_digital_path *path,
	const struct v90_cp_parameters *parameters)
{
	if (path->downstream_stage !=
		    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING ||
	    !path->data_mapping_ready ||
	    !data_mapping_matches(&path->data_mapping, parameters))
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	return pcm_downstream_tx_request_ed(
		       &path->downstream.training) == PCM_DOWNSTREAM_TX_OK ?
		       V90_DIGITAL_PATH_OK : V90_DIGITAL_PATH_INTERNAL_ERROR;
}

static enum v90_digital_path_result startup_result(
	struct v90_digital_path *path,
	enum v90_upstream_rx_result result,
	const struct v90_cp_parameters *event)
{
	switch (result) {
	case V90_UPSTREAM_RX_OK:
	case V90_UPSTREAM_RX_E_COMPLETE:
		return V90_DIGITAL_PATH_OK;
	case V90_UPSTREAM_RX_REQUEST_MP_PRIME:
		return request_mp_prime(path, event);
	case V90_UPSTREAM_RX_REQUEST_ED:
		return request_ed(path, event);
	case V90_UPSTREAM_RX_B1_COMPLETE: {
		struct v34_data_line_receiver data;

		if (v90_upstream_rx_handoff(
			    &path->upstream.startup, &data) !=
		    V90_UPSTREAM_RX_OK)
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		path->upstream = (union v90_digital_path_upstream){ 0 };
		path->upstream.data = data;
		path->upstream_stage = V90_DIGITAL_PATH_UPSTREAM_DATA;
		return V90_DIGITAL_PATH_OK;
	}
	case V90_UPSTREAM_RX_FAILED:
		return V90_DIGITAL_PATH_FAILED;
	case V90_UPSTREAM_RX_SEQUENCE_MISMATCH:
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	case V90_UPSTREAM_RX_INVALID_PARAMETERS:
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	case V90_UPSTREAM_RX_NO_COMMON_RATE:
		return V90_DIGITAL_PATH_NO_COMMON_RATE;
	case V90_UPSTREAM_RX_OUT_OF_RANGE:
		return V90_DIGITAL_PATH_OUT_OF_RANGE;
	case V90_UPSTREAM_RX_NUMERIC_FAILURE:
		return V90_DIGITAL_PATH_NUMERIC_FAILURE;
	case V90_UPSTREAM_RX_COMPLETE:
	case V90_UPSTREAM_RX_INVALID_ARGUMENT:
	case V90_UPSTREAM_RX_INVALID_STATE:
		break;
	}
	return V90_DIGITAL_PATH_INTERNAL_ERROR;
}

static enum v90_digital_path_result receive_upstream(
	struct v90_digital_path *path, int16_t sample,
	struct v34_received_mapping_frame *received,
	bool *frame_ready)
{
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP) {
		struct v90_cp_parameters event = { 0 };
		enum v90_upstream_rx_result result = v90_upstream_rx_sample(
			&path->upstream.startup, sample, &event);

		return startup_result(path, result, &event);
	}
	{
		struct v34_survivor_decision current = { 0 };
		struct v34_received_mapping_frame frame = { 0 };
		enum v34_data_line_receiver_result result =
			v34_data_line_receiver_sample(
				&path->upstream.data, sample,
				&current, &frame);

		switch (result) {
		case V34_DATA_LINE_RECEIVER_OK:
		case V34_DATA_LINE_RECEIVER_INTERVAL_READY:
			return V90_DIGITAL_PATH_OK;
		case V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY:
			*received = frame;
			*frame_ready = true;
			return V90_DIGITAL_PATH_UPSTREAM_FRAME;
		case V34_DATA_LINE_RECEIVER_FAILED:
			return V90_DIGITAL_PATH_FAILED;
		case V34_DATA_LINE_RECEIVER_OUT_OF_RANGE:
			return V90_DIGITAL_PATH_OUT_OF_RANGE;
		case V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE:
			return V90_DIGITAL_PATH_NUMERIC_FAILURE;
		case V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT:
		case V34_DATA_LINE_RECEIVER_B1_IN_PROGRESS:
		case V34_DATA_LINE_RECEIVER_B1_COMPLETE:
			break;
		}
	}
	return V90_DIGITAL_PATH_INTERNAL_ERROR;
}

static bool outputs_overlap(
	const struct v90_digital_path *path,
	const uint8_t *transmit_octet,
	const struct v34_received_mapping_frame *received)
{
	return memory_regions_overlap(
		       path, sizeof(*path),
		       transmit_octet, sizeof(*transmit_octet)) ||
	       memory_regions_overlap(
		       path, sizeof(*path),
		       received, sizeof(*received)) ||
	       memory_regions_overlap(
		       transmit_octet, sizeof(*transmit_octet),
		       received, sizeof(*received));
}

static int16_t decode_octet(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static uint8_t silence_octet(enum g711_law law)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(0) : g711_encode_pcmu(0);
}

enum v90_digital_path_result v90_digital_path_octet(
	struct v90_digital_path *path, uint8_t received_octet,
	v90_downstream_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received)
{
	struct v34_received_mapping_frame frame = { 0 };
	uint8_t generated;
	bool frame_ready = false;
	enum v90_digital_path_result result;

	if (!v90_digital_path_is_valid(path) || transmit_octet == NULL ||
	    received == NULL || outputs_overlap(path, transmit_octet, received))
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_DIGITAL_PATH_FAILED;
	generated = silence_octet(path->law);
	result = emit_downstream(
		path, source, source_context, &generated);
	if (result == V90_DIGITAL_PATH_SOURCE_FAILURE)
		return result;
	if (result == V90_DIGITAL_PATH_OK)
		result = receive_upstream(
			path, decode_octet(path->law, received_octet),
			&frame, &frame_ready);
	if (result != V90_DIGITAL_PATH_OK &&
	    result != V90_DIGITAL_PATH_UPSTREAM_FRAME)
		path->failed = true;
	if (!v90_digital_path_is_valid(path)) {
		path->failed = true;
		result = V90_DIGITAL_PATH_INTERNAL_ERROR;
	}
	*transmit_octet = generated;
	if (frame_ready)
		*received = frame;
	return result;
}

const char *v90_digital_path_result_name(
	enum v90_digital_path_result result)
{
	switch (result) {
	case V90_DIGITAL_PATH_OK:
		return "ok";
	case V90_DIGITAL_PATH_UPSTREAM_FRAME:
		return "upstream frame";
	case V90_DIGITAL_PATH_SOURCE_FAILURE:
		return "frame source failure";
	case V90_DIGITAL_PATH_FAILED:
		return "failed";
	case V90_DIGITAL_PATH_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_DIGITAL_PATH_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DIGITAL_PATH_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_DIGITAL_PATH_NO_COMMON_RATE:
		return "no common rate";
	case V90_DIGITAL_PATH_OUT_OF_RANGE:
		return "out of range";
	case V90_DIGITAL_PATH_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_DIGITAL_PATH_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
