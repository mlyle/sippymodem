// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_analog_station.h>
#include <v90_analog_path.h>
#include <v90_digital_path.h>
#include <v90_phase4_digital_station.h>

#include <g711.h>

#include "test.h"
#include "v90_phase4_station_fixture.h"

#include <string.h>

enum {
	TRAINING_UCODE = 91,
	SAMPLE_LIMIT = 36000,
	DOWNSTREAM_TRAINING_CHARACTER_LIMIT =
		V90_PHASE4_TRN2D_INITIAL_SYMBOLS + 2048,
	DATA_FRAMES_TO_RECEIVE = 4,
	UPSTREAM_FRAMES_TO_RECEIVE = 4,
};

static struct v90_jd_parameters jd_parameters(
	enum v34_constellation_size constellation)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = 0x15555u,
		.training_constellation = constellation,
		.rate_renegotiation_constellation =
			V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters training_parameters(enum g711_law law)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.upstream_rate_mask = 0x1555u,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = -16,
		.shaping_a2_q1_6 = 8,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = 31,
		.selected_rate_index = 13,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v90_mp_record mp_parameters(void)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.upstream_rate_capability_mask = 0x1555u,
		},
	};
}

static bool parameters_match(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	uint8_t left_frame[V90_CP_MAX_FRAME_BYTES] = { 0 };
	uint8_t right_frame[V90_CP_MAX_FRAME_BYTES] = { 0 };
	size_t left_bits = 0;
	size_t right_bits = 0;

	return v90_cp_encode(
		       left, left_frame, sizeof(left_frame), &left_bits) ==
		       V90_CP_OK &&
	       v90_cp_encode(
		       right, right_frame, sizeof(right_frame), &right_bits) ==
		       V90_CP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static bool mapping_matches(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping expected;

	return v90_pcm_mapping_init(&expected, parameters) ==
		       V90_PCM_MAPPING_OK &&
	       v90_pcm_mapping_equal(mapping, &expected);
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static int16_t decode_octet(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static void generate_data_bits(
	uint8_t *bits, size_t bit_count, size_t frame)
{
	for (size_t bit = 0; bit < bit_count; ++bit)
		bits[bit] = (uint8_t)(
			(frame * 13u + bit * 7u + (frame >> (bit % 3u))) & 1u);
}

struct downstream_source {
	size_t frames;
	bool reject_next;
};

static bool source_downstream_frame(
	void *context, uint8_t *bits, size_t bit_count)
{
	struct downstream_source *source = context;

	if (source == NULL || bits == NULL || bit_count == 0 ||
	    bit_count > V90_PCM_FRAME_MAX_BITS || source->reject_next)
		return false;
	generate_data_bits(bits, bit_count, source->frames + 1u);
	source->frames++;
	return true;
}

struct upstream_source {
	size_t frames;
	uint16_t received_delay;
	bool reject_next;
};

static bool source_upstream_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct upstream_source *source = context;

	if (source == NULL || request == NULL || primary_bits == NULL ||
	    request->primary_bits == 0 ||
	    request->primary_bits > V34_MAX_MAPPING_FRAME_BITS ||
	    source->reject_next)
		return false;
	generate_data_bits(
		primary_bits, request->primary_bits, source->frames + 97u);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = (uint8_t)(
			(source->frames + request->index) & 1u);
	source->frames++;
	return true;
}

static bool upstream_frame_matches(
	const struct v34_received_mapping_frame *received,
	struct upstream_source *source, size_t frame_index)
{
	uint8_t expected[V34_MAX_MAPPING_FRAME_BITS];
	uint16_t delayed;
	uint8_t expected_auxiliary = (uint8_t)(
		(frame_index + received->mapping_frame.index) & 1u);

	if (source == NULL || received->primary_bit_count == 0 ||
	    received->primary_bit_count !=
		    received->mapping_frame.primary_bits ||
	    received->auxiliary_bit_count !=
		    (uint8_t)received->mapping_frame.carries_auxiliary_bit)
		return false;
	generate_data_bits(
		expected, received->primary_bit_count, frame_index + 97u);
	delayed = source->received_delay;
	for (size_t bit = 0; bit < received->primary_bit_count; ++bit) {
		uint8_t wire_bit = (uint8_t)(delayed & 1u);

		delayed = (uint16_t)((delayed >> 1) |
				     ((uint16_t)expected[bit] << 15));
		if (received->primary_bits[bit] != wire_bit)
			return false;
	}
	if (received->auxiliary_bit_count != 0 &&
	    received->auxiliary_bit != expected_auxiliary)
		return false;
	source->received_delay = delayed;
	return true;
}

static bool downstream_frame_matches(
	const struct v90_pcm_stream_frame *received,
	const struct downstream_source *source, size_t frame_index)
{
	uint8_t expected[V90_PCM_FRAME_MAX_BITS];

	if (received->bit_count == 0 ||
	    received->bit_count > V90_PCM_FRAME_MAX_BITS ||
	    frame_index >= source->frames)
		return false;
	generate_data_bits(expected, received->bit_count, frame_index + 1u);
	return memcmp(
		       received->bits, expected, received->bit_count) == 0;
}

static bool alter_last_modulus_symbol(
	const struct pcm_downstream_rx *receiver, uint8_t *network_octet)
{
	const struct v90_pcm_stream_rx *stream = &receiver->stream;
	const struct v90_pcm_frame_decoder *decoder = &stream->decoder;
	const struct v90_pcm_mapping *mapping = &decoder->mapping;
	struct v90_pcm_symbol original;
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	uint8_t decoded[V90_PCM_FRAME_MAX_BITS];

	if (network_octet == NULL ||
	    stream->pending_count != V90_CP_FRAME_INTERVALS - 1u ||
	    !v90_pcm_symbol_parse(
		    &original, mapping->law, *network_octet))
		return false;
	for (size_t interval = 0;
	     interval < stream->pending_count; ++interval) {
		if (v90_pcm_mapping_label(
			    mapping, decoder->decision_plane, interval,
			    stream->pending_ucodes[interval],
			    &labels[interval]) != V90_PCM_MAPPING_OK)
			return false;
	}
	if (v90_pcm_mapping_label(
		    mapping, decoder->decision_plane,
		    V90_CP_FRAME_INTERVALS - 1u, original.ucode,
		    &labels[V90_CP_FRAME_INTERVALS - 1u]) !=
	    V90_PCM_MAPPING_OK)
		return false;
	for (uint8_t alternative = 0;
	     alternative < mapping->modulus[V90_CP_FRAME_INTERVALS - 1u];
	     ++alternative) {
		struct v90_pcm_symbol altered;
		uint8_t altered_ucode;
		uint8_t original_label =
			labels[V90_CP_FRAME_INTERVALS - 1u];

		if (alternative == original_label)
			continue;
		labels[V90_CP_FRAME_INTERVALS - 1u] = alternative;
		if (v90_pcm_mapping_decode_modulus(
			    mapping, labels, decoded,
			    mapping->modulus_bits) != V90_PCM_MAPPING_OK ||
		    v90_pcm_mapping_ucode(
			    mapping, decoder->decision_plane,
			    V90_CP_FRAME_INTERVALS - 1u, alternative,
			    &altered_ucode) != V90_PCM_MAPPING_OK ||
		    !v90_pcm_symbol_make(
			    &altered, mapping->law, altered_ucode,
			    original.positive)) {
			labels[V90_CP_FRAME_INTERVALS - 1u] =
				original_label;
			continue;
		}
		*network_octet = altered.octet;
		return true;
	}
	return false;
}

static bool verify_repeated_mp_recovery(
	const struct v90_phase4_cp_line_tx *cp_transmit,
	const struct v90_phase4_digital_transmit_handoff *digital,
	const struct v90_phase4_analog_receive_handoff *analog,
	const struct v90_cp_parameters *training)
{
	struct v90_cp_parameters data_cp = *training;
	struct v90_phase4_analog_handoff handoff = {
		.transmit = *cp_transmit,
		.receive = analog->receiver,
		.rate_renegotiation_constellation =
			V34_CONSTELLATION_4_POINT,
	};
	struct v90_phase4_analog_handoff invalid_handoff = handoff;
	struct pcm_downstream_tx transmitter = digital->transmitter;
	struct v90_analog_path path;
	struct v90_analog_path untouched;
	struct v90_analog_path untouched_saved;
	bool mp_corrupted = false;
	bool mp_rejection_seen = false;

	data_cp.kind = V90_CP_DATA;
	data_cp.selected_rate_index = 1;
	data_cp.acknowledges_peer = false;
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	invalid_handoff.rate_renegotiation_constellation =
		(enum v34_constellation_size)8;
	CHECK(v90_analog_path_init(
		      &untouched, &invalid_handoff, &data_cp) ==
		      V90_ANALOG_PATH_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0 &&
	      v90_analog_path_init(&path, &handoff, &data_cp) ==
		      V90_ANALOG_PATH_OK &&
	      pcm_downstream_tx_is_valid(&transmitter),
	      "repeated-MP fixture did not initialize");
	for (size_t clock = 0;
	     clock < DOWNSTREAM_TRAINING_CHARACTER_LIMIT; ++clock) {
		struct pcm_downstream_symbol symbol;
		struct v90_pcm_stream_frame received;
		struct pcm_downstream_rx *receiver =
			&path.downstream.training;
		uint8_t upstream_octet;
		enum v90_analog_path_result result;

		CHECK(v90_analog_path_transmit(
			      &path, NULL, NULL, &upstream_octet) ==
			      V90_ANALOG_PATH_OK &&
		      pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "repeated-MP line stopped at clock %zu", clock);
		if (!mp_corrupted && !path.peer_mp_received &&
		    receiver->stage == PCM_DOWNSTREAM_RX_MP_SEARCH &&
		    receiver->stream.pending_count ==
			    V90_CP_FRAME_INTERVALS - 1u &&
		    receiver->control.mp.stage == V90_MP_RECEIVE_SEQUENCE &&
		    receiver->control.mp.bit_index >
			    V90_MP_FRAME_SYNC_BITS + 1u &&
		    receiver->control.mp.bit_index +
			    receiver->stream.decoder.mapping.frame_bits +
			    V34_SCRAMBLER_BITS <
			    receiver->control.mp.sequence_bit_count) {
			CHECK(alter_last_modulus_symbol(
				      receiver, &symbol.pcm.octet),
			      "could not corrupt one valid MP symbol");
			mp_corrupted = true;
		}
		{
			struct pcm_downstream_rx shadow = *receiver;
			struct pcm_downstream_event event;

			if (pcm_downstream_rx_receive(
				    &shadow, symbol.pcm.octet, &event) ==
			    PCM_DOWNSTREAM_RX_MP_REJECTED)
				mp_rejection_seen = true;
		}
		result = v90_analog_path_receive(
			&path, symbol.pcm.octet, &received);
		CHECK(result == V90_ANALOG_PATH_OK,
		      "rejected MP stopped the analogue path at clock %zu: %s",
		      clock, v90_analog_path_result_name(result));
		if (!path.peer_mp_received)
			continue;
		CHECK(mp_corrupted && mp_rejection_seen &&
		      v90_mp_record_equal(
			      &path.peer_mp, &transmitter.control.mp) &&
		      v90_analog_path_is_valid(&path) &&
		      !v90_analog_path_has_failed(&path),
		      "repeated MP did not recover the analogue path");
		return true;
	}
	CHECK(false, "repeated MP was not accepted after one rejection");
	return false;
}

static bool verify_final_downstream_exchange(
	struct v90_phase4_cp_line_tx *cp_transmit,
	struct v90_phase4_cp_line_rx *cp_receive,
	struct v90_phase4_digital_transmit_handoff *digital,
	struct v90_phase4_analog_receive_handoff *analog,
	const struct v90_cp_parameters *training,
	uint32_t enabled_downstream_rate_mask,
	bool expect_rate_deferral)
{
	struct v90_cp_parameters data_cp = *training;
	struct v90_phase4_analog_handoff analog_handoff = {
		.transmit = *cp_transmit,
		.receive = analog->receiver,
		.rate_renegotiation_constellation =
			V34_CONSTELLATION_4_POINT,
	};
	struct v90_phase4_digital_handoff digital_handoff = {
		.transmit = *digital,
		.receive = *cp_receive,
		.peer_upstream_rate_mask = training->upstream_rate_mask,
	};
	struct v90_analog_path analog_path;
	struct v90_digital_path digital_path;
	struct downstream_source downstream_source = { 0 };
	struct upstream_source upstream_source = {
		.received_delay = UINT16_MAX,
	};
	size_t downstream_training_characters = 0;
	size_t downstream_data_frames = 0;
	size_t upstream_data_frames = 0;
	uint64_t training_bit_errors = 0;
	bool trn2d_complete = false;
	bool downstream_source_failure_checked = false;
	bool upstream_source_failure_checked = false;
	bool unadvertised_candidate_seen = false;

	data_cp.kind = V90_CP_DATA;
	data_cp.selected_rate_index = 1;
	data_cp.acknowledges_peer = false;
	CHECK(v90_analog_path_init(
		      &analog_path, &analog_handoff, &data_cp) ==
		      V90_ANALOG_PATH_OK &&
	      v90_digital_path_init(
		      &digital_path, &digital_handoff,
		      enabled_downstream_rate_mask,
		      cp_receive->symbols.constellation) ==
		      V90_DIGITAL_PATH_OK,
	      "online paths did not start at the CPt/Ri boundary");
	{
		struct v90_analog_path saved = analog_path;
		struct v90_pcm_stream_frame frame;
		struct v90_pcm_stream_frame saved_frame;

		memset(&frame, 0xa5, sizeof(frame));
		saved_frame = frame;
		CHECK(v90_analog_path_receive(
			      &analog_path, 0, &frame) ==
			      V90_ANALOG_PATH_INVALID_STATE &&
		      memcmp(&analog_path, &saved, sizeof(analog_path)) == 0 &&
		      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
		      "receive accepted an interval with no transmitted sample");
	}

	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		struct v34_received_mapping_frame upstream_frame;
		struct v34_received_mapping_frame saved_upstream_frame;
		struct v90_pcm_stream_frame downstream_frame;
		struct v90_pcm_stream_frame saved_downstream_frame;
		struct v90_analog_path saved_analog;
		struct v90_digital_path saved_digital;
		uint8_t analog_octet = 0x5a;
		uint8_t digital_octet = 0xa5;
		uint8_t saved_octet = 0;
		enum v90_analog_path_result analog_result;
		enum v90_digital_path_result digital_result;
		bool training_active = analog_path.downstream_stage ==
			V90_ANALOG_PATH_DOWNSTREAM_TRAINING;

		if (training_active) {
			training_bit_errors =
				analog_path.downstream.training.trn2d_bit_errors;
			trn2d_complete = trn2d_complete ||
				analog_path.downstream.training.stage !=
					PCM_DOWNSTREAM_RX_TRN2D;
		}
		if (!upstream_source_failure_checked &&
		    !upstream_source.reject_next &&
		    v90_analog_path_upstream_is_online(&analog_path))
			upstream_source.reject_next = true;
		if (upstream_source.reject_next) {
			saved_analog = analog_path;
			saved_octet = analog_octet;
		}
		analog_result = v90_analog_path_transmit(
			&analog_path, source_upstream_frame,
			&upstream_source, &analog_octet);
		if (analog_result == V90_ANALOG_PATH_SOURCE_FAILURE) {
			CHECK(upstream_source.reject_next &&
			      memcmp(&analog_path, &saved_analog,
				     sizeof(analog_path)) == 0 &&
			      analog_octet == saved_octet,
			      "upstream source failure consumed clock %zu", clock);
			upstream_source.reject_next = false;
			analog_result = v90_analog_path_transmit(
				&analog_path, source_upstream_frame,
				&upstream_source, &analog_octet);
			upstream_source_failure_checked = true;
		}
		CHECK(analog_result == V90_ANALOG_PATH_OK,
		      "analogue transmit failed at clock %zu: %s", clock,
		      v90_analog_path_result_name(analog_result));
		if (clock == 0) {
			struct v90_analog_path saved = analog_path;
			uint8_t extra = 0x3c;

			CHECK(v90_analog_path_transmit(
				      &analog_path, NULL, NULL, &extra) ==
				      V90_ANALOG_PATH_INVALID_STATE &&
			      extra == 0x3c &&
			      memcmp(&analog_path, &saved,
				     sizeof(analog_path)) == 0,
			      "transmit accepted two samples in one interval");
		}

		if (!downstream_source_failure_checked &&
		    !downstream_source.reject_next &&
		    v90_digital_path_downstream_is_online(&digital_path))
			downstream_source.reject_next = true;
		memset(&upstream_frame, 0xa5, sizeof(upstream_frame));
		saved_upstream_frame = upstream_frame;
		if (downstream_source.reject_next) {
			saved_digital = digital_path;
			saved_octet = digital_octet;
		}
		digital_result = v90_digital_path_sample(
			&digital_path, decode_octet(digital_path.law, analog_octet),
			source_downstream_frame,
			&downstream_source, &digital_octet, &upstream_frame);
		if (expect_rate_deferral) {
			const struct v90_cp_receiver *records =
				&digital_path.upstream.startup.state.cp.symbols.records;

			if (records->stage == V90_CP_RECEIVER_READ_FRAME)
				unadvertised_candidate_seen = true;
			if (unadvertised_candidate_seen &&
			    records->stage == V90_CP_RECEIVER_SEARCH_SYNC &&
			    records->sync_one_count == 0) {
				CHECK(digital_result == V90_DIGITAL_PATH_OK &&
				      v90_digital_path_is_valid(&digital_path) &&
				      !v90_digital_path_has_failed(&digital_path) &&
				      !digital_path.upstream.startup
					       .data_cp_received &&
				      !digital_path.data_mapping_ready,
				      "unadvertised CP changed the digital path");
				return true;
			}
		}
		if (digital_result == V90_DIGITAL_PATH_SOURCE_FAILURE) {
			CHECK(downstream_source.reject_next &&
			      memcmp(&digital_path, &saved_digital,
				     sizeof(digital_path)) == 0 &&
			      digital_octet == saved_octet &&
			      memcmp(&upstream_frame, &saved_upstream_frame,
				     sizeof(upstream_frame)) == 0,
			      "downstream source failure consumed clock %zu", clock);
			downstream_source.reject_next = false;
			digital_result = v90_digital_path_sample(
				&digital_path,
				decode_octet(digital_path.law, analog_octet),
				source_downstream_frame, &downstream_source,
				&digital_octet, &upstream_frame);
			downstream_source_failure_checked = true;
		}
		CHECK(digital_result == V90_DIGITAL_PATH_OK ||
		      digital_result == V90_DIGITAL_PATH_UPSTREAM_FRAME,
		      "digital path failed at clock %zu: %s", clock,
		      v90_digital_path_result_name(digital_result));
		if (digital_result == V90_DIGITAL_PATH_UPSTREAM_FRAME) {
			CHECK(upstream_data_frames < upstream_source.frames &&
			      upstream_frame_matches(
				      &upstream_frame, &upstream_source,
				      upstream_data_frames),
			      "upstream data frame %zu changed",
			      upstream_data_frames);
			upstream_data_frames++;
		}

		memset(&downstream_frame, 0x5a, sizeof(downstream_frame));
		saved_downstream_frame = downstream_frame;
		analog_result = v90_analog_path_receive(
			&analog_path, digital_octet, &downstream_frame);
		CHECK(analog_result == V90_ANALOG_PATH_OK ||
		      analog_result == V90_ANALOG_PATH_DOWNSTREAM_FRAME,
		      "analogue receive failed at clock %zu: %s", clock,
		      v90_analog_path_result_name(analog_result));
		if (training_active)
			downstream_training_characters++;
		if (analog_result == V90_ANALOG_PATH_DOWNSTREAM_FRAME) {
			CHECK(downstream_frame_matches(
				      &downstream_frame, &downstream_source,
				      downstream_data_frames),
			      "downstream data frame %zu changed",
			      downstream_data_frames);
			downstream_data_frames++;
		} else {
			CHECK(memcmp(
				      &downstream_frame, &saved_downstream_frame,
				      sizeof(downstream_frame)) == 0,
			      "non-data event changed downstream output");
		}
		if (analog_path.downstream_stage ==
		    V90_ANALOG_PATH_DOWNSTREAM_TRAINING) {
			training_bit_errors =
				analog_path.downstream.training.trn2d_bit_errors;
			trn2d_complete = trn2d_complete ||
				analog_path.downstream.training.stage !=
					PCM_DOWNSTREAM_RX_TRN2D;
		}
		if (v90_analog_path_is_online(&analog_path) &&
		    v90_digital_path_is_online(&digital_path) &&
		    downstream_data_frames >= DATA_FRAMES_TO_RECEIVE &&
		    upstream_data_frames >= UPSTREAM_FRAMES_TO_RECEIVE &&
		    downstream_source_failure_checked &&
		    upstream_source_failure_checked)
			break;
	}

	CHECK(analog_path.peer_mp_received &&
	      !expect_rate_deferral &&
	      analog_path.peer_mp_prime_received &&
	      analog_path.cp_prime_sent && analog_path.finishing_cp &&
	      analog_path.ed_armed && trn2d_complete &&
	      training_bit_errors == 0 &&
	      downstream_training_characters <
		      DOWNSTREAM_TRAINING_CHARACTER_LIMIT &&
	      analog_path.downstream_stage ==
		      V90_ANALOG_PATH_DOWNSTREAM_DATA &&
	      analog_path.downstream.data.b1d_frames ==
		      V90_PHASE4_B1D_FRAMES &&
	      analog_path.downstream.data.b1d_bit_errors == 0 &&
	      downstream_data_frames >= DATA_FRAMES_TO_RECEIVE &&
	      upstream_data_frames >= UPSTREAM_FRAMES_TO_RECEIVE &&
	      downstream_source_failure_checked &&
	      upstream_source_failure_checked &&
	      v90_analog_path_is_online(&analog_path) &&
	      v90_digital_path_is_online(&digital_path) &&
	      digital_path.data_mapping_ready &&
	      v90_pcm_mapping_equal(
		      &digital_path.data_mapping, &analog_path.data_mapping),
	      "paired path stopped: CP/MP=%d/%d/%d Ed=%d TRN=%d/%llu "
	      "data=%zu/%zu B1d=%u source=%d/%d online=%d/%d",
	      analog_path.peer_mp_received,
	      analog_path.peer_mp_prime_received,
	      analog_path.cp_prime_sent, analog_path.ed_armed,
	      trn2d_complete, (unsigned long long)training_bit_errors,
	      downstream_data_frames, upstream_data_frames,
	      analog_path.downstream.data.b1d_frames,
	      downstream_source_failure_checked,
	      upstream_source_failure_checked,
	      v90_analog_path_is_online(&analog_path),
	      v90_digital_path_is_online(&digital_path));
	return true;
}

static bool run_exchange(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_jd_parameters jd = jd_parameters(constellation);
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_mp_record mp = mp_parameters();
	struct v90_phase3_return_line_handoff phase3_receive;
	struct v90_phase4_analog_station analog;
	struct v90_phase4_digital_station digital;
	struct v90_phase4_cp_line_tx analog_transmit;
	struct v90_phase4_cp_line_rx digital_receive;
	struct v90_phase4_analog_receive_handoff analog_receive;
	struct v90_phase4_digital_transmit_handoff digital_transmit;
	bool digital_accepted_cpt = false;
	bool digital_receive_handed_off = false;
	bool analog_saw_transition = false;
	bool complete = false;

	CHECK(v90_phase4_station_fixture_init(
		      law, 8u, &jd, TRAINING_UCODE, &training,
		      &analog, &phase3_receive),
	      "paired Phase 4 fixture did not reach CPt");
	CHECK(v90_phase4_digital_station_init(
		      &digital, &phase3_receive, &jd, law,
		      TRAINING_UCODE) ==
		      V90_PHASE4_DIGITAL_STATION_OK,
	      "digital station did not continue the paired line");

	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		if (!v90_phase4_analog_station_transmit_complete(&analog)) {
			int16_t sample;

			CHECK(v90_phase4_analog_station_transmit(
				      &analog, &sample) ==
				      V90_PHASE4_ANALOG_STATION_OK,
			      "CPt direction failed at clock %zu", clock);
			if (!digital_receive_handed_off) {
				CHECK(v90_phase4_digital_station_receive(
					      &digital, quantize(law, sample)) ==
					      V90_PHASE4_DIGITAL_STATION_OK,
				      "CPt receiver failed at clock %zu", clock);
				if (digital.cpt_received) {
					CHECK(v90_phase4_digital_station_receive_handoff(
						      &digital, &digital_receive) ==
						      V90_PHASE4_DIGITAL_STATION_OK,
					      "digital receive handoff was late");
					digital_accepted_cpt = true;
					digital_receive_handed_off = true;
				}
			} else {
				struct v90_cp_receive_event event = { 0 };
				enum v90_phase4_cp_line_rx_result result;

				result = v90_phase4_cp_line_rx_sample(
					&digital_receive, quantize(law, sample),
					&event);
				CHECK(result == V90_PHASE4_CP_LINE_RX_OK ||
				      result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
				      result == V90_PHASE4_CP_LINE_RX_RECORD_READY,
				      "continued CPt receiver failed at clock %zu",
				      clock);
				if (result ==
				    V90_PHASE4_CP_LINE_RX_RECORD_READY) {
					CHECK(v90_phase4_cp_line_rx_pending_record_is_valid(
						      &digital_receive) &&
					      parameters_match(
						      &event.parameters, &training) &&
					      v90_phase4_cp_line_rx_continue(
						      &digital_receive) ==
						      V90_PHASE4_CP_LINE_RX_OK,
					      "trailing CPt was not continuous");
				}
			}
		}

		if (!v90_phase4_digital_station_transmit_complete(&digital)) {
			bool previously_seen = analog.finish_cpt_requested;
			uint8_t octet;

			CHECK(v90_phase4_digital_station_transmit(
				      &digital, &octet) ==
				      V90_PHASE4_DIGITAL_STATION_OK &&
			      v90_phase4_analog_station_receive(
				      &analog, octet) ==
				      V90_PHASE4_ANALOG_STATION_OK,
			      "Ri direction failed at clock %zu", clock);
			if (!previously_seen && analog.finish_cpt_requested) {
				CHECK(digital_accepted_cpt,
				      "Ri-bar preceded CPt acceptance");
				analog_saw_transition = true;
			}
		}

		if (v90_phase4_analog_station_transmit_complete(&analog) &&
		    v90_phase4_analog_station_receive_complete(&analog) &&
		    v90_phase4_digital_station_transmit_complete(&digital) &&
		    v90_phase4_digital_station_receive_complete(&digital)) {
			complete = true;
			break;
		}
	}

	CHECK(complete && digital_accepted_cpt &&
	      digital_receive_handed_off && analog_saw_transition,
	      "paired initial exchange did not reach both handoffs");
	CHECK(v90_phase4_analog_station_transmit_handoff(
		      &analog, &analog_transmit) ==
		      V90_PHASE4_ANALOG_STATION_OK &&
	      v90_phase4_cp_line_rx_is_valid(&digital_receive) &&
	      !v90_phase4_cp_line_rx_has_pending_record(&digital_receive) &&
	      v90_phase4_analog_station_receive_handoff(
		      &analog, &analog_receive) ==
		      V90_PHASE4_ANALOG_STATION_OK &&
	      v90_phase4_digital_station_transmit_handoff(
		      &digital, &mp, &digital_transmit) ==
		      V90_PHASE4_DIGITAL_STATION_OK &&
	      mapping_matches(
		      &analog_receive.receiver.stream.decoder.mapping,
		      &training) &&
	      mapping_matches(
		      &digital_transmit.transmitter.encoder.mapping,
		      &training),
	      "paired owners disagreed about their Phase 4 handoffs");
	CHECK(verify_repeated_mp_recovery(
		      &analog_transmit, &digital_transmit,
		      &analog_receive, &training) &&
	      verify_final_downstream_exchange(
		      &analog_transmit, &digital_receive,
		      &digital_transmit, &analog_receive, &training,
		      V90_JD_RATE_CAPABILITY_MASK, false) &&
	      verify_final_downstream_exchange(
		      &analog_transmit, &digital_receive,
		      &digital_transmit, &analog_receive, &training,
		      UINT32_C(2), true),
	      "paired owners disagreed through downstream data");
	return true;
}

static bool test_paired_exchange(void)
{
	CHECK(run_exchange(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT),
	      "PCMU four-point paired exchange failed");
	CHECK(run_exchange(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT),
	      "PCMA sixteen-point paired exchange failed");
	return true;
}

static bool test_path_contracts(void)
{
	struct v90_phase4_analog_handoff analog_handoff = { 0 };
	struct v90_phase4_digital_handoff handoff = { 0 };
	struct v90_cp_parameters data = { 0 };
	struct v90_analog_path analog;
	struct v90_analog_path saved_analog;
	struct v90_digital_path path;
	struct v90_digital_path saved;
	struct v90_pcm_stream_frame downstream;
	struct v90_pcm_stream_frame saved_downstream;
	struct v34_received_mapping_frame received;
	struct v34_received_mapping_frame saved_received;
	uint8_t octet = 0x5a;

	memset(&analog, 0xa5, sizeof(analog));
	saved_analog = analog;
	memset(&path, 0xa5, sizeof(path));
	saved = path;
	memset(&downstream, 0xa5, sizeof(downstream));
	saved_downstream = downstream;
	memset(&received, 0xa5, sizeof(received));
	saved_received = received;
	CHECK(v90_analog_path_init(NULL, &analog_handoff, &data) ==
		      V90_ANALOG_PATH_INVALID_ARGUMENT &&
	      v90_analog_path_init(&analog, NULL, &data) ==
		      V90_ANALOG_PATH_INVALID_ARGUMENT &&
	      v90_analog_path_init(&analog, &analog_handoff, &data) ==
		      V90_ANALOG_PATH_INVALID_PARAMETERS &&
	      memcmp(&analog, &saved_analog, sizeof(analog)) == 0 &&
	      v90_analog_path_transmit(NULL, NULL, NULL, &octet) ==
		      V90_ANALOG_PATH_INVALID_ARGUMENT &&
	      v90_analog_path_receive(NULL, 0, &downstream) ==
		      V90_ANALOG_PATH_INVALID_ARGUMENT &&
	      octet == 0x5a &&
	      memcmp(&downstream, &saved_downstream,
		     sizeof(downstream)) == 0 &&
	      !v90_analog_path_is_valid(NULL) &&
	      !v90_analog_path_has_failed(NULL) &&
	      !v90_analog_path_is_online(NULL) &&
	      strcmp(v90_analog_path_result_name(
		     V90_ANALOG_PATH_DOWNSTREAM_FRAME),
		     "downstream frame") == 0 &&
	      strcmp(v90_analog_path_result_name(
		     (enum v90_analog_path_result)99),
		     "unknown") == 0 &&
	      v90_digital_path_init(
		      NULL, &handoff, V90_JD_RATE_CAPABILITY_MASK,
		      V34_CONSTELLATION_4_POINT) ==
		      V90_DIGITAL_PATH_INVALID_ARGUMENT &&
	      v90_digital_path_init(
		      &path, NULL, V90_JD_RATE_CAPABILITY_MASK,
		      V34_CONSTELLATION_4_POINT) ==
		      V90_DIGITAL_PATH_INVALID_ARGUMENT &&
	      v90_digital_path_init(
		      &path, &handoff, 0,
		      V34_CONSTELLATION_4_POINT) ==
		      V90_DIGITAL_PATH_INVALID_ARGUMENT &&
	      v90_digital_path_init(
		      &path, &handoff, V90_JD_RATE_CAPABILITY_MASK,
		      (enum v34_constellation_size)99) ==
		      V90_DIGITAL_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      v90_digital_path_sample(
		      NULL, 0, NULL, NULL, &octet, &received) ==
		      V90_DIGITAL_PATH_INVALID_ARGUMENT &&
	      octet == 0x5a &&
	      memcmp(&received, &saved_received, sizeof(received)) == 0 &&
	      !v90_digital_path_is_valid(NULL) &&
	      !v90_digital_path_has_failed(NULL) &&
	      !v90_digital_path_is_online(NULL) &&
	      strcmp(v90_digital_path_result_name(
		     V90_DIGITAL_PATH_UPSTREAM_FRAME),
		     "upstream frame") == 0 &&
	      strcmp(v90_digital_path_result_name(
		     (enum v90_digital_path_result)99),
		     "unknown") == 0,
	      "path contracts changed rejected state or outputs");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] paired station exchange\n");
	if (!test_paired_exchange()) {
		fprintf(stderr, "[  FAILED  ] paired station exchange\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] paired station exchange\n");
	fprintf(stderr, "[ RUN      ] online path contracts\n");
	if (!test_path_contracts())
		return 1;
	fprintf(stderr, "[       OK ] online path contracts\n");
	return 0;
}
