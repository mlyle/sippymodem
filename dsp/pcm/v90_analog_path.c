// SPDX-License-Identifier: BSD-3-Clause

#include "v90_analog_path.h"

#include "g711.h"
#include "memory_region.h"
#include "v90_downstream_data_handoff.h"
#include "v90_upstream_data.h"

#include <stddef.h>
#include <stdint.h>

static bool mapping_matches(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init(&expected, parameters) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(mapping, &expected);
}

static bool handoff_is_valid(
	const struct v90_phase4_analog_handoff *handoff,
	enum g711_law law)
{
	struct v90_cp_parameters training;
	struct v90_pcm_mapping expected;
	const struct v90_phase4_cp_tx *symbols;

	if (handoff == NULL ||
	    !v90_phase4_cp_line_tx_is_handoff_ready(&handoff->transmit) ||
	    !pcm_downstream_rx_is_valid(&handoff->receive) ||
	    handoff->receive.stage != PCM_DOWNSTREAM_RX_TRN2D)
		return false;
	symbols = &handoff->transmit.symbols;
	return v90_cp_decode(
		       symbols->frame, sizeof(symbols->frame),
		       symbols->frame_bit_count, &training) == V90_CP_OK &&
	       training.kind == V90_CP_TRAINING && training.law == law &&
	       v90_pcm_mapping_init(&expected, &training) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(
		       &handoff->receive.stream.decoder.mapping, &expected);
}

static bool data_parameters_are_valid(
	const struct v90_cp_parameters *parameters)
{
	return v90_cp_parameters_are_valid(parameters) &&
	       parameters->kind == V90_CP_DATA &&
	       parameters->selected_rate_index != 0 &&
	       !parameters->request_silence;
}

static bool upstream_is_valid(const struct v90_analog_path *path)
{
	if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_CP)
		return v90_phase4_cp_line_tx_is_valid(&path->upstream.cp);
	if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_E)
		return path->finishing_cp && path->cp_prime_sent &&
		       path->peer_mp_prime_received &&
		       v90_phase4_e_line_tx_is_valid(&path->upstream.e) &&
		       path->upstream.e.cp.law == path->law &&
		       mapping_matches(
			       &path->data_mapping, &path->upstream.e.cp);
	if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_DATA)
		return path->finishing_cp && path->cp_prime_sent &&
		       path->peer_mp_prime_received &&
		       v34_data_stream_is_valid(&path->upstream.data);
	return false;
}

static bool downstream_is_valid(const struct v90_analog_path *path)
{
	if (path->downstream_stage ==
	    V90_ANALOG_PATH_DOWNSTREAM_TRAINING)
		return pcm_downstream_rx_is_valid(&path->downstream.training) &&
		       path->downstream.training.stream.decoder.mapping.law ==
			       path->law &&
		       (path->failed ||
			!pcm_downstream_rx_is_complete(
				&path->downstream.training));
	if (path->downstream_stage == V90_ANALOG_PATH_DOWNSTREAM_DATA)
		return path->ed_armed &&
		       v90_downstream_data_rx_is_valid(&path->downstream.data) &&
		       v90_pcm_mapping_equal(
			       &path->downstream.data.stream.decoder.mapping,
			       &path->data_mapping);
	return false;
}

bool v90_analog_path_is_valid(const struct v90_analog_path *path)
{
	if (path == NULL || !g711_law_is_valid(path->law) ||
	    !data_parameters_are_valid(&path->data_parameters) ||
	    path->data_parameters.law != path->law ||
	    !v90_pcm_mapping_is_valid(&path->data_mapping) ||
	    path->data_mapping.kind != V90_CP_DATA ||
	    path->data_mapping.law != path->law ||
	    !mapping_matches(&path->data_mapping, &path->data_parameters) ||
	    (path->data_parameters.acknowledges_peer &&
	     !path->peer_mp_received) ||
	    (path->peer_mp_received &&
	     !v90_mp_record_is_valid(&path->peer_mp)) ||
	    (path->peer_mp_prime_received && !path->peer_mp_received) ||
	    (path->cp_prime_sent &&
	     !path->data_parameters.acknowledges_peer) ||
	    (path->finishing_cp &&
	     (!path->cp_prime_sent || !path->peer_mp_prime_received)) ||
	    (path->ed_armed &&
	     (!path->cp_prime_sent || !path->peer_mp_prime_received)) ||
	    (path->failed && path->receive_pending))
		return false;
	return upstream_is_valid(path) && downstream_is_valid(path);
}

enum v90_analog_path_result v90_analog_path_init(
	struct v90_analog_path *path,
	const struct v90_phase4_analog_handoff *handoff,
	const struct v90_cp_parameters *data_parameters)
{
	struct v90_analog_path initialized = {
		.upstream_stage = V90_ANALOG_PATH_UPSTREAM_CP,
		.downstream_stage = V90_ANALOG_PATH_DOWNSTREAM_TRAINING,
	};

	if (path == NULL || handoff == NULL || data_parameters == NULL)
		return V90_ANALOG_PATH_INVALID_ARGUMENT;
	if (!data_parameters_are_valid(data_parameters) ||
	    data_parameters->acknowledges_peer)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (!handoff_is_valid(handoff, data_parameters->law))
		return V90_ANALOG_PATH_INVALID_ARGUMENT;
	initialized.law = data_parameters->law;
	initialized.data_parameters = *data_parameters;
	if (v90_pcm_mapping_init(
		    &initialized.data_mapping, data_parameters) !=
	    V90_PCM_MAPPING_OK)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	initialized.upstream.cp = handoff->transmit;
	initialized.downstream.training = handoff->receive;
	if (v90_phase4_cp_line_tx_start_next_record(
		    &initialized.upstream.cp, data_parameters) !=
	    V90_PHASE4_CP_LINE_TX_OK)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (!v90_analog_path_is_valid(&initialized))
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	*path = initialized;
	return V90_ANALOG_PATH_OK;
}

bool v90_analog_path_has_failed(const struct v90_analog_path *path)
{
	return v90_analog_path_is_valid(path) && path->failed;
}

bool v90_analog_path_upstream_is_online(
	const struct v90_analog_path *path)
{
	return v90_analog_path_is_valid(path) && !path->failed &&
	       path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_DATA &&
	       !v34_data_stream_is_transmitting_b1(&path->upstream.data);
}

bool v90_analog_path_downstream_is_online(
	const struct v90_analog_path *path)
{
	return v90_analog_path_is_valid(path) && !path->failed &&
	       path->downstream_stage == V90_ANALOG_PATH_DOWNSTREAM_DATA &&
	       v90_downstream_data_rx_b1d_complete(&path->downstream.data);
}

bool v90_analog_path_is_online(const struct v90_analog_path *path)
{
	return v90_analog_path_upstream_is_online(path) &&
	       v90_analog_path_downstream_is_online(path);
}

static enum v90_analog_path_result cp_line_failure(
	enum v90_phase4_cp_line_tx_result result)
{
	if (result == V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (result == V90_PHASE4_CP_LINE_TX_OUT_OF_RANGE)
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	if (result == V90_PHASE4_CP_LINE_TX_CLIPPING_RISK)
		return V90_ANALOG_PATH_CLIPPING_RISK;
	return V90_ANALOG_PATH_INTERNAL_ERROR;
}

static enum v90_analog_path_result prepare_cp_record(
	struct v90_analog_path *path)
{
	struct v90_cp_parameters parameters;

	if (!v90_phase4_cp_line_tx_record_is_complete(&path->upstream.cp) ||
	    path->finishing_cp)
		return V90_ANALOG_PATH_OK;
	if (path->data_parameters.acknowledges_peer)
		path->cp_prime_sent = true;
	if (path->cp_prime_sent && path->peer_mp_prime_received) {
		path->finishing_cp = true;
		return V90_ANALOG_PATH_OK;
	}
	parameters = path->data_parameters;
	parameters.acknowledges_peer = path->peer_mp_received;
	if (v90_phase4_cp_line_tx_start_next_record(
		    &path->upstream.cp, &parameters) !=
	    V90_PHASE4_CP_LINE_TX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->data_parameters = parameters;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result emit_cp(
	struct v90_analog_path *path, int16_t *sample, bool *ready)
{
	struct v90_phase4_e_line_tx e;
	enum v90_phase4_cp_line_tx_result result;
	enum v90_analog_path_result prepared = prepare_cp_record(path);

	if (prepared != V90_ANALOG_PATH_OK)
		return prepared;
	result = v90_phase4_cp_line_tx_sample(&path->upstream.cp, sample);
	if (result == V90_PHASE4_CP_LINE_TX_OK) {
		*ready = true;
		return V90_ANALOG_PATH_OK;
	}
	if (result != V90_PHASE4_CP_LINE_TX_COMPLETE)
		return cp_line_failure(result);
	if (!path->finishing_cp)
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	if (v90_phase4_e_line_tx_init(&e, &path->upstream.cp) !=
	    V90_PHASE4_E_LINE_TX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->upstream = (union v90_analog_path_upstream){ 0 };
	path->upstream.e = e;
	path->upstream_stage = V90_ANALOG_PATH_UPSTREAM_E;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result e_line_failure(
	enum v90_phase4_e_line_tx_result result)
{
	if (result == V90_PHASE4_E_LINE_TX_OUT_OF_RANGE)
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	if (result == V90_PHASE4_E_LINE_TX_CLIPPING_RISK)
		return V90_ANALOG_PATH_CLIPPING_RISK;
	return V90_ANALOG_PATH_INTERNAL_ERROR;
}

static enum v90_analog_path_result upstream_start_result(
	enum v90_upstream_data_result result)
{
	if (result == V90_UPSTREAM_DATA_OK)
		return V90_ANALOG_PATH_OK;
	if (result == V90_UPSTREAM_DATA_INVALID_PARAMETERS)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (result == V90_UPSTREAM_DATA_NO_COMMON_RATE)
		return V90_ANALOG_PATH_NO_COMMON_RATE;
	if (result == V90_UPSTREAM_DATA_OUT_OF_RANGE)
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	if (result == V90_UPSTREAM_DATA_CLIPPING_RISK)
		return V90_ANALOG_PATH_CLIPPING_RISK;
	if (result == V90_UPSTREAM_DATA_NUMERIC_FAILURE)
		return V90_ANALOG_PATH_NUMERIC_FAILURE;
	return V90_ANALOG_PATH_INTERNAL_ERROR;
}

static enum v90_analog_path_result emit_e(
	struct v90_analog_path *path, int16_t *sample, bool *ready)
{
	struct v34_data_stream data;
	enum v90_phase4_e_line_tx_result result =
		v90_phase4_e_line_tx_sample(&path->upstream.e, sample);

	if (result == V90_PHASE4_E_LINE_TX_OK) {
		*ready = true;
		return V90_ANALOG_PATH_OK;
	}
	if (result != V90_PHASE4_E_LINE_TX_COMPLETE)
		return e_line_failure(result);
	{
		enum v90_analog_path_result start = upstream_start_result(
			v90_upstream_data_start_after_e(
				&data, &path->upstream.e, &path->peer_mp));

		if (start != V90_ANALOG_PATH_OK)
			return start;
	}
	path->upstream = (union v90_analog_path_upstream){ 0 };
	path->upstream.data = data;
	path->upstream_stage = V90_ANALOG_PATH_UPSTREAM_DATA;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result data_stream_result(
	enum v34_data_stream_result result)
{
	if (result == V34_DATA_STREAM_OK)
		return V90_ANALOG_PATH_OK;
	if (result == V34_DATA_STREAM_SOURCE_FAILURE)
		return V90_ANALOG_PATH_SOURCE_FAILURE;
	if (result == V34_DATA_STREAM_OUT_OF_RANGE)
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	if (result == V34_DATA_STREAM_CLIPPING_RISK)
		return V90_ANALOG_PATH_CLIPPING_RISK;
	if (result == V34_DATA_STREAM_NUMERIC_FAILURE)
		return V90_ANALOG_PATH_NUMERIC_FAILURE;
	return V90_ANALOG_PATH_INTERNAL_ERROR;
}

static enum v90_analog_path_result emit_upstream(
	struct v90_analog_path *path,
	v34_mapping_frame_source source, void *source_context,
	int16_t *sample)
{
	for (;;) {
		bool ready = false;
		enum v90_analog_path_result result;

		if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_CP)
			result = emit_cp(path, sample, &ready);
		else if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_E)
			result = emit_e(path, sample, &ready);
		else {
			bool accepted;

			return data_stream_result(v34_data_stream_sample(
				&path->upstream.data, source, source_context,
				sample, &accepted));
		}
		if (result != V90_ANALOG_PATH_OK || ready)
			return result;
	}
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) : g711_encode_pcmu(sample);
}

static bool output_overlaps_path(
	const struct v90_analog_path *path,
	const void *output, size_t output_size)
{
	return memory_regions_overlap(
		path, sizeof(*path), output, output_size);
}

static enum v90_analog_path_result fail_path(
	struct v90_analog_path *path,
	enum v90_analog_path_result result)
{
	path->receive_pending = false;
	path->failed = true;
	return result;
}

enum v90_analog_path_result v90_analog_path_transmit(
	struct v90_analog_path *path,
	v34_mapping_frame_source source, void *source_context,
	uint8_t *transmit_octet)
{
	int16_t sample;
	enum v90_analog_path_result result;

	if (!v90_analog_path_is_valid(path) || transmit_octet == NULL ||
	    output_overlaps_path(path, transmit_octet, sizeof(*transmit_octet)))
		return V90_ANALOG_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_ANALOG_PATH_FAILED;
	if (path->receive_pending)
		return V90_ANALOG_PATH_INVALID_STATE;
	result = emit_upstream(path, source, source_context, &sample);
	if (result == V90_ANALOG_PATH_SOURCE_FAILURE)
		return result;
	if (result != V90_ANALOG_PATH_OK)
		return fail_path(path, result);
	path->receive_pending = true;
	if (!v90_analog_path_is_valid(path))
		return fail_path(path, V90_ANALOG_PATH_INTERNAL_ERROR);
	*transmit_octet = encode_sample(path->law, sample);
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result accept_mp(
	struct v90_analog_path *path,
	const struct v90_mp_record *record, bool prime)
{
	if (prime) {
		if (!path->peer_mp_received ||
		    !v90_mp_record_acknowledges_cp(record) ||
		    !v90_mp_record_parameters_equal(&path->peer_mp, record))
			return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
		path->peer_mp_prime_received = true;
		return V90_ANALOG_PATH_OK;
	}
	if (v90_mp_record_acknowledges_cp(record) ||
	    path->peer_mp_prime_received)
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	if (path->peer_mp_received)
		return v90_mp_record_equal(&path->peer_mp, record) ?
			       V90_ANALOG_PATH_OK :
			       V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	path->peer_mp = *record;
	path->peer_mp_received = true;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result start_downstream_data(
	struct v90_analog_path *path)
{
	struct v90_downstream_data_rx data;
	enum v90_downstream_data_rx_result result;

	if (!path->ed_armed)
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	result = v90_downstream_data_rx_start_after_ed(
		&data, &path->downstream.training, &path->data_mapping);
	if (result == V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (result != V90_DOWNSTREAM_DATA_RX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->downstream = (union v90_analog_path_downstream){ 0 };
	path->downstream.data = data;
	path->downstream_stage = V90_ANALOG_PATH_DOWNSTREAM_DATA;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result receive_training(
	struct v90_analog_path *path, uint8_t octet)
{
	struct pcm_downstream_event event;
	enum pcm_downstream_rx_result result =
		pcm_downstream_rx_receive(
			&path->downstream.training, octet, &event);

	switch (result) {
	case PCM_DOWNSTREAM_RX_OK:
	case PCM_DOWNSTREAM_RX_BUFFERING:
	case PCM_DOWNSTREAM_RX_TRN2D_FRAME:
		return V90_ANALOG_PATH_OK;
	case PCM_DOWNSTREAM_RX_MP:
		return accept_mp(path, &event.mp.record, false);
	case PCM_DOWNSTREAM_RX_MP_PRIME:
		return accept_mp(path, &event.mp.record, true);
	case PCM_DOWNSTREAM_RX_ED:
		return start_downstream_data(path);
	case PCM_DOWNSTREAM_RX_MP_REJECTED:
	case PCM_DOWNSTREAM_RX_ED_REJECTED:
	case PCM_DOWNSTREAM_RX_INVALID_SYMBOL:
		return V90_ANALOG_PATH_FAILED;
	case PCM_DOWNSTREAM_RX_INVALID_PARAMETERS:
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	case PCM_DOWNSTREAM_RX_OUT_OF_RANGE:
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	case PCM_DOWNSTREAM_RX_SUVD:
	case PCM_DOWNSTREAM_RX_SUVD_REJECTED:
	case PCM_DOWNSTREAM_RX_CPD:
	case PCM_DOWNSTREAM_RX_CPD_REJECTED:
	case PCM_DOWNSTREAM_RX_INVALID_ARGUMENT:
	case PCM_DOWNSTREAM_RX_INVALID_STATE:
	case PCM_DOWNSTREAM_RX_INTERNAL_ERROR:
		break;
	}
	return V90_ANALOG_PATH_INTERNAL_ERROR;
}

static enum v90_analog_path_result receive_data(
	struct v90_analog_path *path, uint8_t octet,
	struct v90_pcm_stream_frame *frame, bool *frame_ready)
{
	struct v90_pcm_stream_frame decoded;
	enum v90_downstream_data_rx_result result =
		v90_downstream_data_rx_receive_octet(
			&path->downstream.data, octet, &decoded);

	switch (result) {
	case V90_DOWNSTREAM_DATA_RX_BUFFERING:
	case V90_DOWNSTREAM_DATA_RX_B1D_FRAME:
	case V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE:
		return V90_ANALOG_PATH_OK;
	case V90_DOWNSTREAM_DATA_RX_DATA_FRAME:
		*frame = decoded;
		*frame_ready = true;
		return V90_ANALOG_PATH_DOWNSTREAM_FRAME;
	case V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL:
		return V90_ANALOG_PATH_FAILED;
	case V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS:
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	case V90_DOWNSTREAM_DATA_RX_OUT_OF_RANGE:
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	case V90_DOWNSTREAM_DATA_RX_OK:
	case V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT:
	case V90_DOWNSTREAM_DATA_RX_INVALID_STATE:
	case V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR:
		break;
	}
	return V90_ANALOG_PATH_INTERNAL_ERROR;
}

static enum v90_analog_path_result arm_ed(struct v90_analog_path *path)
{
	if (path->ed_armed || !path->cp_prime_sent ||
	    !path->peer_mp_prime_received)
		return V90_ANALOG_PATH_OK;
	if (path->downstream_stage !=
	    V90_ANALOG_PATH_DOWNSTREAM_TRAINING)
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	if (pcm_downstream_rx_expect_ed(&path->downstream.training) !=
	    PCM_DOWNSTREAM_RX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->ed_armed = true;
	return V90_ANALOG_PATH_OK;
}

enum v90_analog_path_result v90_analog_path_receive(
	struct v90_analog_path *path, uint8_t received_octet,
	struct v90_pcm_stream_frame *received)
{
	struct v90_pcm_stream_frame frame;
	bool frame_ready = false;
	enum v90_analog_path_result result;

	if (!v90_analog_path_is_valid(path) || received == NULL ||
	    output_overlaps_path(path, received, sizeof(*received)))
		return V90_ANALOG_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_ANALOG_PATH_FAILED;
	if (!path->receive_pending)
		return V90_ANALOG_PATH_INVALID_STATE;
	result = path->downstream_stage ==
				 V90_ANALOG_PATH_DOWNSTREAM_TRAINING ?
			 receive_training(path, received_octet) :
			 receive_data(
				 path, received_octet, &frame, &frame_ready);
	if (result == V90_ANALOG_PATH_OK ||
	    result == V90_ANALOG_PATH_DOWNSTREAM_FRAME) {
		enum v90_analog_path_result armed = arm_ed(path);

		if (armed != V90_ANALOG_PATH_OK)
			result = armed;
	}
	path->receive_pending = false;
	if (result != V90_ANALOG_PATH_OK &&
	    result != V90_ANALOG_PATH_DOWNSTREAM_FRAME)
		return fail_path(path, result);
	if (!v90_analog_path_is_valid(path))
		return fail_path(path, V90_ANALOG_PATH_INTERNAL_ERROR);
	if (frame_ready)
		*received = frame;
	return result;
}

const char *v90_analog_path_result_name(
	enum v90_analog_path_result result)
{
	switch (result) {
	case V90_ANALOG_PATH_OK:
		return "ok";
	case V90_ANALOG_PATH_DOWNSTREAM_FRAME:
		return "downstream frame";
	case V90_ANALOG_PATH_SOURCE_FAILURE:
		return "frame source failure";
	case V90_ANALOG_PATH_FAILED:
		return "failed";
	case V90_ANALOG_PATH_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_ANALOG_PATH_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_ANALOG_PATH_INVALID_STATE:
		return "invalid state";
	case V90_ANALOG_PATH_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_ANALOG_PATH_NO_COMMON_RATE:
		return "no common rate";
	case V90_ANALOG_PATH_OUT_OF_RANGE:
		return "out of range";
	case V90_ANALOG_PATH_CLIPPING_RISK:
		return "clipping risk";
	case V90_ANALOG_PATH_NUMERIC_FAILURE:
		return "numeric failure";
	case V90_ANALOG_PATH_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
