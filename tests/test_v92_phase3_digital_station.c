// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_phase3_digital_tx.h>
#include <v90_phase3_sd_receiver.h>
#include <v92_pcm_phase3_cpt_tx.h>
#include <v92_pcm_phase3_initial_tx.h>
#include <v92_pcm_phase4_trn2u.h>
#include <v92_phase3_digital_station.h>
#include <v92_phase3_j_receiver.h>
#include <v92_su.h>

#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	TEST_SU_LEVEL = 4096,
	TEST_SU_THRESHOLD = 256,
	SAMPLE_LIMIT = 20000,
};

enum peer_source_stage {
	PEER_SOURCE_INITIAL,
	PEER_SOURCE_SU,
	PEER_SOURCE_CPT,
	PEER_SOURCE_TRN2U,
};

enum peer_receive_stage {
	PEER_RECEIVE_SD,
	PEER_RECEIVE_J,
	PEER_RECEIVE_RI,
	PEER_RECEIVE_COMPLETE,
};

struct peer {
	struct v92_pcm_phase3_initial_tx initial;
	struct fractional_delay delay;
	struct v92_su su;
	struct v92_pcm_phase3_cpt_tx cpt;
	struct v92_pcm_phase4_trn2u trn2u;
	struct v90_phase3_sd_receiver sd;
	struct v92_phase3_j_receiver j;
	struct v90_phase4_ri_rx ri;
	struct v92_pcm_info1a info1a;
	struct v92_cpt_parameters cpt_parameters;
	enum peer_source_stage source_stage;
	enum peer_receive_stage receive_stage;
	enum g711_law law;
	uint32_t rate_mask;
	uint8_t initial_finish_requests;
	uint8_t cpt_start_requests;
	uint8_t cpt_finish_requests;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters cpt_parameters(enum g711_law law)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = true,
	};

	add_ucode(&parameters.transmit_constellations[0], 24u);
	add_ucode(&parameters.transmit_constellations[0], 80u);
	return parameters;
}

static bool parameters_match(
	const struct v92_cpt_parameters *left,
	const struct v92_cpt_parameters *right)
{
	uint8_t left_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	uint8_t right_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t left_bits = 0u;
	size_t right_bits = 0u;

	return v92_cpt_encode(
		       left, left_frame, sizeof(left_frame), &left_bits) ==
		       V92_CPT_OK &&
	       v92_cpt_encode(
		       right, right_frame, sizeof(right_frame),
		       &right_bits) == V92_CPT_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static int16_t media_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool peer_init(
	struct peer *peer, enum g711_law law, uint32_t rate_mask)
{
	struct peer initialized = {
		.info1a = {
			.filter_sections = V92_PCM_FILTER_P1_Z2,
			.maximum_total_coefficients = 192,
			.maximum_section_coefficients = 128,
			.training_ucode = 103,
		},
		.cpt_parameters = cpt_parameters(law),
		.law = law,
		.rate_mask = rate_mask,
	};

	fractional_delay_init(&initialized.delay);
	if (!v92_cpt_parameters_are_valid(&initialized.cpt_parameters) ||
	    v92_pcm_phase3_initial_tx_init(
		    &initialized.initial, law, TEST_UCODE,
		    &initialized.info1a, rate_mask,
		    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS) !=
		    V92_PCM_PHASE3_INITIAL_TX_OK ||
	    v90_phase3_sd_receiver_init(
		    &initialized.sd, law, TEST_UCODE) !=
		    V90_PHASE3_SD_RECEIVER_OK)
		return false;
	*peer = initialized;
	return true;
}

static bool peer_receive_j(struct peer *peer, uint8_t octet)
{
	enum v92_phase3_j_receiver_result result =
		v92_phase3_j_receiver_receive(&peer->j, octet);

	switch (result) {
	case V92_PHASE3_J_RECEIVER_OK:
	case V92_PHASE3_J_RECEIVER_JD_REPEATED:
	case V92_PHASE3_J_RECEIVER_JP_REPEATED:
		return true;
	case V92_PHASE3_J_RECEIVER_TRN1D_READY:
		return v92_phase3_j_receiver_begin_jd_search(&peer->j) ==
		       V92_PHASE3_J_RECEIVER_OK;
	case V92_PHASE3_J_RECEIVER_JD_DETECTED:
		if (peer->source_stage != PEER_SOURCE_INITIAL ||
		    !v92_pcm_phase3_initial_tx_is_complete(&peer->initial) ||
		    v92_su_init(&peer->su, TEST_SU_LEVEL) != V92_SU_OK)
			return false;
		peer->source_stage = PEER_SOURCE_SU;
		return true;
	case V92_PHASE3_J_RECEIVER_JP_DETECTED: {
		struct v92_jp jp;

		return v92_phase3_j_receiver_jp(&peer->j, &jp) &&
		       v92_su_accept_jp(&peer->su, &jp) == V92_SU_OK;
	}
	case V92_PHASE3_J_RECEIVER_COMPLETE:
		if (v92_su_accept_jp_prime(
			    &peer->su, &peer->delay, true) != V92_SU_COMPLETE)
			return false;
		if (v92_pcm_phase3_cpt_tx_init(
			    &peer->cpt, peer->law, TEST_UCODE,
			    &peer->cpt_parameters) != V92_PCM_PHASE3_CPT_TX_OK)
			return false;
		peer->source_stage = PEER_SOURCE_CPT;
		peer->receive_stage = PEER_RECEIVE_RI;
		return v90_phase4_ri_rx_init(
			       &peer->ri, peer->law, TEST_UCODE) ==
		       V90_PHASE4_RI_OK;
	case V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT:
	case V92_PHASE3_J_RECEIVER_INVALID_STATE:
	case V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE:
		return false;
	}
	return false;
}

static bool peer_receive(
	struct peer *peer, uint8_t octet,
	enum v92_phase3_digital_transmit_stage emitted_stage)
{
	if (peer->receive_stage == PEER_RECEIVE_SD) {
		enum v90_phase3_sd_receiver_result result =
			v90_phase3_sd_receiver_receive(&peer->sd, octet);

		if (result == V90_PHASE3_SD_RECEIVER_SD_DETECTED) {
			if (v92_pcm_phase3_initial_tx_request_finish(
				    &peer->initial) !=
				    V92_PCM_PHASE3_INITIAL_TX_OK)
				return false;
			peer->initial_finish_requests++;
		}
		if (result == V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE) {
			if (v92_phase3_j_receiver_init(
				    &peer->j, peer->law, TEST_UCODE) !=
				    V92_PHASE3_J_RECEIVER_OK)
				return false;
			peer->receive_stage = PEER_RECEIVE_J;
			return true;
		}
		return result == V90_PHASE3_SD_RECEIVER_OK ||
		       result == V90_PHASE3_SD_RECEIVER_SD_DETECTED ||
		       result == V90_PHASE3_SD_RECEIVER_SD_BAR_DETECTED;
	}
	if (peer->receive_stage == PEER_RECEIVE_J)
		return peer_receive_j(peer, octet);
	if (peer->receive_stage == PEER_RECEIVE_RI) {
		enum v90_phase4_ri_result result;

		/* The character which changes the station's stage to RI is the
		 * final SCR character; only characters emitted from an already-RI
		 * state belong to the R detector. */
		if (emitted_stage != V92_PHASE3_DIGITAL_TRANSMIT_RI)
			return true;
		result = v90_phase4_ri_rx_receive(&peer->ri, octet);
		if (peer->cpt.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
		    v92_pcm_phase3_trn1u_can_handoff(&peer->cpt.trn1u) &&
		    result == V90_PHASE4_RI_OK) {
			if (v92_pcm_phase3_cpt_tx_start(&peer->cpt) !=
			    V92_PCM_PHASE3_CPT_TX_OK)
				return false;
			peer->cpt_start_requests++;
		}
		if (result == V90_PHASE4_RI_TRANSITION_DETECTED) {
			if (v92_pcm_phase3_cpt_tx_request_finish(&peer->cpt) !=
			    V92_PCM_PHASE3_CPT_TX_OK)
				return false;
			peer->cpt_finish_requests++;
		}
		if (result == V90_PHASE4_RI_DONE) {
			peer->receive_stage = PEER_RECEIVE_COMPLETE;
			return true;
		}
		return result == V90_PHASE4_RI_OK ||
		       result == V90_PHASE4_RI_TRANSITION_DETECTED;
	}
	return true;
}

static bool peer_next_sample(struct peer *peer, int16_t *sample)
{
	struct v90_pcm_symbol symbol;

	switch (peer->source_stage) {
	case PEER_SOURCE_INITIAL:
		if (v92_pcm_phase3_initial_tx_is_complete(&peer->initial)) {
			*sample = 0;
			return true;
		}
		if (v92_pcm_phase3_initial_tx_next(
			    &peer->initial, NULL, &symbol) !=
		    V92_PCM_PHASE3_INITIAL_TX_OK)
			return false;
		*sample = symbol.sample;
		return true;
	case PEER_SOURCE_SU: {
		enum v92_su_result result =
			v92_su_next(&peer->su, &peer->delay, sample);

		return result == V92_SU_OK ||
		       result == V92_SU_REACHED_JP_WAIT ||
		       result == V92_SU_REACHED_JP_PRIME_WAIT;
	}
	case PEER_SOURCE_CPT:
		if (!v92_pcm_phase3_cpt_tx_is_complete(&peer->cpt)) {
			if (v92_pcm_phase3_cpt_tx_next(&peer->cpt, &symbol) !=
			    V92_PCM_PHASE3_CPT_TX_OK)
				return false;
			*sample = symbol.sample;
			return true;
		}
		{
			struct v90_pcm_symbol lu;

			if (!v90_pcm_symbol_make(
				    &lu, peer->law, TEST_UCODE, true) ||
			    v92_pcm_phase4_trn2u_init(
				    &peer->trn2u,
				    V92_CONSTELLATION_8_POINT,
				    peer->cpt.coding.previous_sign,
				    (int16_t)abs(lu.sample)) !=
				    V92_PCM_PHASE4_TRN2U_OK)
				return false;
			peer->source_stage = PEER_SOURCE_TRN2U;
		}
		/* The equalizer owns delayed E1u samples in the Phase 4 tail. */
		return v92_pcm_phase4_trn2u_next(&peer->trn2u, sample) ==
		       V92_PCM_PHASE4_TRN2U_OK;
	case PEER_SOURCE_TRN2U:
		return v92_pcm_phase4_trn2u_next(&peer->trn2u, sample) ==
		       V92_PCM_PHASE4_TRN2U_OK;
	}
	return false;
}

static bool run_exchange(enum g711_law law)
{
	struct v92_jd jd = {
		.data_rate_capability_mask = UINT32_C(0x002a1555),
		.maximum_spectral_shaping_lookahead = 3,
	};
	struct v92_jp jp = {
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
	};
	struct v92_phase3_digital_station station;
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct peer peer;
	uint32_t rate_mask = UINT32_C(0x52491);
	uint32_t received_mask = 0u;
	bool finished = false;

	CHECK(peer_init(&peer, law, rate_mask) &&
	      v92_phase3_digital_station_init(
		      &station, law, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_OK,
	      "V.92 station fixture did not initialize for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < SAMPLE_LIMIT && !finished; ++index) {
		enum v92_phase3_digital_transmit_stage emitted_stage =
			station.transmit_stage;
		uint8_t octet;
		int16_t sample;

		if (!v92_phase3_digital_station_transmit_complete(&station)) {
			enum v92_phase3_digital_station_result tx_result =
				v92_phase3_digital_station_transmit(
					&station, &octet);
			bool accepted = tx_result ==
					V92_PHASE3_DIGITAL_STATION_OK &&
				peer_receive(&peer, octet, emitted_stage);

			CHECK(accepted,
			      "digital transmit/peer receive failed at %zu: "
			      "result=%s station=%u->%u peer=%u/%u J=%u Su=%u "
			      "CPt=%u Ri=%u",
			      index,
			      v92_phase3_digital_station_result_name(tx_result),
			      (unsigned int)emitted_stage,
			      (unsigned int)station.transmit_stage,
			      (unsigned int)peer.receive_stage,
			      (unsigned int)peer.source_stage,
			      (unsigned int)peer.j.stage,
			      (unsigned int)peer.su.stage,
			      (unsigned int)peer.cpt.stage,
			      (unsigned int)peer.ri.stage);
		}
		CHECK(peer_next_sample(&peer, &sample),
		      "peer source failed at %zu", index);
		sample = media_sample(law, sample);
		if (!v92_phase3_digital_station_receive_complete(&station)) {
			enum v92_phase3_digital_station_result result =
				v92_phase3_digital_station_receive(
					&station, sample);

			CHECK(result == V92_PHASE3_DIGITAL_STATION_OK ||
			      result == V92_PHASE3_DIGITAL_STATION_COMPLETE,
			      "digital receiver returned %s at %zu stage %u/%u",
			      v92_phase3_digital_station_result_name(result),
			      index, (unsigned int)station.receive_stage,
			      (unsigned int)station.transmit_stage);
		}
		finished =
			v92_phase3_digital_station_receive_complete(&station) &&
			v92_phase3_digital_station_transmit_complete(&station);
	}
	CHECK(finished && !v92_phase3_digital_station_has_failed(&station) &&
	      peer.initial_finish_requests == 1u &&
	      peer.cpt_start_requests == 1u &&
	      peer.cpt_finish_requests == 1u &&
	      peer.receive_stage == PEER_RECEIVE_COMPLETE &&
	      peer.source_stage == PEER_SOURCE_TRN2U &&
	      station.downstream.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	      station.downstream.scr_symbols >= 6u &&
	      station.downstream.scr_symbols % 6u == 0u &&
	      station.ri.finish_requested &&
	      station.ri.stage == V90_PHASE4_RI_COMPLETE &&
	      peer.j.jp.su_phase_extension_q16 ==
		      station.jp.su_phase_extension_q16 &&
	      station.jp.su_phase_extension_q16 != 0u,
	      "V.92 station exchange did not reach both boundaries");
	CHECK(v92_phase3_digital_station_upstream_rate_mask(
		      &station, &received_mask) &&
	      received_mask == rate_mask &&
	      v92_phase3_digital_station_receive_handoff(
		      &station, &handoff) == V92_PHASE3_DIGITAL_STATION_OK &&
	      v92_pcm_phase3_cpt_line_handoff_is_valid(&handoff) &&
	      parameters_match(&handoff.parameters, &peer.cpt_parameters),
	      "V.92 station handoff changed negotiated parameters");
	return true;
}

static bool test_contracts(void)
{
	struct v92_jd jd = {
		.data_rate_capability_mask = 1u,
		.maximum_spectral_shaping_lookahead = 1,
	};
	struct v92_jp jp = {
		.training_constellation = V92_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
	};
	struct v92_phase3_digital_station station;
	struct v92_phase3_digital_station before;
	uint8_t octet = 0u;
	uint32_t mask = UINT32_C(0xa5a5a5a5);
	static const char *const names[] = {
		"ok", "complete", "failed", "invalid argument",
		"invalid state", "sequence mismatch", "unsupported",
		"numeric failure",
	};

	memset(&station, 0xa5, sizeof(station));
	before = station;
	CHECK(v92_phase3_digital_station_init(
		      NULL, G711_LAW_PCMU, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, NAN,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, 0.0, 0u,
		      &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      memcmp(&station, &before, sizeof(station)) == 0,
	      "invalid station initialization changed its destination");
	CHECK(v92_phase3_digital_station_init(
		      &station, G711_LAW_PCMU, TEST_UCODE, 0.0,
		      TEST_SU_THRESHOLD, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      v92_phase3_digital_station_is_valid(&station) &&
	      !v92_phase3_digital_station_has_failed(&station) &&
	      !v92_phase3_digital_station_transmit_complete(&station) &&
	      !v92_phase3_digital_station_receive_complete(&station) &&
	      !v92_phase3_digital_station_upstream_rate_mask(
		      &station, &mask) &&
	      mask == UINT32_C(0xa5a5a5a5),
	      "fresh station contracts differed");
	before = station;
	CHECK(v92_phase3_digital_station_transmit(&station, &octet) ==
		      V92_PHASE3_DIGITAL_STATION_OK &&
	      memcmp(&station, &before, sizeof(station)) == 0 &&
	      octet == g711_encode_pcmu(0),
	      "pre-Ja silence changed station state");
	CHECK(v92_phase3_digital_station_transmit(NULL, &octet) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_receive(NULL, 0) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase3_digital_station_receive_handoff(
		      &station, NULL) ==
		      V92_PHASE3_DIGITAL_STATION_INVALID_ARGUMENT,
	      "invalid station calls returned the wrong result");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase3_digital_station_result_name(
				      (enum v92_phase3_digital_station_result)index),
			      names[index]) == 0,
		      "station result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase3_digital_station_result_name(
			      (enum v92_phase3_digital_station_result)99),
		      "unknown") == 0,
	      "unknown station result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "PCMU zero-DIL exchange", NULL },
		{ "PCMA zero-DIL exchange", NULL },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (index == 0u)
			passed = run_exchange(G711_LAW_PCMU);
		else if (index == 1u)
			passed = run_exchange(G711_LAW_PCMA);
		else
			passed = tests[index].run();
		if (passed)
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
