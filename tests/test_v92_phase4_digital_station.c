// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>
#include <v92_phase4_analog_station.h>
#include <v92_phase4_cpu_tx.h>
#include <v92_phase4_digital_station.h>
#include <v92_phase4_suvu_tx.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 6000,
	STARTING_INPUT_SAMPLES = 4096,
	DOWNSTREAM_LIMIT = 4096,
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

	for (uint8_t index = 0u; index < 8u; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v92_cpu_parameters cpu_parameters(enum g711_law law)
{
	struct v92_cpu_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x7654),
		.shaping_a1_q1_6 = 7,
		.shaping_a2_q1_6 = -9,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -5,
		.data_rate_number = 11,
		.sign_redundancy = 1,
		.spectral_lookahead_frames = 2,
		.constellation_count = 1,
	};

	add_ucode(&parameters.transmit_constellations[0], 16u);
	add_ucode(&parameters.transmit_constellations[0], 72u);
	return parameters;
}

static struct v92_cpd_base_parameters cpd_parameters(void)
{
	return (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = UINT16_C(0x4321),
		.data_rate_number = 12u,
		.trellis = V92_CPD_TRELLIS_64_STATE,
		.extend_e2u = true,
	};
}

static int16_t line_sample(
	enum g711_law law, int8_t polarity, int16_t raw)
{
	int16_t quantized = law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(raw)) :
		g711_decode_pcma(g711_encode_pcma(raw));

	return polarity > 0 ? quantized : (int16_t)-quantized;
}

static bool build_predecessor(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity,
	struct v92_pcm_phase4_trn2u *transmitter,
	struct v92_pcm_phase3_cpt_line_handoff *handoff)
{
	struct qam_point reference = { .in_phase = TEST_LU };
	struct qam_equalizer equalizer;
	struct qam_point output = { 0 };
	bool ready = false;

	CHECK(qam_equalizer_init_geometry(
		      &equalizer, &reference, &reference,
		      V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS,
		      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY) ==
		      QAM_EQUALIZER_OK &&
	      v92_pcm_phase4_trn2u_init(
		      transmitter, constellation, previous_sign,
		      TEST_LU) == V92_PCM_PHASE4_TRN2U_OK,
	      "station predecessor did not initialize");
	for (size_t index = 0u;
	     index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t raw;
		struct qam_point input;

		if (index == 0u)
			raw = previous_sign != 0u ? -TEST_LU : TEST_LU;
		else
			CHECK(v92_pcm_phase4_trn2u_next(transmitter, &raw) ==
				      V92_PCM_PHASE4_TRN2U_OK,
			      "station predecessor stopped at %zu", index);
		input = (struct qam_point){
			.in_phase = polarity *
				line_sample(law, polarity, raw),
		};
		CHECK(qam_equalizer_push(
			      &equalizer, &input, &output, &ready) ==
			      QAM_EQUALIZER_OK,
		      "station equalizer fill stopped at %zu", index);
	}
	CHECK(ready && qam_equalizer_adapt(
			       &equalizer, &output, 0.0) == QAM_EQUALIZER_OK,
	      "station equalizer did not become warm");
	*handoff = (struct v92_pcm_phase3_cpt_line_handoff){
		.equalizer = equalizer,
		.parameters = cpt_parameters(law),
		.input_samples = STARTING_INPUT_SAMPLES,
		.signal_level = TEST_LU,
		.adaptation = 0.05,
		.input_advance_symbols =
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
		.previous_sign = previous_sign,
		.polarity = polarity,
	};
	return v92_pcm_phase3_cpt_line_handoff_is_valid(handoff);
}

static bool cpu_equal(
	const struct v92_cpu_parameters *left,
	const struct v92_cpu_parameters *right)
{
	return left->law == right->law &&
	       left->trn1d_ratio_q3_13 == right->trn1d_ratio_q3_13 &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead_frames ==
		       right->spectral_lookahead_frames &&
	       left->constellation_count == right->constellation_count &&
	       left->acknowledges_cpd == right->acknowledges_cpd &&
	       left->codec_constellations_differ ==
		       right->codec_constellations_differ &&
	       memcmp(left->transmit_constellations,
		      right->transmit_constellations,
		      sizeof(left->transmit_constellations)) == 0 &&
	       memcmp(left->codec_constellations,
		      right->codec_constellations,
		      sizeof(left->codec_constellations)) == 0 &&
	       memcmp(left->frame_constellation,
		      right->frame_constellation,
		      sizeof(left->frame_constellation)) == 0;
}

struct downstream_counts {
	size_t initial_suvd;
	size_t cpd;
	size_t status_suvd;
	size_t acknowledged_suvd;
};

static bool cpd_selection_equal(
	const struct v92_cpd_parameters *left,
	const struct v92_cpd_base_parameters *right)
{
	return left->four_times_gain_q0_16 == right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u &&
	       left->acknowledges_cpu == right->acknowledges_cpu &&
	       !left->modulus_present && !left->filters_present &&
	       !left->constellations_present;
}

static bool clock_downstream(
	struct v92_phase4_digital_station *station,
	struct pcm_downstream_rx *receiver,
	struct downstream_counts *counts,
	const struct v92_cpd_base_parameters *expected_cpd,
	bool request_silence)
{
	struct pcm_downstream_event event;
	uint8_t octet;
	enum pcm_downstream_rx_result result;
	enum v92_phase4_digital_station_result station_result;

	station_result = v92_phase4_digital_station_transmit(station, &octet);
	CHECK(station_result == V92_PHASE4_DIGITAL_STATION_OK,
	      "station downstream transmit stopped: %s valid=%u failed=%u "
	      "suvu/tx/cpu/ack=%u/%u/%u/%u child=%u/%u bit=%zu",
	      v92_phase4_digital_station_result_name(station_result),
	      v92_phase4_digital_station_is_valid(station), station->failed,
	      station->suvu_received, station->transmit_stage,
	      station->cpu_received, station->transmit_acknowledges_cpu,
	      station->transmit.stage, station->transmit.v92_control_kind,
	      station->transmit.control_bit_index);
	result = pcm_downstream_rx_receive(receiver, octet, &event);
	if (result == PCM_DOWNSTREAM_RX_BUFFERING ||
	    result == PCM_DOWNSTREAM_RX_TRN2D_FRAME)
		return true;
	if (result == PCM_DOWNSTREAM_RX_SUVD) {
		CHECK(event.control.type == V92_CONTROL_EVENT_SUVD &&
		      event.control.record.suvd.frame_result == V92_SUVD_OK &&
		      event.control.record.suvd.parameters.request_silence ==
			      request_silence,
		      "station emitted malformed SUVd");
		if (event.control.record.suvd.parameters.acknowledges_cpu)
			counts->acknowledged_suvd++;
		else {
			counts->initial_suvd++;
			if (counts->cpd != 0u)
				counts->status_suvd++;
		}
		return true;
	}
	if (result == PCM_DOWNSTREAM_RX_CPD) {
		CHECK(event.control.type == V92_CONTROL_EVENT_CPD &&
		      event.control.record.cpd.frame_result == V92_CPD_OK &&
		      cpd_selection_equal(
			      &event.control.record.cpd.parameters,
			      expected_cpd),
		      "station emitted malformed CPd");
		counts->cpd++;
		return true;
	}
	CHECK(false, "station downstream receiver returned %s",
	      pcm_downstream_rx_result_name(result));
	return false;
}

static bool receive_upstream_sample(
	struct v92_phase4_digital_station *station,
	struct pcm_downstream_rx *downstream,
	struct downstream_counts *counts, enum g711_law law,
	int8_t polarity, int16_t raw, size_t *events,
	const struct v92_cpd_base_parameters *expected_cpd,
	bool request_silence)
{
	enum v92_phase4_digital_station_result result =
		v92_phase4_digital_station_receive(
			station, line_sample(law, polarity, raw));

	CHECK(result == V92_PHASE4_DIGITAL_STATION_OK ||
	      result == V92_PHASE4_DIGITAL_STATION_EVENT,
	      "station upstream receive stopped: %s",
	      v92_phase4_digital_station_result_name(result));
	if (result == V92_PHASE4_DIGITAL_STATION_EVENT)
		(*events)++;
	return clock_downstream(
		station, downstream, counts, expected_cpd, request_silence);
}

static bool run_exchange(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	struct v92_suvu_parameters acknowledged_suvu = suvu;
	const struct v92_suvd_parameters suvd = {
		.request_silence = law == G711_LAW_PCMA,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v92_phase4_suvu_tx suvu_tx;
	struct v92_phase4_cpu_tx cpu_tx;
	struct v92_phase4_modulator modulator;
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct v92_phase4_digital_station station;
	struct v92_cpu_parameters retained;
	struct downstream_counts counts = { 0 };
	size_t events = 0u;

	CHECK(build_predecessor(
		      law, constellation, previous_sign, polarity,
		      &upstream, &handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &handoff.parameters) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream_tx, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&downstream_tx, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(
		      &downstream_rx, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_digital_station_init(
		      &station, &handoff, constellation,
		      &downstream_tx, &cpd) ==
		      V92_PHASE4_DIGITAL_STATION_OK,
	      "law %u constellation %u station did not initialize",
	      (unsigned int)law, (unsigned int)constellation);
	for (uint32_t symbol = 0u;
	     symbol < V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
			      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     ++symbol) {
		int16_t raw;

		CHECK(v92_pcm_phase4_trn2u_next(&upstream, &raw) ==
			      V92_PCM_PHASE4_TRN2U_OK &&
		      receive_upstream_sample(
			      &station, &downstream_rx, &counts, law,
			      polarity, raw, &events, &cpd,
			      suvd.request_silence),
		      "station TRN2u stopped at %u", symbol);
	}
	CHECK(v92_phase4_suvu_tx_init(
		      &suvu_tx, &upstream, true, &suvu) ==
		      V92_PHASE4_SUVU_TX_OK,
	      "station SUVu did not initialize");
	for (unsigned int repeat = 0u; repeat < 4u; ++repeat) {
		size_t record_symbol = 0u;

		if (repeat != 0u)
			CHECK(v92_phase4_suvu_tx_start_next_record(
				      &suvu_tx, &suvu) ==
				      V92_PHASE4_SUVU_TX_OK,
			      "station repeated SUVu did not start");
		while (!v92_phase4_suvu_tx_record_is_complete(&suvu_tx)) {
			int16_t raw;

			CHECK(v92_phase4_suvu_tx_next(&suvu_tx, &raw) ==
				      V92_PHASE4_SUVU_TX_OK,
			      "station SUVu generator stopped");
			/* Corrupt one first-record decision. The rejected record
			 * must not trigger CPd; canonical repeats exercise idempotent
			 * handling and flush the equalizer's delayed decision. */
			if (repeat == 0u && record_symbol == 5u)
				raw = (int16_t)-raw;
			record_symbol++;
			CHECK(
			      receive_upstream_sample(
				      &station, &downstream_rx, &counts,
				      law, polarity, raw, &events, &cpd,
				      suvd.request_silence),
			      "station SUVu stopped");
		}
		if (repeat == 1u)
			CHECK(!station.suvu_received &&
			      (station.transmit_stage ==
				       V92_PHASE4_DIGITAL_STATION_INITIAL_SUVD ||
			       station.transmit_stage ==
				       V92_PHASE4_DIGITAL_STATION_READY_FOR_CPD) &&
			      station.transmit.v92_control_kind ==
				      PCM_DOWNSTREAM_V92_CONTROL_SUVD,
			      "rejected SUVu changed downstream procedure state");
	}
	for (size_t symbol = 0u;
	     (counts.cpd == 0u || counts.status_suvd == 0u) &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol)
		CHECK(clock_downstream(
			      &station, &downstream_rx, &counts, &cpd,
			      suvd.request_silence),
		      "station CPd/SUVd completion stopped");
	CHECK(counts.cpd == 1u && counts.status_suvd >= 1u &&
	      station.transmit_stage ==
		      V92_PHASE4_DIGITAL_STATION_STATUS_SUVD &&
	      !station.transmit_acknowledges_cpu &&
	      !v92_phase4_digital_station_is_ed_ready(&station),
	      "station did not send one CPd followed by SUVd: %zu/%zu/%zu "
	      "events=%zu suvu=%u stage=%u child=%u/%u frame=%llu bit=%zu "
	      "rx=%u errors=%llu",
	      counts.initial_suvd, counts.cpd, counts.status_suvd, events,
	      station.suvu_received,
	      station.transmit_stage, station.transmit.stage,
	      station.transmit.v92_control_kind,
	      (unsigned long long)station.transmit.accepted_frames,
	      station.transmit.control_bit_index, downstream_rx.stage,
	      (unsigned long long)downstream_rx.trn2d_bit_errors);
	acknowledged_suvu.acknowledges_cpd = true;
	CHECK(v92_phase4_suvu_tx_start_next_record(
		      &suvu_tx, &acknowledged_suvu) ==
		      V92_PHASE4_SUVU_TX_OK,
	      "acknowledging SUVu did not start");
	while (!v92_phase4_suvu_tx_record_is_complete(&suvu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_suvu_tx_next(&suvu_tx, &raw) ==
			      V92_PHASE4_SUVU_TX_OK &&
		      receive_upstream_sample(
			      &station, &downstream_rx, &counts, law,
			      polarity, raw, &events, &cpd,
			      suvd.request_silence),
		      "acknowledging SUVu stopped");
	}
	CHECK(v92_phase4_suvu_tx_handoff(&suvu_tx, &modulator) ==
		      V92_PHASE4_SUVU_TX_OK &&
	      v92_phase4_cpu_tx_init(&cpu_tx, &modulator, &cpu) ==
		      V92_PHASE4_CPU_TX_OK,
	      "station CPu did not initialize");
	for (unsigned int repeat = 0u; repeat < 3u; ++repeat) {
		if (repeat == 1u) {
			cpu.acknowledges_cpd = true;
		}
		if (repeat != 0u)
			CHECK(v92_phase4_cpu_tx_start_next_record(
				      &cpu_tx, &cpu) == V92_PHASE4_CPU_TX_OK,
			      "station repeated CPu did not start");
		while (!v92_phase4_cpu_tx_record_is_complete(&cpu_tx)) {
			int16_t raw;

			CHECK(v92_phase4_cpu_tx_next(&cpu_tx, &raw) ==
				      V92_PHASE4_CPU_TX_OK &&
			      receive_upstream_sample(
				      &station, &downstream_rx, &counts,
				      law, polarity, raw, &events, &cpd,
				      suvd.request_silence),
			      "station CPu stopped");
		}
	}
	for (size_t symbol = 0u;
	     (counts.acknowledged_suvd == 0u ||
	      !v92_phase4_digital_station_is_ed_ready(&station)) &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol)
		CHECK(clock_downstream(
			      &station, &downstream_rx, &counts, &cpd,
			      suvd.request_silence),
		      "station downstream completion stopped");
	CHECK(events >= 5u && events <= 7u && station.suvu_received &&
	      station.transmit_stage ==
		      V92_PHASE4_DIGITAL_STATION_STATUS_SUVD &&
	      station.cpu_received && station.transmit_acknowledges_cpu &&
	      station.peer_acknowledged_cpd &&
	      station.suvu.acknowledges_cpd &&
	      counts.initial_suvd >= 1u && counts.cpd == 1u &&
	      counts.status_suvd >= 1u &&
	      counts.acknowledged_suvd >= 1u &&
	      v92_phase4_digital_station_cpu_parameters(
		      &station, &retained) &&
	      v92_phase4_digital_station_peer_acknowledged_cpd(&station) &&
	      v92_phase4_digital_station_is_ed_ready(&station) &&
	      cpu_equal(&retained, &cpu) &&
	      v92_phase4_digital_station_is_valid(&station) &&
	      !v92_phase4_digital_station_has_failed(&station),
	      "law %u constellation %u station exchange differed: "
	      "events=%zu records=%zu/%zu/%zu/%zu",
	      (unsigned int)law, (unsigned int)constellation, events,
	      counts.initial_suvd, counts.cpd, counts.status_suvd,
	      counts.acknowledged_suvd);
	if (law == G711_LAW_PCMU &&
	    constellation == V92_CONSTELLATION_4_POINT) {
		struct v92_cpu_parameters changed = cpu;
		bool rejected = false;

		changed.data_rate_number++;
		for (unsigned int repeat = 0u; repeat < 3u && !rejected;
		     ++repeat) {
			CHECK(v92_phase4_cpu_tx_start_next_record(
				      &cpu_tx, &changed) == V92_PHASE4_CPU_TX_OK,
			      "changed CPu did not start");
			while (!v92_phase4_cpu_tx_record_is_complete(&cpu_tx)) {
				int16_t raw;
				enum v92_phase4_digital_station_result result;

				CHECK(v92_phase4_cpu_tx_next(&cpu_tx, &raw) ==
					      V92_PHASE4_CPU_TX_OK,
				      "changed CPu generator stopped");
				result = v92_phase4_digital_station_receive(
					&station,
					line_sample(law, polarity, raw));
				if (result ==
				    V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR) {
					rejected = true;
					break;
				}
				CHECK(result == V92_PHASE4_DIGITAL_STATION_OK ||
				      result == V92_PHASE4_DIGITAL_STATION_EVENT,
				      "changed CPu stopped with %s",
				      v92_phase4_digital_station_result_name(result));
			}
		}
		memset(&retained, 0xa5, sizeof(retained));
		changed = retained;
		CHECK(rejected &&
		      v92_phase4_digital_station_has_failed(&station) &&
		      !v92_phase4_digital_station_cpu_parameters(
			      &station, &retained) &&
		      memcmp(&retained, &changed, sizeof(retained)) == 0 &&
		      !v92_phase4_digital_station_peer_acknowledged_cpd(
			      &station),
		      "changed CPu or failed handoff was accepted");
	}
	return true;
}

static bool test_generated_exchange(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0u;
		     index < ARRAY_LENGTH(constellations); ++index) {
			enum v92_constellation_size constellation =
				constellations[index];

			CHECK(run_exchange(
				      (enum g711_law)law,
				      constellation,
				      (uint8_t)((law + index) & 1u),
				      constellation == V92_CONSTELLATION_4_POINT ?
					      1 : -1),
			      "generated station matrix stopped");
		}
	}
	return true;
}

static bool base_cpd_equal(
	const struct v92_cpd_base_parameters *left,
	const struct v92_cpd_base_parameters *right)
{
	return left->four_times_gain_q0_16 == right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static bool run_paired_station_exchange(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	const struct v92_suvd_parameters suvd = {
		.request_silence = law == G711_LAW_PCMA,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v92_pcm_phase3_cpt_line_handoff digital_handoff;
	struct v92_phase3_analog_transmit_handoff analog_handoff = {
		.lu = TEST_LU,
		.training_constellation = constellation,
		.previous_encoded_sign = previous_sign,
	};
	struct v92_pcm_phase4_trn2u upstream;
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct v92_phase4_analog_station analog;
	struct v92_phase4_digital_station digital;
	struct v92_cpu_parameters retained_cpu;
	struct v92_cpu_parameters expected_cpu;
	struct v92_cpd_base_parameters retained_cpd;
	struct v92_phase4_modulator handoff;
	size_t cpu_repeat_wait = 0u;
	bool ready = false;

	CHECK(build_predecessor(
		      law, constellation, previous_sign, polarity,
		      &upstream, &digital_handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &digital_handoff.parameters) ==
		      V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream_tx, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&downstream_tx, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(
		      &downstream_rx, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_analog_station_init(
		      &analog, &analog_handoff, &downstream_rx,
		      &suvu, &cpu, NULL, NULL) ==
		      V92_PHASE4_ANALOG_STATION_OK &&
	      v92_phase4_digital_station_init(
		      &digital, &digital_handoff, constellation,
		      &downstream_tx, &cpd) ==
		      V92_PHASE4_DIGITAL_STATION_OK,
	      "paired station initialization failed law=%u constellation=%u",
	      (unsigned int)law, (unsigned int)constellation);

	/* The warm receive handoff already owns these delayed input symbols. */
	for (size_t symbol = 0u;
	     symbol < digital_handoff.input_advance_symbols; ++symbol) {
		int16_t ignored;

		CHECK(v92_phase4_analog_station_transmit(&analog, &ignored) ==
			      V92_PHASE4_ANALOG_STATION_OK,
		      "paired station alignment stopped at %zu", symbol);
	}
	for (size_t symbol = 0u; symbol < DOWNSTREAM_LIMIT * 8u; ++symbol) {
		int16_t upstream_sample;
		uint8_t downstream_octet;
		enum v92_phase4_analog_station_result analog_result;
		enum v92_phase4_digital_station_result digital_result;

		analog_result = v92_phase4_analog_station_transmit(
			&analog, &upstream_sample);
		if (analog_result == V92_PHASE4_ANALOG_STATION_E2U_READY) {
			ready = true;
			break;
		}
		CHECK(analog_result == V92_PHASE4_ANALOG_STATION_OK,
		      "paired analogue transmit stopped at %zu: %s", symbol,
		      v92_phase4_analog_station_result_name(analog_result));
		CHECK(v92_phase4_digital_station_transmit(
			      &digital, &downstream_octet) ==
			      V92_PHASE4_DIGITAL_STATION_OK,
		      "paired digital transmit stopped at %zu", symbol);
		digital_result = v92_phase4_digital_station_receive(
			&digital,
			line_sample(law, polarity, upstream_sample));
		CHECK(digital_result == V92_PHASE4_DIGITAL_STATION_OK ||
		      digital_result == V92_PHASE4_DIGITAL_STATION_EVENT,
		      "paired digital receive stopped at %zu: %s", symbol,
		      v92_phase4_digital_station_result_name(digital_result));
		analog_result = v92_phase4_analog_station_receive(
			&analog, downstream_octet);
		CHECK(analog_result == V92_PHASE4_ANALOG_STATION_OK ||
		      analog_result == V92_PHASE4_ANALOG_STATION_EVENT,
		      "paired analogue receive stopped at %zu: %s", symbol,
		      v92_phase4_analog_station_result_name(analog_result));
		if (analog.transmit.cpu_sent && !digital.cpu_received &&
		    !analog.transmit.cpu_pending) {
			if (cpu_repeat_wait == 0u)
				cpu_repeat_wait = 800u;
			else if (--cpu_repeat_wait == 0u)
				CHECK(v92_phase4_analog_station_repeat_cpu(&analog) ==
					      V92_PHASE4_ANALOG_STATION_OK,
				      "paired CPu repeat was rejected at %zu",
				      symbol);
		}
	}
	for (size_t symbol = 0u;
	     ready && !v92_phase4_digital_station_is_ed_ready(&digital) &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol) {
		uint8_t ignored;

		CHECK(v92_phase4_digital_station_transmit(&digital, &ignored) ==
			      V92_PHASE4_DIGITAL_STATION_OK,
		      "paired digital handoff stopped at %zu", symbol);
	}
	expected_cpu = cpu;
	expected_cpu.acknowledges_cpd = true;
	CHECK(ready && v92_phase4_analog_station_is_e2u_ready(&analog) &&
	      v92_phase4_analog_station_cpd_parameters(
		      &analog, &retained_cpd) &&
	      base_cpd_equal(&retained_cpd, &cpd) &&
	      v92_phase4_digital_station_cpu_parameters(
		      &digital, &retained_cpu) &&
	      cpu_equal(&retained_cpu, &expected_cpu) &&
	      v92_phase4_digital_station_peer_acknowledged_cpd(&digital) &&
	      v92_phase4_digital_station_is_ed_ready(&digital) &&
	      analog.suvd.request_silence == suvd.request_silence &&
	      v92_phase4_analog_station_handoff(&analog, &handoff) ==
		      V92_PHASE4_ANALOG_STATION_OK &&
	      v92_phase4_modulator_is_valid(&handoff),
	      "paired station exchange did not reach E2u law=%u constellation=%u "
	      "ready=%u a=%u/%u/%u/%u/%u tx=%u/%u/%u/%u d=%u/%u/%u/%u",
	      (unsigned int)law, (unsigned int)constellation, ready,
	      analog.suvd_received, analog.cpd_received,
	      analog.transmit.cpu_sent, analog.transmit.acknowledgment_sent,
	      analog.transmit.peer_confirmation_received,
	      analog.transmit.stage, analog.receive.stage,
	      analog.transmit.received_suvd, analog.transmit.received_cpd,
	      digital.suvu_received, digital.cpu_received,
	      digital.peer_acknowledged_cpd,
	      digital.transmit_acknowledges_cpu);
	return true;
}

static bool test_paired_station_matrix(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0u;
		     index < ARRAY_LENGTH(constellations); ++index) {
			CHECK(run_paired_station_exchange(
				      (enum g711_law)law,
				      constellations[index],
				      (uint8_t)((law + index) & 1u),
				      index == 0u ? 1 : -1),
			      "paired station matrix stopped");
		}
	}
	return true;
}

static bool test_analog_early_control(void)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	const struct v92_suvd_parameters early_suvd = {
		.request_silence = true,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(G711_LAW_PCMU);
	struct v92_cpd_base_parameters early_cpd = cpd_parameters();
	struct v92_phase3_analog_transmit_handoff handoff = {
		.lu = TEST_LU,
		.training_constellation = V92_CONSTELLATION_4_POINT,
		.previous_encoded_sign = 1u,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_rx downstream;
	struct v92_phase4_analog_station station;

	{
		struct v92_cpt_parameters cpt = cpt_parameters(G711_LAW_PCMU);

		CHECK(v90_pcm_mapping_init_from_v92_cpt(&mapping, &cpt) ==
			      V90_PCM_MAPPING_OK,
		      "early-control mapping differed");
	}
	CHECK(pcm_downstream_rx_init(
		      &downstream, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_analog_station_init(
		      &station, &handoff, &downstream, &suvu, &cpu,
		      &early_suvd, &early_cpd) ==
		      V92_PHASE4_ANALOG_STATION_OK &&
	      station.suvd_received && station.cpd_received &&
	      station.transmit.received_suvd &&
	      station.transmit.received_cpd &&
	      v92_phase4_analog_tx_silence_requested(&station.transmit),
	      "early downstream controls were not retained");
	return true;
}

static bool test_compact_boundary(void)
{
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v92_suvd_parameters suvd = { 0 };
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream;
	struct v92_phase4_digital_station station;
	struct v92_phase4_digital_station saved;
	uint8_t octet = UINT8_C(0xa5);

	CHECK(build_predecessor(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1,
		      &upstream, &handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &handoff.parameters) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&downstream, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "compact-boundary station predecessor did not initialize");
	for (unsigned int mutation = 0u; mutation < 2u; ++mutation) {
		struct v92_cpd_base_parameters rejected = cpd;

		if (mutation == 0u)
			rejected.four_times_gain_q0_16 = 0u;
		else
			rejected.acknowledges_cpu = true;
		memset(&station, 0xa5, sizeof(station));
		saved = station;
		CHECK(v92_phase4_digital_station_init(
			      &station, &handoff,
			      V92_CONSTELLATION_4_POINT,
			      &downstream, &rejected) ==
			      V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS &&
		      memcmp(&station, &saved, sizeof(station)) == 0,
		      "invalid initial CPd mutation %u changed station",
		      mutation);
	}
	CHECK(v92_phase4_digital_station_receive(NULL, 0) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase4_digital_station_receive(&station, 0) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_STATE &&
	      v92_phase4_digital_station_transmit(NULL, &octet) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase4_digital_station_transmit(&station, &octet) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_STATE &&
	      octet == UINT8_C(0xa5) &&
	      strcmp(v92_phase4_digital_station_result_name(
		     V92_PHASE4_DIGITAL_STATION_INVALID_STATE),
		     "invalid state") == 0,
	      "station invalid-call contract changed");
	return true;
}

static bool test_early_cpu_acknowledgement(void)
{
	const struct v92_suvd_parameters suvd = {
		.request_silence = true,
	};
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream;
	struct v92_phase4_digital_station station;
	struct pcm_downstream_symbol ignored;
	uint8_t octet;

	CHECK(build_predecessor(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1,
		      &upstream, &handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &handoff.parameters) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&downstream, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "early-CPu station predecessor did not initialize");
	for (size_t symbol = 0u;
	     !pcm_downstream_tx_is_v92_control_boundary(&downstream) &&
		     symbol < DOWNSTREAM_LIMIT * 4u;
	     ++symbol)
		CHECK(pcm_downstream_tx_next(&downstream, &ignored) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "inherited SUVd stopped at %zu", symbol);
	CHECK(pcm_downstream_tx_is_v92_control_boundary(&downstream) &&
	      v92_phase4_digital_station_init(
		      &station, &handoff, V92_CONSTELLATION_4_POINT,
		      &downstream, &cpd) == V92_PHASE4_DIGITAL_STATION_OK,
	      "early-CPu station did not inherit completed SUVd");
	station.suvu_received = true;
	station.suvu = (struct v92_suvu_parameters){
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	station.cpu = cpu_parameters(G711_LAW_PCMU);
	station.cpu_received = true;
	CHECK(v92_phase4_digital_station_is_valid(&station),
	      "early-CPu fixture is invalid");
	for (size_t symbol = 0u;
	     station.transmit_stage != V92_PHASE4_DIGITAL_STATION_CPD &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol)
		CHECK(v92_phase4_digital_station_transmit(&station, &octet) ==
			      V92_PHASE4_DIGITAL_STATION_OK,
		      "early-CPu downstream stopped at %zu", symbol);
	CHECK(station.transmit_stage == V92_PHASE4_DIGITAL_STATION_CPD &&
	      station.transmit_acknowledges_cpu &&
	      station.transmit.v92_control_kind ==
		      PCM_DOWNSTREAM_V92_CONTROL_CPD &&
	      station.transmit.control.cpd.acknowledges_cpu &&
	      v92_phase4_digital_station_is_valid(&station),
	      "CPu received before CPd did not select CPd prime");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated digital station exchange", test_generated_exchange },
		{ "paired analogue/digital station matrix",
		  test_paired_station_matrix },
		{ "analogue early control handoff", test_analog_early_control },
		{ "compact station boundary", test_compact_boundary },
		{ "early CPu acknowledgement", test_early_cpu_acknowledgement },
	};
	unsigned int failures = 0u;

	for (size_t index = 0u; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run()) {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		} else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
