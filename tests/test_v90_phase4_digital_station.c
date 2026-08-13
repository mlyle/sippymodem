// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_digital_station.h>
#include <v90_info1.h>

#include <g711.h>

#include "test.h"
#include "v90_phase4_line_fixture.h"

#include <math.h>
#include <string.h>

enum {
	TRAINING_UCODE = 91,
	SAMPLE_LIMIT = 10000,
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

static struct v90_cp_parameters parameters(enum g711_law law)
{
	struct v90_cp_parameters selected = {
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
			&selected.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return selected;
}

static struct v90_mp_record mp_parameters(bool acknowledged)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.acknowledges_cp = acknowledged,
			.upstream_rate_capability_mask = 0x1555u,
		},
	};
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

static bool ri_transmit_matches(
	const struct v90_phase4_ri_tx *left,
	const struct v90_phase4_ri_tx *right)
{
	return left->stage == right->stage &&
	       left->law == right->law &&
	       left->ri_symbols == right->ri_symbols &&
	       left->ri_bar_symbols == right->ri_bar_symbols &&
	       left->phase == right->phase &&
	       left->training_ucode == right->training_ucode &&
	       left->finish_requested == right->finish_requested;
}

static bool run_exchange(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_jd_parameters jd = jd_parameters(constellation);
	struct v90_mp_record mp = mp_parameters(false);
	struct v90_cp_parameters records[8];
	struct v90_phase3_return_line_handoff phase3_receive;
	struct v90_phase4_cp_line_tx analog_transmit;
	struct v90_phase4_digital_station station;
	struct v90_phase4_ri_tx model_ri;
	struct v90_phase4_cp_line_rx receive_handoff;
	struct v90_phase4_digital_transmit_handoff transmit_handoff;
	size_t next_record = 1;
	bool cpt_seen = false;
	bool receive_handoff_checked = false;
	bool complete = false;

	for (size_t index = 0; index < ARRAY_LENGTH(records); ++index)
		records[index] = parameters(law);
	records[0].selected_rate_index = 0;
	records[1].kind = V90_CP_DATA;
	records[2].law = law == G711_LAW_PCMU ?
			 G711_LAW_PCMA : G711_LAW_PCMU;
	records[3].spectral_lookahead = 3;
	records[4].request_silence = true;
	records[5].acknowledges_peer = true;
	memset(records[6].transmit_constellations[0].ucode_mask, 0,
	       sizeof(records[6].transmit_constellations[0].ucode_mask));
	add_ucode(&records[6].transmit_constellations[0], 12);
	add_ucode(&records[6].transmit_constellations[0], TRAINING_UCODE);
	add_ucode(&records[6].transmit_constellations[0], 119);

	CHECK(v90_phase4_line_fixture_init(
		      law, constellation, &records[0],
		      &analog_transmit, &phase3_receive),
	      "Phase 4 digital fixture did not reach CPt");
	CHECK(v90_phase4_digital_station_init(
		      &station, &phase3_receive, &jd, law,
		      TRAINING_UCODE, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_OK &&
	      v90_phase4_ri_tx_init(&model_ri, law, TRAINING_UCODE) ==
		      V90_PHASE4_RI_OK,
	      "digital station did not initialize");

	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		if (!v90_phase4_digital_station_transmit_complete(&station)) {
			uint8_t actual;
			uint8_t expected;
			enum v90_phase4_ri_result model_result;
			enum v90_phase4_digital_station_result station_result;

			model_result = v90_phase4_ri_tx_next(
				&model_ri, &expected);
			station_result = v90_phase4_digital_station_transmit(
				&station, &actual);
			CHECK(model_result == V90_PHASE4_RI_OK &&
			      station_result == V90_PHASE4_DIGITAL_STATION_OK &&
			      actual == expected &&
			      ri_transmit_matches(
				      &station.transmit, &model_ri),
			      "Ri waveform diverged at clock %zu: "
			      "result=%d/%d octet=%u/%u state=%u/%u/%u vs %u/%u/%u",
			      clock, model_result, station_result,
			      expected, actual,
			      model_ri.ri_symbols, model_ri.phase,
			      model_ri.training_ucode,
			      station.transmit.ri_symbols,
			      station.transmit.phase,
			      station.transmit.training_ucode);
		}

		if (!v90_phase4_digital_station_receive_complete(&station)) {
			int16_t sample;
			bool previously_received = station.cpt_received;

			if (v90_phase4_cp_line_tx_record_is_complete(
				    &analog_transmit)) {
				const struct v90_cp_parameters *next =
					next_record < ARRAY_LENGTH(records) ?
						&records[next_record++] :
						&records[ARRAY_LENGTH(records) - 1u];

				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &analog_transmit, next) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "analogue side could not continue CP record %zu",
				      next_record);
			}
			CHECK(v90_phase4_cp_line_tx_sample(
				      &analog_transmit, &sample) ==
				      V90_PHASE4_CP_LINE_TX_OK &&
			      v90_phase4_digital_station_receive(
				      &station, quantize(law, sample)) ==
				      V90_PHASE4_DIGITAL_STATION_OK,
			      "CPt receive path failed at clock %zu", clock);
			if (!previously_received && station.cpt_received) {
				CHECK(next_record == ARRAY_LENGTH(records) &&
				      mapping_matches(
					      &station.training_mapping,
					      &records[ARRAY_LENGTH(records) - 1u]) &&
				      v90_phase4_ri_tx_request_finish(&model_ri) ==
					      V90_PHASE4_RI_OK &&
				      ri_transmit_matches(
					      &station.transmit, &model_ri),
				      "digital side accepted a non-initial CPt");
				cpt_seen = true;
			}
		}

		if (v90_phase4_digital_station_receive_complete(&station) &&
		    !receive_handoff_checked) {
			CHECK(v90_phase4_digital_station_receive_handoff(
				      &station, &receive_handoff) ==
				      V90_PHASE4_DIGITAL_STATION_OK &&
			      v90_phase4_cp_line_rx_is_valid(&receive_handoff) &&
			      !v90_phase4_cp_line_rx_has_pending_record(
				      &receive_handoff) &&
			      memcmp(&receive_handoff, &station.receive,
				     sizeof(receive_handoff)) == 0,
			      "CP receive handoff lost its exact boundary");
			receive_handoff_checked = true;
		}

		if (v90_phase4_digital_station_transmit_complete(&station) &&
		    v90_phase4_digital_station_receive_complete(&station)) {
			complete = true;
			break;
		}
	}

	CHECK(complete && cpt_seen && receive_handoff_checked &&
	      v90_phase4_ri_tx_is_complete(&model_ri) &&
	      v90_phase4_digital_station_transmit_handoff(
		      &station, &mp, &transmit_handoff) ==
		      V90_PHASE4_DIGITAL_STATION_OK &&
	      v90_phase4_digital_transmit_handoff_is_valid(
		      &transmit_handoff) &&
	      mapping_matches(
		      &transmit_handoff.transmitter.encoder.mapping,
		      &records[ARRAY_LENGTH(records) - 1u]),
	      "TRN2d transmit handoff changed the accepted mapping");
	{
		struct v90_phase4_digital_station saved = station;
		uint8_t octet = 0xa5;

		CHECK(v90_phase4_digital_station_transmit(
			      &station, &octet) ==
			      V90_PHASE4_DIGITAL_STATION_COMPLETE &&
		      v90_phase4_digital_station_receive(&station, 1234) ==
			      V90_PHASE4_DIGITAL_STATION_COMPLETE &&
		      memcmp(&station, &saved, sizeof(station)) == 0 &&
		      octet == 0xa5,
		      "completed digital station consumed another boundary");
	}
	return true;
}

static bool resume_after_ri_prefix(enum g711_law law)
{
	struct v90_jd_parameters jd =
		jd_parameters(V34_CONSTELLATION_4_POINT);
	struct v90_cp_parameters training = parameters(law);
	struct v90_phase3_return_line_handoff phase3_receive;
	struct v90_phase4_cp_line_tx analog_transmit;
	struct v90_phase4_ri_tx transmit;
	struct v90_phase4_digital_station station;

	CHECK(v90_phase4_line_fixture_init(
		      law, V34_CONSTELLATION_4_POINT, &training,
		      &analog_transmit, &phase3_receive) &&
	      v90_phase4_ri_tx_init(&transmit, law, TRAINING_UCODE) ==
		      V90_PHASE4_RI_OK,
	      "law %u resume fixture did not initialize",
	      (unsigned int)law);
	for (size_t index = 0; index < 37; ++index) {
		uint8_t octet;

		CHECK(v90_phase4_ri_tx_next(&transmit, &octet) ==
			      V90_PHASE4_RI_OK,
		      "law %u Ri prefix stopped at %zu",
		      (unsigned int)law, index);
	}
	CHECK(v90_phase4_digital_station_resume(
		      &station, &phase3_receive, &jd,
		      &transmit, 0.05) == V90_PHASE4_DIGITAL_STATION_OK &&
	      ri_transmit_matches(&station.transmit, &transmit),
	      "law %u advanced Ri transmitter was not preserved",
	      (unsigned int)law);
	{
		struct v90_phase4_ri_tx model = transmit;
		uint8_t expected;
		uint8_t actual;

		CHECK(v90_phase4_ri_tx_next(&model, &expected) ==
			      V90_PHASE4_RI_OK &&
		      v90_phase4_digital_station_transmit(
			      &station, &actual) ==
			      V90_PHASE4_DIGITAL_STATION_OK &&
		      actual == expected &&
		      ri_transmit_matches(&station.transmit, &model),
		      "law %u resumed Ri transmitter repeated its prefix",
		      (unsigned int)law);
	}
	{
		struct v90_phase4_digital_station untouched;
		struct v90_phase4_digital_station saved;
		struct v90_phase4_ri_tx requested = transmit;

		memset(&untouched, 0xa5, sizeof(untouched));
		saved = untouched;
		CHECK(v90_phase4_ri_tx_request_finish(&requested) ==
			      V90_PHASE4_RI_OK &&
		      v90_phase4_digital_station_resume(
			      &untouched, &phase3_receive, &jd,
			      &requested, 0.05) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
		      memcmp(&untouched, &saved, sizeof(saved)) == 0,
		      "law %u resume accepted an unexplained finish request",
		      (unsigned int)law);
	}
	return true;
}

static bool test_directional_resume(void)
{
	CHECK(resume_after_ri_prefix(G711_LAW_PCMU) &&
	      resume_after_ri_prefix(G711_LAW_PCMA),
	      "advanced Ri transmitter resume failed");
	return true;
}

static bool test_initial_exchange(void)
{
	CHECK(run_exchange(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT),
	      "PCMU four-point digital exchange failed");
	CHECK(run_exchange(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT),
	      "PCMA sixteen-point digital exchange failed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_jd_parameters jd =
		jd_parameters(V34_CONSTELLATION_4_POINT);
	struct v90_jd_parameters bad_jd = jd;
	struct v90_mp_record mp = mp_parameters(false);
	struct v90_mp_record acknowledged_mp = mp_parameters(true);
	struct v90_cp_parameters training = parameters(G711_LAW_PCMU);
	struct v90_phase3_return_line_handoff phase3_receive;
	struct v90_phase3_return_line_handoff bad_receive;
	struct v90_phase4_cp_line_tx analog_transmit;
	struct v90_phase4_digital_station station;
	struct v90_phase4_digital_station untouched;
	struct v90_phase4_digital_station untouched_saved;
	struct v90_phase4_cp_line_rx receive_handoff;
	struct v90_phase4_cp_line_rx receive_saved;
	struct v90_phase4_digital_transmit_handoff transmit_handoff;
	struct v90_phase4_digital_transmit_handoff transmit_saved;
	struct v90_pcm_mapping training_mapping;
	uint8_t octet = 0xa5;

	CHECK(v90_phase4_line_fixture_init(
		      G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      &training, &analog_transmit, &phase3_receive),
	      "digital validation fixture did not initialize");
	bad_receive = phase3_receive;
	bad_receive.phase3.equalizer.equalizer.fill = 0;
	bad_jd.maximum_spectral_shaping_lookahead = 0;
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_phase4_digital_station_init(
		      NULL, &phase3_receive, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase4_digital_station_init(
		      &untouched, &phase3_receive, NULL, G711_LAW_PCMU,
		      TRAINING_UCODE, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase4_digital_station_init(
		      &untouched, &phase3_receive, &bad_jd, G711_LAW_PCMU,
		      TRAINING_UCODE, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase4_digital_station_init(
		      &untouched, &bad_receive, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase4_digital_station_init(
		      &untouched, &phase3_receive, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, NAN) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v90_phase4_digital_station_init(
		      &untouched, &phase3_receive, &jd, G711_LAW_PCMU,
		      V90_INFO1A_MIN_TRAINING_UCODE - 1u, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected digital initialization changed its destination");

	CHECK(v90_phase4_digital_station_init(
		      &station, &phase3_receive, &jd, G711_LAW_PCMU,
		      TRAINING_UCODE, 0.05) ==
		      V90_PHASE4_DIGITAL_STATION_OK,
	      "valid digital station did not initialize");
	memset(&receive_handoff, 0x5a, sizeof(receive_handoff));
	receive_saved = receive_handoff;
	memset(&transmit_handoff, 0x5a, sizeof(transmit_handoff));
	transmit_saved = transmit_handoff;
	{
		struct v90_phase4_digital_station saved = station;

		CHECK(v90_phase4_digital_station_transmit(NULL, &octet) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
		      v90_phase4_digital_station_transmit(&station, NULL) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
		      v90_phase4_digital_station_receive(NULL, 0) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
		      v90_phase4_digital_station_receive_handoff(
			      &station, &receive_handoff) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_STATE &&
		      v90_phase4_digital_station_transmit_handoff(
			      &station, NULL, &transmit_handoff) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
		      v90_phase4_digital_station_transmit_handoff(
			      &station, &mp, NULL) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
		      v90_phase4_digital_station_transmit_handoff(
			      &station, &mp, &transmit_handoff) ==
			      V90_PHASE4_DIGITAL_STATION_INVALID_STATE &&
		      memcmp(&station, &saved, sizeof(station)) == 0 &&
		      memcmp(&receive_handoff, &receive_saved,
			     sizeof(receive_handoff)) == 0 &&
		      memcmp(&transmit_handoff, &transmit_saved,
			     sizeof(transmit_handoff)) == 0 &&
		      octet == 0xa5,
		      "rejected digital operation was not transactional");
	}
	CHECK(v90_pcm_mapping_init(&training_mapping, &training) ==
		      V90_PCM_MAPPING_OK,
	      "valid handoff mapping did not initialize");
	memset(&transmit_handoff, 0, sizeof(transmit_handoff));
	CHECK(v90_downstream_tx_init(
		      &transmit_handoff.transmitter,
		      &training_mapping) == V90_DOWNSTREAM_TX_OK &&
	      v90_downstream_tx_start_mp(
		      &transmit_handoff.transmitter, &mp) ==
		      V90_DOWNSTREAM_TX_OK,
	      "valid handoff transmitter did not initialize");
	CHECK(v90_phase4_digital_transmit_handoff_is_valid(
		      &transmit_handoff),
	      "valid TRN2d/MP handoff was rejected");
	transmit_handoff.transmitter.encoder.mapping.modulus[0]--;
	CHECK(!v90_phase4_digital_station_is_valid(NULL) &&
	      !v90_phase4_digital_station_has_failed(NULL) &&
	      !v90_phase4_digital_transmit_handoff_is_valid(NULL) &&
	      !v90_phase4_digital_transmit_handoff_is_valid(
		      &transmit_handoff) &&
	      v90_phase4_digital_station_transmit_handoff(
		      &station, &acknowledged_mp, &transmit_saved) ==
		      V90_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      strcmp(v90_phase4_digital_station_result_name(
		     V90_PHASE4_DIGITAL_STATION_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v90_phase4_digital_station_result_name(
		     (enum v90_phase4_digital_station_result)99),
		     "unknown") == 0,
	      "digital validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "initial exchange", test_initial_exchange },
		{ "advanced Ri transmit resume", test_directional_resume },
		{ "contracts", test_contracts },
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
