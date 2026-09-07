// SPDX-License-Identifier: BSD-3-Clause

#include "v90_analog_path.h"

#include "g711.h"
#include "memory_region.h"
#include "v90_downstream_data_handoff.h"
#include "v90_upstream_data.h"

#include <stddef.h>
#include <stdint.h>

enum {
	CLEARDOWN_RECORDS = 2,
	/* The slowest admitted upstream is 2400 symbols/s. */
	CLEARDOWN_MAX_TAIL_SAMPLES =
		((V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS + 1u) * 8000u +
		 2399u) / 2400u + V34_TRAINING_LINE_FILTER_TAIL_SAMPLES,
};

static bool mapping_matches(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init(&expected, parameters) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(mapping, &expected);
}

static bool control_mapping_matches(
	const struct v90_analog_path *path)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init_rate_renegotiation(
		       &expected, &path->training_mapping,
		       &path->data_mapping) == V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(&path->control_mapping, &expected);
}

static bool handoff_is_valid(
	const struct v90_phase4_analog_handoff *handoff,
	enum g711_law law)
{
	struct v90_cp_parameters training;
	struct v90_pcm_mapping expected;
	const struct v90_phase4_cp_tx *symbols;

	if (handoff == NULL ||
	    !v34_constellation_size_is_valid(
		    handoff->rate_renegotiation_constellation) ||
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

static bool decision_plane_is_valid(enum v90_pcm_mapping_plane plane)
{
	return plane == V90_PCM_MAPPING_TRANSMIT ||
	       plane == V90_PCM_MAPPING_CODEC;
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
		       path->upstream_rate_2400_bps_units != 0u &&
		       v34_data_stream_is_valid(&path->upstream.data) &&
		       path->upstream.data.points.schedule.geometry
			       .primary_data_rate_2400_bps_units ==
			       path->upstream_rate_2400_bps_units;
	if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_RRN_RETURN)
		return path->rate_renegotiation_active &&
		       v90_phase3_return_line_tx_is_valid(
			       &path->upstream.rrn_return);
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
		return (path->ed_armed || path->rate_renegotiation_active) &&
		       v90_downstream_data_rx_is_valid(&path->downstream.data) &&
		       v90_pcm_mapping_equal(
			       &path->downstream.data.stream.decoder.mapping,
			       &path->data_mapping);
	return false;
}

bool v90_analog_path_is_valid(const struct v90_analog_path *path)
{
	if (path == NULL || !g711_law_is_valid(path->law) ||
	    path->cleardown_records > CLEARDOWN_RECORDS ||
	    path->cleardown_tail_samples > CLEARDOWN_MAX_TAIL_SAMPLES ||
	    (path->cleardown_records != 0u &&
	     path->upstream_stage != V90_ANALOG_PATH_UPSTREAM_CP) ||
	    (path->cleardown_requested && !path->rate_renegotiation_active &&
	     path->upstream_stage != V90_ANALOG_PATH_UPSTREAM_CP) ||
	    (!path->cleardown_requested &&
	     (path->cleardown_records != 0u ||
	      path->cleardown_tail_samples != 0u)) ||
	    !v34_constellation_size_is_valid(
		    path->rate_renegotiation_constellation) ||
	    !decision_plane_is_valid(path->decision_plane) ||
	    !v90_pcm_mapping_is_valid(&path->training_mapping) ||
	    path->training_mapping.kind != V90_CP_TRAINING ||
	    path->training_mapping.law != path->law ||
	    !v90_pcm_mapping_is_valid(&path->control_mapping) ||
	    path->control_mapping.kind != V90_CP_TRAINING ||
	    path->control_mapping.law != path->law ||
	    !control_mapping_matches(path) ||
	    !data_parameters_are_valid(&path->data_parameters) ||
	    path->data_parameters.law != path->law ||
	    !v90_pcm_mapping_is_valid(&path->data_mapping) ||
	    path->data_mapping.kind != V90_CP_DATA ||
	    path->data_mapping.law != path->law ||
	    !mapping_matches(&path->data_mapping, &path->data_parameters) ||
	    !v90_phase4_cp_tx_is_valid(&path->control_symbols) ||
	    !v90_phase4_cp_tx_record_is_complete(&path->control_symbols) ||
	    !v90_downstream_rrn_rx_is_valid(&path->rate_renegotiation) ||
	    path->rate_renegotiation.decision_plane != path->decision_plane ||
	    !v90_pcm_mapping_equal(
		    &path->rate_renegotiation.mapping,
		    &path->data_mapping) ||
	    (path->rate_renegotiation_active &&
	     path->upstream_rate_2400_bps_units == 0u) ||
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
	struct v90_cp_parameters training_parameters;
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
	if (v90_cp_decode(
		    handoff->transmit.symbols.frame,
		    sizeof(handoff->transmit.symbols.frame),
		    handoff->transmit.symbols.frame_bit_count,
		    &training_parameters) != V90_CP_OK ||
	    v90_pcm_mapping_init(
		    &initialized.training_mapping,
		    &training_parameters) != V90_PCM_MAPPING_OK ||
	    v90_pcm_mapping_init(
		    &initialized.data_mapping, data_parameters) !=
		    V90_PCM_MAPPING_OK)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (v90_pcm_mapping_init_rate_renegotiation(
		    &initialized.control_mapping,
		    &initialized.training_mapping,
		    &initialized.data_mapping) != V90_PCM_MAPPING_OK)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	initialized.control_symbols = handoff->transmit.symbols;
	initialized.rate_renegotiation_constellation =
		handoff->rate_renegotiation_constellation;
	initialized.decision_plane =
		handoff->receive.stream.decoder.decision_plane;
	if (v90_downstream_rrn_rx_init(
		    &initialized.rate_renegotiation,
		    &initialized.data_mapping,
		    initialized.decision_plane) !=
	    V90_DOWNSTREAM_RRN_RX_OK)
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
	return v90_analog_path_is_valid(path) && !path->failed &&
	       (path->rate_renegotiation_active ||
		(v90_analog_path_upstream_is_online(path) &&
		 v90_analog_path_downstream_is_online(path)));
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
	if (path->cleardown_requested) {
		if (path->cleardown_records == CLEARDOWN_RECORDS)
			return V90_ANALOG_PATH_OK;
		/* Cancellation may have been requested in an ordinary CP. Only
		 * count complete zero-rate records, never the preceding data CP. */
		if (v90_cp_decode(path->upstream.cp.symbols.frame,
			    sizeof(path->upstream.cp.symbols.frame),
			    path->upstream.cp.symbols.frame_bit_count,
			    &parameters) != V90_CP_OK)
			return V90_ANALOG_PATH_INTERNAL_ERROR;
		if (parameters.selected_rate_index == 0u)
			path->cleardown_records++;
		if (path->cleardown_records == CLEARDOWN_RECORDS)
			return V90_ANALOG_PATH_OK;
		parameters = path->data_parameters;
		parameters.selected_rate_index = 0u;
		return v90_phase4_cp_line_tx_start_next_record(
			       &path->upstream.cp, &parameters) ==
				       V90_PHASE4_CP_LINE_TX_OK ?
			       V90_ANALOG_PATH_OK : V90_ANALOG_PATH_INTERNAL_ERROR;
	}
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
	if (path->cleardown_requested &&
	    path->cleardown_records == CLEARDOWN_RECORDS) {
		const struct qam_point zero = { 0 };
		struct v34_training_line *line = &path->upstream.cp.line;
		bool accepted;

		if (path->cleardown_tail_samples == 0u)
			return V90_ANALOG_PATH_CLEARDOWN_COMPLETE;
		if (v34_training_line_sample(line,
			    v34_training_line_needs_point(line) ? &zero : NULL,
			    sample, &accepted) != V34_TRAINING_LINE_OK)
			return V90_ANALOG_PATH_INTERNAL_ERROR;
		path->cleardown_tail_samples--;
		*ready = true;
		return V90_ANALOG_PATH_OK;
	}
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
	path->control_symbols = path->upstream.cp.symbols;
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
	path->upstream_rate_2400_bps_units =
		data.points.schedule.geometry.primary_data_rate_2400_bps_units;
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

static void finish_rate_renegotiation_if_online(
	struct v90_analog_path *path)
{
	if (path->rate_renegotiation_active &&
	    path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_DATA &&
	    !v34_data_stream_is_transmitting_b1(&path->upstream.data) &&
	    path->downstream_stage == V90_ANALOG_PATH_DOWNSTREAM_DATA &&
	    v90_downstream_data_rx_b1d_complete(&path->downstream.data))
		path->rate_renegotiation_active = false;
}

static enum v90_analog_path_result emit_rrn_return(
	struct v90_analog_path *path, int16_t *sample, bool *ready)
{
	struct v34_training_line line;
	struct v90_phase4_cp_line_tx cp;
	struct v90_cp_parameters parameters = path->data_parameters;
	enum v90_phase3_return_line_tx_result result =
		v90_phase3_return_line_tx_sample(
			&path->upstream.rrn_return, sample);

	if (result == V90_PHASE3_RETURN_LINE_TX_OK) {
		*ready = true;
		return V90_ANALOG_PATH_OK;
	}
	if (result == V90_PHASE3_RETURN_LINE_TX_OUT_OF_RANGE)
		return V90_ANALOG_PATH_OUT_OF_RANGE;
	if (path->cleardown_requested)
		parameters.selected_rate_index = 0u;
	if (result != V90_PHASE3_RETURN_LINE_TX_COMPLETE ||
	    v90_phase3_return_line_tx_handoff(
		    &path->upstream.rrn_return, &line) !=
		    V90_PHASE3_RETURN_LINE_TX_OK ||
	    v90_phase4_cp_line_tx_resume_online(
		    &cp, &line, &path->control_symbols,
		    path->rate_renegotiation_constellation,
		    &parameters) != V90_PHASE4_CP_LINE_TX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->upstream = (union v90_analog_path_upstream){ 0 };
	path->upstream.cp = cp;
	path->upstream_stage = V90_ANALOG_PATH_UPSTREAM_CP;
	return V90_ANALOG_PATH_OK;
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
		else if (path->upstream_stage ==
			 V90_ANALOG_PATH_UPSTREAM_RRN_RETURN)
			result = emit_rrn_return(path, sample, &ready);
		else {
			bool accepted;
			enum v34_data_stream_result stream_result;

			stream_result = v34_data_stream_sample(
				&path->upstream.data,
				path->rate_renegotiation_active ? NULL : source,
				path->rate_renegotiation_active ? NULL :
					source_context,
				sample, &accepted);
			result = data_stream_result(stream_result);
			if (result == V90_ANALOG_PATH_OK)
				finish_rate_renegotiation_if_online(path);
			return result;
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
	if (result == V90_ANALOG_PATH_CLEARDOWN_COMPLETE) {
		*transmit_octet = encode_sample(path->law, 0);
		return result;
	}
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
	const struct v90_mp_parameters *parameters =
		record->type == V90_MP_RECORD_TYPE0 ?
			&record->parameters.type0 : &record->parameters.type1.common;

	/* A CRC-checked zero-rate MP ends either control epoch, even if its
	 * acknowledgement does not match an ordinary rate negotiation. */
	if (parameters->maximum_upstream_rate_2400_bps_units == 0u)
		return V90_ANALOG_PATH_CLEARDOWN_COMPLETE;
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
	struct v90_downstream_rrn_rx rate_renegotiation;
	enum v90_downstream_data_rx_result result;

	if (!path->ed_armed)
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	result = v90_downstream_data_rx_start_after_ed(
		&data, &path->downstream.training, &path->data_mapping);
	if (result == V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (result != V90_DOWNSTREAM_DATA_RX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	if (v90_downstream_rrn_rx_init(
		    &rate_renegotiation, &path->data_mapping,
		    path->decision_plane) != V90_DOWNSTREAM_RRN_RX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->downstream = (union v90_analog_path_downstream){ 0 };
	path->downstream.data = data;
	path->rate_renegotiation = rate_renegotiation;
	path->downstream_stage = V90_ANALOG_PATH_DOWNSTREAM_DATA;
	finish_rate_renegotiation_if_online(path);
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
		return V90_ANALOG_PATH_OK;
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

static enum v90_analog_path_result begin_rate_renegotiation(
	struct v90_analog_path *path)
{
	struct v90_pcm_mapping control_mapping;
	struct v90_phase3_return_line_tx returning;

	if (path->rate_renegotiation_active ||
	    path->upstream_stage != V90_ANALOG_PATH_UPSTREAM_DATA ||
	    path->downstream_stage != V90_ANALOG_PATH_DOWNSTREAM_DATA ||
	    v34_data_stream_is_transmitting_b1(&path->upstream.data) ||
	    !v90_downstream_data_rx_b1d_complete(&path->downstream.data))
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	if (v90_pcm_mapping_init_rate_renegotiation(
		    &control_mapping, &path->training_mapping,
		    &path->data_mapping) != V90_PCM_MAPPING_OK)
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	if (v90_phase3_return_line_tx_init_rrn(
		    &returning, &path->upstream.data.line) !=
	    V90_PHASE3_RETURN_LINE_TX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;

	path->control_mapping = control_mapping;
	path->upstream = (union v90_analog_path_upstream){ 0 };
	path->upstream.rrn_return = returning;
	path->upstream_stage = V90_ANALOG_PATH_UPSTREAM_RRN_RETURN;
	path->data_parameters.acknowledges_peer = false;
	path->peer_mp_received = false;
	path->peer_mp_prime_received = false;
	path->cp_prime_sent = false;
	path->finishing_cp = false;
	path->ed_armed = false;
	path->rate_renegotiation_active = true;
	return V90_ANALOG_PATH_RATE_RENEGOTIATION;
}

enum v90_analog_path_result v90_analog_path_start_cleardown(
	struct v90_analog_path *path)
{
	const struct v34_phase3_profile *profile;
	uint32_t tail_samples;
	enum v90_analog_path_result result;

	if (!v90_analog_path_is_valid(path) || path->failed)
		return V90_ANALOG_PATH_INVALID_ARGUMENT;
	if (path->cleardown_requested)
		return V90_ANALOG_PATH_OK;
	if (path->receive_pending)
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	if (path->upstream_stage == V90_ANALOG_PATH_UPSTREAM_CP &&
	    !path->finishing_cp) {
		profile = &path->upstream.cp.line.profile;
	} else {
		if (!v90_analog_path_is_online(path) || path->rate_renegotiation_active)
			return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
		profile = &path->upstream.data.line.profile;
	}
	/* Include the already accepted final CP point, then flush every pulse
	 * history point and the sample-rate pre-emphasis filter. */
	tail_samples = ((V34_TRAINING_LINE_PULSE_TAIL_SYMBOLS + 1u) *
		8000u * profile->symbol_rate_denominator +
		profile->symbol_rate_numerator - 1u) /
		profile->symbol_rate_numerator +
		V34_TRAINING_LINE_FILTER_TAIL_SAMPLES;
	if (path->upstream_stage != V90_ANALOG_PATH_UPSTREAM_CP) {
		result = begin_rate_renegotiation(path);
		if (result != V90_ANALOG_PATH_RATE_RENEGOTIATION)
			return result;
	}
	path->cleardown_requested = true;
	path->cleardown_tail_samples = (uint16_t)tail_samples;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result start_rrn_downstream_training(
	struct v90_analog_path *path)
{
	struct pcm_downstream_rx training;

	if (!path->rate_renegotiation_active ||
	    !v90_downstream_rrn_rx_is_complete(
		    &path->rate_renegotiation))
		return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
	if (pcm_downstream_rx_init(
		    &training, PCM_PROTOCOL_V90,
		    &path->control_mapping, path->decision_plane) !=
	    PCM_DOWNSTREAM_RX_OK)
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	path->downstream = (union v90_analog_path_downstream){ 0 };
	path->downstream.training = training;
	path->downstream_stage = V90_ANALOG_PATH_DOWNSTREAM_TRAINING;
	return V90_ANALOG_PATH_OK;
}

static enum v90_analog_path_result receive_data_frame(
	struct v90_analog_path *path, uint8_t octet,
	struct v90_pcm_stream_frame *frame, bool *frame_ready,
	bool suppress_frame)
{
	struct v90_pcm_stream_frame decoded;
	enum v90_downstream_data_rx_result result =
		v90_downstream_data_rx_receive_octet(
			&path->downstream.data, octet, &decoded);

	switch (result) {
	case V90_DOWNSTREAM_DATA_RX_BUFFERING:
	case V90_DOWNSTREAM_DATA_RX_B1D_FRAME:
	case V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE:
		finish_rate_renegotiation_if_online(path);
		return V90_ANALOG_PATH_OK;
	case V90_DOWNSTREAM_DATA_RX_DATA_FRAME:
		finish_rate_renegotiation_if_online(path);
		if (suppress_frame || path->rate_renegotiation_active)
			return V90_ANALOG_PATH_OK;
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

static enum v90_analog_path_result receive_data(
	struct v90_analog_path *path, uint8_t octet,
	struct v90_pcm_stream_frame *frame, bool *frame_ready)
{
	enum v90_downstream_rrn_rx_result rrn_result;

	if (!v90_downstream_data_rx_b1d_complete(&path->downstream.data))
		return receive_data_frame(
			path, octet, frame, frame_ready,
			path->rate_renegotiation_active);
	if (path->rate_renegotiation_active &&
	    path->rate_renegotiation.stage ==
		    V90_DOWNSTREAM_RRN_RX_SEARCH)
		return receive_data_frame(
			path, octet, frame, frame_ready, false);

	rrn_result = v90_downstream_rrn_rx_receive(
		&path->rate_renegotiation, octet);
	switch (rrn_result) {
	case V90_DOWNSTREAM_RRN_RX_BUFFERING:
	case V90_DOWNSTREAM_RRN_RX_RD_DETECTED:
		break;
	case V90_DOWNSTREAM_RRN_RX_RD_BAR_STARTED:
		return begin_rate_renegotiation(path);
	case V90_DOWNSTREAM_RRN_RX_RD_BAR_COMPLETE:
		return start_rrn_downstream_training(path);
	case V90_DOWNSTREAM_RRN_RX_SEQUENCE_MISMATCH:
		if (path->rate_renegotiation_active)
			return V90_ANALOG_PATH_SEQUENCE_MISMATCH;
		break;
	case V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS:
		return V90_ANALOG_PATH_INVALID_PARAMETERS;
	case V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT:
	case V90_DOWNSTREAM_RRN_RX_INVALID_STATE:
	case V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR:
	case V90_DOWNSTREAM_RRN_RX_OK:
		return V90_ANALOG_PATH_INTERNAL_ERROR;
	}
	if (path->rate_renegotiation_active)
		return V90_ANALOG_PATH_OK;
	return receive_data_frame(
		path, octet, frame, frame_ready,
		v90_downstream_rrn_rx_rd_detected(
			&path->rate_renegotiation));
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
	if (path->cleardown_requested) {
		path->receive_pending = false;
		return V90_ANALOG_PATH_OK;
	}
	result = path->downstream_stage ==
				 V90_ANALOG_PATH_DOWNSTREAM_TRAINING ?
			 receive_training(path, received_octet) :
			 receive_data(
				 path, received_octet, &frame, &frame_ready);
	if (result == V90_ANALOG_PATH_OK ||
	    result == V90_ANALOG_PATH_DOWNSTREAM_FRAME ||
	    result == V90_ANALOG_PATH_RATE_RENEGOTIATION) {
		enum v90_analog_path_result armed = arm_ed(path);

		if (armed != V90_ANALOG_PATH_OK)
			result = armed;
	}
	path->receive_pending = false;
	if (result != V90_ANALOG_PATH_OK &&
	    result != V90_ANALOG_PATH_DOWNSTREAM_FRAME &&
	    result != V90_ANALOG_PATH_CLEARDOWN_COMPLETE &&
	    result != V90_ANALOG_PATH_RATE_RENEGOTIATION)
		return fail_path(path, result);
	if (!v90_analog_path_is_valid(path))
		return fail_path(path, V90_ANALOG_PATH_INTERNAL_ERROR);
	if (frame_ready)
		*received = frame;
	return result;
}

enum v90_analog_path_result v90_analog_path_discard_receive(
	struct v90_analog_path *path)
{
	if (!v90_analog_path_is_valid(path))
		return V90_ANALOG_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_ANALOG_PATH_FAILED;
	if (!path->receive_pending)
		return V90_ANALOG_PATH_INVALID_STATE;
	path->receive_pending = false;
	return v90_analog_path_is_valid(path) ?
		       V90_ANALOG_PATH_OK : V90_ANALOG_PATH_INTERNAL_ERROR;
}

const char *v90_analog_path_result_name(
	enum v90_analog_path_result result)
{
	switch (result) {
	case V90_ANALOG_PATH_OK:
		return "ok";
	case V90_ANALOG_PATH_DOWNSTREAM_FRAME:
		return "downstream frame";
	case V90_ANALOG_PATH_RATE_RENEGOTIATION:
		return "rate renegotiation";
	case V90_ANALOG_PATH_CLEARDOWN_COMPLETE:
		return "cleardown complete";
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
