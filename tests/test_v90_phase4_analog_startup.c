// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_analog_startup.h>
#include <v90_phase4_owned.h>
#include <v90_phase4_digital_station.h>

#include <g711.h>
#include <pcm_downstream_tx.h>
#include <v90_phase4_ri.h>

#include "test.h"
#include "v90_phase4_state_fixture.h"
#include "v90_phase4_station_fixture.h"

#include <string.h>
#include <stdlib.h>
#include "memory_region.h"

enum {
	TRAINING_UCODE = 91,
	CHARACTER_LIMIT = 12000,
};

/* The public operation owns validation; the PCM continuation must preserve
 * the same waveform and complete state for every accepted training sample. */
static enum v90_phase4_startup_result checked_octet(
	struct v90_phase4_analog_startup *startup,
	uint8_t received, uint8_t *transmitted)
{
	struct v90_phase4_analog_startup owned;
	uint8_t expected = 0;
	bool compare = v90_phase4_analog_startup_is_valid(startup) &&
		transmitted != NULL && !memory_regions_overlap(
			startup, sizeof(*startup), transmitted, sizeof(*transmitted));
	enum v90_phase4_startup_result expected_result;
	enum v90_phase4_startup_result result;

	if (compare) {
		owned = *startup;
		expected_result = v90_phase4_analog_startup_octet_owned(
			&owned, received, &expected);
	}
	result = v90_phase4_analog_startup_octet(startup, received, transmitted);
	if (compare && (result != expected_result ||
	    !v90_analog_startup_states_equal(startup, &owned) ||
	    (result == V90_PHASE4_STARTUP_OK && *transmitted != expected))) {
		fprintf(stderr, "owned Phase 4 continuation changed output or state\n");
		abort();
	}
	return result;
}

#define v90_phase4_analog_startup_octet checked_octet


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

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = 0x15555u,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation =
			V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
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

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) : g711_encode_pcmu(sample);
}

static int16_t decode_octet(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static bool prepare_stations(
	enum g711_law law, struct v90_phase4_analog_station *analog,
	struct v90_phase4_digital_station *digital)
{
	struct v90_jd_parameters jd = jd_parameters();
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_phase3_return_line_handoff receive;

	return v90_phase4_station_fixture_init(
		       law, 8u, &jd, TRAINING_UCODE, &training,
		       analog, &receive) &&
	       v90_phase4_digital_station_init(
		       digital, &receive, &jd, law,
		       TRAINING_UCODE) ==
		       V90_PHASE4_DIGITAL_STATION_OK;
}

static bool run_exchange(enum g711_law law)
{
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station digital_station;
	struct v90_phase4_analog_startup startup;
	struct v90_phase4_analog_handoff analog_handoff;
	struct v90_phase4_digital_transmit_handoff digital_handoff;
	struct v90_mp_record mp = mp_parameters();
	size_t downstream_octets = 0;
	uint8_t to_analog;
	uint8_t to_digital;
	bool digital_handed_off = false;
	bool complete = false;

	CHECK(prepare_stations(law, &analog_station, &digital_station),
	      "law %u analogue fixture did not initialize",
	      (unsigned int)law);
	{
		int16_t sample;

		CHECK(v90_phase4_analog_station_transmit(
			      &analog_station, &sample) ==
			      V90_PHASE4_ANALOG_STATION_OK &&
		      v90_phase4_digital_station_transmit(
			      &digital_station, &to_analog) ==
			      V90_PHASE4_DIGITAL_STATION_OK,
		      "law %u first characters were unavailable",
		      (unsigned int)law);
		to_digital = encode_sample(law, sample);
	}
	CHECK(v90_phase4_analog_startup_init(
		      &startup, &analog_station) == V90_PHASE4_STARTUP_OK,
	      "law %u analogue startup did not initialize",
	      (unsigned int)law);

	for (size_t clock = 0; clock < CHARACTER_LIMIT; ++clock) {
		bool was_downstream =
			startup.stage == V90_PHASE4_STARTUP_CONTINUING &&
			startup.state.continuing.receive_ready;
		uint8_t next_to_analog;
		uint8_t next_to_digital;

		CHECK(v90_phase4_analog_startup_octet(
			      &startup, to_analog, &next_to_digital) ==
			      V90_PHASE4_STARTUP_OK,
		      "law %u analogue startup stopped at %zu",
		      (unsigned int)law, clock);
		downstream_octets += was_downstream;

		if (!digital_handed_off) {
			CHECK(v90_phase4_digital_station_transmit(
				      &digital_station, &next_to_analog) ==
				      V90_PHASE4_DIGITAL_STATION_OK,
			      "law %u Ri stopped at %zu",
			      (unsigned int)law, clock);
			if (!v90_phase4_digital_station_receive_complete(
				    &digital_station))
				CHECK(v90_phase4_digital_station_receive(
					      &digital_station,
					      decode_octet(law, to_digital)) ==
					      V90_PHASE4_DIGITAL_STATION_OK,
				      "law %u CPt stopped at %zu",
				      (unsigned int)law, clock);
			if (v90_phase4_digital_station_transmit_complete(
				    &digital_station)) {
				CHECK(v90_phase4_digital_station_receive_complete(
					      &digital_station) &&
				      v90_phase4_digital_station_transmit_handoff(
					      &digital_station, &mp,
					      &digital_handoff) ==
					      V90_PHASE4_DIGITAL_STATION_OK,
				      "law %u downstream handoff failed",
				      (unsigned int)law);
				digital_handed_off = true;
			}
		} else {
			struct pcm_downstream_symbol symbol;

			CHECK(pcm_downstream_tx_next(
				      &digital_handoff.transmitter, &symbol) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "law %u TRN2d stopped at %zu",
			      (unsigned int)law, clock);
			next_to_analog = symbol.pcm.octet;
		}
		to_analog = next_to_analog;
		to_digital = next_to_digital;
		if (v90_phase4_analog_startup_is_complete(&startup)) {
			complete = true;
			break;
		}
	}

	CHECK(complete && digital_handed_off && downstream_octets > 0 &&
	      v90_phase4_analog_startup_handoff(
		      &startup, &analog_handoff) ==
		      V90_PHASE4_STARTUP_OK &&
	      analog_handoff.receive.stream.received_symbols ==
		      downstream_octets &&
	      v90_phase4_cp_line_tx_is_handoff_ready(
		      &analog_handoff.transmit) &&
	      pcm_downstream_rx_is_valid(&analog_handoff.receive),
	      "law %u analogue startup lost a character: %zu/%llu",
	      (unsigned int)law, downstream_octets,
	      (unsigned long long)
		      analog_handoff.receive.stream.received_symbols);
	{
		struct v90_phase4_analog_startup mismatched = startup;

		mismatched.state.complete.receive.stream.decoder.mapping
			.shaping_a1_q1_6++;
		CHECK(pcm_downstream_rx_is_valid(
			      &mismatched.state.complete.receive) &&
		      !v90_phase4_analog_startup_is_valid(&mismatched),
		      "law %u mismatched handoff mapping was accepted",
		      (unsigned int)law);
	}
	{
		struct v90_phase4_analog_startup saved = startup;
		uint8_t output = 0xa5;

		CHECK(v90_phase4_analog_startup_octet(
			      &startup, to_analog, &output) ==
			      V90_PHASE4_STARTUP_DONE &&
		      output == 0xa5 &&
		      v90_analog_startup_states_equal(&startup, &saved),
		      "law %u completed startup consumed a character",
		      (unsigned int)law);
	}
	return true;
}

static bool test_generated_exchange(void)
{
	CHECK(run_exchange(G711_LAW_PCMU) && run_exchange(G711_LAW_PCMA),
	      "generated analogue startup exchanges failed");
	return true;
}

static const struct v90_phase4_analog_station *active_station(
	const struct v90_phase4_analog_startup *startup)
{
	return startup->stage == V90_PHASE4_STARTUP_INITIAL ?
		       &startup->state.initial :
		       &startup->state.continuing.station;
}

static bool run_cpt_first_exchange(enum g711_law law)
{
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station unused_digital;
	struct v90_phase4_analog_startup startup;
	struct v90_phase4_analog_handoff handoff;
	struct v90_phase4_ri_tx ri;
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream;
	size_t early_scr_octets = 0;
	size_t downstream_octets = 0;
	bool finish_requested = false;
	bool cpt_finished_first = false;
	bool scrambled_ones_seen = false;
	bool false_edge_checked = false;
	bool complete = false;

	CHECK(prepare_stations(
		      law, &analog_station, &unused_digital) &&
	      v90_phase4_analog_startup_init(
		      &startup, &analog_station) == V90_PHASE4_STARTUP_OK &&
	      v90_phase4_ri_tx_init(&ri, law, TRAINING_UCODE) ==
		      V90_PHASE4_RI_OK &&
	      v90_pcm_mapping_init(&mapping, &training) ==
		      V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "law %u CPt-first fixture did not initialize",
	      (unsigned int)law);

	for (size_t clock = 0; clock < CHARACTER_LIMIT; ++clock) {
		const struct v90_phase4_analog_station *station =
			active_station(&startup);
		struct pcm_downstream_symbol symbol;
		uint8_t received;
		uint8_t transmitted;
		uint32_t history_before = station->transmit.symbols
			.scrambler.history;
		bool was_early_scr =
			startup.stage == V90_PHASE4_STARTUP_CONTINUING &&
			!startup.state.continuing.receive_ready;
		bool was_downstream =
			startup.stage == V90_PHASE4_STARTUP_CONTINUING &&
			startup.state.continuing.receive_ready;

		if (!finish_requested &&
		    ri.ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
		    ri.phase == 0u &&
		    !v90_phase4_cp_line_tx_record_is_complete(
			    &station->transmit) &&
		    station->transmit.symbols.frame_bit_count -
			    station->transmit.symbols.bit_index <= 24u) {
			CHECK(v90_phase4_ri_tx_request_finish(&ri) ==
				      V90_PHASE4_RI_OK,
			      "law %u Ri did not accept the early finish",
			      (unsigned int)law);
			finish_requested = true;
		}
		if (!v90_phase4_ri_tx_is_complete(&ri)) {
			CHECK(v90_phase4_ri_tx_next(&ri, &received) ==
				      V90_PHASE4_RI_OK,
			      "law %u Ri stopped at %zu",
			      (unsigned int)law, clock);
		} else {
			CHECK(pcm_downstream_tx_next(&downstream, &symbol) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "law %u TRN2d stopped at %zu",
			      (unsigned int)law, clock);
			received = symbol.pcm.octet;
		}
		CHECK(v90_phase4_analog_startup_octet(
			      &startup, received, &transmitted) ==
			      V90_PHASE4_STARTUP_OK,
		      "law %u CPt-first startup stopped at %zu",
		      (unsigned int)law, clock);
		if (was_early_scr) {
			const struct v90_phase4_cp_tx *symbols_after =
				startup.stage == V90_PHASE4_STARTUP_COMPLETE ?
					&startup.state.complete.transmit.symbols :
					&active_station(&startup)->transmit.symbols;

			early_scr_octets++;
			scrambled_ones_seen |=
				symbols_after->scrambler.history != history_before;
		}
		downstream_octets += was_downstream;
		if (startup.stage == V90_PHASE4_STARTUP_CONTINUING &&
		    !startup.state.continuing.receive_ready) {
			cpt_finished_first = true;
			if (!false_edge_checked) {
				struct v90_phase4_analog_startup retry = startup;
				uint8_t retry_output;

				CHECK(v90_phase4_analog_startup_octet(
					      &retry, encode_sample(law, 0),
					      &retry_output) ==
					      V90_PHASE4_STARTUP_OK &&
				      retry.stage == V90_PHASE4_STARTUP_INITIAL &&
				      !retry.state.initial.finish_cpt_requested &&
				      v90_phase4_analog_startup_is_valid(&retry),
				      "law %u false Ri-bar edge did not resume CPt",
				      (unsigned int)law);
				false_edge_checked = true;
			}
		}
		if (v90_phase4_analog_startup_is_complete(&startup)) {
			complete = true;
			break;
		}
	}
	CHECK(complete && finish_requested && cpt_finished_first &&
	      early_scr_octets > 0 && scrambled_ones_seen &&
	      false_edge_checked &&
	      v90_phase4_ri_tx_is_complete(&ri) &&
	      v90_phase4_analog_startup_handoff(&startup, &handoff) ==
		      V90_PHASE4_STARTUP_OK &&
	      handoff.receive.stream.received_symbols == downstream_octets &&
	      v90_phase4_cp_line_tx_is_handoff_ready(&handoff.transmit),
	      "law %u CPt-first SCR bridge failed: scr=%zu data=%zu",
	      (unsigned int)law, early_scr_octets, downstream_octets);
	return true;
}

static bool test_cpt_first_exchange(void)
{
	CHECK(run_cpt_first_exchange(G711_LAW_PCMU) &&
	      run_cpt_first_exchange(G711_LAW_PCMA),
	      "CPt-first startup exchanges failed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station digital_station;
	struct v90_phase4_analog_startup startup;
	struct v90_phase4_analog_startup saved;
	struct v90_phase4_analog_handoff handoff;
	uint8_t output = 0xa5;

	CHECK(prepare_stations(
		      G711_LAW_PCMU, &analog_station, &digital_station),
	      "contract fixture did not initialize");
	memset(&startup, 0xa5, sizeof(startup));
	saved = startup;
	CHECK(v90_phase4_analog_startup_init(NULL, &analog_station) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_analog_startup_init(&startup, NULL) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_analog_startup_states_equal(&startup, &saved),
	      "invalid initialization changed the destination");
	CHECK(v90_phase4_analog_startup_init(
		      &startup, &analog_station) == V90_PHASE4_STARTUP_OK,
	      "valid startup did not initialize");
	saved = startup;
	{
		struct v90_phase4_analog_startup invalid = startup;
		struct v90_phase4_analog_startup before;

		invalid.state.initial.transmit.symbols.frame[4] ^= 1u;
		before = invalid;
		CHECK(!v90_phase4_analog_startup_is_valid(&invalid) &&
		      !v90_phase4_analog_startup_is_complete(&invalid) &&
		      !v90_phase4_analog_startup_has_failed(&invalid) &&
		      v90_phase4_analog_startup_octet(&invalid, 0, &output) ==
			      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
		      v90_analog_startup_states_equal(&invalid, &before) && output == 0xa5,
		      "public startup accepted a corrupt CP record");
	}
	CHECK(v90_phase4_analog_startup_octet(
		      &startup, 0, (uint8_t *)&startup) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_analog_startup_handoff(
		      &startup, &handoff) ==
		      V90_PHASE4_STARTUP_INVALID_STATE &&
	      v90_analog_startup_states_equal(&startup, &saved),
	      "invalid active operations changed the startup");
	for (size_t index = 0; index < V90_PHASE4_RI_SEARCH_SYMBOLS;
	     ++index) {
		uint8_t received;

		CHECK(v90_phase4_digital_station_transmit(
			      &digital_station, &received) ==
			      V90_PHASE4_DIGITAL_STATION_OK &&
		      v90_phase4_analog_startup_octet(
			      &startup, received, &output) ==
			      V90_PHASE4_STARTUP_OK,
		      "Ri acquisition stopped at %zu", index);
	}
	CHECK(v90_phase4_ri_search_is_acquired(
		      &startup.state.initial.receive),
	      "startup did not acquire Ri before sequence check");
	output = 0xa5;
	CHECK(v90_phase4_analog_startup_octet(&startup, 0, &output) ==
		      V90_PHASE4_STARTUP_OK &&
	      output != 0xa5 &&
	      !v90_phase4_analog_startup_has_failed(&startup) &&
	      !v90_phase4_ri_search_is_acquired(
		      &startup.state.initial.receive),
	      "recoverable Ri loss did not commit the accepted character");
	for (size_t index = 0; index < V90_PHASE4_RI_SEARCH_SYMBOLS;
	     ++index) {
		uint8_t received;

		CHECK(v90_phase4_digital_station_transmit(
			      &digital_station, &received) ==
			      V90_PHASE4_DIGITAL_STATION_OK &&
		      v90_phase4_analog_startup_octet(
			      &startup, received, &output) ==
			      V90_PHASE4_STARTUP_OK,
		      "Ri reacquisition stopped at %zu", index);
	}
	CHECK(v90_phase4_ri_search_is_acquired(
		      &startup.state.initial.receive) &&
	      !v90_phase4_analog_startup_has_failed(&startup),
	      "startup did not reacquire Ri after recoverable loss");
	startup.failed = true;
	CHECK(v90_phase4_analog_startup_is_valid(&startup) &&
	      v90_phase4_analog_startup_has_failed(&startup),
	      "failed startup fixture is invalid");
	output = 0xa5;
	CHECK(v90_phase4_analog_startup_octet(&startup, 0, &output) ==
		      V90_PHASE4_STARTUP_FAILED &&
	      output == 0xa5 &&
	      !v90_phase4_analog_startup_is_valid(NULL) &&
	      !v90_phase4_analog_startup_has_failed(NULL) &&
	      !v90_phase4_analog_startup_is_complete(NULL),
	      "failed startup consumed another character");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok", "complete", "failed", "invalid argument",
		"invalid state", "invalid parameters", "out of range",
		"clipping risk", "sequence mismatch", "numeric failure",
	};

	for (size_t index = 0; index < ARRAY_LENGTH(expected); ++index)
		CHECK(strcmp(
			      v90_phase4_startup_result_name(
				      (enum v90_phase4_startup_result)index),
			      expected[index]) == 0,
		      "startup result name %zu changed", index);
	CHECK(strcmp(
		      v90_phase4_startup_result_name(
			      (enum v90_phase4_startup_result)99),
		      "unknown") == 0,
	      "unknown startup result name changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated PCMU and PCMA continuation",
		  test_generated_exchange },
		{ "CPt-first optional SCR continuation",
		  test_cpt_first_exchange },
		{ "contracts and failure", test_contracts },
		{ "result names", test_result_names },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
