// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase3_return_line_rx.h>
#include <v90_phase4_cp_line_rx.h>
#include <v90_phase4_cp_line_tx.h>

#include "test.h"
#include "v90_phase4_line_fixture.h"

#include <math.h>
#include <string.h>

enum {
	SAMPLE_LIMIT = 50000,
};

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static void add_ucode(
	struct v90_cp_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters parameters(
	enum v90_cp_kind kind, enum g711_law law, uint8_t variant)
{
	struct v90_cp_parameters selected = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = (uint16_t)(0x1555u ^ variant),
		.trn1d_ratio_q3_13 = (uint16_t)(0x2000u + variant),
		.shaping_a1_q1_6 = (int8_t)(-16 - variant),
		.shaping_a2_q1_6 = (int8_t)(8 + variant),
		.shaping_b1_q1_6 = (int8_t)(-1 - variant),
		.shaping_b2_q1_6 = (int8_t)(31 - variant),
		.selected_rate_index = (uint8_t)(1u + variant),
		.sign_redundancy = (uint8_t)(variant % 4u),
		.spectral_lookahead = (uint8_t)((variant + 1u) % 4u),
		.constellation_count = 1,
		.request_silence = variant != 0,
		.acknowledges_peer = variant != 0,
	};

	add_ucode(&selected.transmit_constellations[0], variant);
	add_ucode(&selected.transmit_constellations[0], 127u - variant);
	return selected;
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

static bool verify_line(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_cp_parameters training =
		parameters(V90_CP_TRAINING, law, 0);
	struct v90_cp_parameters data = parameters(V90_CP_DATA, law, 1);
	struct v90_cp_parameters filler = parameters(V90_CP_DATA, law, 2);
	const struct v90_cp_parameters *expected[] = { &training, &data };
	struct v90_phase3_return_line_handoff handoff;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_rx rx;
	unsigned int started_records = 1;
	unsigned int received_records = 0;

	CHECK(v90_phase4_line_fixture_init(
		      law, constellation, &training, &tx, &handoff),
	      "law/constellation %d/%d did not reach CPt", law,
	      constellation);
	CHECK(v90_phase4_cp_line_rx_init(
		      &rx, &handoff, constellation, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_OK,
	      "CP line receiver did not accept the delayed handoff");
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		struct v90_cp_receive_event event = { 0 };
		int16_t sample;
		enum v90_phase4_cp_line_rx_result result;

		if (v90_phase4_cp_line_tx_record_is_complete(&tx)) {
			const struct v90_cp_parameters *next =
				started_records == 1 ? &data : &filler;

			CHECK(v90_phase4_cp_line_tx_start_next_record(
				      &tx, next) == V90_PHASE4_CP_LINE_TX_OK,
			      "CP transmitter could not start record %u",
			      started_records);
			started_records++;
		}
		CHECK(v90_phase4_cp_line_tx_sample(&tx, &sample) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "CP transmitter stopped before record %u",
		      received_records);
		result = v90_phase4_cp_line_rx_sample(
			&rx, quantize(law, sample), &event);
		if (result != V90_PHASE4_CP_LINE_RX_RECORD_READY) {
			CHECK(result == V90_PHASE4_CP_LINE_RX_OK ||
			      result == V90_PHASE4_CP_LINE_RX_SYMBOL,
			      "CP line receiver reported %s at sample %u",
			      v90_phase4_cp_line_rx_result_name(result),
			      sample_index);
			continue;
		}
		CHECK(received_records < ARRAY_LENGTH(expected) &&
		      event.type == V90_CP_RECEIVE_FRAME &&
		      event.frame_result == V90_CP_OK &&
		      event.observed_bits ==
			      v90_cp_frame_bits(expected[received_records]) &&
		      parameters_match(
			      &event.parameters,
			      expected[received_records]) &&
		      v90_phase4_cp_line_rx_has_pending_record(&rx) &&
		      v90_phase4_cp_line_rx_pending_record_is_valid(&rx),
		      "CP line record %u changed", received_records);
		{
			struct v90_phase4_cp_line_rx saved = rx;
			struct v90_cp_receive_event sentinel = {
				.type = V90_CP_RECEIVE_REJECTED,
				.observed_bits = 777,
			};
			struct v90_cp_receive_event saved_event = sentinel;

			CHECK(v90_phase4_cp_line_rx_sample(
				      &rx, 0, &sentinel) ==
				      V90_PHASE4_CP_LINE_RX_INVALID_STATE &&
			      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
			      memcmp(&sentinel, &saved_event,
				     sizeof(sentinel)) == 0 &&
			      v90_phase4_cp_line_rx_continue(&rx) ==
				      V90_PHASE4_CP_LINE_RX_OK,
			      "CP record pause consumed another sample");
		}
		received_records++;
		if (received_records == ARRAY_LENGTH(expected))
			return !v90_phase4_cp_line_rx_has_failed(&rx);
	}
	CHECK(false, "CP line receiver did not complete two records");
	return false;
}

static bool test_continuous_g711_lines(void)
{
	CHECK(verify_line(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT),
	      "PCMU four-point CP line failed");
	CHECK(verify_line(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT),
	      "PCMA sixteen-point CP line failed");
	return true;
}

static bool test_transactional_contracts(void)
{
	struct v90_cp_parameters training =
		parameters(V90_CP_TRAINING, G711_LAW_PCMU, 0);
	struct v90_phase3_return_line_handoff handoff;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_rx rx;
	struct v90_phase4_cp_line_rx saved;
	struct v90_phase4_cp_line_rx untouched;
	struct v90_phase4_cp_line_rx untouched_saved;
	struct v90_cp_receive_event event = { 0 };

	CHECK(v90_phase4_line_fixture_init(
		      G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      &training, &tx, &handoff),
	      "CP validation fixture did not reach CPt");
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_phase4_cp_line_rx_init(
		      NULL, &handoff, V34_CONSTELLATION_4_POINT, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_init(
		      &untouched, &handoff,
		      (enum v34_constellation_size)8, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_init(
		      &untouched, &handoff,
		      V34_CONSTELLATION_4_POINT, NAN) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved,
		     sizeof(untouched)) == 0 &&
	      v90_phase4_cp_line_rx_init(
		      &rx, &handoff, V34_CONSTELLATION_4_POINT, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_OK,
	      "CP line initialization contract changed");
	saved = rx;
	CHECK(v90_phase4_cp_line_rx_sample(NULL, 0, &event) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_sample(&rx, 0, NULL) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_continue(&rx) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      !v90_phase4_cp_line_rx_has_failed(&rx) &&
	      !v90_phase4_cp_line_rx_has_pending_record(&rx) &&
	      !v90_phase4_cp_line_rx_pending_record_is_valid(&rx),
	      "rejected CP line operation changed state");
	rx.symbols.previous_clockwise_quarter_turns = 4;
	CHECK(!v90_phase4_cp_line_rx_is_valid(&rx) &&
	      strcmp(v90_phase4_cp_line_rx_result_name(
		     V90_PHASE4_CP_LINE_RX_RECORD_READY),
		     "record ready") == 0 &&
	      strcmp(v90_phase4_cp_line_rx_result_name(
		     V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v90_phase4_cp_line_rx_result_name(
		     (enum v90_phase4_cp_line_rx_result)99),
		     "unknown") == 0,
	      "CP line validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous G.711 lines", test_continuous_g711_lines },
		{ "transactional contracts", test_transactional_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
