// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_analog_station.h>
#include <v90_phase4_digital_station.h>

#include <g711.h>

#include "test.h"
#include "v90_phase4_station_fixture.h"

#include <string.h>

enum {
	TRAINING_UCODE = 91,
	SAMPLE_LIMIT = 12000,
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

static bool trn2d_symbols_match(
	const struct v90_trn2d_symbol *left,
	const struct v90_trn2d_symbol *right)
{
	return left->pcm.ucode == right->pcm.ucode &&
	       left->pcm.positive == right->pcm.positive &&
	       left->pcm.octet == right->pcm.octet &&
	       left->pcm.sample == right->pcm.sample &&
	       left->shaping_rule == right->shaping_rule &&
	       left->shaping_rule_valid == right->shaping_rule_valid &&
	       left->shaping_frame_start == right->shaping_frame_start;
}

static bool verify_trn2d_handoffs(
	struct v90_phase4_digital_transmit_handoff *digital,
	struct v90_phase4_analog_receive_handoff *analog)
{
	struct v90_trn2d_tx expected;
	const struct v90_pcm_mapping *mapping =
		&analog->receiver.decoder.mapping;

	CHECK(v90_trn2d_tx_init(
		      &expected, mapping) ==
		      V90_TRN2D_TX_OK,
	      "analogue mapping could not initialize TRN2d model");
	for (size_t symbol = 0; symbol < 48; ++symbol) {
		struct v90_trn2d_symbol actual_symbol;
		struct v90_trn2d_symbol expected_symbol;
		struct v90_trn2d_frame frame;
		struct v90_trn2d_frame untouched;
		enum v90_trn2d_rx_result receive_result;

		memset(&frame, 0xa5, sizeof(frame));
		untouched = frame;

		CHECK(v90_trn2d_tx_next(
			      &digital->transmitter, &actual_symbol) ==
			      V90_TRN2D_TX_OK &&
		      v90_trn2d_tx_next(&expected, &expected_symbol) ==
			      V90_TRN2D_TX_OK &&
		      trn2d_symbols_match(
			      &actual_symbol, &expected_symbol),
		      "paired TRN2d symbol %zu differed", symbol);
		receive_result = v90_trn2d_rx_receive_octet(
			&analog->receiver, actual_symbol.pcm.octet, &frame);
		if ((symbol + 1u) % V90_CP_FRAME_INTERVALS != 0) {
			CHECK(receive_result == V90_TRN2D_RX_BUFFERING &&
			      memcmp(&frame, &untouched, sizeof(frame)) == 0,
			      "TRN2d frame advanced early at symbol %zu", symbol);
			continue;
		}
		CHECK(receive_result == V90_TRN2D_RX_FRAME_READY &&
		      frame.bit_count == mapping->frame_bits &&
		      frame.training_bit_errors == 0,
		      "TRN2d frame %zu did not decode cleanly",
		      symbol / V90_CP_FRAME_INTERVALS);
		for (size_t bit = 0; bit < frame.bit_count; ++bit)
			CHECK(frame.bits[bit] == 1,
			      "TRN2d frame %zu bit %zu was not training data",
			      symbol / V90_CP_FRAME_INTERVALS, bit);
	}
	CHECK(digital->transmitter.transmitted_symbols == 48 &&
	      expected.transmitted_symbols == 48 &&
	      analog->receiver.received_symbols == 48 &&
	      analog->receiver.received_frames == 8 &&
	      analog->receiver.received_bits ==
		      8u * mapping->frame_bits &&
	      analog->receiver.training_bit_errors == 0 &&
	      v90_trn2d_rx_is_valid(&analog->receiver),
	      "paired TRN2d handoff lost its exact Ri boundary");
	return true;
}

static bool verify_cp_handoffs(
	enum g711_law law, struct v90_phase4_cp_line_tx *transmit,
	struct v90_phase4_cp_line_rx *receive,
	const struct v90_cp_parameters *training)
{
	struct v90_cp_parameters data = *training;
	bool received = false;

	data.kind = V90_CP_DATA;
	data.selected_rate_index = 1;
	data.acknowledges_peer = true;
	CHECK(v90_phase4_cp_line_tx_start_next_record(transmit, &data) ==
		      V90_PHASE4_CP_LINE_TX_OK,
	      "analogue CP handoff could not start CP");
	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		struct v90_cp_receive_event event = { 0 };
		enum v90_phase4_cp_line_rx_result result;
		int16_t sample;

		if (v90_phase4_cp_line_tx_record_is_complete(transmit)) {
			CHECK(v90_phase4_cp_line_tx_start_next_record(
				      transmit, &data) ==
				      V90_PHASE4_CP_LINE_TX_OK,
			      "analogue CP handoff could not repeat CP");
		}
		CHECK(v90_phase4_cp_line_tx_sample(transmit, &sample) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "analogue CP handoff failed at clock %zu", clock);
		result = v90_phase4_cp_line_rx_sample(
			receive, quantize(law, sample), &event);
		CHECK(result == V90_PHASE4_CP_LINE_RX_OK ||
		      result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
		      result == V90_PHASE4_CP_LINE_RX_RECORD_READY,
		      "digital CP handoff failed at clock %zu", clock);
		if (result == V90_PHASE4_CP_LINE_RX_RECORD_READY) {
			CHECK(v90_phase4_cp_line_rx_pending_record_is_valid(
				      receive),
			      "continued CP record failed its frame checks");
			if (event.parameters.kind == V90_CP_TRAINING) {
				CHECK(parameters_match(
					      &event.parameters, training),
				      "trailing CPt changed at the handoff");
			} else {
				CHECK(parameters_match(&event.parameters, &data),
				      "continued CP changed at the handoff");
				received = true;
			}
			CHECK(v90_phase4_cp_line_rx_continue(receive) ==
				      V90_PHASE4_CP_LINE_RX_OK,
			      "continued CP record was not released");
			if (received)
				break;
		}
	}
	CHECK(received, "continued CP record was not recovered");
	return true;
}

static bool run_exchange(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_jd_parameters jd = jd_parameters(constellation);
	struct v90_cp_parameters training = training_parameters(law);
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
		      &digital, &digital_transmit) ==
		      V90_PHASE4_DIGITAL_STATION_OK &&
	      mapping_matches(
		      &analog_receive.receiver.decoder.mapping, &training) &&
	      mapping_matches(
		      &digital_transmit.transmitter.encoder.mapping,
		      &training),
	      "paired owners disagreed about their Phase 4 handoffs");
	CHECK(verify_trn2d_handoffs(&digital_transmit, &analog_receive),
	      "paired owners disagreed after the Ri-to-TRN2d boundary");
	CHECK(verify_cp_handoffs(
		      law, &analog_transmit, &digital_receive, &training),
	      "paired CP histories did not continue");
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
