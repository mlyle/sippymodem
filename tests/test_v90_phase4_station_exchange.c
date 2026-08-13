// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_analog_station.h>
#include <v90_phase4_digital_station.h>
#include <v90_phase4_e_line_tx.h>
#include <v90_upstream_data.h>
#include <v90_downstream_data_handoff.h>
#include <v90_downstream_data_rx.h>
#include <v90_downstream_data_tx.h>

#include <g711.h>

#include "test.h"
#include "v90_phase4_station_fixture.h"

#include <string.h>

enum {
	TRAINING_UCODE = 91,
	SAMPLE_LIMIT = 12000,
	DOWNSTREAM_SYMBOL_LIMIT = 4096,
	DATA_FRAMES_TO_RECEIVE = 4,
	DATA_FRAME_LIMIT = 16,
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
	struct v90_cp_constellation *constellation, uint8_t ucode)
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
	       memcmp(mapping, &expected, sizeof(expected)) == 0;
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool mp_acknowledges_cp(const struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       record->parameters.type0.acknowledges_cp :
		       record->parameters.type1.common.acknowledges_cp;
}

static void generate_data_bits(
	uint8_t *bits, size_t bit_count, size_t frame)
{
	for (size_t bit = 0; bit < bit_count; ++bit)
		bits[bit] = (uint8_t)(
			(frame * 13u + bit * 7u + (frame >> (bit % 3u))) & 1u);
}

static bool receive_data_output(
	struct v90_downstream_data_rx *receiver,
	const struct v90_pcm_frame_output *output,
	const uint8_t expected[DATA_FRAME_LIMIT][V90_PCM_FRAME_MAX_BITS],
	size_t accepted_data_frames, size_t *b1d_frames,
	size_t *data_frames)
{
	for (size_t symbol = 0; symbol < output->symbol_count; ++symbol) {
		struct v90_pcm_stream_frame frame;
		enum v90_downstream_data_rx_result result =
			v90_downstream_data_rx_receive_octet(
				receiver, output->symbols[symbol].octet, &frame);

		if (result == V90_DOWNSTREAM_DATA_RX_BUFFERING)
			continue;
		if (result == V90_DOWNSTREAM_DATA_RX_B1D_FRAME ||
		    result == V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE) {
			for (size_t bit = 0; bit < frame.bit_count; ++bit)
				CHECK(frame.bits[bit] == 1,
				      "B1d frame %zu bit %zu changed",
				      *b1d_frames, bit);
			(*b1d_frames)++;
			CHECK((result == V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE) ==
				      (*b1d_frames == V90_PHASE4_B1D_FRAMES),
			      "B1d completion moved to frame %zu", *b1d_frames);
			continue;
		}
		CHECK(result == V90_DOWNSTREAM_DATA_RX_DATA_FRAME &&
		      *data_frames < accepted_data_frames &&
		      memcmp(frame.bits, expected[*data_frames],
			     frame.bit_count) == 0,
		      "post-B1d data frame %zu changed (%s)",
		      *data_frames,
		      v90_downstream_data_rx_result_name(result));
		(*data_frames)++;
	}
	return true;
}

static bool verify_data_epoch(
	const struct v90_downstream_tx *completed_transmit,
	const struct v90_downstream_rx *completed_receive,
	const struct v90_pcm_mapping *data_mapping)
{
	struct v90_downstream_data_tx transmit;
	struct v90_downstream_data_rx receive;
	uint8_t expected[DATA_FRAME_LIMIT][V90_PCM_FRAME_MAX_BITS] = { 0 };
	size_t accepted_data_frames = 0;
	size_t b1d_frames = 0;
	size_t data_frames = 0;

	CHECK(v90_downstream_data_tx_start_after_ed(
		      &transmit, completed_transmit, data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      v90_downstream_data_rx_start_after_ed(
		      &receive, completed_receive, data_mapping) ==
		      V90_DOWNSTREAM_DATA_RX_OK,
	      "completed Ed did not start B1d");
	for (size_t frame = 0; frame < V90_PHASE4_B1D_FRAMES; ++frame) {
		struct v90_pcm_frame_output output;
		enum v90_downstream_data_tx_result result =
			v90_downstream_data_tx_next_b1d(&transmit, &output);

		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "B1d source frame %zu failed", frame);
		if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
			CHECK(receive_data_output(
				      &receive, &output, expected,
				      accepted_data_frames, &b1d_frames,
				      &data_frames),
			      "B1d output frame %zu failed", frame);
	}
	while (data_frames < DATA_FRAMES_TO_RECEIVE &&
	       accepted_data_frames < DATA_FRAME_LIMIT) {
		struct v90_pcm_frame_output output;
		uint8_t *bits = expected[accepted_data_frames];
		enum v90_downstream_data_tx_result result;

		generate_data_bits(
			bits, data_mapping->frame_bits,
			accepted_data_frames + 1u);
		result = v90_downstream_data_tx_process(
			&transmit, bits, data_mapping->frame_bits, &output);
		CHECK(result == V90_DOWNSTREAM_DATA_TX_BUFFERING ||
		      result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
		      "data source frame %zu failed", accepted_data_frames);
		accepted_data_frames++;
		if (result == V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY)
			CHECK(receive_data_output(
				      &receive, &output, expected,
				      accepted_data_frames, &b1d_frames,
				      &data_frames),
			      "data output frame %zu failed",
			      accepted_data_frames - 1u);
	}
	CHECK(v90_downstream_data_tx_b1d_complete(&transmit) &&
	      v90_downstream_data_rx_b1d_complete(&receive) &&
	      b1d_frames == V90_PHASE4_B1D_FRAMES &&
	      receive.b1d_bit_errors == 0 &&
	      data_frames == DATA_FRAMES_TO_RECEIVE,
	      "B1d/data epoch stopped at %zu B1d and %zu data frames",
	      b1d_frames, data_frames);
	return true;
}

static bool verify_final_downstream_exchange(
	enum g711_law law, struct v90_phase4_cp_line_tx *cp_transmit,
	struct v90_phase4_cp_line_rx *cp_receive,
	struct v90_phase4_digital_transmit_handoff *digital,
	struct v90_phase4_analog_receive_handoff *analog,
	const struct v90_cp_parameters *training)
{
	struct v90_cp_parameters active_cp = *training;
	struct v90_cp_parameters accepted_cp;
	struct v90_pcm_mapping data_mapping;
	struct v90_phase4_e_line_tx e_transmit;
	struct v34_data_stream upstream_data;
	size_t training_frames = 0;
	size_t downstream_symbols = 0;
	bool cp_received = false;
	bool cp_prime_received = false;
	bool cp_prime_sent = false;
	bool cp_finishing = false;
	bool cp_stopped = false;
	bool e_started = false;
	bool e_complete = false;
	bool upstream_started = false;
	bool upstream_b1_complete = false;
	bool mp_received = false;
	bool mp_prime_received = false;
	bool ed_armed = false;
	bool ed_received = false;

	active_cp.kind = V90_CP_DATA;
	active_cp.selected_rate_index = 1;
	active_cp.acknowledges_peer = false;
	CHECK(v90_pcm_mapping_init(&data_mapping, &active_cp) ==
		      V90_PCM_MAPPING_OK &&
	      v90_phase4_cp_line_tx_start_next_record(
		      cp_transmit, &active_cp) ==
		      V90_PHASE4_CP_LINE_TX_OK,
	      "data CP did not start at the CPt boundary");

	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		if (!cp_stopped && !cp_finishing &&
		    v90_phase4_cp_line_tx_record_is_complete(cp_transmit)) {
			if (active_cp.acknowledges_peer)
				cp_prime_sent = true;
			if (cp_prime_sent && mp_prime_received) {
				cp_finishing = true;
			} else {
				active_cp.acknowledges_peer = mp_received;
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      cp_transmit, &active_cp) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "CP repetition failed at clock %zu", clock);
			}
		}

		if (!cp_stopped || (e_started && !e_complete) ||
		    (upstream_started && !upstream_b1_complete)) {
			struct v90_cp_receive_event event = { 0 };
			enum v90_phase4_cp_line_rx_result result;
			bool sample_ready = false;
			int16_t sample;

			if (!cp_stopped) {
				enum v90_phase4_cp_line_tx_result transmit_result =
					v90_phase4_cp_line_tx_sample(
						cp_transmit, &sample);

				CHECK(transmit_result == V90_PHASE4_CP_LINE_TX_OK ||
				      transmit_result ==
					      V90_PHASE4_CP_LINE_TX_COMPLETE,
				      "CP line stopped at clock %zu", clock);
				if (transmit_result == V90_PHASE4_CP_LINE_TX_OK) {
					sample_ready = true;
				} else {
					CHECK(v90_phase4_e_line_tx_init(
						      &e_transmit, cp_transmit) ==
						      V90_PHASE4_E_LINE_TX_OK,
					      "CP prime did not hand off to E");
					cp_stopped = true;
					e_started = true;
				}
			}
			if (!sample_ready && e_started && !e_complete) {
				enum v90_phase4_e_line_tx_result transmit_result =
					v90_phase4_e_line_tx_sample(
						&e_transmit, &sample);

				CHECK(transmit_result == V90_PHASE4_E_LINE_TX_OK ||
				      transmit_result ==
					      V90_PHASE4_E_LINE_TX_COMPLETE,
				      "E line stopped at clock %zu", clock);
				if (transmit_result == V90_PHASE4_E_LINE_TX_OK)
					sample_ready = true;
				else {
					e_complete = true;
					CHECK(v90_upstream_data_start_after_e(
						      &upstream_data, &e_transmit,
						      &digital->transmitter.mp_record) ==
						      V90_UPSTREAM_DATA_OK,
					      "E did not hand off to upstream B1");
					upstream_started = true;
				}
			}
			if (!sample_ready && upstream_started &&
			    !upstream_b1_complete) {
				bool accepted;

				CHECK(v34_data_stream_sample(
					      &upstream_data, NULL, NULL, &sample,
					      &accepted) == V34_DATA_STREAM_OK,
				      "upstream B1 failed at clock %zu", clock);
				sample_ready = true;
				upstream_b1_complete =
					!v34_data_stream_is_transmitting_b1(
						&upstream_data);
			}
			if (sample_ready && !cp_prime_received) {
				result = v90_phase4_cp_line_rx_sample(
					cp_receive, quantize(law, sample), &event);
				CHECK(result == V90_PHASE4_CP_LINE_RX_OK ||
				      result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
				      result == V90_PHASE4_CP_LINE_RX_RECORD_READY,
				      "CP receive failed at clock %zu", clock);
			}
			if (sample_ready && !cp_prime_received &&
			    result == V90_PHASE4_CP_LINE_RX_RECORD_READY) {
				CHECK(v90_phase4_cp_line_rx_pending_record_is_valid(
					      cp_receive),
				      "continued CP failed its frame checks");
				if (event.parameters.kind == V90_CP_DATA) {
					struct v90_cp_parameters normalized =
						event.parameters;

					normalized.acknowledges_peer = false;
					struct v90_cp_parameters expected = active_cp;

					expected.acknowledges_peer = false;
					CHECK(parameters_match(
						      &normalized, &expected),
					      "received CP parameters changed");
					if (!cp_received) {
						accepted_cp = normalized;
						cp_received = true;
						CHECK(v90_downstream_tx_request_mp_prime(
							      &digital->transmitter) ==
							      V90_DOWNSTREAM_TX_OK,
						      "CP did not authorize MP prime");
					} else {
						CHECK(parameters_match(
							      &normalized,
							      &accepted_cp),
						      "repeated CP changed");
					}
					if (event.parameters.acknowledges_peer &&
					    !cp_prime_received) {
						cp_prime_received = true;
						CHECK(v90_downstream_tx_request_ed(
							      &digital->transmitter) ==
							      V90_DOWNSTREAM_TX_OK,
						      "CP prime did not authorize Ed");
					}
				}
				CHECK(v90_phase4_cp_line_rx_continue(cp_receive) ==
					      V90_PHASE4_CP_LINE_RX_OK,
				      "CP receiver did not resume");
			}
		}

		if (!v90_downstream_tx_is_complete(&digital->transmitter)) {
			struct v90_downstream_symbol symbol;
			struct v90_downstream_event event;
			enum v90_downstream_tx_result tx_result =
				v90_downstream_tx_next(
					&digital->transmitter, &symbol);

			CHECK(tx_result == V90_DOWNSTREAM_TX_OK ||
			      tx_result == V90_DOWNSTREAM_TX_COMPLETE,
			      "downstream transmit failed at clock %zu", clock);
			if (tx_result == V90_DOWNSTREAM_TX_OK) {
				enum v90_downstream_rx_result rx_result =
					v90_downstream_rx_receive(
						&analog->receiver,
						symbol.pcm.octet, &event);

				downstream_symbols++;
				if (rx_result == V90_DOWNSTREAM_RX_TRN2D_FRAME) {
					CHECK(event.trn2d.training_bit_errors == 0,
					      "TRN2d frame %zu changed",
					      training_frames);
					training_frames++;
				} else if (rx_result == V90_DOWNSTREAM_RX_MP) {
					CHECK(!mp_acknowledges_cp(
						      &event.mp.record),
					      "ordinary MP acknowledged CP");
					mp_received = true;
				} else if (rx_result ==
					   V90_DOWNSTREAM_RX_MP_PRIME) {
					CHECK(mp_received &&
					      mp_acknowledges_cp(
						      &event.mp.record),
					      "MP prime preceded MP");
					mp_prime_received = true;
				} else if (rx_result == V90_DOWNSTREAM_RX_ED) {
					CHECK(ed_armed && event.ed.bit_errors == 0,
					      "Ed was early or corrupt");
					ed_received = true;
				} else {
					CHECK(rx_result ==
						      V90_DOWNSTREAM_RX_BUFFERING,
					      "downstream receive returned %s",
					      v90_downstream_rx_result_name(
						      rx_result));
				}
			}
		}

		if (!ed_armed && cp_prime_sent && mp_prime_received) {
			CHECK(v90_downstream_rx_expect_ed(&analog->receiver) ==
				      V90_DOWNSTREAM_RX_OK,
			      "reciprocal acknowledgements did not arm Ed");
			ed_armed = true;
		}
		if (v90_downstream_tx_is_complete(&digital->transmitter) &&
		    v90_downstream_rx_is_complete(&analog->receiver) &&
		    upstream_b1_complete)
			break;
	}

	CHECK(cp_received && cp_prime_received && cp_prime_sent &&
	      mp_received && mp_prime_received && ed_armed && ed_received &&
	      e_started && e_complete && upstream_started &&
	      upstream_b1_complete &&
	      training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      downstream_symbols < DOWNSTREAM_SYMBOL_LIMIT &&
	      v90_downstream_tx_is_complete(&digital->transmitter) &&
	      v90_downstream_rx_is_complete(&analog->receiver),
	      "paired control stopped: CP=%d/%d/%d MP=%d/%d Ed=%d/%d "
	      "E/B1=%d/%d/%d/%d "
	      "TRN=%zu symbols=%zu stages=%d/%d complete=%d/%d",
	      cp_received, cp_prime_received, cp_prime_sent,
	      mp_received, mp_prime_received, ed_armed, ed_received,
	      e_started, e_complete, upstream_started,
	      upstream_b1_complete,
	      training_frames, downstream_symbols,
	      digital->transmitter.stage, analog->receiver.stage,
	      v90_downstream_tx_is_complete(&digital->transmitter),
	      v90_downstream_rx_is_complete(&analog->receiver));
	CHECK(verify_data_epoch(
		      &digital->transmitter, &analog->receiver,
		      &data_mapping),
	      "paired B1d/data epoch failed");
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
		      law, &jd, TRAINING_UCODE, &training,
		      &analog, &phase3_receive),
	      "paired Phase 4 fixture did not reach CPt");
	CHECK(v90_phase4_digital_station_init(
		      &digital, &phase3_receive, &jd, law,
		      TRAINING_UCODE, 0.05) ==
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
	CHECK(verify_final_downstream_exchange(
		      law, &analog_transmit, &digital_receive,
		      &digital_transmit, &analog_receive, &training),
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

int main(void)
{
	fprintf(stderr, "[ RUN      ] paired station exchange\n");
	if (!test_paired_exchange()) {
		fprintf(stderr, "[  FAILED  ] paired station exchange\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] paired station exchange\n");
	return 0;
}
