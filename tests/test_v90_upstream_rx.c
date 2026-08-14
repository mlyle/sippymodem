// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase4_cp_line_rx.h>
#include <v90_phase4_e_line_tx.h>
#include <v90_upstream_data.h>
#include <v90_upstream_rx.h>

#include "test.h"
#include "v90_phase4_line_fixture.h"

#include <string.h>

enum { SAMPLE_LIMIT = 70000 };

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
	enum v90_cp_kind kind, enum g711_law law, bool acknowledged,
	uint8_t rate_2400_bps_units)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = rate_bit(rate_2400_bps_units),
		.trn1d_ratio_q3_13 = 0x2000,
		.shaping_a1_q1_6 = -12,
		.shaping_a2_q1_6 = 6,
		.shaping_b1_q1_6 = -3,
		.shaping_b2_q1_6 = 24,
		.selected_rate_index = kind == V90_CP_TRAINING ? 13 : 1,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = acknowledged,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v90_mp_record local_mp(uint8_t rate_2400_bps_units)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units =
				rate_2400_bps_units,
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_rate_capability_mask =
				rate_bit(rate_2400_bps_units),
		},
	};
}

static bool line_profile(
	enum v34_symbol_rate symbol_rate, uint8_t rate_2400_bps_units,
	struct v34_phase3_profile *profile)
{
	const struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = rate_2400_bps_units,
		.high_carrier = true,
	};

	return v34_phase3_profile_init(
		       profile, V34_ANSWERING_STATION, symbol_rate, &probe,
		       0, 0) == V34_PHASE3_PROFILE_OK;
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
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
	       memcmp(left_frame, right_frame,
		      (left_bits + 7u) / 8u) == 0;
}

static bool run_line(
	enum g711_law law, enum v34_constellation_size constellation,
	enum v34_symbol_rate symbol_rate, uint8_t rate_2400_bps_units)
{
	struct v90_cp_parameters training =
		cp_parameters(
			V90_CP_TRAINING, law, false,
			rate_2400_bps_units);
	struct v90_cp_parameters data =
		cp_parameters(
			V90_CP_DATA, law, false,
			rate_2400_bps_units);
	struct v90_cp_parameters prime = data;
	struct v90_mp_record mp = local_mp(rate_2400_bps_units);
	struct v90_pcm_mapping training_mapping;
	struct v34_phase3_profile profile;
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx cp_tx;
	struct v90_phase4_cp_line_rx cp_rx;
	struct v90_phase4_e_line_tx e_tx;
	struct v34_data_stream data_tx;
	struct v90_upstream_rx receiver;
	struct v34_data_line_receiver data_handoff;
	bool data_started = false;
	bool prime_started = false;
	bool cp_finished = false;
	bool e_started = false;
	bool b1_started = false;
	bool mp_prime_requested = false;
	bool ed_requested = false;
	bool e_received = false;
	bool b1_received = false;

	prime.acknowledges_peer = true;
	CHECK(line_profile(symbol_rate, rate_2400_bps_units, &profile) &&
	      v90_phase4_line_fixture_init_for_profile(
		      &profile, law, constellation, &training, &cp_tx,
		      &phase3),
	      "line fixture did not initialize law=%d constellation=%d "
	      "symbol_rate=%d rate=%u",
	      law, constellation, symbol_rate, rate_2400_bps_units);
	CHECK(v90_phase4_cp_line_rx_init(
		      &cp_rx, &phase3, constellation, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_OK,
	      "CP receiver did not initialize law=%d constellation=%d",
	      law, constellation);
	CHECK(v90_pcm_mapping_init(&training_mapping, &training) ==
		      V90_PCM_MAPPING_OK,
	      "training mapping did not initialize law=%d", law);
	CHECK(v90_upstream_rx_init(
		      &receiver, &cp_rx, &training_mapping, &mp) ==
		      V90_UPSTREAM_RX_OK,
	      "upstream owner did not initialize law=%d constellation=%d",
	      law, constellation);

	for (size_t clock = 0;
	     clock < SAMPLE_LIMIT && !v90_upstream_rx_is_complete(&receiver);
	     ++clock) {
		int16_t sample = 0;
		bool sample_ready = false;

		if (!e_started &&
		    v90_phase4_cp_line_tx_record_is_complete(&cp_tx)) {
			if (!data_started) {
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, &data) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "data CP did not start at clock %zu", clock);
				data_started = true;
			} else if (!prime_started) {
				const struct v90_cp_parameters *next =
					mp_prime_requested ? &prime : &data;

				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, next) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "CP repetition failed at clock %zu", clock);
				prime_started = mp_prime_requested;
			} else {
				cp_finished = true;
			}
		}
		if (!e_started) {
			enum v90_phase4_cp_line_tx_result result =
				v90_phase4_cp_line_tx_sample(&cp_tx, &sample);

			CHECK(result == V90_PHASE4_CP_LINE_TX_OK ||
			      (cp_finished &&
			       result == V90_PHASE4_CP_LINE_TX_COMPLETE),
			      "CP source stopped at clock %zu: %s", clock,
			      v90_phase4_cp_line_tx_result_name(result));
			if (result == V90_PHASE4_CP_LINE_TX_OK) {
				sample_ready = true;
			} else {
				CHECK(v90_phase4_e_line_tx_init(&e_tx, &cp_tx) ==
					      V90_PHASE4_E_LINE_TX_OK,
				      "CP prime did not hand off to E");
				e_started = true;
			}
		}
		if (!sample_ready && e_started && !b1_started) {
			enum v90_phase4_e_line_tx_result result =
				v90_phase4_e_line_tx_sample(&e_tx, &sample);

			CHECK(result == V90_PHASE4_E_LINE_TX_OK ||
			      result == V90_PHASE4_E_LINE_TX_COMPLETE,
			      "E source stopped at clock %zu: %s", clock,
			      v90_phase4_e_line_tx_result_name(result));
			if (result == V90_PHASE4_E_LINE_TX_OK) {
				sample_ready = true;
			} else {
				CHECK(v90_upstream_data_start_after_e(
					      &data_tx, &e_tx, &mp) ==
					      V90_UPSTREAM_DATA_OK,
				      "E did not hand off to upstream B1");
				b1_started = true;
			}
		}
		if (!sample_ready && b1_started) {
			bool accepted;

			CHECK(v34_data_stream_sample(
				      &data_tx, NULL, NULL, &sample, &accepted) ==
				      V34_DATA_STREAM_OK,
			      "B1 source stopped at clock %zu", clock);
			(void)accepted;
			sample_ready = true;
		}
		CHECK(sample_ready, "line produced no sample at clock %zu", clock);
		{
			struct v90_cp_parameters event;
			struct v90_cp_parameters saved;
			enum v90_upstream_rx_result result;

			memset(&event, 0xa5, sizeof(event));
			saved = event;
			result = v90_upstream_rx_sample(
				&receiver, quantize(law, sample), &event);
			CHECK(result == V90_UPSTREAM_RX_OK ||
			      result == V90_UPSTREAM_RX_REQUEST_MP_PRIME ||
			      result == V90_UPSTREAM_RX_REQUEST_ED ||
			      result == V90_UPSTREAM_RX_E_COMPLETE ||
			      result == V90_UPSTREAM_RX_B1_COMPLETE,
			      "upstream owner stopped at clock %zu: %s "
			      "symbol_rate=%d rate=%u",
			      clock, v90_upstream_rx_result_name(result),
			      symbol_rate, rate_2400_bps_units);
			if (result == V90_UPSTREAM_RX_OK) {
				CHECK(memcmp(&event, &saved, sizeof(event)) == 0,
				      "quiet sample changed its event");
			} else if (result ==
				   V90_UPSTREAM_RX_REQUEST_MP_PRIME) {
				CHECK(!mp_prime_requested && !ed_requested &&
				      parameters_match(&event, &data),
				      "first data CP event changed");
				mp_prime_requested = true;
			} else if (result == V90_UPSTREAM_RX_REQUEST_ED) {
				CHECK(mp_prime_requested && !ed_requested &&
				      parameters_match(&event, &prime),
				      "CP-prime event changed or arrived early");
				ed_requested = true;
			} else if (result == V90_UPSTREAM_RX_E_COMPLETE) {
				e_received = true;
			} else {
				b1_received = true;
			}
		}
	}

	{
		enum v90_upstream_rx_result handoff_result =
			v90_upstream_rx_handoff(&receiver, &data_handoff);
		bool handoff_valid = handoff_result == V90_UPSTREAM_RX_OK &&
			v34_data_line_receiver_is_valid(&data_handoff);

		CHECK(data_started && prime_started && cp_finished && e_started &&
		      b1_started && mp_prime_requested && ed_requested &&
		      e_received && b1_received &&
		      v90_upstream_rx_is_complete(&receiver) &&
		      !v90_upstream_rx_has_failed(&receiver) && handoff_valid &&
		      !v34_data_line_receiver_is_receiving_b1(&data_handoff) &&
		      data_handoff.geometry
			      .primary_data_rate_2400_bps_units ==
			      rate_2400_bps_units &&
		      data_handoff.equalizer_receiver.data_receiver.data_receiver
			      .b1_error_bits == 0,
		      "upstream entry at rate %u stopped: source=%d/%d/%d/%d/%d "
		      "events=%d/%d/%d/%d stage=%d failed=%d handoff=%s "
		      "valid=%d rate=%u errors=%u",
		      rate_2400_bps_units,
		      data_started, prime_started, cp_finished, e_started,
		      b1_started, mp_prime_requested, ed_requested, e_received,
		      b1_received,
		      receiver.stage, v90_upstream_rx_has_failed(&receiver),
		      v90_upstream_rx_result_name(handoff_result), handoff_valid,
		      handoff_valid ?
			      data_handoff.geometry
				      .primary_data_rate_2400_bps_units : 0,
		      handoff_valid ?
			      (unsigned int)data_handoff.equalizer_receiver
				      .data_receiver.data_receiver.b1_error_bits : 0);
	}
	{
		struct v90_upstream_rx saved_receiver = receiver;
		struct v90_cp_parameters event;
		struct v90_cp_parameters saved_event;

		memset(&event, 0xa5, sizeof(event));
		saved_event = event;
		CHECK(v90_upstream_rx_sample(&receiver, 1234, &event) ==
			      V90_UPSTREAM_RX_COMPLETE &&
		      memcmp(&receiver, &saved_receiver,
			     sizeof(receiver)) == 0 &&
		      memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "completed owner consumed another sample");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_cp_parameters training =
		cp_parameters(V90_CP_TRAINING, G711_LAW_PCMU, false, 4);
	struct v90_mp_record mp = local_mp(4);
	struct v90_mp_record acknowledged = mp;
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping wrong_mapping;
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx cp_tx;
	struct v90_phase4_cp_line_rx cp_rx;
	struct v90_upstream_rx receiver;
	struct v90_upstream_rx untouched;
	struct v90_upstream_rx saved;
	struct v34_data_line_receiver data;
	struct v34_data_line_receiver saved_data;

	acknowledged.parameters.type0.acknowledges_cp = true;
	CHECK(v90_phase4_line_fixture_init(
		      G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      &training, &cp_tx, &phase3) &&
	      v90_phase4_cp_line_rx_init(
		      &cp_rx, &phase3, V34_CONSTELLATION_4_POINT, 0.05) ==
		      V90_PHASE4_CP_LINE_RX_OK &&
	      v90_pcm_mapping_init(&mapping, &training) ==
		      V90_PCM_MAPPING_OK,
	      "contract fixture did not initialize");
	wrong_mapping = mapping;
	wrong_mapping.kind = V90_CP_DATA;
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(v90_upstream_rx_init(NULL, &cp_rx, &mapping, &mp) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(&untouched, NULL, &mapping, &mp) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(&untouched, &cp_rx, NULL, &mp) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(&untouched, &cp_rx, &mapping, NULL) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &wrong_mapping, &mp) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &mapping, &acknowledged) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(v90_upstream_rx_init(&receiver, &cp_rx, &mapping, &mp) ==
		      V90_UPSTREAM_RX_OK,
	      "valid contract owner did not initialize");
	memset(&data, 0xa5, sizeof(data));
	saved_data = data;
	CHECK(v90_upstream_rx_handoff(NULL, &data) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_handoff(&receiver, NULL) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_handoff(&receiver, &data) ==
		      V90_UPSTREAM_RX_INVALID_STATE &&
	      memcmp(&data, &saved_data, sizeof(data)) == 0 &&
	      strcmp(v90_upstream_rx_result_name(
		     V90_UPSTREAM_RX_REQUEST_MP_PRIME),
		     "request MP prime") == 0 &&
	      strcmp(v90_upstream_rx_result_name(
		     (enum v90_upstream_rx_result)99),
		     "unknown") == 0,
	      "owner contracts or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		enum g711_law law;
		enum v34_constellation_size constellation;
	} paths[] = {
		{ G711_LAW_PCMU, V34_CONSTELLATION_4_POINT },
		{ G711_LAW_PCMA, V34_CONSTELLATION_16_POINT },
	};
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t maximum_rate_2400_bps_units;
	} profiles[] = {
		{ V34_SYMBOL_RATE_3000, 9 },
		{ V34_SYMBOL_RATE_3200, 9 },
		{ V34_SYMBOL_RATE_3429, 10 },
	};

	fprintf(stderr, "[ RUN      ] generated upstream entry\n");
	for (size_t path = 0; path < ARRAY_LENGTH(paths); ++path)
		for (size_t profile = 0;
		     profile < ARRAY_LENGTH(profiles); ++profile)
			for (uint8_t rate =
				     V90_MP_MINIMUM_RATE_2400_BPS_UNITS;
			     rate <= profiles[profile]
					     .maximum_rate_2400_bps_units;
			     ++rate)
				if (!run_line(
					    paths[path].law,
					    paths[path].constellation,
					    profiles[profile].symbol_rate,
					    rate))
					return 1;
	fprintf(stderr, "[       OK ] generated upstream entry\n");
	fprintf(stderr, "[ RUN      ] contracts\n");
	if (!test_contracts())
		return 1;
	fprintf(stderr, "[       OK ] contracts\n");
	return 0;
}
