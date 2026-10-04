// SPDX-License-Identifier: BSD-3-Clause

#include "v90_digital_path.h"

#include "g711.h"
#include "memory_region.h"
#include "pcm_rate.h"
#include "v34_data_quality.h"
#include "v34_survivor.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(
	V90_DIGITAL_PATH_MAPPING_FRAME_SYMBOLS == 8,
	"V.90 upstream control buffering follows the V.34 frame cadence");
_Static_assert(
	V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY >=
		V90_DIGITAL_PATH_CANDIDATE_FRAME_CAPACITY &&
		V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY >=
			V90_DIGITAL_PATH_RRN_FRAME_CAPACITY,
	"V.90 upstream control buffer must cover both bounded epochs");

static bool pending_upstream_is_valid(const struct v90_digital_path *path)
{
	return path->pending_upstream_head <
		       V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY &&
	       path->pending_upstream_count <=
		       V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY &&
	       (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA ||
		path->pending_upstream_count == 0u);
}

static bool downstream_is_valid(const struct v90_digital_path *path)
{
	bool reconditioning =
		path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_SILENCE ||
		path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_RT;

	if (reconditioning &&
	    (!path->rate_renegotiation_active ||
	     !pcm_downstream_tx_is_complete(&path->downstream.training) ||
	     (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_SILENCE ?
		path->reconditioning_symbols >= V90_CP_FRAME_INTERVALS :
		!path->data_mapping_ready ||
		path->reconditioning_symbols > PCM_R_SEQUENCE_SYMBOLS)))
		return false;
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE &&
	    !path->rate_renegotiation_active)
		return false;
	if (!reconditioning && path->reconditioning_symbols != 0u)
		return false;
	if (path->downstream_stage ==
		    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING ||
	    path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE ||
	    reconditioning) {
		const struct pcm_downstream_tx *training =
			&path->downstream.training;

		return pcm_downstream_tx_is_valid(training) &&
		       training->encoder.mapping.kind == V90_CP_TRAINING &&
		       training->encoder.mapping.law == path->law &&
		       v90_pcm_mapping_equal(
			       &training->encoder.mapping,
			       &path->control_mapping) &&
		       (reconditioning || path->failed || path->cleardown_requested ||
			!pcm_downstream_tx_is_complete(training));
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
	if (path->downstream_stage ==
	    V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN) {
		const struct v90_downstream_data_stream *data =
			&path->downstream.data;

		return path->rate_renegotiation_active &&
		       v90_downstream_data_stream_is_valid(data) &&
		       data->transmitter.encoder.mapping.kind == V90_CP_DATA &&
		       data->transmitter.encoder.mapping.law == path->law;
	}
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_RD)
		return path->rate_renegotiation_active &&
		       pcm_r_tx_is_valid(&path->downstream.rd) &&
		       path->downstream.rd.signal == PCM_R_RD &&
		       path->downstream.rd.mapping.kind == V90_CP_DATA &&
		       path->downstream.rd.mapping.law == path->law;
	return false;
}

static bool upstream_is_valid(const struct v90_digital_path *path)
{
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP) {
		const struct v90_upstream_rx *startup = &path->upstream.startup;
		bool mapping_state_is_valid =
			(startup->data_cp_received == path->data_mapping_ready &&
			 (!startup->data_cp_received ||
			  v90_pcm_mapping_matches_transmit(
				  &path->data_mapping,
				  &startup->data_parameters,
				  path->data_mapping.law)));

		return v90_upstream_rx_is_valid(startup) &&
		       startup->training_mapping.law == path->law &&
		       v90_pcm_mapping_equal(
			       &startup->training_mapping,
			       &path->training_mapping) &&
		       v90_mp_record_equal(
			       &startup->local_mp, &path->local_mp) &&
		       startup->enabled_downstream_rate_mask ==
			       path->enabled_downstream_rate_mask &&
		       v90_phase4_cp_rx_equal(&startup->control_symbols,
					  &path->control_symbols) &&
		       mapping_state_is_valid &&
		       (path->rate_renegotiation_active ?
				path->upstream_rate_2400_bps_units != 0u :
				path->upstream_rate_2400_bps_units == 0u) &&
		       (path->failed || !v90_upstream_rx_is_complete(startup));
	}
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_DATA)
		return (path->data_mapping_ready ||
			(path->rate_renegotiation_active &&
			 path->pending_upstream_count == 0u)) &&
		       path->upstream_rate_2400_bps_units != 0u &&
		       v34_data_line_receiver_is_valid(&path->upstream.data) &&
		       path->upstream.data.geometry
			       .primary_data_rate_2400_bps_units ==
			       path->upstream_rate_2400_bps_units &&
		       (path->failed ||
			!v34_data_line_receiver_has_failed(
				&path->upstream.data));
	return false;
}

bool v90_digital_path_is_valid(const struct v90_digital_path *path)
{
	if (path == NULL || !g711_law_is_valid(path->law) ||
	    !v90_pcm_mapping_is_valid(&path->training_mapping) ||
	    path->training_mapping.kind != V90_CP_TRAINING ||
	    path->training_mapping.law != path->law ||
	    !v90_pcm_mapping_is_valid(&path->control_mapping) ||
	    path->control_mapping.kind != V90_CP_TRAINING ||
	    path->control_mapping.law != path->law ||
	    !v90_phase4_cp_rx_is_valid(&path->control_symbols) ||
	    !v90_mp_record_is_initial(&path->local_mp) ||
	    (path->peer_upstream_rate_mask &
	     (uint16_t)~V90_MP_RATE_CAPABILITY_MASK) != 0u ||
	    !pcm_downstream_rate_mask_is_usable(
		    path->enabled_downstream_rate_mask) ||
	    !v34_constellation_size_is_valid(
		    path->rate_renegotiation_constellation) ||
	    !pending_upstream_is_valid(path) ||
	    (path->cleardown_requested &&
	     ((!path->rate_renegotiation_active &&
	       path->downstream_stage != V90_DIGITAL_PATH_DOWNSTREAM_TRAINING) ||
	      path->pending_upstream_count != 0u)) ||
	    (path->rate_renegotiation_active &&
	     path->upstream_stage != V90_DIGITAL_PATH_UPSTREAM_STARTUP &&
	     path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_DATA) ||
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
	uint32_t enabled_downstream_rate_mask,
	enum v34_constellation_size rate_renegotiation_constellation)
{
	struct v90_digital_path initialized = {
		.downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_TRAINING,
		.upstream_stage = V90_DIGITAL_PATH_UPSTREAM_STARTUP,
	};
	const struct v90_pcm_mapping *training;

	if (path == NULL || handoff == NULL ||
	    !pcm_downstream_rate_mask_is_usable(
		    enabled_downstream_rate_mask) ||
	    !v34_constellation_size_is_valid(
		    rate_renegotiation_constellation) ||
	    !v90_phase4_digital_transmit_handoff_is_valid(
		    &handoff->transmit) ||
	    !v90_phase4_cp_line_rx_is_valid(&handoff->receive) ||
	    v90_phase4_cp_line_rx_has_failed(&handoff->receive) ||
	    v90_phase4_cp_line_rx_has_pending_record(&handoff->receive))
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	initialized.downstream.training = handoff->transmit.transmitter;
	training = &initialized.downstream.training.encoder.mapping;
	initialized.law = training->law;
	initialized.training_mapping = *training;
	initialized.control_mapping = *training;
	initialized.control_symbols = handoff->receive.symbols;
	initialized.local_mp =
		initialized.downstream.training.control.mp;
	initialized.enabled_downstream_rate_mask =
		enabled_downstream_rate_mask;
	initialized.peer_upstream_rate_mask = handoff->peer_upstream_rate_mask;
	initialized.rate_renegotiation_constellation =
		rate_renegotiation_constellation;
	if (v90_upstream_rx_init(
		    &initialized.upstream.startup, &handoff->receive,
		    training,
		    &initialized.downstream.training.control.mp,
		    enabled_downstream_rate_mask) !=
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
	return v90_digital_path_is_valid(path) && !path->failed &&
	       (path->rate_renegotiation_active ||
		(v90_digital_path_downstream_is_online(path) &&
		 v90_digital_path_upstream_is_online(path)));
}

enum v90_digital_path_result v90_digital_path_start_cleardown(
	struct v90_digital_path *path)
{
	struct v90_pcm_mapping control;

	if (!v90_digital_path_is_valid(path) || path->failed)
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (path->cleardown_requested)
		return V90_DIGITAL_PATH_OK;
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING &&
	    path->downstream.training.stage != PCM_DOWNSTREAM_TX_ED &&
	    !path->downstream.training.finish_requested) {
		/* CP/MP is already available, either in initial training or a
		 * renegotiation. Finish its current record instead of restarting
		 * the control coder or interrupting the line with another Rd. */
		path->pending_upstream_count = 0u;
		path->pending_upstream_head = 0u;
		path->downstream.training.ed_requested = false;
		path->cleardown_requested = true;
		return V90_DIGITAL_PATH_OK;
	}
	if (!v90_digital_path_is_online(path) ||
	    path->rate_renegotiation_active)
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	if (v90_pcm_mapping_init_rate_renegotiation(
		    &control, &path->training_mapping,
		    &path->data_mapping) != V90_PCM_MAPPING_OK)
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	path->control_mapping = control;
	path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN;
	path->pending_upstream_count = 0u;
	path->pending_upstream_head = 0u;
	path->rate_renegotiation_active = true;
	path->cleardown_requested = true;
	return V90_DIGITAL_PATH_OK;
}

enum v90_digital_path_result v90_digital_path_downshift_upstream(
	struct v90_digital_path *path)
{
	struct v90_pcm_mapping control;
	struct v90_mp_record mp;
	const struct v90_mp_parameters *parameters;
	int rate;

	if (!v90_digital_path_is_valid(path) || path->failed)
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (!v90_digital_path_is_online(path) || path->rate_renegotiation_active ||
	    path->cleardown_requested)
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	mp = path->local_mp;
	parameters = v90_mp_record_common(&mp);
	for (rate = path->upstream_rate_2400_bps_units - 1;
	     rate >= V90_MP_MINIMUM_RATE_2400_BPS_UNITS; --rate) {
		uint16_t bit = (uint16_t)(1u <<
			(rate - V90_MP_MINIMUM_RATE_2400_BPS_UNITS));
		if (rate <= parameters->maximum_upstream_rate_2400_bps_units &&
		    (parameters->upstream_rate_capability_mask &
		     path->peer_upstream_rate_mask & bit) != 0u)
			break;
	}
	if (rate < V90_MP_MINIMUM_RATE_2400_BPS_UNITS)
		return V90_DIGITAL_PATH_NO_COMMON_RATE;
	if (!v90_mp_record_limit_maximum_upstream_rate(&mp, (uint8_t)rate) ||
	    v90_pcm_mapping_init_rate_renegotiation(
		    &control, &path->training_mapping,
		    &path->data_mapping) != V90_PCM_MAPPING_OK)
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	path->local_mp = mp;
	path->control_mapping = control;
	path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN;
	path->pending_upstream_count = 0u;
	path->pending_upstream_head = 0u;
	/* Keep the trained receiver hunting S/S-bar. The old mapping is not
	 * a CP acknowledgement for this exchange, nor is old data host payload. */
	path->data_mapping_ready = false;
	path->rate_renegotiation_active = true;
	return V90_DIGITAL_PATH_OK;
}

static enum v90_digital_path_result stream_result(
	enum v90_downstream_data_stream_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_DATA_STREAM_OK:
		return V90_DIGITAL_PATH_OK;
	case V90_DOWNSTREAM_DATA_STREAM_COMPLETE:
		break;
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

static bool select_initial_receive_rate(struct v90_digital_path *path)
{
	struct pcm_downstream_tx *transmit = &path->downstream.training;
	struct v90_upstream_rx *receive = &path->upstream.startup;
	struct v90_mp_record selected = path->local_mp;
	const struct v90_mp_parameters *parameters =
		selected.type == V90_MP_RECORD_TYPE0 ?
			&selected.parameters.type0 :
			&selected.parameters.type1.common;
	uint16_t common_mask = (uint16_t)(
		parameters->upstream_rate_capability_mask &
		path->peer_upstream_rate_mask);
	uint8_t maximum = parameters->maximum_upstream_rate_2400_bps_units;
	uint8_t rate;

	/* Wait until the full conditioning interval has entered the coder.
	 * After the next frame MP is immutable, including spectral look-ahead.
	 * Online renegotiation retains its preceding MP and receive geometry. */
	if (path->rate_renegotiation_active ||
	    !pcm_downstream_tx_mp_starts_next_frame(transmit) ||
	    path->upstream_stage != V90_DIGITAL_PATH_UPSTREAM_STARTUP ||
	    receive->stage != V90_UPSTREAM_RX_STAGE_CP || maximum == 0u ||
	    common_mask == 0u)
		return true;
	/* V.90's mask starts at 4800; the shared QAM quality policy starts
	 * at 2400. Do not invent a rate if there is no usable intersection. */
	if (v34_data_quality_select_receive_rate(
		    receive->state.cp.transmitting_profile.symbol_rate,
		    (uint16_t)(common_mask << 1u), maximum,
		    &receive->state.cp.line.equalizer.quality, &rate) !=
	    V34_DATA_QUALITY_OK)
		return true;
	if (!v90_mp_record_limit_maximum_upstream_rate(&selected, rate) ||
	    pcm_downstream_tx_start_mp(transmit, &selected) !=
		    PCM_DOWNSTREAM_TX_OK)
		return false;
	path->local_mp = selected;
	receive->local_mp = selected;
	return true;
}

static enum v90_digital_path_result emit_downstream(
	struct v90_digital_path *path,
	pcm_frame_source source, void *source_context,
	uint8_t *octet)
{
	if (path->downstream_stage ==
		    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING ||
	    path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE) {
		struct pcm_downstream_tx training;
		struct pcm_downstream_symbol symbol;
		enum pcm_downstream_tx_result result;

		if (!path->cleardown_requested && !select_initial_receive_rate(path))
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		training = path->downstream.training;
		if (path->cleardown_requested && training.control_bit_index == 0u &&
		    !training.finish_requested) {
			struct v90_mp_record zero = path->local_mp;
			struct v90_mp_parameters *parameters =
				zero.type == V90_MP_RECORD_TYPE0 ?
				&zero.parameters.type0 : &zero.parameters.type1.common;

			parameters->maximum_upstream_rate_2400_bps_units = 0u;
			if (pcm_downstream_tx_start_mp(&training, &zero) !=
			    PCM_DOWNSTREAM_TX_OK)
				return V90_DIGITAL_PATH_INTERNAL_ERROR;
		}
		result = pcm_downstream_tx_next(&training, &symbol);

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
		if (path->cleardown_requested) {
			path->downstream.training = training;
			return V90_DIGITAL_PATH_CLEARDOWN_COMPLETE;
		}
		if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE) {
			path->downstream.training = training;
			path->reconditioning_symbols = 0u;
			path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_SILENCE;
			return emit_downstream(path, NULL, NULL, octet);
		}
		if (!path->data_mapping_ready)
			return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
		{
			struct v90_downstream_data_stream data;
			struct v90_pcm_symbol pcm;
			pcm_frame_source effective_source = source;
			void *effective_context = source_context;
			enum v90_downstream_data_stream_result data_result =
				v90_downstream_data_stream_start_after_ed(
					&data, &training,
					&path->data_mapping);

			if (path->rate_renegotiation_active) {
				effective_source = NULL;
				effective_context = NULL;
			}
			if (data_result == V90_DOWNSTREAM_DATA_STREAM_OK)
				data_result = v90_downstream_data_stream_next_owned(
					&data, effective_source,
					effective_context, &pcm);
			if (data_result != V90_DOWNSTREAM_DATA_STREAM_OK)
				return stream_result(data_result);
			path->downstream =
				(union v90_digital_path_downstream){ 0 };
			path->downstream.data = data;
			path->downstream_stage =
				V90_DIGITAL_PATH_DOWNSTREAM_DATA;
			if (path->rate_renegotiation_active &&
			    path->upstream_stage ==
				    V90_DIGITAL_PATH_UPSTREAM_DATA)
				path->rate_renegotiation_active = false;
			*octet = pcm.octet;
			return V90_DIGITAL_PATH_OK;
		}
	}
	if (path->downstream_stage ==
	    V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN) {
		struct v90_pcm_symbol pcm;
		enum v90_downstream_data_stream_result result =
			v90_downstream_data_stream_finish(
				&path->downstream.data, &pcm);

		if (result == V90_DOWNSTREAM_DATA_STREAM_OK) {
			*octet = pcm.octet;
			return V90_DIGITAL_PATH_OK;
		}
		if (result != V90_DOWNSTREAM_DATA_STREAM_COMPLETE)
			return stream_result(result);
		{
			struct pcm_r_tx rd;
			const struct v90_pcm_mapping *mapping =
				&path->downstream.data.transmitter.encoder.mapping;

			if (pcm_r_tx_init(&rd, mapping, PCM_R_RD) != PCM_R_TX_OK)
				return V90_DIGITAL_PATH_INTERNAL_ERROR;
			path->downstream =
				(union v90_digital_path_downstream){ 0 };
			path->downstream.rd = rd;
			path->downstream_stage =
				V90_DIGITAL_PATH_DOWNSTREAM_RD;
			return emit_downstream(
				path, NULL, NULL, octet);
		}
	}
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_SILENCE) {
		if (path->data_mapping_ready && path->reconditioning_symbols == 0u) {
			path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_RT;
			return emit_downstream(path, NULL, NULL, octet);
		} else {
			struct v90_pcm_symbol silence;

			if (!v90_pcm_symbol_make(&silence, path->law, 0u, true))
				return V90_DIGITAL_PATH_INTERNAL_ERROR;
			path->reconditioning_symbols = (uint16_t)(
				(path->reconditioning_symbols + 1u) % V90_CP_FRAME_INTERVALS);
			*octet = silence.octet;
			return V90_DIGITAL_PATH_OK;
		}
	}
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_RT) {
		struct pcm_r_tx rt;
		struct v90_pcm_symbol symbol;
		struct pcm_downstream_tx training;

		if (pcm_r_tx_init(&rt, &path->training_mapping, PCM_R_RT) != PCM_R_TX_OK)
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		rt.transmitted_symbols = path->reconditioning_symbols;
		if (!pcm_r_tx_is_complete(&rt)) {
			if (pcm_r_tx_next(&rt, &symbol) != PCM_R_TX_OK)
				return V90_DIGITAL_PATH_INTERNAL_ERROR;
			path->reconditioning_symbols = rt.transmitted_symbols;
			*octet = symbol.octet;
			return V90_DIGITAL_PATH_OK;
		}
		training = path->downstream.training;
		if (pcm_downstream_tx_resume_after_rt(&training) != PCM_DOWNSTREAM_TX_OK)
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		path->downstream.training = training;
		path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_TRAINING;
		path->reconditioning_symbols = 0u;
		return emit_downstream(path, NULL, NULL, octet);
	}
	if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_RD) {
		struct v90_pcm_symbol pcm;
		enum pcm_r_tx_result result =
			pcm_r_tx_next(&path->downstream.rd, &pcm);

		if (result == PCM_R_TX_OK) {
			*octet = pcm.octet;
			return V90_DIGITAL_PATH_OK;
		}
		if (result != PCM_R_TX_COMPLETE)
			return result == PCM_R_TX_INVALID_PARAMETERS ?
				       V90_DIGITAL_PATH_INVALID_PARAMETERS :
				       V90_DIGITAL_PATH_INTERNAL_ERROR;
		{
			struct pcm_downstream_tx training;
			bool silence_requested =
				path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP &&
				path->upstream.startup.stage == V90_UPSTREAM_RX_STAGE_CPS;

			if (pcm_downstream_tx_init(
				    &training, PCM_PROTOCOL_V90,
				    &path->control_mapping) !=
					    PCM_DOWNSTREAM_TX_OK ||
			    pcm_downstream_tx_start_mp(
				    &training, &path->local_mp) !=
					    PCM_DOWNSTREAM_TX_OK ||
			    ((path->data_mapping_ready || silence_requested) &&
			     !path->cleardown_requested &&
			     pcm_downstream_tx_request_mp_prime(&training) !=
				     PCM_DOWNSTREAM_TX_OK))
				return V90_DIGITAL_PATH_INTERNAL_ERROR;
			path->downstream =
				(union v90_digital_path_downstream){ 0 };
			path->downstream.training = training;
			path->downstream_stage =
				V90_DIGITAL_PATH_DOWNSTREAM_TRAINING;
			return emit_downstream(
				path, NULL, NULL, octet);
		}
	}
	{
		struct v90_pcm_symbol pcm;
		pcm_frame_source effective_source =
			path->rate_renegotiation_active ? NULL : source;
		void *effective_context =
			path->rate_renegotiation_active ? NULL : source_context;
		enum v90_downstream_data_stream_result result =
			v90_downstream_data_stream_next_owned(
				&path->downstream.data, effective_source,
				effective_context, &pcm);

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

	if (path->data_mapping_ready ||
	    (!path->rate_renegotiation_active &&
	     path->downstream_stage !=
		     V90_DIGITAL_PATH_DOWNSTREAM_TRAINING) ||
	    (path->rate_renegotiation_active &&
	     path->downstream_stage !=
		     V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN &&
	     path->downstream_stage != V90_DIGITAL_PATH_DOWNSTREAM_RD &&
	     path->downstream_stage !=
		     V90_DIGITAL_PATH_DOWNSTREAM_TRAINING))
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	if (parameters->request_silence) {
		if (!path->rate_renegotiation_active)
			return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	} else {
		if (v90_pcm_mapping_init_transmit(&mapping, parameters, path->law) !=
		    V90_PCM_MAPPING_OK)
			return V90_DIGITAL_PATH_INVALID_PARAMETERS;
		/* Publish the accepted CP before the fallible downstream request so
		 * an internal failure still leaves one diagnosable owner. */
		path->data_mapping = mapping;
		path->data_mapping_ready = true;
	}
	/* CP may arrive while the accepted data tail or Rd is still being sent.
	 * Retain it there; the freshly initialized TRN2d/MP owner observes the
	 * latch as soon as Rd-bar completes. */
	if (path->downstream_stage ==
		    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING &&
	    pcm_downstream_tx_request_mp_prime(
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
	    !v90_pcm_mapping_matches_transmit(
		    &path->data_mapping, parameters,
		    path->data_mapping.law))
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
		return V90_DIGITAL_PATH_OK;
	case V90_UPSTREAM_RX_E_COMPLETE:
		/* E can replace a lost CP-prime acknowledgement. The transmitter
		 * still finishes a complete MP-prime record before emitting Ed. */
		if (path->downstream_stage == V90_DIGITAL_PATH_DOWNSTREAM_TRAINING &&
		    !path->downstream.training.ed_requested)
			return request_ed(path, &path->upstream.startup.data_parameters);
		return V90_DIGITAL_PATH_OK;
	case V90_UPSTREAM_RX_CLEARDOWN:
		return V90_DIGITAL_PATH_CLEARDOWN_COMPLETE;
	case V90_UPSTREAM_RX_REQUEST_MP_PRIME:
		return request_mp_prime(path, event);
	case V90_UPSTREAM_RX_REQUEST_ED:
		return request_ed(path, event);
	case V90_UPSTREAM_RX_REQUEST_SILENCE:
		if (!path->rate_renegotiation_active || path->downstream_stage !=
			    V90_DIGITAL_PATH_DOWNSTREAM_TRAINING)
			return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
		if (pcm_downstream_tx_request_ed(&path->downstream.training) != PCM_DOWNSTREAM_TX_OK)
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE;
		return V90_DIGITAL_PATH_OK;
	case V90_UPSTREAM_RX_REQUEST_RT:
		if (!path->rate_renegotiation_active ||
		    (path->downstream_stage != V90_DIGITAL_PATH_DOWNSTREAM_SILENCE &&
		     path->downstream_stage != V90_DIGITAL_PATH_DOWNSTREAM_TRAINING_SILENCE))
			return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
		if (v90_pcm_mapping_init_transmit(&path->data_mapping, event, path->law) !=
		    V90_PCM_MAPPING_OK)
			return V90_DIGITAL_PATH_INVALID_PARAMETERS;
		path->data_mapping_ready = true;
		return V90_DIGITAL_PATH_OK;
	case V90_UPSTREAM_RX_B1_COMPLETE: {
		struct v34_data_line_receiver data;

		if (v90_upstream_rx_handoff(
			    &path->upstream.startup, &data) !=
		    V90_UPSTREAM_RX_OK)
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		path->upstream_rate_2400_bps_units = data.geometry
			.primary_data_rate_2400_bps_units;
		path->upstream = (union v90_digital_path_upstream){ 0 };
		path->upstream.data = data;
		path->upstream_stage = V90_DIGITAL_PATH_UPSTREAM_DATA;
		if (path->rate_renegotiation_active &&
		    path->downstream_stage ==
			    V90_DIGITAL_PATH_DOWNSTREAM_DATA)
			path->rate_renegotiation_active = false;
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

static enum v90_digital_path_result begin_rate_renegotiation(
	struct v90_digital_path *path)
{
	struct v90_pcm_mapping control_mapping;
	struct v90_phase4_cp_line_rx cp;
	struct v90_upstream_rx startup;
	const struct v90_pcm_mapping *preceding_data;
	bool requested = path->rate_renegotiation_active;

	if ((requested ? path->data_mapping_ready :
	     path->downstream_stage != V90_DIGITAL_PATH_DOWNSTREAM_DATA) ||
	    path->upstream_stage != V90_DIGITAL_PATH_UPSTREAM_DATA ||
	    path->upstream_rate_2400_bps_units == 0u)
		return V90_DIGITAL_PATH_SEQUENCE_MISMATCH;
	control_mapping = path->control_mapping;
	preceding_data =
		&path->downstream.data.transmitter.encoder.mapping;
	if (!requested && v90_pcm_mapping_init_rate_renegotiation(
		    &control_mapping, &path->training_mapping,
		    preceding_data) != V90_PCM_MAPPING_OK)
		return V90_DIGITAL_PATH_INVALID_PARAMETERS;
	if (v90_phase4_cp_line_rx_resume_online(
		    &cp, &path->upstream.data, &path->control_symbols,
		    path->rate_renegotiation_constellation) !=
	    V90_PHASE4_CP_LINE_RX_OK)
		return V90_DIGITAL_PATH_INTERNAL_ERROR;
	if (v90_upstream_rx_init(
		    &startup, &cp, &path->training_mapping,
		    &path->local_mp,
		    path->enabled_downstream_rate_mask) != V90_UPSTREAM_RX_OK)
		return V90_DIGITAL_PATH_INTERNAL_ERROR;

	path->control_mapping = control_mapping;
	path->control_symbols = startup.control_symbols;
	path->upstream = (union v90_digital_path_upstream){ 0 };
	path->upstream.startup = startup;
	path->upstream_stage = V90_DIGITAL_PATH_UPSTREAM_STARTUP;
	if (!requested)
		path->downstream_stage = V90_DIGITAL_PATH_DOWNSTREAM_DATA_DRAIN;
	path->pending_upstream_head = 0u;
	path->pending_upstream_count = 0u;
	path->data_mapping_ready = false;
	path->rate_renegotiation_active = true;
	return requested ? V90_DIGITAL_PATH_OK : V90_DIGITAL_PATH_RATE_RENEGOTIATION;
}

static enum v90_digital_path_result receive_upstream(
	struct v90_digital_path *path, int16_t sample,
	struct v34_received_mapping_frame *received,
	bool defer_host_frame, bool *frame_ready)
{
	if (path->cleardown_requested)
		return V90_DIGITAL_PATH_OK;
	if (path->upstream_stage == V90_DIGITAL_PATH_UPSTREAM_STARTUP) {
		struct v90_cp_parameters event = { 0 };
		enum v90_upstream_rx_result result = v90_upstream_rx_sample(
			&path->upstream.startup, sample, &event);

		path->control_symbols =
			path->upstream.startup.control_symbols;
		return startup_result(path, result, &event);
	}
	{
		struct v34_survivor_decision current = { 0 };
		struct v34_received_mapping_frame frame = { 0 };
		enum v34_data_receive_result result =
			v34_data_line_receiver_sample(
				&path->upstream.data, sample,
				&current, &frame);
		bool candidate_after =
			v34_data_line_receiver_has_rate_renegotiation_candidate(
				&path->upstream.data);
		bool hold_frame = defer_host_frame || candidate_after ||
			path->rate_renegotiation_active;
		bool generated_frame =
			result == V34_DATA_RECEIVE_MAPPING_FRAME_READY;
		bool released_frame = false;

		if (result == V34_DATA_RECEIVE_RATE_RENEGOTIATION) {
			return begin_rate_renegotiation(path);
		}
		switch (result) {
		case V34_DATA_RECEIVE_OK:
		case V34_DATA_RECEIVE_INTERVAL_READY:
		case V34_DATA_RECEIVE_MAPPING_FRAME_READY:
			break;
		case V34_DATA_RECEIVE_FAILED:
			return V90_DIGITAL_PATH_FAILED;
		case V34_DATA_RECEIVE_OUT_OF_RANGE:
			return V90_DIGITAL_PATH_OUT_OF_RANGE;
		case V34_DATA_RECEIVE_NUMERIC_FAILURE:
			return V90_DIGITAL_PATH_NUMERIC_FAILURE;
		case V34_DATA_RECEIVE_INVALID_ARGUMENT:
		case V34_DATA_RECEIVE_INVALID_STATE:
		case V34_DATA_RECEIVE_B1_IN_PROGRESS:
		case V34_DATA_RECEIVE_B1_COMPLETE:
		case V34_DATA_RECEIVE_RATE_RENEGOTIATION:
			return V90_DIGITAL_PATH_INTERNAL_ERROR;
		}
		/* A local request clamps old host data while waiting for S/S-bar.
		 * It must not fill the bounded post-B1/candidate queue. */
		if (path->rate_renegotiation_active && !path->data_mapping_ready)
			return V90_DIGITAL_PATH_OK;
		/* Survivor traceback hides the complete early S/S-bar prefix. Also
		 * retain post-B1 frames when the upstream half finishes RRN first;
		 * neither kind belongs to the host until its control epoch resolves. */
		if (!hold_frame && path->pending_upstream_count != 0u) {
			*received = path->pending_upstream_frames[
				path->pending_upstream_head];
			path->pending_upstream_head = (uint8_t)(
				(path->pending_upstream_head + 1u) %
				V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY);
			path->pending_upstream_count--;
			released_frame = true;
		}
		if (generated_frame) {
			size_t tail;

			if (!hold_frame &&
			    path->pending_upstream_count == 0u &&
			    !released_frame) {
				*received = frame;
				*frame_ready = true;
				return V90_DIGITAL_PATH_UPSTREAM_FRAME;
			}

			if (path->pending_upstream_count ==
			    V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY)
				return V90_DIGITAL_PATH_INTERNAL_ERROR;
			tail = (path->pending_upstream_head +
				path->pending_upstream_count) %
			       V90_DIGITAL_PATH_PENDING_FRAME_CAPACITY;
			path->pending_upstream_frames[tail] = frame;
			path->pending_upstream_count++;
			result = V34_DATA_RECEIVE_INTERVAL_READY;
		}
		if (released_frame) {
			*frame_ready = true;
			return V90_DIGITAL_PATH_UPSTREAM_FRAME;
		}
		return V90_DIGITAL_PATH_OK;
	}
}

static bool output_overlaps_path(
	const struct v90_digital_path *path, const void *output,
	size_t output_size)
{
	return memory_regions_overlap(path, sizeof(*path), output, output_size);
}

enum v90_digital_path_result v90_digital_path_transmit(
	struct v90_digital_path *path,
	pcm_frame_source source, void *source_context,
	uint8_t *transmit_octet)
{
	uint8_t generated;
	enum v90_digital_path_result result;

	if (!v90_digital_path_is_valid(path) || transmit_octet == NULL ||
	    output_overlaps_path(path, transmit_octet, sizeof(*transmit_octet)))
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_DIGITAL_PATH_FAILED;
	generated = g711_encode(path->law, 0);
	result = emit_downstream(
		path, source, source_context, &generated);
	if (result == V90_DIGITAL_PATH_SOURCE_FAILURE)
		return result;
	if (result != V90_DIGITAL_PATH_OK &&
	    result != V90_DIGITAL_PATH_CLEARDOWN_COMPLETE)
		path->failed = true;
	if (!v90_digital_path_is_valid(path)) {
		path->failed = true;
		result = V90_DIGITAL_PATH_INTERNAL_ERROR;
	}
	*transmit_octet = generated;
	return result;
}

enum v90_digital_path_result v90_digital_path_receive_sample(
	struct v90_digital_path *path, int16_t received_sample,
	struct v34_received_mapping_frame *received)
{
	struct v34_received_mapping_frame frame = { 0 };
	bool frame_ready = false;
	enum v90_digital_path_result result;

	if (!v90_digital_path_is_valid(path) || received == NULL ||
	    output_overlaps_path(path, received, sizeof(*received)))
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_DIGITAL_PATH_FAILED;
	result = receive_upstream(
		path, received_sample, &frame,
		path->rate_renegotiation_active, &frame_ready);
	if (result != V90_DIGITAL_PATH_OK &&
	    result != V90_DIGITAL_PATH_UPSTREAM_FRAME &&
	    result != V90_DIGITAL_PATH_CLEARDOWN_COMPLETE &&
	    result != V90_DIGITAL_PATH_RATE_RENEGOTIATION)
		path->failed = true;
	if (!v90_digital_path_is_valid(path)) {
		path->failed = true;
		result = V90_DIGITAL_PATH_INTERNAL_ERROR;
	}
	if (frame_ready)
		*received = frame;
	return result;
}

static enum v90_digital_path_result sample_owned(
	struct v90_digital_path *path, int16_t received_sample,
	pcm_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received)
{
	struct v34_received_mapping_frame frame = { 0 };
	uint8_t generated;
	bool rate_renegotiation_was_active =
		path->rate_renegotiation_active;
	bool frame_ready = false;
	enum v90_digital_path_result result;

	generated = g711_encode(path->law, 0);
	result = emit_downstream(
		path, source, source_context, &generated);
	if (result == V90_DIGITAL_PATH_SOURCE_FAILURE)
		return result;
	if (result == V90_DIGITAL_PATH_OK)
		result = receive_upstream(
			path, received_sample, &frame,
			rate_renegotiation_was_active, &frame_ready);
	if (result != V90_DIGITAL_PATH_OK &&
	    result != V90_DIGITAL_PATH_UPSTREAM_FRAME &&
	    result != V90_DIGITAL_PATH_CLEARDOWN_COMPLETE &&
	    result != V90_DIGITAL_PATH_RATE_RENEGOTIATION)
		path->failed = true;
	*transmit_octet = generated;
	if (frame_ready)
		*received = frame;
	return result;
}

enum v90_digital_path_result v90_digital_path_sample(
	struct v90_digital_path *path, int16_t received_sample,
	pcm_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received)
{
	struct v34_received_mapping_frame frame;
	uint8_t generated;
	enum v90_digital_path_result result;

	if (!v90_digital_path_is_valid(path) || transmit_octet == NULL ||
	    received == NULL ||
	    output_overlaps_path(path, transmit_octet, sizeof(*transmit_octet)) ||
	    output_overlaps_path(path, received, sizeof(*received)) ||
	    memory_regions_overlap(
		transmit_octet, sizeof(*transmit_octet),
		received, sizeof(*received)))
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_DIGITAL_PATH_FAILED;
	result = sample_owned(
		path, received_sample, source, source_context,
		&generated, &frame);
	if (result != V90_DIGITAL_PATH_SOURCE_FAILURE &&
	    !v90_digital_path_is_valid(path)) {
		path->failed = true;
		return V90_DIGITAL_PATH_INTERNAL_ERROR;
	}
	if (result != V90_DIGITAL_PATH_SOURCE_FAILURE) {
		*transmit_octet = generated;
		if (result == V90_DIGITAL_PATH_UPSTREAM_FRAME)
			*received = frame;
	}
	return result;
}

enum v90_digital_path_result v90_digital_path_sample_owned(
	struct v90_digital_path *path, int16_t received_sample,
	pcm_frame_source source, void *source_context,
	uint8_t *transmit_octet,
	struct v34_received_mapping_frame *received)
{
	if (path == NULL || transmit_octet == NULL || received == NULL)
		return V90_DIGITAL_PATH_INVALID_ARGUMENT;
	if (path->failed)
		return V90_DIGITAL_PATH_FAILED;
	return sample_owned(
		path, received_sample, source, source_context,
		transmit_octet, received);
}

const char *v90_digital_path_result_name(
	enum v90_digital_path_result result)
{
	switch (result) {
	case V90_DIGITAL_PATH_OK:
		return "ok";
	case V90_DIGITAL_PATH_UPSTREAM_FRAME:
		return "upstream frame";
	case V90_DIGITAL_PATH_RATE_RENEGOTIATION:
		return "rate renegotiation";
	case V90_DIGITAL_PATH_CLEARDOWN_COMPLETE:
		return "cleardown complete";
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
