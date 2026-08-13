// SPDX-License-Identifier: BSD-3-Clause

#include <v90_downstream_rx.h>
#include <v90_downstream_tx.h>

#include "test.h"

#include <string.h>

enum {
	FRAME_BITS = 17,
	MAXIMUM_STREAM_SYMBOLS = 4096,
};

static void add_ucode(
	struct v90_cp_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.shaping_a1_q1_6 = 29,
		.shaping_a2_q1_6 = -13,
		.shaping_b1_q1_6 = 43,
		.shaping_b2_q1_6 = -5,
		.selected_rate_index = FRAME_BITS - 8,
		.sign_redundancy = 2,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static struct v90_mp_record make_record(bool type1)
{
	if (!type1) {
		return (struct v90_mp_record){
			.type = V90_MP_RECORD_TYPE0,
			.parameters.type0 = {
				.maximum_upstream_rate_2400_bps_units = 12,
				.upstream_trellis = V34_TRELLIS_32_STATE,
				.upstream_nonlinear_encoding = true,
				.upstream_expanded_shaping = true,
				.upstream_rate_capability_mask = 0x1fffu,
			},
		};
	}
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_upstream_rate_2400_bps_units = 14,
				.upstream_trellis = V34_TRELLIS_64_STATE,
				.upstream_rate_capability_mask = 0x1555u,
			},
			.precoder_coefficients = {
				{ .real = 1, .imaginary = -1 },
				{ .real = 8192, .imaginary = -8192 },
				{ .real = INT16_MAX, .imaginary = -32767 },
			},
		},
	};
}

static void set_acknowledgment(
	struct v90_mp_record *record, bool acknowledged)
{
	if (record->type == V90_MP_RECORD_TYPE0)
		record->parameters.type0.acknowledges_cp = acknowledged;
	else
		record->parameters.type1.common.acknowledges_cp =
			acknowledged;
}

static bool records_match(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	uint8_t left_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	uint8_t right_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t left_bits = 0;
	size_t right_bits = 0;

	return v90_mp_encode(
		       left, FRAME_BITS, left_sequence,
		       sizeof(left_sequence), &left_bits) == V90_MP_OK &&
	       v90_mp_encode(
		       right, FRAME_BITS, right_sequence,
		       sizeof(right_sequence), &right_bits) == V90_MP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_sequence, right_sequence,
		      (left_bits + 7u) / 8u) == 0;
}

static bool check_training_frame(const struct v90_trn2d_frame *frame)
{
	if (frame->bit_count != FRAME_BITS ||
	    frame->training_bit_errors != 0)
		return false;
	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		if (frame->bits[bit] != 1)
			return false;
	}
	return true;
}

static bool run_stream(
	enum g711_law law, bool type1, bool request_during_trn2d)
{
	struct v90_pcm_mapping mapping;
	struct v90_mp_record record = make_record(type1);
	struct v90_downstream_tx transmitter;
	struct v90_downstream_rx receiver;
	size_t training_frames = 0;
	size_t mp_records = 0;
	size_t prime_records = 0;
	size_t symbols = 0;
	bool prime_requested = false;
	bool ed_requested = false;
	bool ed_received = false;

	CHECK(make_mapping(&mapping, law) &&
	      mapping.frame_bits == FRAME_BITS &&
	      v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_rx_init(
		      &receiver, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_OK &&
	      v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_OK,
	      "stream fixture did not initialize");
	if (request_during_trn2d) {
		CHECK(v90_downstream_tx_request_mp_prime(&transmitter) ==
			      V90_DOWNSTREAM_TX_OK,
		      "early MP prime request failed");
		prime_requested = true;
	}

	while (!v90_downstream_tx_is_complete(&transmitter) &&
	       symbols < MAXIMUM_STREAM_SYMBOLS) {
		struct v90_downstream_symbol symbol;
		struct v90_downstream_event event;
		enum v90_downstream_tx_result transmit_result;
		enum v90_downstream_rx_result result;

		transmit_result =
			v90_downstream_tx_next(&transmitter, &symbol);
		if (transmit_result == V90_DOWNSTREAM_TX_COMPLETE)
			break;
		CHECK(transmit_result == V90_DOWNSTREAM_TX_OK,
		      "stream transmitter stopped at symbol %zu", symbols);
		symbols++;
		if (!prime_requested &&
		    transmitter.stage == V90_DOWNSTREAM_TX_MP &&
		    transmitter.mp_bit_index != 0) {
			CHECK(v90_downstream_tx_request_mp_prime(&transmitter) ==
				      V90_DOWNSTREAM_TX_OK &&
			      v90_downstream_tx_request_mp_prime(&transmitter) ==
				      V90_DOWNSTREAM_TX_OK,
			      "mid-record MP prime request failed");
			prime_requested = true;
		}
		result = v90_downstream_rx_receive(
			&receiver, symbol.pcm.octet, &event);
		if (result == V90_DOWNSTREAM_RX_BUFFERING)
			continue;
		if (result == V90_DOWNSTREAM_RX_TRN2D_FRAME) {
			CHECK(check_training_frame(&event.trn2d),
			      "TRN2d frame %zu changed", training_frames);
			training_frames++;
			continue;
		}
		if (result == V90_DOWNSTREAM_RX_MP) {
			CHECK(prime_records == 0 &&
			      records_match(&event.mp.record, &record),
			      "MP record %zu changed or followed MP prime",
			      mp_records);
			mp_records++;
			continue;
		}
		if (result == V90_DOWNSTREAM_RX_ED) {
			CHECK(ed_requested && !ed_received &&
			      event.ed.bit_count ==
				      V90_PHASE4_ED_FRAMES * FRAME_BITS &&
			      event.ed.bit_errors == 0,
			      "stream Ed event changed");
			ed_received = true;
			continue;
		}
		CHECK(result == V90_DOWNSTREAM_RX_MP_PRIME,
		      "stream receiver returned %s",
		      v90_downstream_rx_result_name(result));
		struct v90_mp_record expected = record;

		set_acknowledgment(&expected, true);
		CHECK(mp_records >= 1 &&
		      records_match(&event.mp.record, &expected),
		      "MP prime record %zu changed", prime_records);
		prime_records++;
		if (prime_records == 2) {
			CHECK(v90_downstream_rx_expect_ed(&receiver) ==
				      V90_DOWNSTREAM_RX_OK &&
			      v90_downstream_rx_expect_ed(&receiver) ==
				      V90_DOWNSTREAM_RX_OK &&
			      v90_downstream_tx_request_ed(&transmitter) ==
				      V90_DOWNSTREAM_TX_OK &&
			      v90_downstream_tx_request_ed(&transmitter) ==
				      V90_DOWNSTREAM_TX_OK,
			      "Ed requests were not idempotent");
			ed_requested = true;
		}
	}
	CHECK(symbols < MAXIMUM_STREAM_SYMBOLS && prime_requested &&
	      ed_requested && ed_received &&
	      training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      mp_records == 1 && prime_records == 3 &&
	      transmitter.stage == V90_DOWNSTREAM_TX_ED &&
	      transmitter.ed_frames == V90_PHASE4_ED_FRAMES &&
	      transmitter.transmitted_symbols == symbols &&
	      receiver.stream.training_bit_errors == 0 &&
	      v90_downstream_tx_is_complete(&transmitter) &&
	      v90_downstream_rx_is_complete(&receiver) &&
	      v90_downstream_tx_is_valid(&transmitter) &&
	      v90_downstream_rx_is_valid(&receiver),
	      "continuous Ed boundary or accounting changed");
	struct v90_downstream_tx completed = transmitter;
	struct v90_downstream_symbol untouched;
	struct v90_downstream_symbol saved;

	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_COMPLETE &&
	      v90_downstream_tx_next(&transmitter, &untouched) ==
		      V90_DOWNSTREAM_TX_COMPLETE &&
	      memcmp(&transmitter, &completed, sizeof(transmitter)) == 0 &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "completed Ed transmitter changed state or output");
	return true;
}

static bool test_continuous_streams(void)
{
	CHECK(run_stream(G711_LAW_PCMU, false, true),
	      "PCMU early-request stream failed");
	CHECK(run_stream(G711_LAW_PCMA, true, false),
	      "PCMA mid-record stream failed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_mp_record record = make_record(false);
	struct v90_mp_record changed = record;
	struct v90_mp_record acknowledged = record;
	struct v90_downstream_tx transmitter;
	struct v90_downstream_tx saved;

	CHECK(make_mapping(&mapping, G711_LAW_PCMU) &&
	      v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK,
	      "contract fixture did not initialize");
	saved = transmitter;
	set_acknowledgment(&acknowledged, true);
	CHECK(v90_downstream_tx_start_mp(NULL, &record) ==
		      V90_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      v90_downstream_tx_start_mp(&transmitter, NULL) ==
		      V90_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      v90_downstream_tx_request_mp_prime(NULL) ==
		      V90_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      v90_downstream_tx_request_ed(NULL) ==
		      V90_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      v90_downstream_tx_request_mp_prime(&transmitter) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      v90_downstream_tx_start_mp(&transmitter, &acknowledged) ==
		      V90_DOWNSTREAM_TX_INVALID_PARAMETERS &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "rejected MP setup changed state");

	CHECK(v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_OK,
	      "valid MP setup failed");
	saved = transmitter;
	changed.parameters.type0.maximum_upstream_rate_2400_bps_units = 10;
	CHECK(v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_tx_start_mp(&transmitter, &changed) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      v90_downstream_tx_request_finish(&transmitter) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "configured MP transaction changed");
	CHECK(v90_downstream_tx_request_mp_prime(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK,
	      "valid MP prime request failed");
	saved = transmitter;
	CHECK(v90_downstream_tx_request_mp_prime(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "repeated MP prime request changed state");
	CHECK(v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK,
	      "early Ed request failed");
	saved = transmitter;
	CHECK(v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "repeated Ed request changed state");

	CHECK(v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_tx_request_finish(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK,
	      "finished setup fixture failed");
	saved = transmitter;
	CHECK(v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "MP started after finish request");

	CHECK(v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_OK,
	      "corruption fixture did not initialize");
	transmitter.mp_sequence[0] ^= 1u;
	saved = transmitter;
	CHECK(!v90_downstream_tx_is_valid(&transmitter) &&
	      v90_downstream_tx_request_mp_prime(&transmitter) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "corrupt MP sequence was accepted or changed");
	return true;
}

static bool test_boundary_request(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_mp_record record = make_record(false);
	struct v90_downstream_tx transmitter;
	struct v90_downstream_symbol symbol;
	size_t symbols = 0;

	CHECK(make_mapping(&mapping, G711_LAW_PCMU) &&
	      v90_downstream_tx_init(&transmitter, &mapping) ==
		      V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_tx_start_mp(&transmitter, &record) ==
		      V90_DOWNSTREAM_TX_OK,
	      "boundary fixture did not initialize");
	while ((transmitter.stage != V90_DOWNSTREAM_TX_MP ||
		transmitter.mp_bit_index != 0) &&
	       symbols < MAXIMUM_STREAM_SYMBOLS) {
		CHECK(v90_downstream_tx_next(&transmitter, &symbol) ==
			      V90_DOWNSTREAM_TX_OK,
		      "boundary fixture stopped at symbol %zu", symbols);
		symbols++;
	}
	CHECK(symbols < MAXIMUM_STREAM_SYMBOLS &&
	      transmitter.stage == V90_DOWNSTREAM_TX_MP &&
	      transmitter.mp_bit_index == 0 &&
	      v90_downstream_tx_request_mp_prime(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK &&
	      transmitter.stage == V90_DOWNSTREAM_TX_MP_PRIME &&
	      transmitter.mp_bit_index == 0 &&
	      v90_downstream_tx_is_valid(&transmitter),
	      "record-boundary request did not switch immediately");
	while ((!transmitter.mp_prime_record_accepted ||
		transmitter.mp_bit_index != 0) &&
	       symbols < MAXIMUM_STREAM_SYMBOLS) {
		CHECK(v90_downstream_tx_next(&transmitter, &symbol) ==
			      V90_DOWNSTREAM_TX_OK,
		      "MP-prime boundary fixture stopped at symbol %zu",
		      symbols);
		symbols++;
	}
	CHECK(symbols < MAXIMUM_STREAM_SYMBOLS &&
	      transmitter.stage == V90_DOWNSTREAM_TX_MP_PRIME &&
	      transmitter.mp_prime_record_accepted &&
	      transmitter.mp_bit_index == 0 &&
	      v90_downstream_tx_request_ed(&transmitter) ==
		      V90_DOWNSTREAM_TX_OK &&
	      transmitter.stage == V90_DOWNSTREAM_TX_ED &&
	      transmitter.ed_frames == 0 &&
	      v90_downstream_tx_is_valid(&transmitter),
	      "record-boundary Ed request did not switch immediately");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous streams", test_continuous_streams },
		{ "record-boundary request", test_boundary_request },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
