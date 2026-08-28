// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase4_e_line_rx.h>
#include <v90_phase4_e_line_tx.h>
#include <v90_upstream_data.h>
#include <v90_upstream_rx.h>

#include "test.h"
#include "v90_phase4_line_fixture.h"

#include <string.h>

enum { SAMPLE_LIMIT = 70000 };

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static uint16_t rate_bit(uint8_t rate_2400_bps_units)
{
	return (uint16_t)(1u <<
		(rate_2400_bps_units -
		 V90_MP_MINIMUM_RATE_2400_BPS_UNITS));
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters cp_parameters(
	enum v90_cp_kind kind, enum g711_law law, bool acknowledged)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = rate_bit(4),
		.trn1d_ratio_q3_13 = 0x2000,
		.shaping_a1_q1_6 = -12,
		.shaping_a2_q1_6 = 6,
		.shaping_b1_q1_6 = -3,
		.shaping_b2_q1_6 = 24,
		.selected_rate_index = 1,
		.constellation_count = 1,
		.acknowledges_peer = acknowledged,
	};

	add_ucode(&parameters.transmit_constellations[0], 36);
	add_ucode(&parameters.transmit_constellations[0], 72);
	return parameters;
}

static struct v90_mp_record peer_mp(void)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 4,
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_rate_capability_mask = rate_bit(4),
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

static bool run_line(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_cp_parameters training =
		cp_parameters(V90_CP_TRAINING, law, false);
	struct v90_cp_parameters final =
		cp_parameters(V90_CP_DATA, law, true);
	struct v90_mp_record mp = peer_mp();
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx cp_tx;
	struct v90_phase4_cp_line_rx cp_rx;
	struct v90_phase4_e_line_tx e_tx;
	struct v90_phase4_e_line_rx e_rx;
	struct v34_data_stream data_tx;
	struct v34_data_line_receiver data_rx;
	bool final_started = false;
	bool cp_tx_finishing = false;
	bool e_tx_started = false;
	bool data_tx_started = false;
	bool training_received = false;
	bool e_rx_started = false;
	bool data_rx_started = false;
	bool b1_complete = false;
	unsigned int final_records = 0;

	CHECK(v90_phase4_line_fixture_init(
		      law, constellation, &training, &cp_tx, &phase3) &&
	      v90_phase4_cp_line_rx_init(
		      &cp_rx, &phase3, constellation, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_OK,
	      "CP/E fixture did not initialize law=%d constellation=%d",
	      law, constellation);
	for (size_t clock = 0; clock < SAMPLE_LIMIT && !b1_complete; ++clock) {
		int16_t sample = 0;
		bool sample_ready = false;

		if (!final_started &&
		    v90_phase4_cp_line_tx_record_is_complete(&cp_tx)) {
			CHECK(v90_phase4_cp_line_tx_start_next_record(
				      &cp_tx, &final) ==
				      V90_PHASE4_CP_LINE_TX_OK,
			      "final CP did not start at sample %zu", clock);
			final_started = true;
			final_records = 1;
		}
		if (final_started &&
		    v90_phase4_cp_line_tx_record_is_complete(&cp_tx)) {
			if (final_records == 1) {
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, &final) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "repeated final CP did not start at sample %zu",
				      clock);
				final_records++;
			} else {
				cp_tx_finishing = true;
			}
		}
		if (!e_tx_started) {
			enum v90_phase4_cp_line_tx_result result =
				v90_phase4_cp_line_tx_sample(&cp_tx, &sample);

			CHECK(result == V90_PHASE4_CP_LINE_TX_OK ||
			      (cp_tx_finishing &&
			       result == V90_PHASE4_CP_LINE_TX_COMPLETE),
			      "CP line stopped at sample %zu: %s", clock,
			      v90_phase4_cp_line_tx_result_name(result));
			if (result == V90_PHASE4_CP_LINE_TX_OK) {
				sample_ready = true;
			} else {
				CHECK(v90_phase4_e_line_tx_init(&e_tx, &cp_tx) ==
					      V90_PHASE4_E_LINE_TX_OK,
				      "final CP did not hand off to E");
				e_tx_started = true;
			}
		}
		if (!sample_ready && e_tx_started && !data_tx_started) {
			enum v90_phase4_e_line_tx_result result =
				v90_phase4_e_line_tx_sample(&e_tx, &sample);

			CHECK(result == V90_PHASE4_E_LINE_TX_OK ||
			      result == V90_PHASE4_E_LINE_TX_COMPLETE,
			      "E transmitter stopped at sample %zu: %s", clock,
			      v90_phase4_e_line_tx_result_name(result));
			if (result == V90_PHASE4_E_LINE_TX_OK) {
				sample_ready = true;
			} else {
				CHECK(v90_upstream_data_start_after_e(
					      &data_tx, &e_tx, &mp) ==
					      V90_UPSTREAM_DATA_OK,
				      "E did not hand off to B1");
				data_tx_started = true;
			}
		}
		if (!sample_ready && data_tx_started) {
			bool accepted;

			CHECK(v34_data_stream_sample(
				      &data_tx, NULL, NULL, &sample,
				      &accepted) == V34_DATA_STREAM_OK,
			      "B1 source stopped at sample %zu", clock);
			(void)accepted;
			sample_ready = true;
		}
		CHECK(sample_ready, "line produced no sample at clock %zu", clock);
		sample = quantize(law, sample);
		if (!e_rx_started) {
			struct v90_cp_receive_event event = { 0 };
			enum v90_phase4_cp_line_rx_result result =
				v90_phase4_cp_line_rx_sample(
					&cp_rx, sample, &event);

			CHECK(result == V90_PHASE4_CP_LINE_RX_OK ||
			      result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
			      result == V90_PHASE4_CP_LINE_RX_RECORD_READY,
			      "CP receiver stopped at sample %zu: %s", clock,
			      v90_phase4_cp_line_rx_result_name(result));
			if (result == V90_PHASE4_CP_LINE_RX_RECORD_READY) {
				struct v90_cp_parameters pending;
				const struct v90_cp_parameters *expected =
					training_received ? &final : &training;

				CHECK(v90_phase4_cp_line_rx_pending_parameters(
					      &cp_rx, &pending) ==
					      V90_PHASE4_CP_LINE_RX_OK &&
				      parameters_match(&pending, expected) &&
				      parameters_match(&event.parameters, expected),
				      "pending CP record changed at sample %zu",
				      clock);
				if (!training_received) {
					struct v90_phase4_e_line_rx rejected;
					struct v90_phase4_e_line_rx saved_rejected;

					memset(&rejected, 0xa5, sizeof(rejected));
					saved_rejected = rejected;
					CHECK(v90_phase4_e_line_rx_init(
						      &rejected, &cp_rx) ==
						      V90_PHASE4_E_LINE_RX_INVALID_STATE &&
					      memcmp(&rejected, &saved_rejected,
						     sizeof(rejected)) == 0,
					      "E accepted the training CP record");
					training_received = true;
					CHECK(v90_phase4_cp_line_rx_continue(
						      &cp_rx) ==
						      V90_PHASE4_CP_LINE_RX_OK,
					      "CPt receiver did not continue");
				} else {
					CHECK(v90_phase4_e_line_rx_init(
						      &e_rx, &cp_rx) ==
						      V90_PHASE4_E_LINE_RX_OK,
					      "CP prime did not hand off to E");
					{
						struct v34_data_line_receiver rejected;
						struct v34_data_line_receiver saved_rejected;
						struct v34_data_line_receiver_configuration
							invalid = { 0 };

						memset(&rejected, 0xa5, sizeof(rejected));
						saved_rejected = rejected;
						CHECK(v90_phase4_e_line_rx_start_data(
							      &e_rx, &rejected, &invalid) ==
							      V90_PHASE4_E_LINE_RX_INVALID_STATE &&
						      memcmp(&rejected, &saved_rejected,
							     sizeof(rejected)) == 0,
						      "E exported data before completion");
					}
					e_rx_started = true;
				}
			}
		} else if (!data_rx_started) {
			enum v90_phase4_e_line_rx_result result =
				v90_phase4_e_line_rx_sample(&e_rx, sample);

			CHECK(result == V90_PHASE4_E_LINE_RX_OK ||
			      result == V90_PHASE4_E_LINE_RX_SYMBOL ||
			      result == V90_PHASE4_E_LINE_RX_E_COMPLETE,
			      "E receiver stopped at sample %zu: %s", clock,
			      v90_phase4_e_line_rx_result_name(result));
			if (result == V90_PHASE4_E_LINE_RX_E_COMPLETE) {
				CHECK(data_tx_started &&
				      v90_upstream_rx_start_after_e(
					      &data_rx, &e_rx, &mp) ==
					      V90_UPSTREAM_DATA_OK,
				      "E receiver did not hand off to B1");
				data_rx_started = true;
			}
		} else {
			struct v34_survivor_decision current;
			struct v34_received_mapping_frame received;
			enum v34_data_receive_result result =
				v34_data_line_receiver_sample(
					&data_rx, sample, &current, &received);

			CHECK(result != V34_DATA_RECEIVE_FAILED &&
			      result != V34_DATA_RECEIVE_INVALID_ARGUMENT &&
			      result != V34_DATA_RECEIVE_NUMERIC_FAILURE &&
			      result != V34_DATA_RECEIVE_OUT_OF_RANGE,
			      "B1 receiver stopped at sample %zu: %d", clock,
			      result);
			b1_complete =
				result == V34_DATA_RECEIVE_B1_COMPLETE;
		}
	}
	CHECK(final_started && final_records == 2 && e_tx_started && data_tx_started &&
	      training_received && e_rx_started && data_rx_started &&
	      b1_complete && v90_phase4_e_line_rx_is_complete(&e_rx) &&
	      !v90_phase4_e_line_rx_has_failed(&e_rx) &&
	      !v34_data_line_receiver_has_failed(&data_rx) &&
	      data_rx.equalizer_receiver.data_receiver.data_receiver
			      .b1_error_bits == 0,
	      "CP/E/B1 path stopped law=%d constellation=%d "
	      "tx=%d/%d/%d rx=%d/%d/%d/%d Ebits=%u B1errors=%u",
	      law, constellation, final_started, e_tx_started,
	      data_tx_started, training_received, e_rx_started,
	      data_rx_started, b1_complete,
	      e_rx_started ? e_rx.symbols.received_bits : 0,
	      (unsigned int)(data_rx_started ?
		      data_rx.equalizer_receiver.data_receiver.data_receiver
			      .b1_error_bits :
		      0));
	{
		struct v90_phase4_e_line_rx saved = e_rx;
		struct v90_phase4_e_line_rx incomplete = e_rx;
		struct v90_mp_record cleardown = mp;
		struct v34_data_line_receiver rejected;
		struct v34_data_line_receiver saved_rejected;

		incomplete.symbols.received_bits--;
		cleardown.parameters.type0
			.maximum_upstream_rate_2400_bps_units = 0;
		memset(&rejected, 0xa5, sizeof(rejected));
		saved_rejected = rejected;
		CHECK(v90_phase4_e_line_rx_sample(&e_rx, 1234) ==
			      V90_PHASE4_E_LINE_RX_COMPLETE &&
		      memcmp(&e_rx, &saved, sizeof(e_rx)) == 0 &&
		      v90_upstream_rx_start_after_e(NULL, &e_rx, &mp) ==
			      V90_UPSTREAM_DATA_INVALID_ARGUMENT &&
		      v90_upstream_rx_start_after_e(
			      &rejected, NULL, &mp) ==
			      V90_UPSTREAM_DATA_INVALID_ARGUMENT &&
		      v90_upstream_rx_start_after_e(
			      &rejected, &e_rx, NULL) ==
			      V90_UPSTREAM_DATA_INVALID_ARGUMENT &&
		      v90_upstream_rx_start_after_e(
			      &rejected, &incomplete, &mp) ==
			      V90_UPSTREAM_DATA_INVALID_STATE &&
		      v90_upstream_rx_start_after_e(
			      &rejected, &e_rx, &cleardown) ==
			      V90_UPSTREAM_DATA_NO_COMMON_RATE &&
		      memcmp(&rejected, &saved_rejected,
			     sizeof(rejected)) == 0,
		      "completed-E handoff contracts changed state");
	}
	return true;
}

static bool test_generated_lines(void)
{
	CHECK(run_line(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT),
	      "PCMU four-point line failed");
	CHECK(run_line(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT),
	      "PCMA sixteen-point line failed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_cp_parameters training =
		cp_parameters(V90_CP_TRAINING, G711_LAW_PCMU, false);
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx cp_tx;
	struct v90_phase4_cp_line_rx cp_rx;
	struct v90_phase4_e_line_rx untouched;
	struct v90_phase4_e_line_rx saved;
	struct v90_cp_parameters pending = training;

	CHECK(v90_phase4_line_fixture_init(
		      G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      &training, &cp_tx, &phase3) &&
	      v90_phase4_cp_line_rx_init(
		      &cp_rx, &phase3, V34_CONSTELLATION_4_POINT,
		      0.05) == V90_PHASE4_CP_LINE_RX_OK,
	      "contract fixture did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(v90_phase4_e_line_rx_init(NULL, &cp_rx) ==
		      V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_e_line_rx_init(&untouched, NULL) ==
		      V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_e_line_rx_init(&untouched, &cp_rx) ==
		      V90_PHASE4_E_LINE_RX_INVALID_STATE &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "E accepted a receiver without final CP");
	CHECK(v90_phase4_cp_line_rx_pending_parameters(NULL, &pending) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_pending_parameters(&cp_rx, NULL) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_pending_parameters(&cp_rx, &pending) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_STATE &&
	      parameters_match(&pending, &training),
	      "rejected pending-parameter read changed its output");
	CHECK(strcmp(v90_phase4_e_line_rx_result_name(
		     V90_PHASE4_E_LINE_RX_E_COMPLETE),
		     "E complete") == 0 &&
	      strcmp(v90_phase4_e_line_rx_result_name(
		     (enum v90_phase4_e_line_rx_result)99),
		     "unknown") == 0,
	      "E result names changed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] generated G.711 lines\n");
	if (!test_generated_lines())
		return 1;
	fprintf(stderr, "[       OK ] generated G.711 lines\n");
	fprintf(stderr, "[ RUN      ] contracts\n");
	if (!test_contracts())
		return 1;
	fprintf(stderr, "[       OK ] contracts\n");
	return 0;
}
