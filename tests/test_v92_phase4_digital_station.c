// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>
#include <v34_framing.h>
#include <v90_pcm_frame_encoder.h>
#include <v92_cpd_direct.h>
#include <v92_pcm_frame.h>
#include <v92_phase4_analog_station.h>
#include <v92_phase4_digital_station.h>

#include "test.h"
#include "v92_phase4_record_fixture.h"

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

static bool full_cpd_parameters(
	enum g711_law law, struct v92_cpd_parameters *parameters)
{
	return parameters != NULL &&
	       v92_cpd_direct_init(parameters, law, 12u, true) == V92_CPD_OK;
}

static struct v92_cpd_parameters cpd_parameters(enum g711_law law)
{
	struct v92_cpd_parameters full = { 0 };

	(void)full_cpd_parameters(law, &full);
	return full;
}

static struct v92_cpd_base_parameters compact_cpd_parameters(enum g711_law law)
{
	struct v92_cpd_parameters full;

	if (!full_cpd_parameters(law, &full))
		return (struct v92_cpd_base_parameters){ 0 };
	return (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = full.four_times_gain_q0_16,
		.data_rate_number = full.data_rate_number,
		.trellis = full.trellis,
		.extend_e2u = full.extend_e2u,
	};
}

struct full_cpd_source {
	struct v90_pcm_frame_encoder encoder;
	struct v34_scrambler scrambler;
};

static uint8_t sequence_bit(const uint8_t *sequence, size_t bit)
{
	return (uint8_t)((sequence[bit / 8u] >> (7u - bit % 8u)) & 1u);
}

static bool full_cpd_source_init(
	struct full_cpd_source *source,
	const struct v90_pcm_mapping *mapping)
{
	struct full_cpd_source initialized = { 0 };

	if (source == NULL || mapping == NULL ||
	    v90_pcm_frame_encoder_init(&initialized.encoder, mapping) !=
		    V90_PCM_FRAME_ENCODER_OK ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return false;
	*source = initialized;
	return true;
}

static enum v92_phase4_analog_station_result push_full_cpd_frame(
	struct full_cpd_source *source,
	struct v92_phase4_analog_station *station, const uint8_t *bits,
	size_t *events)
{
	struct v90_pcm_frame_output output;
	uint8_t scrambled[V90_PCM_FRAME_MAX_BITS];
	size_t frame_bits = source->encoder.mapping.frame_bits;
	enum v90_pcm_frame_encoder_result encoder_result;

	for (size_t bit = 0u; bit < frame_bits; ++bit) {
		if (v34_scramble_group(
			    &source->scrambler, bits[bit], 1u,
			    &scrambled[bit]) != V34_FRAMING_OK)
			return V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR;
	}
	encoder_result = v90_pcm_frame_encoder_process(
		&source->encoder, scrambled, frame_bits, &output);
	if (encoder_result == V90_PCM_FRAME_ENCODER_BUFFERING)
		return V92_PHASE4_ANALOG_STATION_OK;
	if (encoder_result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
		return V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR;
	for (size_t symbol = 0u; symbol < output.symbol_count; ++symbol) {
		enum v92_phase4_analog_station_result result =
			v92_phase4_analog_station_receive(
				station, output.symbols[symbol].octet);

		if (result == V92_PHASE4_ANALOG_STATION_EVENT) {
			(*events)++;
			continue;
		}
		if (result != V92_PHASE4_ANALOG_STATION_OK)
			return result;
	}
	return V92_PHASE4_ANALOG_STATION_OK;
}

static enum v92_phase4_analog_station_result push_full_cpd_sequence(
	struct full_cpd_source *source,
	struct v92_phase4_analog_station *station,
	const uint8_t *sequence, size_t bit_count, size_t *events)
{
	uint8_t frame[V90_PCM_FRAME_MAX_BITS];
	size_t frame_bits = source->encoder.mapping.frame_bits;

	if (bit_count == 0u || bit_count % frame_bits != 0u)
		return V92_PHASE4_ANALOG_STATION_INTERNAL_ERROR;
	for (size_t first = 0u; first < bit_count; first += frame_bits) {
		for (size_t bit = 0u; bit < frame_bits; ++bit)
			frame[bit] = sequence_bit(sequence, first + bit);
		{
			enum v92_phase4_analog_station_result result =
				push_full_cpd_frame(
					source, station, frame, events);

			if (result != V92_PHASE4_ANALOG_STATION_OK)
				return result;
		}
	}
	return V92_PHASE4_ANALOG_STATION_OK;
}

static bool push_full_cpd_training(
	struct full_cpd_source *source,
	struct v92_phase4_analog_station *station, size_t *events)
{
	uint8_t ones[V90_PCM_FRAME_MAX_BITS];

	memset(ones, 1, sizeof(ones));
	for (size_t frame = 0u;
	     frame < V90_PHASE4_TRN2D_MINIMUM_FRAMES; ++frame) {
		if (push_full_cpd_frame(source, station, ones, events) !=
		    V92_PHASE4_ANALOG_STATION_OK)
			return false;
	}
	return true;
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
	const struct v92_cpd_parameters *right, enum g711_law law)
{
	struct v92_pcm_mapping actual, expected;

	return v92_pcm_mapping_init(&actual, law, left) == V92_PCM_MAPPING_OK &&
	       v92_pcm_mapping_init(&expected, law, right) == V92_PCM_MAPPING_OK &&
	       v92_pcm_mapping_equal(&actual, &expected);
}

static bool clock_downstream(
	struct v92_phase4_digital_station *station,
	struct pcm_downstream_rx *receiver,
	struct downstream_counts *counts,
	const struct v92_cpd_parameters *expected_cpd,
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
	      v92_phase4_digital_station_is_valid(station), station->control.failed,
	      station->control.suvu_received, station->control.transmit_stage,
	      station->control.cpu_received, station->control.transmit_acknowledges_cpu,
	      station->control.transmit.stage, station->control.transmit.v92_control_kind,
	      station->control.transmit.control_bit_index);
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
		struct v92_cpd_parameters expected = *expected_cpd;

		expected.acknowledges_cpu = event.control.record.cpd.parameters.acknowledges_cpu;
		CHECK(event.control.type == V92_CONTROL_EVENT_CPD &&
		      event.control.record.cpd.frame_result == V92_CPD_OK &&
		      cpd_selection_equal(
			      &event.control.record.cpd.parameters,
			      &expected, station->control.transmit.encoder.mapping.law) &&
		      (!expected.acknowledges_cpu || station->control.cpu_received),
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
	const struct v92_cpd_parameters *expected_cpd,
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

static bool feed_retry_status(
	struct v92_phase4_digital_station *station,
	struct v92_phase4_record_tx *source,
	const struct v92_suvu_parameters *suvu, bool corrupt,
	enum g711_law law, int8_t polarity, size_t *events)
{
	size_t rejected = 0u;

	for (unsigned int repeat = 0u; repeat < 3u; ++repeat) {
		CHECK(v92_record_fixture_start_suvu(source, NULL, suvu),
		      "retry SUVu did not start");
		if (corrupt)
			source->frame[5] ^= 1u;
		while (!v92_phase4_record_tx_is_complete(source)) {
			struct v92_phase4_digital_rx observer = station->receive;
			struct v92_control_event event = { 0 };
			int16_t raw;
			enum v92_phase4_digital_station_result result;

			CHECK(v92_phase4_record_tx_next(source, &raw) == V92_PHASE4_RECORD_TX_OK,
			      "retry SUVu generator stopped");
			raw = line_sample(law, polarity, raw);
			(void)v92_phase4_digital_rx_sample(&observer, raw, &event);
			rejected += event.type == V92_CONTROL_EVENT_SUVU_REJECTED;
			result = v92_phase4_digital_station_receive(station, raw);
			CHECK(result == V92_PHASE4_DIGITAL_STATION_OK ||
			      result == V92_PHASE4_DIGITAL_STATION_EVENT,
			      "retry status receive failed");
			*events += result == V92_PHASE4_DIGITAL_STATION_EVENT;
		}
	}
	CHECK(!corrupt || rejected != 0u, "retry fixture did not deliver a CRC rejection");
	return true;
}

static bool exercise_cpd_retry(
	struct v92_phase4_digital_station *station, struct pcm_downstream_rx *receive,
	struct downstream_counts *counts, struct v92_phase4_record_tx *source,
	const struct v92_suvu_parameters *suvu, const struct v92_cpd_parameters *cpd,
	enum g711_law law, int8_t polarity, bool silence, size_t *events)
{
	uint64_t deadline = station->control.status_start_frame * V90_CP_FRAME_INTERVALS +
		800u + station->control.round_trip_delay_samples;
	size_t status_count;

	CHECK(station->control.transmit.transmitted_symbols < deadline, "retry deadline already passed");
	while (station->control.transmit.transmitted_symbols + 1u < deadline)
		CHECK(clock_downstream(station, receive, counts, cpd, silence),
		      "pre-deadline downstream stopped");
	CHECK(feed_retry_status(station, source, suvu, false, law, polarity, events) &&
	      !station->control.repeat_cpd, "valid SUVu retried before 100 ms + RTD");
	/* Flush the receiver's delayed valid decision before crossing the timer. */
	CHECK(feed_retry_status(station, source, suvu, true, law, polarity, events) &&
	      !station->control.repeat_cpd, "damaged SUVu retried before the deadline");
	CHECK(clock_downstream(station, receive, counts, cpd, silence) &&
	      station->control.transmit.transmitted_symbols == deadline && !station->control.repeat_cpd,
	      "timer alone restarted CPd");
	CHECK(feed_retry_status(station, source, suvu, true, law, polarity, events) &&
	      !station->control.repeat_cpd, "CRC-rejected SUVu triggered CPd retry");
	CHECK(feed_retry_status(station, source, suvu, false, law, polarity, events) &&
	      station->control.repeat_cpd, "complete unacknowledging SUVu did not trigger CPd retry");
	for (size_t index = 0u; counts->cpd < 2u && index < DOWNSTREAM_LIMIT; ++index)
		CHECK(clock_downstream(station, receive, counts, cpd, silence),
		      "first repeated CPd stopped");
	status_count = counts->status_suvd;
	for (size_t index = 0u; counts->cpd < 4u && index < DOWNSTREAM_LIMIT; ++index)
		CHECK(clock_downstream(station, receive, counts, cpd, silence),
		      "continuous CPd repeat stopped");
	CHECK(counts->cpd == 4u && counts->status_suvd == status_count,
	      "CPd retry inserted SUVd or did not preserve complete repeated records");
	return true;
}

static bool run_exchange(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity, bool retry, bool other_codec, bool e2u_confirmation)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	struct v92_suvu_parameters acknowledged_suvu = suvu;
	const struct v92_suvd_parameters suvd = {
		.request_silence = law == G711_LAW_PCMA,
	};
	enum g711_law peer_codec = other_codec ?
		(law == G711_LAW_PCMU ? G711_LAW_PCMA : G711_LAW_PCMU) : law;
	struct v92_cpu_parameters cpu = cpu_parameters(peer_codec);
	struct v92_cpd_parameters cpd = cpd_parameters(law);
	if (e2u_confirmation)
		cpd.extend_e2u = constellation == V92_CONSTELLATION_8_POINT;
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v92_phase4_record_tx suvu_tx;
	struct v92_phase4_record_tx cpu_tx;
	struct v92_phase4_modulator modulator;
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct v92_phase4_digital_station station;
	struct v92_cpu_parameters retained;
	struct downstream_counts counts = { 0 };
	const struct v92_upstream_plan plan = {
		.gain = 2.0, .delay = 60u, .observation = V92_PCM_FRAME_CONSTELLATION,
	};
	size_t events = 0u;

	CHECK(build_predecessor(
		      law, constellation, previous_sign, polarity,
		      &upstream, &handoff), "station predecessor failed");
	handoff.parameters.law = peer_codec;
	CHECK(v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &handoff.parameters, law) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream_tx, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      (retry || pcm_downstream_tx_start_suvd(&downstream_tx, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK) &&
	      pcm_downstream_rx_init(
		      &downstream_rx, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_digital_station_init(
		      &station, &handoff, constellation,
		      &downstream_tx, &suvd, &cpd,
		      retry && constellation == V92_CONSTELLATION_8_POINT ? 2400u : 0u) ==
		      V92_PHASE4_DIGITAL_STATION_OK,
	      "law %u constellation %u station did not initialize",
	      (unsigned int)law, (unsigned int)constellation);
	CHECK(station.receive.signal.trn2u.law == law &&
	      station.control.transmit.encoder.mapping.law == law &&
	      handoff.parameters.law == peer_codec,
	      "peer codec changed network encoding or CPt metadata");
	CHECK(!v92_downstream_control_silence_requested(&station.control) &&
	      !v92_downstream_control_note_e2u(&station.control),
	      "initial training entered the rate-renegotiation silence procedure");
	CHECK(v92_phase4_digital_station_select_upstream(&station, &cpd, &plan) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_STATE,
	      "selected upstream before channel measurement");
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
	CHECK(v92_record_fixture_start_suvu(
		      &suvu_tx, &upstream.modulator, &suvu),
	      "station SUVu did not initialize");
	for (unsigned int repeat = 0u; repeat < 4u; ++repeat) {
		size_t record_symbol = 0u;

		if (repeat != 0u)
			CHECK(v92_record_fixture_start_suvu(
				      &suvu_tx, NULL, &suvu),
			      "station repeated SUVu did not start");
		while (!v92_phase4_record_tx_is_complete(&suvu_tx)) {
			int16_t raw;

			CHECK(v92_phase4_record_tx_next(&suvu_tx, &raw) ==
				      V92_PHASE4_RECORD_TX_OK,
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
			if (station.receive.channel.symbols == V92_UPSTREAM_CHANNEL_SYMBOLS &&
			    station.upstream_plan.observation == V92_PCM_FRAME_FILTER_OUTPUT) {
				struct v92_phase4_digital_station saved = station;
				struct v92_upstream_plan bad = plan;

				bad.delay = 0u;
				CHECK(v92_phase4_digital_station_select_upstream(&station, &cpd, &bad) ==
					      V92_PHASE4_DIGITAL_STATION_INVALID_PARAMETERS &&
				      memcmp(&station, &saved, sizeof(station)) == 0,
				      "invalid receive plan changed CPd");
				++cpd.four_times_gain_q0_16;
				CHECK(v92_phase4_digital_station_select_upstream(&station, &cpd, &plan) ==
					      V92_PHASE4_DIGITAL_STATION_OK,
				      "completed channel did not select initial CPd");
			}
		}
		if (repeat == 1u)
			CHECK(!station.control.suvu_received &&
			      (station.control.transmit_stage ==
				       V92_DOWNSTREAM_CONTROL_INITIAL_SUVD ||
			       station.control.transmit_stage ==
				       V92_DOWNSTREAM_CONTROL_READY_FOR_CPD) &&
			      station.control.transmit.v92_control_kind ==
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
	      station.control.transmit_stage ==
		      V92_DOWNSTREAM_CONTROL_STATUS_SUVD &&
	      !station.control.transmit_acknowledges_cpu &&
	      !v92_phase4_digital_station_is_ed_ready(&station),
	      "station did not send one CPd followed by SUVd: %zu/%zu/%zu "
	      "events=%zu suvu=%u stage=%u child=%u/%u frame=%llu bit=%zu "
	      "rx=%u errors=%llu",
	      counts.initial_suvd, counts.cpd, counts.status_suvd, events,
	      station.control.suvu_received,
	      station.control.transmit_stage, station.control.transmit.stage,
	      station.control.transmit.v92_control_kind,
	      (unsigned long long)station.control.transmit.accepted_frames,
	      station.control.transmit.control_bit_index, downstream_rx.stage,
	      (unsigned long long)downstream_rx.trn2d_bit_errors);
	if (retry)
		CHECK(exercise_cpd_retry(&station, &downstream_rx, &counts, &suvu_tx,
			      &suvu, &cpd, law, polarity, suvd.request_silence, &events),
		      "CPd retry procedure failed");
	acknowledged_suvu.acknowledges_cpd = !e2u_confirmation;
	CHECK(v92_record_fixture_start_suvu(
		      &suvu_tx, NULL, &acknowledged_suvu),
	      "acknowledging SUVu did not start");
	while (!v92_phase4_record_tx_is_complete(&suvu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_record_tx_next(&suvu_tx, &raw) ==
			      V92_PHASE4_RECORD_TX_OK &&
		      receive_upstream_sample(
			      &station, &downstream_rx, &counts, law,
			      polarity, raw, &events, &cpd,
			      suvd.request_silence),
		      "acknowledging SUVu stopped");
	}
	CHECK(v92_phase4_record_tx_handoff(&suvu_tx, &modulator) ==
		      V92_PHASE4_RECORD_TX_OK &&
	      v92_record_fixture_start_cpu(&cpu_tx, &modulator, &cpu),
	      "station CPu did not initialize");
	for (unsigned int repeat = 0u; repeat < 3u; ++repeat) {
		if (repeat == 1u && !e2u_confirmation) {
			cpu.acknowledges_cpd = true;
		}
		if (repeat != 0u)
			CHECK(v92_record_fixture_start_cpu(
				      &cpu_tx, NULL, &cpu),
			      "station repeated CPu did not start");
		while (!v92_phase4_record_tx_is_complete(&cpu_tx)) {
			int16_t raw;

			CHECK(v92_phase4_record_tx_next(&cpu_tx, &raw) ==
				      V92_PHASE4_RECORD_TX_OK &&
			      receive_upstream_sample(
				      &station, &downstream_rx, &counts,
				      law, polarity, raw, &events, &cpd,
				      suvd.request_silence),
			      "station CPu stopped");
		}
	}
	if (e2u_confirmation) {
		CHECK(v92_phase4_record_tx_handoff(&cpu_tx, &modulator) == V92_PHASE4_RECORD_TX_OK,
		      "unacknowledged CPu did not retain its modulator");
		for (unsigned repeat = 0; repeat < 8u; ++repeat) {
			CHECK(v92_record_fixture_start_suvu(&suvu_tx, repeat ? NULL : &modulator, &suvu),
			      "unacknowledged status did not start");
			while (!v92_phase4_record_tx_is_complete(&suvu_tx)) {
				int16_t raw;
				CHECK(v92_phase4_record_tx_next(&suvu_tx, &raw) == V92_PHASE4_RECORD_TX_OK &&
				      receive_upstream_sample(&station, &downstream_rx, &counts, law,
					polarity, raw, &events, &cpd, suvd.request_silence) &&
				      !v92_phase4_digital_station_is_ed_ready(&station),
				      "unacknowledged control falsely completed the exchange");
			}
		}
		CHECK(station.control.cpu_received && counts.acknowledged_suvd &&
		      v92_downstream_control_e2u_delay(&station.control) == 0u,
		      "local acknowledgement did not reach the peer");
		/* Lose the final SUVu' to a CRC error, not a missing input interval.
		 * E2u is the alternative confirmation in V.92 9.6.1.1.4. */
		uint8_t damaged[V92_SUVU_FRAME_BYTES];
		size_t bits;
		acknowledged_suvu.acknowledges_cpd = true;
		CHECK(v92_suvu_encode(&acknowledged_suvu,
			v92_phase4_modulator_bits_per_symbol(constellation) * V92_SUVU_SYMBOLS_PER_FRAME,
			damaged, sizeof(damaged), &bits) == V92_SUVU_OK, "final SUVu encode failed");
		damaged[4] ^= 0x80u;
		CHECK(v92_phase4_record_tx_start_next(&suvu_tx, damaged, sizeof(damaged), bits) ==
		      V92_PHASE4_RECORD_TX_OK, "damaged final SUVu did not start");
		while (!v92_phase4_record_tx_is_complete(&suvu_tx)) {
			int16_t raw;
			CHECK(v92_phase4_record_tx_next(&suvu_tx, &raw) == V92_PHASE4_RECORD_TX_OK &&
			      receive_upstream_sample(&station, &downstream_rx, &counts, law,
				polarity, raw, &events, &cpd, suvd.request_silence), "final SUVu failed");
		}
		CHECK(v92_phase4_record_tx_handoff(&suvu_tx, &modulator) == V92_PHASE4_RECORD_TX_OK &&
		      v92_phase4_record_tx_init_e2u(&suvu_tx, &modulator, cpd.extend_e2u) ==
		      V92_PHASE4_RECORD_TX_OK, "E2u did not start");
		for (unsigned n = 0; n < 128u && !v92_phase4_digital_station_is_ed_ready(&station); ++n) {
			int16_t raw = 0;
			if (!v92_phase4_record_tx_is_complete(&suvu_tx))
				CHECK(v92_phase4_record_tx_next(&suvu_tx, &raw) == V92_PHASE4_RECORD_TX_OK,
				      "E2u generation failed");
			/* Zero tail only flushes the causal receive delay after exact E2u. */
			CHECK(receive_upstream_sample(&station, &downstream_rx, &counts, law,
				polarity, raw, &events, &cpd, suvd.request_silence), "E2u reception failed");
		}
		struct v92_phase4_digital_handoff selected;
		CHECK(v92_phase4_digital_station_handoff(&station, &selected) == V92_PHASE4_DIGITAL_STATION_OK &&
		      !selected.pause && selected.searching_e2u && v92_e2u_rx_is_complete(&selected.e2u) &&
		      !station.control.cpu.acknowledges_cpd && !station.control.suvu.acknowledges_cpd &&
		      selected.mapping.cpd.extend_e2u == cpd.extend_e2u,
		      "lost acknowledgement prevented the exact E2u handoff");
		struct v92_control_event repeat = {
			.type = V92_CONTROL_EVENT_CPU,
			.record.cpu = { .frame_result = V92_CPU_OK, .parameters = cpu },
		};
		CHECK(v92_downstream_control_receive(&station.control, &repeat) &&
		      v92_phase4_digital_station_is_ed_ready(&station),
		      "an in-flight CPu erased E2u confirmation");
		struct v92_phase4_digital_station saved = station;
		CHECK(v92_phase4_digital_station_receive(&station, 0) == V92_PHASE4_DIGITAL_STATION_INVALID_STATE &&
		      memcmp(&station, &saved, sizeof(station)) == 0,
		      "completed E2u consumed the next owner's sample");
		return true;
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
	CHECK(events >= 5u && (retry || events <= 7u) && station.control.suvu_received &&
	      station.control.transmit_stage ==
		      V92_DOWNSTREAM_CONTROL_STATUS_SUVD &&
	      station.control.cpu_received && station.control.transmit_acknowledges_cpu &&
	      station.control.peer_confirmation_received &&
	      station.control.suvu.acknowledges_cpd &&
	      counts.initial_suvd >= 1u && (retry ? counts.cpd >= 4u : counts.cpd == 1u) &&
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
	{
		struct v92_phase4_digital_handoff selected;
		struct v92_phase4_digital_station saved = station;

		CHECK(v92_phase4_digital_station_handoff(&station, &selected) ==
			      V92_PHASE4_DIGITAL_STATION_OK &&
		      memcmp(&selected.upstream_plan, &plan, sizeof(plan)) == 0 &&
		      v92_phase4_digital_station_select_upstream(&station, &cpd, &plan) ==
			      V92_PHASE4_DIGITAL_STATION_INVALID_STATE &&
		      memcmp(&station, &saved, sizeof(station)) == 0,
		      "CPd repeat or acknowledgement changed the frozen receive plan");
		selected.upstream_plan.delay = 0u;
		CHECK(!v92_phase4_digital_handoff_is_valid(&selected),
		      "invalid receive plan crossed station handoff");
	}
	if (law == G711_LAW_PCMU &&
	    constellation == V92_CONSTELLATION_4_POINT) {
		struct v92_cpu_parameters changed = cpu;
		bool rejected = false;

		changed.data_rate_number++;
		for (unsigned int repeat = 0u; repeat < 3u && !rejected;
		     ++repeat) {
			CHECK(v92_record_fixture_start_cpu(
				      &cpu_tx, NULL, &changed),
			      "changed CPu did not start");
			while (!v92_phase4_record_tx_is_complete(&cpu_tx)) {
				int16_t raw;
				enum v92_phase4_digital_station_result result;

				CHECK(v92_phase4_record_tx_next(&cpu_tx, &raw) ==
					      V92_PHASE4_RECORD_TX_OK,
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
					      1 : -1, false, false, false) &&
			      run_exchange((enum g711_law)law, constellation, 1u, 1, true, false, false) &&
			      run_exchange((enum g711_law)law, constellation, 1u, -1, false, false, true),
			      "generated station matrix stopped");
		}
		CHECK(run_exchange(law, V92_CONSTELLATION_4_POINT, 0u, 1, false, true, false),
		      "opposite peer codec changed the network-law exchange");
	}
	return true;
}

static bool base_cpd_equal(
	const struct v92_cpd_base_parameters *left,
	const struct v92_cpd_parameters *right)
{
	return left->four_times_gain_q0_16 == right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static struct v92_cpd_parameters omit_cpd_sections(struct v92_cpd_parameters parameters, unsigned omitted)
{
	if (omitted & 1u) {
		parameters.modulus_present = false;
		memset(parameters.modulus, 0, sizeof(parameters.modulus));
	}
	if (omitted & 2u) {
		parameters.filters_present = false;
		memset(parameters.filter, 0, sizeof(parameters.filter));
	}
	if (omitted & 4u) {
		parameters.constellations_present = false;
		memset(parameters.constellation, 0, sizeof(parameters.constellation));
		memset(parameters.frame_constellation, 0, sizeof(parameters.frame_constellation));
	}
	return parameters;
}

static bool run_full_cpd_ownership(enum g711_law law, unsigned omitted)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_parameters full;
	struct v92_cpd_parameters wire;
	struct v92_cpd_parameters expected;
	struct v92_cpd_base_parameters compact = compact_cpd_parameters(law);
	struct v92_phase3_analog_transmit_handoff handoff = {
		.lu = TEST_LU,
		.training_constellation = V92_CONSTELLATION_4_POINT,
		.previous_encoded_sign = 1u,
	};
	struct v92_cpt_parameters cpt = cpt_parameters(law);
	struct v90_pcm_mapping training_mapping;
	struct v92_pcm_mapping expected_mapping;
	struct pcm_downstream_rx downstream;
	struct v92_phase4_analog_station station;
	struct full_cpd_source source;
	uint8_t full_sequence[V92_CPD_DIRECT_MAX_FRAME_BYTES] = { 0 };
	uint8_t compact_sequence[V92_CPD_BASE_MAX_FRAME_BYTES] = { 0 };
	size_t full_bits = 0u;
	size_t compact_bits = 0u;
	size_t events = 0u;

	CHECK(full_cpd_parameters(law, &full) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &training_mapping, &cpt, cpt.law) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_rx_init(
		      &downstream, PCM_PROTOCOL_V92, &training_mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_analog_station_init(
		      &station, &handoff, &downstream, &suvu, &cpu,
		      NULL, NULL) == V92_PHASE4_ANALOG_STATION_OK &&
	      full_cpd_source_init(&source, &training_mapping),
	      "full CPd fixture did not initialize for law %u",
	      (unsigned int)law);
	wire = full;
	if (omitted) {
		struct v92_cpd_parameters previous = full;
		struct v92_pcm_mapping retained;
		previous.data_rate_number--;
		previous.acknowledges_cpu = cpu.acknowledges_cpd = true;
		full.extend_e2u = compact.extend_e2u = false;
		CHECK(v92_pcm_mapping_init(&retained, law, &previous) == V92_PCM_MAPPING_OK &&
		      v92_phase4_analog_station_init_after_ru(&station, V92_CONSTELLATION_4_POINT,
			TEST_LU, &downstream, &cpu, &retained, false, 80u) == V92_PHASE4_ANALOG_STATION_OK &&
		      !station.cpd_received && !station.transmit.control.received_cpd &&
		      !station.transmit.control.cpu.acknowledges_cpd && !station.mapping.cpd.acknowledges_cpu &&
		      !v92_phase4_analog_station_can_handoff(&station) &&
		      station.mapping.cpd.data_rate_number == previous.data_rate_number,
		      "rate reentry reused an acknowledgement or lost the prior mapping");
		wire = omit_cpd_sections(full, omitted);
		struct v92_phase4_analog_station before = station;
		struct pcm_downstream_rx invalid = downstream;
		invalid.protocol = PCM_PROTOCOL_V90;
		CHECK(v92_phase4_analog_station_init_after_ru(NULL, V92_CONSTELLATION_4_POINT,
			TEST_LU, &downstream, &cpu, &retained, false, 80u) == V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
		      v92_phase4_analog_station_init_after_ru(&station, V92_CONSTELLATION_4_POINT,
			TEST_LU, &invalid, &cpu, &retained, false, 80u) == V92_PHASE4_ANALOG_STATION_INVALID_PARAMETERS &&
		      v92_phase4_analog_station_init_after_ru(&station, V92_CONSTELLATION_4_POINT,
			TEST_LU, &downstream, &cpu, NULL, false, 80u) == V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
		      memcmp(&station, &before, sizeof(station)) == 0, "invalid station reentry was not transactional");
	}
	CHECK(v92_cpd_encode(&wire, training_mapping.frame_bits, full_sequence,
		      sizeof(full_sequence), &full_bits) == V92_CPD_OK &&
	      push_full_cpd_training(&source, &station, &events), "CPd wire setup failed");
	{
		struct v92_phase4_analog_station rejected = station;
		struct full_cpd_source rejected_source = source;
		uint8_t invalid[V92_CPD_BASE_MAX_FRAME_BYTES];
		size_t bits, ignored = 0u;
		const struct v92_suvd_parameters silence = { .request_silence = true };
		/* Initial training cannot invent a prior mapping. A silence request
		 * during RRN retains one, but still needs mutual acknowledgement. */
		CHECK(omitted ?
		      v92_suvd_encode(&silence, training_mapping.frame_bits, invalid, sizeof(invalid), &bits) == V92_SUVD_OK :
		      v92_cpd_encode_base(&compact, training_mapping.frame_bits, invalid, sizeof(invalid), &bits) == V92_CPD_OK,
		      "rejected control did not encode");
		enum v92_phase4_analog_station_result result = V92_PHASE4_ANALOG_STATION_OK;
		for (unsigned repeat = 0u; repeat < 3u && result == V92_PHASE4_ANALOG_STATION_OK; ++repeat)
			result = push_full_cpd_sequence(&rejected_source, &rejected, invalid, bits, &ignored);
		CHECK((omitted ? result == V92_PHASE4_ANALOG_STATION_OK &&
		      v92_phase4_analog_tx_silence_requested(&rejected.transmit) :
		      result == V92_PHASE4_ANALOG_STATION_FAILED && rejected.failed) &&
		      !v92_phase4_analog_station_can_handoff(&rejected) &&
		      memcmp(&rejected.mapping, &station.mapping, sizeof(station.mapping)) == 0 &&
		      (omitted || memcmp(&rejected.transmit, &station.transmit, sizeof(station.transmit)) == 0),
		      "control request changed the committed mapping or prematurely finished");
	}
	for (unsigned int repeat = 0u; repeat < 2u; ++repeat)
		CHECK(push_full_cpd_sequence(
			      &source, &station, full_sequence, full_bits,
			      &events) == V92_PHASE4_ANALOG_STATION_OK,
		      "full CPd stopped for law %u repeat %u",
		      (unsigned int)law, repeat);
	CHECK(station.cpd_received && !station.cpd.acknowledges_cpu &&
	      v92_pcm_mapping_init(&expected_mapping, law, &full) ==
		      V92_PCM_MAPPING_OK &&
	      v92_pcm_mapping_equal(&station.mapping, &expected_mapping),
	      "full CPd was not retained for law %u", (unsigned int)law);
	/* Identical partial repeats must also work during initial training. */
	for (unsigned mask = 1u; omitted == 0u && mask < 7u; ++mask) {
		struct v92_cpd_parameters partial = omit_cpd_sections(full, mask);
		CHECK(v92_cpd_encode(&partial, training_mapping.frame_bits, full_sequence,
			sizeof(full_sequence), &full_bits) == V92_CPD_OK, "partial repeat did not encode");
		for (unsigned repeat = 0; repeat < 2u; ++repeat)
			CHECK(push_full_cpd_sequence(&source, &station, full_sequence, full_bits, &events) ==
				V92_PHASE4_ANALOG_STATION_OK, "partial CPd repeat rejected");
		CHECK(v92_pcm_mapping_equal(&station.mapping, &expected_mapping), "partial repeat changed mapping");
	}

	CHECK(v92_cpd_encode_base(
		      &compact, training_mapping.frame_bits, compact_sequence,
		      sizeof(compact_sequence), &compact_bits) == V92_CPD_OK,
	      "compact CPd did not encode for law %u", (unsigned int)law);
	for (unsigned int repeat = 0u; repeat < 2u; ++repeat)
		CHECK(push_full_cpd_sequence(
			      &source, &station, compact_sequence, compact_bits,
			      &events) == V92_PHASE4_ANALOG_STATION_OK,
		      "compact CPd repeat stopped for law %u", (unsigned int)law);
	CHECK(!station.cpd.acknowledges_cpu &&
	      v92_pcm_mapping_equal(&station.mapping, &expected_mapping),
	      "compact repeat changed the mapping for law %u",
	      (unsigned int)law);

	compact.acknowledges_cpu = true;
	memset(compact_sequence, 0, sizeof(compact_sequence));
	CHECK(v92_cpd_encode_base(
		      &compact, training_mapping.frame_bits, compact_sequence,
		      sizeof(compact_sequence), &compact_bits) == V92_CPD_OK,
	      "acknowledging CPd did not encode for law %u",
	      (unsigned int)law);
	for (unsigned int repeat = 0u; repeat < 2u; ++repeat)
		CHECK(push_full_cpd_sequence(
			      &source, &station, compact_sequence, compact_bits,
			      &events) == V92_PHASE4_ANALOG_STATION_OK,
		      "acknowledging CPd stopped for law %u", (unsigned int)law);
	expected = full;
	expected.acknowledges_cpu = true;
	CHECK(v92_pcm_mapping_init(&expected_mapping, law, &expected) ==
		      V92_PCM_MAPPING_OK &&
	      station.cpd.acknowledges_cpu && events >= 4u &&
	      v92_pcm_mapping_equal(&station.mapping, &expected_mapping),
	      "CPd acknowledgment was not retained for law %u events=%zu",
	      (unsigned int)law, events);

	for (unsigned int mutation = 0u; mutation < 5u; ++mutation) {
		struct v92_phase4_analog_station rejected = station;
		struct full_cpd_source rejected_source = source;
		struct v92_pcm_mapping retained_mapping = station.mapping;
		struct v92_phase4_analog_tx retained_transmit = station.transmit;
		struct v92_cpd_base_parameters retained_cpd = station.cpd;
		struct v92_cpd_base_parameters output;
		struct v92_cpd_base_parameters output_before;
		uint8_t changed_sequence[V92_CPD_DIRECT_MAX_FRAME_BYTES] = { 0 };
		size_t changed_bits = 0u;
		size_t rejected_events = events;
		enum v92_phase4_analog_station_result result =
			V92_PHASE4_ANALOG_STATION_OK;

		if (mutation < 4u) {
			struct v92_cpd_base_parameters changed = compact;

			if (mutation == 0u)
				changed.data_rate_number--;
			else if (mutation == 1u)
				changed.trellis = V92_CPD_TRELLIS_32_STATE;
			else if (mutation == 2u)
				changed.four_times_gain_q0_16++;
			else
				changed.extend_e2u = !compact.extend_e2u;
			CHECK(v92_cpd_encode_base(
				      &changed, training_mapping.frame_bits,
				      changed_sequence, sizeof(changed_sequence),
				      &changed_bits) == V92_CPD_OK,
			      "compact mutation %u did not encode", mutation);
		} else {
			struct v92_cpd_parameters changed = expected;
			struct v92_pcm_mapping valid_changed;

			changed.filter[V92_CPD_PREFILTER_FEED_FORWARD]
				.coefficient[0]--;
			CHECK(v92_pcm_mapping_init(
				      &valid_changed, law, &changed) ==
				      V92_PCM_MAPPING_OK &&
			      v92_cpd_encode(
				      &changed, training_mapping.frame_bits,
				      changed_sequence, sizeof(changed_sequence),
				      &changed_bits) == V92_CPD_OK,
			      "mapping mutation did not encode for law %u",
			      (unsigned int)law);
		}
		for (unsigned int repeat = 0u;
		     repeat < 3u && result == V92_PHASE4_ANALOG_STATION_OK;
		     ++repeat)
			result = push_full_cpd_sequence(
				&rejected_source, &rejected, changed_sequence,
				changed_bits, &rejected_events);
		memset(&output, 0xa5, sizeof(output));
		output_before = output;
		CHECK(result == V92_PHASE4_ANALOG_STATION_FAILED &&
		      v92_phase4_analog_station_has_failed(&rejected) &&
		      memcmp(&rejected.mapping, &retained_mapping,
			     sizeof(retained_mapping)) == 0 &&
		      memcmp(&rejected.transmit, &retained_transmit,
			     sizeof(retained_transmit)) == 0 &&
		      memcmp(&rejected.cpd, &retained_cpd,
			     sizeof(retained_cpd)) == 0 &&
		      !v92_phase4_analog_station_cpd_parameters(
			      &rejected, &output) &&
		      memcmp(&output, &output_before, sizeof(output)) == 0,
		      "CPd mutation %u was not rejected transactionally for law %u",
		      mutation, (unsigned int)law);
	}
	return true;
}

static bool test_full_cpd_ownership(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law)
		for (unsigned omitted = 0; omitted <= 8u; ++omitted)
			CHECK(run_full_cpd_ownership((enum g711_law)law, omitted),
			      "CPd ownership law %u omitted %u stopped", law, omitted);
	return true;
}

static bool check_digital_mapping_handoff(
	const struct v92_phase4_digital_station *ready)
{
	struct v92_phase4_digital_station prime = *ready;
	struct v92_cpd_parameters selection;
	struct v92_phase4_digital_handoff handoff;
	struct v92_pcm_mapping expected;
	struct v92_cpd_parameters parameters;

	struct v92_downstream_control timed = ready->control;
	timed.round_trip_delay_samples = 2130u;
	uint64_t delay = v92_downstream_control_e2u_delay(&timed);
	CHECK(delay != 0u && delay != UINT64_MAX &&
	      timed.transmit.transmitted_symbols + delay ==
		      timed.acknowledgement_end_frame * V90_CP_FRAME_INTERVALS + 2130u,
	      "E2u deadline did not include the wire tail and round trip");
	for (uint64_t n = 0u; n <= delay; ++n) {
		uint8_t octet;
		CHECK(v92_downstream_control_e2u_delay(&timed) == (n < delay ? delay - n : 0u) &&
		      v92_downstream_control_transmit(&timed, &octet), "E2u response guard lost the wire clock");
	}
	CHECK(v92_downstream_control_e2u_delay(&timed) == 0u, "response guard restarted after expiry");
	timed.acknowledgement_end_frame = UINT64_MAX;
	CHECK(v92_downstream_control_e2u_delay(&timed) == UINT64_MAX &&
	      v92_downstream_control_e2u_delay(NULL) == UINT64_MAX, "invalid response deadline accepted");

	/* A retained CPd-prime changes only the acknowledgment in the record. */
	CHECK(v92_cpd_decode(prime.control.initial_cpd.sequence,
		      sizeof(prime.control.initial_cpd.sequence), prime.control.initial_cpd.sequence_bits,
		      prime.control.initial_cpd.frame_bits, &selection) == V92_CPD_OK,
	      "retained CPd fixture did not decode");
	selection.acknowledges_cpu = true;
	CHECK(v92_phase4_cpd_tx_init(&prime.control.initial_cpd, &prime.control.transmit, &selection),
	      "CPd-prime fixture did not encode");
	prime.control.initial_cpd.bit_index = prime.control.initial_cpd.sequence_bits;
	prime.control.initial_cpd.complete = true;
	CHECK(v92_phase4_digital_station_is_valid(&prime) &&
	      v92_cpd_decode(prime.control.initial_cpd.sequence,
		      sizeof(prime.control.initial_cpd.sequence), prime.control.initial_cpd.sequence_bits,
		      prime.control.initial_cpd.frame_bits, &parameters) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&expected, prime.control.transmit.encoder.mapping.law,
		      &parameters) == V92_PCM_MAPPING_OK &&
	      v92_phase4_digital_station_handoff(&prime, &handoff) ==
		      V92_PHASE4_DIGITAL_STATION_OK &&
	      handoff.mapping.cpd.acknowledges_cpu &&
	      handoff.e2u_delay_samples == v92_downstream_control_e2u_delay(&prime.control) &&
	      v92_pcm_mapping_equal(&handoff.mapping, &expected),
	      "handoff regenerated CPd instead of retaining the transmitted record");
	for (unsigned int mutation = 0u; mutation < 4u; ++mutation) {
		struct v92_phase4_digital_handoff rejected = handoff;

		if (mutation == 0u)
			rejected.mapping.data_bits_per_frame++;
		else if (mutation == 1u)
			rejected.mapping.law ^= 1u;
		else if (mutation == 2u)
			rejected.receive.signal.line.law ^= 1u;
		else
			rejected.e2u_delay_samples = UINT64_MAX;
		CHECK(!v92_phase4_digital_handoff_is_valid(&rejected),
		      "inconsistent digital mapping handoff %u accepted", mutation);
	}
	return true;
}

static bool check_mapping_payload(
	const struct v92_pcm_mapping *transmit_mapping,
	const struct v92_pcm_mapping *receive_mapping)
{
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	unsigned int delivered = 0u;

	CHECK(v92_pcm_frame_tx_init(&transmitter, transmit_mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init(&receiver, receive_mapping, 4096) == V92_PCM_FRAME_OK,
	      "selected mapping did not initialize the data coders");
	/* Exercise coding histories after the actual control handoff. The aligned
	 * codec channel here isolates CPd ownership from line acquisition/timing. */
	for (unsigned int frame = 0u;
	     frame < V92_B1U_FRAMES + 64u + V92_PCM_RX_DELAY_FRAMES; ++frame) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame encoded;
		struct v92_pcm_frame_decision decision;
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
		uint32_t random = UINT32_C(0x193a6371) ^ frame;

		for (size_t bit = 0u; bit < transmit_mapping->data_bits_per_frame; ++bit) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			bits[bit] = frame < V92_B1U_FRAMES ? 1u : random >> 31u;
		}
		CHECK(v92_pcm_frame_encode(&transmitter, bits,
			      transmit_mapping->data_bits_per_frame, &encoded) == V92_PCM_FRAME_OK,
		      "selected mapping stopped encoding frame %u", frame);
		for (size_t symbol = 0u; symbol < ARRAY_LENGTH(samples); ++symbol) {
			CHECK(v92_pcm_frame_scale(transmit_mapping, 4096, encoded.v[symbol],
				      &samples[symbol]) == V92_PCM_FRAME_OK,
			      "selected mapping did not scale symbol %zu", symbol);
			samples[symbol] = line_sample(transmit_mapping->law, 1, samples[symbol]);
		}
		CHECK(v92_pcm_frame_decode(&receiver, samples, frame < V92_B1U_FRAMES,
			      decoded, receive_mapping->data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
		      "selected mapping stopped decoding frame %u", frame);
		if (!decision.bits_ready)
			continue;
		CHECK(!decision.ambiguous, "selected mapping is ambiguous at frame %u", delivered);
		random = UINT32_C(0x193a6371) ^ delivered;
		for (size_t bit = 0u; bit < receive_mapping->data_bits_per_frame; ++bit) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			CHECK(decoded[bit] == (delivered < V92_B1U_FRAMES ? 1u : random >> 31u),
			      "selected mapping changed frame %u bit %zu", delivered, bit);
		}
		delivered++;
	}
	CHECK(delivered == V92_B1U_FRAMES + 64u, "selected mapping did not drain payload");
	return true;
}

static bool check_cleardown_pair(
	const struct v92_phase4_analog_station *analog_start,
	const struct v92_phase4_digital_station *digital_start, int8_t polarity,
	unsigned int initiators, bool drop_peer)
{
	struct v92_phase4_analog_station analog = *analog_start;
	struct v92_phase4_digital_station digital = *digital_start;
	bool done[2] = { false, false };
	uint64_t ended[2] = { 0, 0 };
	enum g711_law law = analog.transmit.control.cpu.law;
	uint16_t rtd = 161u;

	analog.round_trip_delay_samples = rtd;
	digital.control.round_trip_delay_samples = rtd;
	if (initiators & 1u)
		CHECK(v92_phase4_analog_station_start_cleardown(&analog) &&
		      v92_phase4_analog_station_start_cleardown(&analog), "analogue cancellation failed");
	if (initiators & 2u)
		CHECK(v92_phase4_digital_station_start_cleardown(&digital) &&
		      v92_phase4_digital_station_start_cleardown(&digital), "digital cancellation failed");
	for (unsigned int clock = 0u; clock < 16000u; ++clock) {
		int16_t up = 0;
		uint8_t down = law == G711_LAW_PCMU ? g711_encode_pcmu(0) : g711_encode_pcma(0);

		if (!done[0] && (!drop_peer || (initiators & 1u))) {
			enum v92_phase4_analog_station_result result = v92_phase4_analog_station_transmit(&analog, &up);
			CHECK(result == V92_PHASE4_ANALOG_STATION_OK ||
			      result == V92_PHASE4_ANALOG_STATION_CLEARDOWN_COMPLETE,
			      "analogue close stopped at %u: %s", clock, v92_phase4_analog_station_result_name(result));
			done[0] = result == V92_PHASE4_ANALOG_STATION_CLEARDOWN_COMPLETE;
			if (done[0]) ended[0] = analog.transmitted_symbols;
		}
		if (!done[1] && (!drop_peer || (initiators & 2u))) {
			enum v92_phase4_digital_station_result result = v92_phase4_digital_station_transmit(&digital, &down);
			CHECK(result == V92_PHASE4_DIGITAL_STATION_OK ||
			      result == V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE,
			      "digital close stopped at %u: %s", clock, v92_phase4_digital_station_result_name(result));
			done[1] = result == V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE;
			if (done[1]) ended[1] = digital.control.transmit.transmitted_symbols;
		}
		if (!drop_peer) {
			if (!done[0]) {
				enum v92_phase4_analog_station_result result = v92_phase4_analog_station_receive(&analog, down);
				CHECK(result == V92_PHASE4_ANALOG_STATION_OK || result == V92_PHASE4_ANALOG_STATION_EVENT,
				      "analogue closing receive failed");
			}
			if (!done[1]) {
				enum v92_phase4_digital_station_result result = v92_phase4_digital_station_receive(&digital,
					line_sample(law, polarity, up));
				CHECK(result == V92_PHASE4_DIGITAL_STATION_OK || result == V92_PHASE4_DIGITAL_STATION_EVENT,
				      "digital closing receive failed");
			}
		}
		CHECK(v92_phase4_analog_station_is_valid(&analog) &&
		      v92_phase4_digital_station_is_valid(&digital), "cleardown state invalid");
		if ((drop_peer ? ((!(initiators & 1u) || done[0]) && (!(initiators & 2u) || done[1])) :
		     (done[0] && done[1]))) break;
	}
	for (unsigned int side = 0; side < 2u; ++side) {
		const struct v92_cleardown *state = side == 0u ? &analog.cleardown : &digital.control.cleardown;
		if (drop_peer && !(initiators & (1u << side))) continue;
		CHECK(done[side] && state->cp_end && v92_cleardown_is_ready(state, ended[side]),
		      "side %u never completed bounded cleardown (%u/%u)", side, done[0], done[1]);
		if (!state->initiator)
			CHECK(state->acknowledgement_end && ended[side] - state->acknowledgement_end >= 881u,
			      "responder omitted acknowledgement or disconnected early");
		if (drop_peer)
			CHECK(!state->acknowledged && ended[side] - state->cp_end >= 961u,
			      "lost-peer timeout started before the physical CP boundary");
	}
	for (unsigned int repeat = 0; repeat < 3u; ++repeat) {
		int16_t up = 123;
		uint8_t down = 123u;
		CHECK(!done[0] || (v92_phase4_analog_station_transmit(&analog, &up) ==
		      V92_PHASE4_ANALOG_STATION_CLEARDOWN_COMPLETE && up == 123 &&
		      analog.transmitted_symbols == ended[0]), "analogue close emitted another symbol");
		CHECK(!done[1] || (v92_phase4_digital_station_transmit(&digital, &down) ==
		      V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE && down == 123u &&
		      digital.control.transmit.transmitted_symbols == ended[1]), "digital close emitted another symbol");
	}
	CHECK(!v92_phase4_analog_station_can_handoff(&analog) &&
	      !v92_phase4_digital_station_is_ed_ready(&digital), "cleardown restarted data");
	return true;
}

static bool run_paired_station_exchange(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity,
	const struct v92_cpd_parameters *selected)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	const struct v92_suvd_parameters suvd = {
		.request_silence = law == G711_LAW_PCMA,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_parameters cpd = selected != NULL ? *selected : cpd_parameters(law);
	struct v92_cpd_parameters full_cpd = cpd;
	struct v92_pcm_phase3_cpt_line_handoff digital_handoff;
	struct v92_phase3_analog_transmit_handoff analog_handoff = {
		.lu = TEST_LU,
		.training_constellation = constellation,
		.previous_encoded_sign = previous_sign,
	};
	struct v92_pcm_phase4_trn2u upstream;
	struct v90_pcm_mapping mapping;
	struct v92_pcm_mapping expected_upstream_mapping;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct v92_phase4_analog_station analog;
	struct v92_phase4_digital_station digital;
	struct v92_phase4_analog_station terminal_analog;
	struct v92_phase4_digital_station terminal_digital;
	struct v92_cpu_parameters retained_cpu;
	struct v92_cpu_parameters expected_cpu = cpu;
	struct v92_cpd_base_parameters retained_cpd;
	struct v92_phase4_analog_handoff analog_handoff_out;
	struct v92_phase4_analog_handoff ed_handoff;
	struct v92_phase4_digital_handoff digital_handoff_out;
	struct v92_phase4_record_tx e2u;
	enum pcm_downstream_tx_result request_ed_result;
	size_t cpu_repeat_wait = 0u;
	bool ready = false;
	bool terminal_status_seen = false;
	bool terminal_probe_initialized = false;
	bool cleardown_checked = false;

	CHECK(build_predecessor(
		      law, constellation, previous_sign, polarity,
		      &upstream, &digital_handoff), "paired training predecessor failed");
	/* Cover physical source-to-wire offsets at every supported lookahead. */
	digital_handoff.parameters.spectral_lookahead = (uint8_t)(2u * law +
		(constellation == V92_CONSTELLATION_8_POINT));
	CHECK(v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &digital_handoff.parameters, digital_handoff.parameters.law) ==
		      V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream_tx, PCM_PROTOCOL_V92, &mapping) ==
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
		      &downstream_tx, &suvd, &cpd, 0u) ==
		      V92_PHASE4_DIGITAL_STATION_OK,
	      "paired station initialization failed law=%u constellation=%u",
	      (unsigned int)law, (unsigned int)constellation);

	/* Leave SUVd readiness to the station's channel measurement. Sending it
	 * before the receiver listens for control can lose all initial SUVu;
	 * a conforming peer then repeats only CPu after its first wait. */
	CHECK(!digital.control.transmit.control_configured, "fixture announced readiness early");
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

		if (!cleardown_checked && digital.control.transmit_stage == V92_DOWNSTREAM_CONTROL_CPD &&
		    digital.control.initial_cpd.bit_index > digital.control.initial_cpd.frame_bits &&
		    analog.transmit.stage != V92_PHASE4_ANALOG_TX_TRN2U) {
			for (unsigned int initiators = 1u; initiators <= 3u; ++initiators)
				CHECK(check_cleardown_pair(&analog, &digital, polarity, initiators, false),
				      "paired cleardown failed law=%u initiators=%u", (unsigned int)law, initiators);
			for (unsigned int initiators = 1u; initiators <= 2u; ++initiators)
				CHECK(check_cleardown_pair(&analog, &digital, polarity, initiators, true),
				      "unacknowledged cleardown failed");
			cleardown_checked = true;
		}
		analog_result = v92_phase4_analog_station_transmit(
			&analog, &upstream_sample);
		if (analog_result == V92_PHASE4_ANALOG_STATION_E2U_READY) {
			CHECK(v92_phase4_analog_station_handoff(&analog, &analog_handoff_out) ==
				      V92_PHASE4_ANALOG_STATION_OK &&
			      v92_phase4_record_tx_init_e2u(&e2u, &analog_handoff_out.transmit.modulator,
				      cpd.extend_e2u) == V92_PHASE4_RECORD_TX_OK,
			      "paired station did not begin E2u");
			ready = true;
			break;
		}
		CHECK(analog_result == V92_PHASE4_ANALOG_STATION_OK,
		      "paired analogue transmit stopped at %zu: %s", symbol,
		      v92_phase4_analog_station_result_name(analog_result));
		if (analog.transmit.stage == V92_PHASE4_ANALOG_TX_CPU)
			expected_cpu.acknowledges_cpd =
				analog.transmit.control.current_record_acknowledges;
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
		if (analog.transmit.control.cpu_sent && !digital.control.cpu_received &&
		    !analog.transmit.control.cpu_pending &&
		    analog.transmit.control.retry == V92_UPSTREAM_RETRY_IDLE) {
			if (cpu_repeat_wait == 0u)
				cpu_repeat_wait = 800u;
			else if (--cpu_repeat_wait == 0u)
				CHECK(v92_phase4_analog_station_expire_cpu_wait(&analog) ==
					      V92_PHASE4_ANALOG_STATION_OK,
				      "paired CPu repeat was rejected at %zu",
				      symbol);
		}
	}
	CHECK(cleardown_checked, "paired control did not reach cancellation boundary");
	for (size_t symbol = 0u;
	     ready && (!v92_phase4_digital_station_is_ed_ready(&digital) ||
		       !analog.receive.ed_requested) &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol) {
		uint8_t octet;
		enum v92_phase4_analog_station_result result;

		/* The final upstream acknowledgment is still in the causal receive
		 * delay. Real E2u samples release it after the sender leaves control. */
		if (!v92_phase4_digital_station_peer_acknowledged_cpd(&digital)) {
			int16_t sample;
			enum v92_phase4_digital_station_result received;

			CHECK(v92_phase4_record_tx_next(&e2u, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "paired station exhausted E2u before the final acknowledgment");
			received = v92_phase4_digital_station_receive(
				&digital, line_sample(law, polarity, sample));
			CHECK(received == V92_PHASE4_DIGITAL_STATION_OK ||
			      received == V92_PHASE4_DIGITAL_STATION_EVENT,
			      "paired station lost the final delayed acknowledgment");
		}
		CHECK(v92_phase4_digital_station_transmit(&digital, &octet) ==
			      V92_PHASE4_DIGITAL_STATION_OK,
		      "paired digital handoff stopped at %zu", symbol);
		result = v92_phase4_analog_station_receive(&analog, octet);
		CHECK(result == V92_PHASE4_ANALOG_STATION_OK ||
		      result == V92_PHASE4_ANALOG_STATION_EVENT,
		      "terminal status advance was rejected at %zu: %s", symbol,
		      v92_phase4_analog_station_result_name(result));
		terminal_status_seen |=
			result == V92_PHASE4_ANALOG_STATION_EVENT;
	}
	if (ready && v92_phase4_digital_station_is_ed_ready(&digital)) {
		terminal_analog = analog;
		terminal_digital = digital;
		terminal_status_seen = false;
		terminal_probe_initialized = true;
	}
	for (size_t symbol = 0u;
	     terminal_probe_initialized && !terminal_status_seen &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol) {
		uint8_t octet;
		enum v92_phase4_analog_station_result result;

		CHECK(v92_phase4_digital_station_transmit(
			      &terminal_digital, &octet) ==
		      V92_PHASE4_DIGITAL_STATION_OK,
		      "terminal status repeat stopped at %zu", symbol);
		result = v92_phase4_analog_station_receive(
			&terminal_analog, octet);
		CHECK(result == V92_PHASE4_ANALOG_STATION_OK ||
		      result == V92_PHASE4_ANALOG_STATION_EVENT,
		      "terminal status repeat was rejected at %zu: %s", symbol,
		      v92_phase4_analog_station_result_name(result));
		terminal_status_seen =
			result == V92_PHASE4_ANALOG_STATION_EVENT;
	}
	full_cpd.acknowledges_cpu = analog.cpd.acknowledges_cpu;
	CHECK(v92_pcm_mapping_init(
		      &expected_upstream_mapping, law, &full_cpd) ==
		      V92_PCM_MAPPING_OK,
	      "paired upstream mapping did not initialize law=%u",
	      (unsigned int)law);
	CHECK(ready && terminal_probe_initialized && terminal_status_seen &&
	      v92_phase4_analog_station_can_handoff(&terminal_analog) &&
	      v92_phase4_digital_station_is_valid(&terminal_digital) &&
	      !v92_phase4_digital_station_has_failed(&terminal_digital) &&
	      v92_phase4_analog_station_can_handoff(&analog) &&
	      v92_phase4_analog_station_cpd_parameters(
		      &analog, &retained_cpd) &&
	      base_cpd_equal(&retained_cpd, &cpd) &&
	      v92_phase4_digital_station_cpu_parameters(
		      &digital, &retained_cpu) &&
	      cpu_equal(&retained_cpu, &expected_cpu) &&
	      v92_phase4_digital_station_peer_acknowledged_cpd(&digital) &&
	      v92_phase4_digital_station_is_ed_ready(&digital) &&
	      analog.receive.ed_requested &&
	      analog.suvd.request_silence == suvd.request_silence,
	      "paired station exchange did not reach E2u law=%u constellation=%u "
	      "ready=%u terminal=%u a=%u/%u/%u/%u/%u tx=%u/%u/%u/%u "
	      "d=%u/%u/%u/%u",
	      (unsigned int)law, (unsigned int)constellation, ready,
	      terminal_status_seen,
	      analog.suvd_received, analog.cpd_received,
	      analog.transmit.control.cpu_sent, analog.transmit.control.acknowledgment_sent,
	      analog.transmit.control.peer_confirmation_received,
	      analog.transmit.stage, analog.receive.stage,
	      analog.transmit.control.received_suvd, analog.transmit.control.received_cpd,
	      digital.control.suvu_received, digital.control.cpu_received,
	      digital.control.peer_confirmation_received,
	      digital.control.transmit_acknowledges_cpu);
	CHECK(v92_phase4_analog_station_handoff(
		      &analog, &analog_handoff_out) ==
		      V92_PHASE4_ANALOG_STATION_OK &&
	      v92_phase4_analog_handoff_is_valid(&analog_handoff_out) &&
	      memcmp(&analog_handoff_out.receive, &analog.receive,
		     sizeof(analog.receive)) == 0 &&
	      cpu_equal(&analog_handoff_out.cpu, &cpu) &&
	      v92_pcm_mapping_equal(
		      &analog_handoff_out.mapping,
		      &expected_upstream_mapping) &&
	      v92_phase4_digital_station_handoff(
		      &digital, &digital_handoff_out) ==
		      V92_PHASE4_DIGITAL_STATION_OK &&
	      v92_phase4_digital_handoff_is_valid(&digital_handoff_out) &&
	      memcmp(&digital_handoff_out.receive,
		     &digital.receive.signal.control,
		     sizeof(digital_handoff_out.receive)) == 0 &&
	      digital_handoff_out.transmit.transmit.transmitted_symbols ==
		      digital.control.transmit.transmitted_symbols &&
	      memcmp(&digital_handoff_out.transmit.transmit.encoder,
		     &digital.control.transmit.encoder, sizeof(digital.control.transmit.encoder)) == 0 &&
	      cpu_equal(&digital_handoff_out.cpu, &expected_cpu) &&
	      v92_pcm_mapping_equal(&digital_handoff_out.mapping,
				    &expected_upstream_mapping),
	      "paired station handoff lost duplex state law=%u constellation=%u",
	      (unsigned int)law, (unsigned int)constellation);
	CHECK(check_digital_mapping_handoff(&digital),
	      "digital mapping handoff contract changed");
	if (selected != NULL)
		CHECK(check_mapping_payload(&analog_handoff_out.mapping, &digital_handoff_out.mapping),
		      "selected filters were not usable after the control handoff");
	request_ed_result = pcm_downstream_tx_request_ed(
		&digital_handoff_out.transmit.transmit);
	CHECK(request_ed_result == PCM_DOWNSTREAM_TX_OK &&
	      digital_handoff_out.transmit.transmit.ed_requested,
	      "digital handoff could not enter Ed law=%u constellation=%u "
	      "result=%s stage=%u",
	      (unsigned int)law, (unsigned int)constellation,
	      pcm_downstream_tx_result_name(request_ed_result),
	      digital_handoff_out.transmit.transmit.stage);
	for (size_t symbol = 0u;
	     !pcm_downstream_tx_is_complete(&digital_handoff_out.transmit.transmit) &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol) {
		struct pcm_downstream_symbol output;
		enum pcm_downstream_tx_result transmit_result =
			v92_phase4_ed_tx_next(
				&digital_handoff_out.transmit, &output);
		enum v92_phase4_analog_station_result receive_result;

		if (transmit_result == PCM_DOWNSTREAM_TX_COMPLETE)
			break;
		CHECK(transmit_result == PCM_DOWNSTREAM_TX_OK,
		      "paired Ed source stopped at %zu: %s", symbol,
		      pcm_downstream_tx_result_name(transmit_result));
		receive_result = v92_phase4_analog_station_receive(
			&analog, output.pcm.octet);
		CHECK(receive_result == V92_PHASE4_ANALOG_STATION_OK ||
		      receive_result == V92_PHASE4_ANALOG_STATION_EVENT,
		      "paired Ed receive stopped at %zu: %s", symbol,
		      v92_phase4_analog_station_result_name(receive_result));
	}
	CHECK(pcm_downstream_tx_is_complete(&digital_handoff_out.transmit.transmit) &&
	      pcm_downstream_rx_is_complete(&analog.receive) &&
	      v92_phase4_analog_station_can_handoff(&analog) &&
	      v92_phase4_analog_station_handoff(&analog, &ed_handoff) ==
		      V92_PHASE4_ANALOG_STATION_OK &&
	      pcm_downstream_rx_is_complete(&ed_handoff.receive),
	      "paired acknowledged control did not retain complete Ed law=%u constellation=%u",
	      (unsigned int)law, (unsigned int)constellation);
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
				      index == 0u ? 1 : -1, NULL),
			      "paired station matrix stopped");
		}
	}
	return true;
}

static bool test_selected_filters(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v92_cpd_parameters cpd;

		CHECK(v92_cpd_direct_init(&cpd, law, 13u, false) == V92_CPD_OK,
		      "filtered CPd did not initialize");
		cpd.trellis = V92_CPD_TRELLIS_64_STATE;
		memset(cpd.filter, 0, sizeof(cpd.filter));
		cpd.filter[0] = (struct v92_cpd_filter){ .count = 1u, .coefficient = { 16384 } };
		cpd.filter[1] = (struct v92_cpd_filter){ .count = 1u, .coefficient = { -4096 } };
		cpd.filter[2] = (struct v92_cpd_filter){ .count = 2u, .coefficient = { 24576, 8192 } };
		cpd.filter[3] = (struct v92_cpd_filter){ .count = 1u, .coefficient = { 4096 } };
		CHECK(run_paired_station_exchange(law, V92_CONSTELLATION_4_POINT, 0u, 1, &cpd),
		      "four-section selected filter exchange failed law=%u", (unsigned int)law);
		CHECK(v92_cpd_direct_init(&cpd, law, 4u, true) == V92_CPD_OK,
		      "long-filter CPd did not initialize");
		memset(cpd.filter, 0, sizeof(cpd.filter));
		cpd.filter[1].count = 90u;
		cpd.filter[1].coefficient[89] = 128;
		cpd.filter[2].count = 85u;
		cpd.filter[2].coefficient[0] = INT16_MAX;
		cpd.filter[2].coefficient[84] = -64;
		CHECK(v92_cpd_frame_bits(&cpd, 17u) > V92_CPD_DIRECT_MAX_FRAME_BITS &&
		      run_paired_station_exchange(law, V92_CONSTELLATION_8_POINT, 1u, -1, &cpd),
		      "175-coefficient selected filter exchange failed law=%u", (unsigned int)law);
	}
	return true;
}

static bool run_analog_retry_boundary(enum g711_law law, bool parameters, bool ack, bool corrupt)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = V92_SUVU_NO_MEASUREMENT_Q2_2,
	};
	struct v92_suvd_parameters suvd = { 0 };
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_parameters cpd = cpd_parameters(law);
	struct v92_cpt_parameters cpt = cpt_parameters(law);
	const struct v92_phase3_analog_transmit_handoff handoff = {
		.lu = TEST_LU, .training_constellation = V92_CONSTELLATION_4_POINT,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_rx downstream;
	struct v92_phase4_analog_station station;
	struct full_cpd_source source;
	uint8_t sequence[V92_CPD_DIRECT_MAX_FRAME_BYTES];
	size_t bits, events = 0;
	bool expired = false;
	CHECK(v90_pcm_mapping_init_from_v92_cpt(&mapping, &cpt, cpt.law) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_rx_init(&downstream, PCM_PROTOCOL_V92, &mapping,
		V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_analog_station_init(&station, &handoff, &downstream, &suvu, &cpu,
		&suvd, &cpd) == V92_PHASE4_ANALOG_STATION_OK &&
	      full_cpd_source_init(&source, &mapping) &&
	      push_full_cpd_training(&source, &station, &events), "retry fixture setup failed");
	for (unsigned symbol = 0; symbol < 4000u &&
	     (!station.transmit.control.cpu_sent || station.transmit.stage != V92_PHASE4_ANALOG_TX_SUVU); ++symbol) {
		int16_t sample;
		CHECK(v92_phase4_analog_station_transmit(&station, &sample) == V92_PHASE4_ANALOG_STATION_OK,
		      "retry fixture did not send the first CPu");
	}
	CHECK(station.transmit.control.cpu_sent && station.transmit.stage == V92_PHASE4_ANALOG_TX_SUVU &&
	      !events, "retry fixture did not reach the first CPu wait");
	cpd.acknowledges_cpu = suvd.acknowledges_cpu = ack;
	CHECK(parameters ?
	      v92_cpd_encode(&cpd, mapping.frame_bits, sequence, sizeof(sequence), &bits) == V92_CPD_OK :
	      v92_suvd_encode(&suvd, mapping.frame_bits, sequence, sizeof(sequence), &bits) == V92_SUVD_OK,
	      "retry boundary record did not encode");
	/* Damage a payload bit, retaining sync and record length but invalidating CRC. */
	if (corrupt)
		sequence[5] ^= 0x80u;
	for (unsigned repeat = 0; repeat < 4u && events < (corrupt ? 2u : 1u); ++repeat) {
		for (size_t first = 0; first < bits && events < (corrupt ? 2u : 1u); first += mapping.frame_bits) {
			const struct v92_control_rx *receive = &station.receive.control.v92;
			if (!expired && receive->bit_index >= 32u && receive->bit_index < receive->frame_bit_count) {
				CHECK(v92_phase4_analog_station_expire_cpu_wait(&station) == V92_PHASE4_ANALOG_STATION_OK,
				      "expiry during a partial peer record was rejected");
				expired = true;
			}
			uint8_t frame[V90_PCM_FRAME_MAX_BITS];
			for (size_t bit = 0; bit < mapping.frame_bits; ++bit)
				frame[bit] = sequence_bit(sequence, first + bit);
			CHECK(push_full_cpd_frame(&source, &station, frame, &events) == V92_PHASE4_ANALOG_STATION_OK,
			      "retry boundary wire receive failed");
			if (expired && (corrupt || !events))
				CHECK(station.transmit.control.retry == V92_UPSTREAM_RETRY_ELIGIBLE &&
				      !station.transmit.control.peer_confirmation_received,
				      "partial or CRC-rejected peer record changed retry state");
		}
	}
	CHECK(expired && events == (corrupt ? 2u : 1u), "retry test missed the peer record boundary");
	CHECK(station.transmit.control.retry == (corrupt ? V92_UPSTREAM_RETRY_ELIGIBLE :
		ack ? V92_UPSTREAM_RETRY_IDLE : V92_UPSTREAM_RETRY_ACTIVE) &&
	      station.transmit.control.peer_confirmation_received == (ack && !corrupt),
	      "complete peer record did not resolve retry: law=%u CPd=%u ACK=%u corrupt=%u",
	      (unsigned)law, parameters, ack, corrupt);
	return true;
}

static bool test_analog_retry_boundary(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned variant = 0; variant < 8u; ++variant)
			CHECK(run_analog_retry_boundary(law, variant & 1u, variant & 2u, variant & 4u),
			      "analogue retry boundary failed law=%u variant=%u", (unsigned)law, variant);
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
	struct v92_cpd_parameters early_cpd;
	struct v92_phase3_analog_transmit_handoff handoff = {
		.lu = TEST_LU,
		.training_constellation = V92_CONSTELLATION_4_POINT,
		.previous_encoded_sign = 1u,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_rx downstream;
	struct v92_phase4_analog_station station;
	struct v92_phase4_analog_handoff exported;
	struct v92_phase4_analog_handoff exported_before;

	{
		struct v92_cpt_parameters cpt = cpt_parameters(G711_LAW_PCMU);

		CHECK(v90_pcm_mapping_init_from_v92_cpt(&mapping, &cpt, cpt.law) ==
			      V90_PCM_MAPPING_OK,
		      "early-control mapping differed");
	}
	CHECK(full_cpd_parameters(G711_LAW_PCMU, &early_cpd) &&
	      pcm_downstream_rx_init(
		      &downstream, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_phase4_analog_station_init(
		      &station, &handoff, &downstream, &suvu, &cpu,
		      &early_suvd, &early_cpd) ==
		      V92_PHASE4_ANALOG_STATION_OK &&
	      station.suvd_received && station.cpd_received &&
	      v92_pcm_mapping_is_valid(&station.mapping) &&
	      station.transmit.control.received_suvd &&
	      station.transmit.control.received_cpd &&
	      station.transmit.control.peer_requests_silence &&
	      !v92_phase4_analog_tx_silence_requested(&station.transmit),
	      "early downstream controls were not retained");
	memset(&exported, 0xa5, sizeof(exported));
	exported_before = exported;
	CHECK(!v92_phase4_analog_handoff_is_valid(NULL) &&
	      !v92_phase4_analog_handoff_is_valid(&exported) &&
	      v92_phase4_analog_station_handoff(NULL, &exported) ==
		      V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase4_analog_station_handoff(&station, NULL) ==
		      V92_PHASE4_ANALOG_STATION_INVALID_ARGUMENT &&
	      v92_phase4_analog_station_handoff(&station, &exported) ==
		      V92_PHASE4_ANALOG_STATION_INVALID_STATE &&
	      memcmp(&exported, &exported_before, sizeof(exported)) == 0,
	      "analogue handoff rejection changed output");
	return true;
}

static bool test_compact_boundary(void)
{
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v92_cpd_parameters cpd = cpd_parameters(G711_LAW_PCMU);
	struct v92_suvd_parameters suvd = { 0 };
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream;
	struct v92_phase4_digital_station station;
	struct v92_phase4_digital_station saved;
	struct v92_phase4_digital_handoff exported;
	struct v92_phase4_digital_handoff exported_before;
	uint8_t octet = UINT8_C(0xa5);

	CHECK(build_predecessor(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1,
		      &upstream, &handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &handoff.parameters, handoff.parameters.law) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(
		      &downstream, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&downstream, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "compact-boundary station predecessor did not initialize");
	for (unsigned int mutation = 0u; mutation < 2u; ++mutation) {
		struct v92_cpd_parameters rejected = cpd;

		if (mutation == 0u)
			rejected.four_times_gain_q0_16 = 0u;
		else
			rejected.acknowledges_cpu = true;
		memset(&station, 0xa5, sizeof(station));
		saved = station;
		CHECK(v92_phase4_digital_station_init(
			      &station, &handoff,
			      V92_CONSTELLATION_4_POINT,
			      &downstream, &downstream.control.suvd, &rejected, 0u) ==
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
	memset(&exported, 0xa5, sizeof(exported));
	exported_before = exported;
	CHECK(!v92_phase4_digital_handoff_is_valid(NULL) &&
	      !v92_phase4_digital_handoff_is_valid(&exported) &&
	      v92_phase4_digital_station_handoff(NULL, &exported) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase4_digital_station_handoff(&station, NULL) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_ARGUMENT &&
	      v92_phase4_digital_station_handoff(&station, &exported) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_STATE &&
	      memcmp(&exported, &exported_before, sizeof(exported)) == 0,
	      "digital handoff rejection changed output");
	return true;
}

static bool cpd_tx_contracts_are_transactional(
	const struct v92_phase4_cpd_tx *valid,
	const struct pcm_downstream_tx *valid_downstream)
{
	struct v92_phase4_cpd_tx transmitter = *valid;
	struct pcm_downstream_tx downstream = *valid_downstream;
	struct v92_phase4_cpd_tx saved_transmitter = transmitter;
	struct pcm_downstream_tx saved_downstream = downstream;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol saved_symbol;

	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	CHECK(!v92_phase4_cpd_tx_next(
		      &transmitter, &downstream,
		      (struct pcm_downstream_symbol *)(void *)&transmitter) &&
	      !v92_phase4_cpd_tx_next(
		      &transmitter, &downstream,
		      (struct pcm_downstream_symbol *)(void *)&downstream) &&
	      !v92_phase4_cpd_tx_next(
		      &transmitter,
		      (struct pcm_downstream_tx *)(void *)&transmitter,
		      &symbol) &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&downstream, &saved_downstream,
		     sizeof(downstream)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "overlapping full CPd owners changed state or output");

	transmitter.sequence[0] ^= 1u;
	saved_transmitter = transmitter;
	CHECK(!v92_phase4_cpd_tx_next(
		      &transmitter, &downstream, &symbol) &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&downstream, &saved_downstream,
		     sizeof(downstream)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "corrupt full CPd source changed state or output");
	return true;
}

static bool test_early_cpu_acknowledgement(void)
{
	const struct v92_suvd_parameters suvd = {
		.request_silence = true,
	};
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v92_cpd_parameters cpd = cpd_parameters(G711_LAW_PCMU);
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream;
	struct v92_phase4_digital_station station;
	struct v92_cpd_parameters selected_cpd;
	struct pcm_downstream_symbol ignored;
	uint8_t octet;

	cpd.trellis = V92_CPD_TRELLIS_64_STATE;
	cpd.filter[V92_CPD_PRECODER_FEEDBACK].count = 1u;
	cpd.filter[V92_CPD_PRECODER_FEEDBACK].coefficient[0] = -4096;
	CHECK(build_predecessor(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1,
		      &upstream, &handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(
		      &mapping, &handoff.parameters, handoff.parameters.law) == V90_PCM_MAPPING_OK &&
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
		      &downstream, &downstream.control.suvd, &cpd, 0u) == V92_PHASE4_DIGITAL_STATION_OK,
	      "early-CPu station did not inherit completed SUVd");
	CHECK(cpd_tx_contracts_are_transactional(
		      &station.control.initial_cpd, &station.control.transmit),
	      "full CPd owner contract changed");
	{
		struct pcm_downstream_tx overlapping = station.control.transmit;
		struct pcm_downstream_tx before = overlapping;

		CHECK(!v92_phase4_cpd_tx_init(
			      (struct v92_phase4_cpd_tx *)(void *)&overlapping,
			      &overlapping, &cpd) &&
		      memcmp(&overlapping, &before, sizeof(overlapping)) == 0,
		      "overlapping full CPd initialization changed the line");
	}
	station.control.suvu_received = true;
	station.control.suvu = (struct v92_suvu_parameters){
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
	};
	station.control.cpu = cpu_parameters(G711_LAW_PCMU);
	station.control.cpu_received = true;
	CHECK(v92_phase4_digital_station_is_valid(&station),
	      "early-CPu fixture is invalid");
	for (size_t symbol = 0u;
	     station.control.transmit_stage != V92_DOWNSTREAM_CONTROL_CPD &&
		     symbol < DOWNSTREAM_LIMIT;
	     ++symbol)
		CHECK(v92_phase4_digital_station_transmit(&station, &octet) ==
			      V92_PHASE4_DIGITAL_STATION_OK,
		      "early-CPu downstream stopped at %zu", symbol);
	cpd.acknowledges_cpu = true;
	CHECK(station.control.transmit_stage == V92_DOWNSTREAM_CONTROL_CPD &&
	      station.control.transmit_acknowledges_cpu &&
	      station.control.transmit.v92_control_kind ==
		      PCM_DOWNSTREAM_V92_CONTROL_SUVD &&
	      station.control.transmit.control.suvd.acknowledges_cpu &&
	      v92_cpd_decode(
		      station.control.initial_cpd.sequence,
		      sizeof(station.control.initial_cpd.sequence),
		      station.control.initial_cpd.sequence_bits,
		      station.control.initial_cpd.frame_bits, &selected_cpd) ==
		      V92_CPD_OK &&
	      selected_cpd.acknowledges_cpu &&
	      selected_cpd.modulus_present && selected_cpd.filters_present &&
	      selected_cpd.constellations_present &&
	      cpd_selection_equal(&selected_cpd, &cpd, G711_LAW_PCMU) &&
	      v92_phase4_digital_station_is_valid(&station),
	      "CPu received before CPd did not select CPd prime");
	return true;
}

static bool run_short_cpu(bool zero_rate)
{
	const struct v92_cpus_parameters cpus = {
		.data_rate_number = zero_rate ? 0u : 11u, .acknowledges_cpd = true,
	};
	const struct v92_suvd_parameters suvd = { 0 };
	struct v92_cpd_parameters cpd = cpd_parameters(G711_LAW_PCMU);
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase4_trn2u upstream;
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx downstream;
	struct v92_phase4_digital_station station;
	struct v92_phase4_record_tx record;
	uint8_t frame[V92_CPUS_MAX_FRAME_BYTES];
	size_t bits = 0u;

	CHECK(build_predecessor(G711_LAW_PCMU, V92_CONSTELLATION_4_POINT,
		      0u, 1, &upstream, &handoff) &&
	      v90_pcm_mapping_init_from_v92_cpt(&mapping, &handoff.parameters, handoff.parameters.law) ==
		      V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(&downstream, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      (zero_rate || pcm_downstream_tx_start_suvd(&downstream, &suvd) == PCM_DOWNSTREAM_TX_OK) &&
	      v92_phase4_digital_station_init(&station, &handoff,
		      V92_CONSTELLATION_4_POINT, &downstream, &suvd, &cpd, 0u) ==
		      V92_PHASE4_DIGITAL_STATION_OK,
	      "CPus phase guard fixture did not initialize");
	for (size_t symbol = 0u;
	     symbol < V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
			      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++symbol) {
		int16_t raw;
		CHECK(v92_pcm_phase4_trn2u_next(&upstream, &raw) == V92_PCM_PHASE4_TRN2U_OK &&
		      v92_phase4_digital_station_receive(&station,
			      line_sample(G711_LAW_PCMU, 1, raw)) == V92_PHASE4_DIGITAL_STATION_OK,
		      "CPus phase guard lost training");
	}
	CHECK(v92_cpus_encode(&cpus, 24u, frame, sizeof(frame), &bits) == V92_CPU_OK,
	      "CPus encoding failed");
	frame[5] ^= 1u; /* CRC corruption must not request cleardown. */
	CHECK(v92_phase4_record_tx_init(&record, &upstream.modulator,
		      frame, sizeof(frame), bits) == V92_PHASE4_RECORD_TX_OK,
	      "CPus phase guard record did not initialize");
	bool rejected = false;
	for (unsigned int repeat = 0u; repeat < 6u; ++repeat) {
		if (repeat == 2u) frame[5] ^= 1u;
		if (repeat != 0u)
			CHECK(v92_phase4_record_tx_start_next(&record, frame, sizeof(frame), bits) ==
				      V92_PHASE4_RECORD_TX_OK, "CPus repeat did not start");
		while (!v92_phase4_record_tx_is_complete(&record)) {
			struct v92_phase4_digital_rx observer = station.receive;
			struct v92_control_event event = { 0 };
			enum v92_phase4_digital_station_result result;
			int16_t raw;
			CHECK(v92_phase4_record_tx_next(&record, &raw) == V92_PHASE4_RECORD_TX_OK,
			      "CPus source stopped");
			raw = line_sample(G711_LAW_PCMU, 1, raw);
			(void)v92_phase4_digital_rx_sample(&observer, raw, &event);
			result = v92_phase4_digital_station_receive(&station, raw);
			if (event.type == V92_CONTROL_EVENT_CPUS_REJECTED) {
				CHECK(!station.control.failed && !station.control.cleardown.active,
				      "corrupt zero-rate CPus started cleardown");
				rejected = true;
			}
			if (event.type == V92_CONTROL_EVENT_CPUS) {
				CHECK(rejected, "CPus corruption case was not observed");
				if (zero_rate) {
					CHECK(result == V92_PHASE4_DIGITAL_STATION_OK &&
					      !station.control.failed && station.control.cleardown.active &&
					      station.control.cleardown.peer_requested && !station.control.cpu_received &&
					      !v92_phase4_digital_station_is_ed_ready(&station),
					      "zero-rate CPus was treated as a data selection");
					for (unsigned int clock = 0u; clock < 16000u; ++clock) {
						uint8_t down;
						result = v92_phase4_digital_station_transmit(&station, &down);
						if (result == V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE) {
							CHECK(station.control.cleardown.acknowledgement_end &&
							      !station.control.transmit.ed_requested &&
							      v92_phase4_digital_station_is_valid(&station),
							      "early zero-rate CPus did not drain its acknowledgement");
							return true;
						}
						CHECK(result == V92_PHASE4_DIGITAL_STATION_OK,
						      "early zero-rate CPus response failed");
					}
					CHECK(false, "early zero-rate CPus response did not finish");
					return true;
				}
				CHECK(result == V92_PHASE4_DIGITAL_STATION_INTERNAL_ERROR &&
				      station.control.failed && !station.control.cpu_received &&
				      !station.control.peer_confirmation_received,
				      "initial training accepted a CPus in place of a full mapping");
				return true;
			}
			CHECK(result == V92_PHASE4_DIGITAL_STATION_OK ||
			      result == V92_PHASE4_DIGITAL_STATION_EVENT,
			      "station failed before receiving CPus");
		}
	}
	CHECK(false, "CPus was not decoded on the trained PCM line");
	return false;
}

static bool test_short_cpu_requires_renegotiation(void)
{
	return run_short_cpu(false) && run_short_cpu(true);
}

static bool test_cleardown_timing(void)
{
	static const uint16_t delays[] = { 0u, 1u, 160u, 161u, UINT16_MAX };

	for (size_t index = 0; index < ARRAY_LENGTH(delays); ++index) {
		for (unsigned int initiator = 0; initiator < 2u; ++initiator) {
			struct v92_cleardown state = { 0 };
			uint32_t wait = 800u + (initiator ? delays[index] :
				((uint32_t)delays[index] + 1u) / 2u);

			CHECK(v92_cleardown_is_valid(&state) && !v92_cleardown_is_ready(&state, UINT64_MAX),
			      "empty cleardown state was ready");
			v92_cleardown_start(&state, delays[index], initiator);
			v92_cleardown_receive(&state, !initiator, true, 0u);
			CHECK(!state.acknowledged, "old acknowledgement escaped initial guard");
			v92_cleardown_sent(&state, 100u, !initiator);
			v92_cleardown_receive(&state, !initiator, true, 99u);
			CHECK(!state.acknowledged && !v92_cleardown_is_ready(&state, 99u),
			      "queued source completion was mistaken for transmitted CP");
			v92_cleardown_sent(&state, 200u, !initiator);
			CHECK(state.cp_end == 100u && v92_cleardown_is_valid(&state) &&
			      !v92_cleardown_is_ready(&state, 100u + wait - 1u) &&
			      v92_cleardown_is_ready(&state, 100u + wait),
			      "cleardown timeout moved on a repeated CP or rounded RTD down");
			v92_cleardown_receive(&state, !initiator, true, 100u);
			CHECK(v92_cleardown_is_ready(&state, 100u) == (initiator != 0u),
			      "responder used initiator's acknowledgement shortcut");
			if (initiator) {
				v92_cleardown_receive(&state, true, true, 101u);
				CHECK(!v92_cleardown_is_ready(&state, 101u), "simultaneous request went unacknowledged");
				v92_cleardown_sent(&state, 210u, true);
				CHECK(!v92_cleardown_is_ready(&state, 209u) && v92_cleardown_is_ready(&state, 210u),
				      "simultaneous acknowledgement was not physically drained");
			}
		}
	}
	return true;
}

static bool run_silence_status(enum g711_law law, unsigned redundancy,
	unsigned lookahead, unsigned requests, unsigned arrival, bool inverted, unsigned confirmation)
{
	struct v92_cpt_parameters cpt = cpt_parameters(law);
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_parameters cpd = cpd_parameters(law);
	struct v92_suvd_parameters suvd = { .request_silence = (requests & 1u) != 0u };
	struct v92_control_event upstream = {
		.type = V92_CONTROL_EVENT_SUVU,
		.record.suvu = {
			.frame_result = V92_SUVU_OK,
			.parameters = {
				.request_silence = (requests & 2u) != 0u,
				.acknowledges_cpd = confirmation == 0u,
			},
		},
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmit;
	struct pcm_downstream_rx receive;
	struct v92_downstream_control control;
	unsigned plain = 0u, acknowledged = 0u;

	cpt.sign_redundancy = redundancy;
	cpt.spectral_lookahead = lookahead;
	cpd.extend_e2u = false;
	CHECK(v90_pcm_mapping_init_from_v92_cpt(&mapping, &cpt, cpt.law) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(&transmit, PCM_PROTOCOL_V92, &mapping) == PCM_DOWNSTREAM_TX_OK &&
	      v92_downstream_control_init(&control, &transmit, &suvd, &cpd, &cpu, 160u) &&
	      v92_downstream_control_ready(&control) &&
	      pcm_downstream_rx_init(&receive, PCM_PROTOCOL_V92, &mapping,
		V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK,
	      "digital silence fixture did not initialize");
	struct v92_downstream_control before = control;
	CHECK(!v92_downstream_control_note_e2u(&control) &&
	      memcmp(&control, &before, sizeof(before)) == 0,
	      "silence accepted E2u before sending its acknowledgement");
	for (unsigned clock = 0u; clock < 5000u; ++clock) {
		if (clock == arrival) {
			struct pcm_downstream_tx before = control.transmit;
			CHECK(v92_downstream_control_receive(&control, &upstream) &&
			      memcmp(&before, &control.transmit, sizeof(before)) == 0,
			      "silence request rewrote the pending downstream record");
		}
		uint8_t octet;
		struct pcm_downstream_event event;
		CHECK(v92_downstream_control_transmit(&control, &octet) &&
		      control.cpd_start_frame == 0u && !control.cpu_received &&
		      !control.transmit_acknowledges_cpu && !v92_downstream_control_is_ed_ready(&control) &&
		      (!v92_downstream_control_is_silence_ready(&control) || confirmation == 0u),
		      "digital silence sent or invented parameter negotiation");
		enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(&receive,
			inverted ? octet ^ 0x80u : octet, &event);
		if (result == PCM_DOWNSTREAM_RX_SUVD) {
			CHECK(event.control.record.suvd.frame_result == V92_SUVD_OK &&
			      event.control.record.suvd.parameters.request_silence == suvd.request_silence,
			      "silence status lost its request or CRC");
			if (event.control.record.suvd.parameters.acknowledges_cpu) {
				CHECK(plain != 0u && clock >= arrival, "silence ACK replaced the first SUVd");
				acknowledged++;
				if (!receive.ed_requested)
					CHECK(pcm_downstream_rx_expect_ed(&receive) == PCM_DOWNSTREAM_RX_OK,
					      "silence receiver could not arm Ed after its status boundary");
			} else {
				CHECK(acknowledged == 0u, "silence ACK regressed");
				plain++;
			}
		} else {
			CHECK(result == PCM_DOWNSTREAM_RX_BUFFERING || result == PCM_DOWNSTREAM_RX_TRN2D_FRAME,
			      "unexpected silence wire result %s", pcm_downstream_rx_result_name(result));
		}
		if (confirmation == 0u ? v92_downstream_control_is_silence_ready(&control) :
		    acknowledged >= 2u && control.transmit.control_bit_index != 0u)
			break;
	}
	CHECK((confirmation == 0u ? plain != 0u :
		(acknowledged >= 2u && control.transmit.control_bit_index != 0u)) &&
	      v92_downstream_control_is_silence_ready(&control) == (confirmation == 0u) &&
	      memcmp(&control.cpu, &cpu, sizeof(cpu)) == 0,
	      "digital silence did not acknowledge while retaining prior CPu");
	/* Invalid parameter traffic cannot reinterpret the pause or replace its
	 * retained mapping, but zero-rate cleardown remains authoritative. */
	{
		struct v92_downstream_control rejected = control;
		struct v92_control_event event = upstream;
		event.type = V92_CONTROL_EVENT_SUVU_REJECTED;
		CHECK(v92_downstream_control_receive(&rejected, &event) &&
		      memcmp(&rejected, &control, sizeof(control)) == 0,
		      "damaged SUVu acknowledged silence or changed its state");
		event = upstream;
		event.record.suvu.parameters.request_silence = !event.record.suvu.parameters.request_silence;
		CHECK(!v92_downstream_control_receive(&rejected, &event) && rejected.failed,
		      "late silence change was accepted");
		rejected.failed = false;
		CHECK(memcmp(&rejected, &control, sizeof(control)) == 0,
		      "rejected silence change altered the pending record");
		event = (struct v92_control_event){
			.type = V92_CONTROL_EVENT_CPU,
			.record.cpu = { .frame_result = V92_CPU_OK, .parameters = cpu },
		};
		CHECK(!v92_downstream_control_receive(&rejected, &event) && rejected.failed,
		      "silence accepted an ordinary CPu");
		rejected.failed = false;
		CHECK(memcmp(&rejected, &control, sizeof(control)) == 0,
		      "rejected CPu replaced the retained parameters");
		event.record.cpu.parameters.data_rate_number = 0u;
		CHECK(v92_downstream_control_receive(&rejected, &event) && rejected.cleardown.active &&
		      !v92_downstream_control_is_silence_ready(&rejected) &&
		      !v92_downstream_control_note_e2u(&rejected),
		      "silence hid a zero-rate cleardown request");
		for (unsigned invalid = 0u; invalid < 4u; ++invalid) {
			rejected = control;
			switch (invalid) {
			case 0: rejected.cpu_received = true; break;
			case 1: rejected.repeat_cpd = true; break;
			case 2: rejected.transmit_acknowledges_cpu = true; break;
			case 3: rejected.cpd_start_frame = 1u; break;
			}
			CHECK(!v92_downstream_control_is_valid(&rejected),
			      "silence admitted parameter state %u", invalid);
		}
		if (requests == 2u) {
			rejected = control;
			rejected.suvu.request_silence = false;
			rejected.cpd_start_frame = 1u;
			rejected.acknowledgement_end_frame = 0u;
			CHECK(!v92_downstream_control_is_valid(&rejected),
			      "parameter negotiation admitted the silence-only transmit stage");
		}
	}
	before = control;
	upstream.record.suvu.parameters.acknowledges_cpd = true;
	CHECK((confirmation == 2u ? v92_downstream_control_note_e2u(&control) :
		v92_downstream_control_receive(&control, &upstream)) &&
	      v92_downstream_control_is_silence_ready(&control) && !v92_downstream_control_is_ed_ready(&control) &&
	      memcmp(&before.transmit, &control.transmit, sizeof(control.transmit)) == 0 &&
	      memcmp(&before.initial_cpd, &control.initial_cpd, sizeof(control.initial_cpd)) == 0,
	      "silence confirmation lost the record tail or authorized data");
	if (confirmation == 2u) {
		upstream.record.suvu.parameters.acknowledges_cpd = false;
		CHECK(v92_downstream_control_receive(&control, &upstream) &&
		      v92_downstream_control_is_silence_ready(&control),
		      "an in-flight non-ACK SUVu erased E2u confirmation");
	}
	/* Ed drains this partial SUVd and the coder's lookahead, not a CPd. */
	uint64_t end_frame = control.transmit.accepted_frames +
		(control.transmit.control_bit_index ?
		 (control.transmit.control_sequence_bits - control.transmit.control_bit_index) / mapping.frame_bits : 0u);
	struct v92_phase4_ed_tx tail;
	bool ed_received = false;
	CHECK(v92_phase4_ed_tx_init(&tail, &control.transmit, NULL) && !tail.cpd_pending,
	      "silence Ed handoff required CPd");
	for (unsigned clock = 0u; clock < 256u && !tail.transmit.complete; ++clock) {
		struct pcm_downstream_symbol symbol;
		enum pcm_downstream_tx_result generated = v92_phase4_ed_tx_next(&tail, &symbol);
		if (generated == PCM_DOWNSTREAM_TX_COMPLETE)
			break;
		CHECK(generated == PCM_DOWNSTREAM_TX_OK, "silence Ed transmitter stopped");
		struct pcm_downstream_event event;
		enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(&receive,
			inverted ? symbol.pcm.octet ^ 0x80u : symbol.pcm.octet, &event);
		if (result == PCM_DOWNSTREAM_RX_SUVD) {
			CHECK(event.control.record.suvd.frame_result == V92_SUVD_OK &&
			      event.control.record.suvd.parameters.acknowledges_cpu &&
			      event.control.record.suvd.parameters.request_silence == suvd.request_silence,
			      "final silence SUVd was truncated or changed");
			acknowledged++;
			if (!receive.ed_requested)
				CHECK(pcm_downstream_rx_expect_ed(&receive) == PCM_DOWNSTREAM_RX_OK,
				      "queued acknowledgement did not arm Ed");
		} else if (result == PCM_DOWNSTREAM_RX_ED) {
			CHECK(!ed_received && event.ed.bit_errors == 0u, "silence Ed lost continuous coding");
			ed_received = true;
		} else
			CHECK(result == PCM_DOWNSTREAM_RX_BUFFERING, "silence tail emitted %s",
			      pcm_downstream_rx_result_name(result));
	}
	CHECK(tail.transmit.complete && acknowledged != 0u && ed_received &&
	      tail.transmit.accepted_frames == end_frame + 2u &&
	      tail.transmit.transmitted_symbols == tail.transmit.accepted_frames * V90_CP_FRAME_INTERVALS,
	      "silence handoff lost its complete SUVd, two Ed frames or six-symbol alignment");
	return true;
}

static bool test_silence_status(void)
{
	static const unsigned arrivals[] = { 0u, 2041u, 2303u };
	for (unsigned law = 0u; law < 2u; ++law)
		for (unsigned redundancy = 0u; redundancy < 4u; ++redundancy)
			for (unsigned lookahead = 0u; lookahead < 4u; ++lookahead)
				for (unsigned requests = 1u; requests < 4u; ++requests)
					for (size_t arrival = 0u; arrival < ARRAY_LENGTH(arrivals); ++arrival)
						for (unsigned inverted = 0u; inverted < 2u; ++inverted)
							for (unsigned confirmation = 0u; confirmation < 3u; ++confirmation)
								CHECK(run_silence_status(law, redundancy, lookahead, requests,
									arrivals[arrival], inverted, confirmation),
								      "silence law=%u redundancy=%u lookahead=%u requests=%u arrival=%u invert=%u confirmation=%u",
								      law, redundancy, lookahead, requests, arrivals[arrival], inverted, confirmation);
	return true;
}

static bool run_analog_confirmation(enum g711_law law, enum v92_constellation_size constellation,
	unsigned lookahead, unsigned requests, unsigned confirmation, bool renegotiation)
{
	struct v92_cpt_parameters cpt = cpt_parameters(law);
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_cpd_parameters cpd = cpd_parameters(law);
	struct v92_pcm_mapping upstream;
	struct v90_pcm_mapping training;
	struct v90_pcm_mapping downstream_data;
	struct pcm_downstream_rx receive;
	struct full_cpd_source source;
	struct v92_phase4_analog_station station;
	const struct v92_phase3_analog_transmit_handoff previous = {
		.lu = TEST_LU, .training_constellation = constellation,
	};
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = V92_SUVU_NO_MEASUREMENT_Q2_2,
	};
	struct v92_suvd_parameters suvd = { .request_silence = (requests & 2u) != 0u };
	size_t events = 0;
	uint8_t sequence[V92_CPD_MAX_FRAME_BYTES];
	size_t bits;
	cpt.spectral_lookahead = lookahead;
	cpt.sign_redundancy = lookahead ? 1u : 0u;
	/* Unlike the parser-only CPU fixture, this selection must support data
	 * so the returned handoff is usable by the next receiver owner. */
	cpu.data_rate_number = 1u;
	memset(cpu.transmit_constellations, 0, sizeof(cpu.transmit_constellations));
	for (unsigned n = 0; n < 8u; ++n)
		add_ucode(&cpu.transmit_constellations[0], (uint8_t)(24u + 12u * n));
	if (!requests)
		cpd.extend_e2u = !renegotiation && constellation == V92_CONSTELLATION_8_POINT;
	CHECK(v90_pcm_mapping_init_from_v92_cpt(&training, &cpt, cpt.law) == V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_init_from_v92_cpu(&downstream_data, &cpu, law) == V90_PCM_MAPPING_OK &&
	      v92_pcm_mapping_init(&upstream, law, &cpd) == V92_PCM_MAPPING_OK &&
	      pcm_downstream_rx_init(&receive, PCM_PROTOCOL_V92, &training, V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      (renegotiation ?
	       v92_phase4_analog_station_init_after_ru(&station, constellation, TEST_LU, &receive, &cpu,
		&upstream, (requests & 1u) != 0u, 240u) :
	       v92_phase4_analog_station_init(&station, &previous, &receive, &suvu, &cpu, NULL, NULL)) ==
		V92_PHASE4_ANALOG_STATION_OK &&
	      full_cpd_source_init(&source, &training) && push_full_cpd_training(&source, &station, &events),
	      "analogue confirmation fixture failed");
	/* Ordinary exchange receives SUVd then CPd; a pause retains its mapping.
	 * Unacknowledged repeats let our ACK reach the wire before a good final
	 * ACK, a CRC-damaged ACK, or Ed with no ACK. Keep the same scrambler and
	 * shaper throughout, including their retained control tail before Ed. */
	for (unsigned record = 0; record < 30u && !v92_phase4_analog_station_can_handoff(&station); ++record) {
		bool final = station.transmit.control.acknowledgment_sent;
		suvd.acknowledges_cpu = final && confirmation != 2u;
		CHECK(!requests && record == 1u ?
		      v92_cpd_encode(&cpd, training.frame_bits, sequence, sizeof(sequence), &bits) == V92_CPD_OK :
		      v92_suvd_encode(&suvd, training.frame_bits, sequence, sizeof(sequence), &bits) == V92_SUVD_OK,
		      "confirmation control encode failed");
		if (final && confirmation == 1u)
			sequence[4] ^= 0x80u;
		for (size_t first = 0; first < bits; first += training.frame_bits) {
			uint8_t frame[V90_PCM_FRAME_MAX_BITS];
			for (unsigned n = 0; n < V90_CP_FRAME_INTERVALS; ++n) {
				int16_t up;
				CHECK(v92_phase4_analog_station_transmit(&station, &up) == V92_PHASE4_ANALOG_STATION_OK,
				      "confirmation upstream control stopped");
			}
			for (unsigned n = 0; n < training.frame_bits; ++n)
				frame[n] = sequence_bit(sequence, first + n);
			CHECK(push_full_cpd_frame(&source, &station, frame, &events) == V92_PHASE4_ANALOG_STATION_OK,
			      "confirmation downstream control stopped");
			CHECK(station.transmit.control.acknowledgment_sent ||
			      !v92_phase4_analog_station_can_handoff(&station),
			      "confirmation bypassed the local wire acknowledgement");
			if (!requests && !station.cpd_received)
				CHECK(!station.receive.ed_requested, "Ed armed before receiving CPd");
		}
		if (final)
			break;
	}
	CHECK(station.transmit.control.acknowledgment_sent && station.cpd_received == !requests &&
	      station.transmit.control.cpu_sent == !requests &&
	      (!confirmation || !v92_phase4_analog_station_can_handoff(&station)),
	      "lost acknowledgement completed control or changed parameter ownership");
	/* The final Ed frame belongs to this receiver and must survive handoff. */
	for (unsigned n = 0; n < lookahead + V90_PHASE4_ED_FRAMES + 2u &&
	     !v92_phase4_analog_station_can_handoff(&station); ++n) {
		uint8_t zeros[V90_PCM_FRAME_MAX_BITS] = { 0 };
		CHECK(push_full_cpd_frame(&source, &station, zeros, &events) == V92_PHASE4_ANALOG_STATION_OK,
		      "Ed confirmation failed");
	}
	struct v92_phase4_analog_handoff handoff;
	CHECK(v92_phase4_analog_station_handoff(&station, &handoff) == V92_PHASE4_ANALOG_STATION_OK &&
	      handoff.pause == (requests != 0u) && station.cpd_received == !requests &&
	      station.transmit.control.cpu_sent == !requests &&
	      (!confirmation || pcm_downstream_rx_is_complete(&handoff.receive)) &&
	      (!confirmation || (!station.cpd.acknowledges_cpu && !station.suvd.acknowledges_cpu)) &&
	      v92_pcm_mapping_equal(&handoff.mapping, &upstream) &&
	      handoff.cpu.data_rate_number == cpu.data_rate_number &&
	      handoff.receive.stream.received_symbols == station.receive.stream.received_symbols,
	      "confirmation lost its mapping, exact Ed boundary, or actual ACK flags");
	return true;
}

static bool test_analog_pause_confirmation(void)
{
	for (unsigned law = 0; law < 2u; ++law)
		for (unsigned size = 0; size < 2u; ++size)
			for (unsigned lookahead = 0; lookahead < 4u; ++lookahead)
				for (unsigned requests = 1; requests < 4u; ++requests)
					for (unsigned confirmation = 0; confirmation < 3u; ++confirmation)
						CHECK(run_analog_confirmation(law, size ? V92_CONSTELLATION_8_POINT :
							V92_CONSTELLATION_4_POINT, lookahead, requests, confirmation, true),
						      "analogue pause law=%u size=%u lookahead=%u requests=%u confirmation=%u",
						      law, size, lookahead, requests, confirmation);
	return true;
}

static bool test_analog_data_confirmation(void)
{
	for (unsigned law = 0; law < 2u; ++law)
		for (unsigned size = 0; size < 2u; ++size)
			for (unsigned lookahead = 0; lookahead < 4u; ++lookahead)
				for (unsigned renegotiation = 0; renegotiation < 2u; ++renegotiation)
					for (unsigned confirmation = 0; confirmation < 3u; ++confirmation)
						CHECK(run_analog_confirmation(law, size ? V92_CONSTELLATION_8_POINT :
							V92_CONSTELLATION_4_POINT, lookahead, 0u, confirmation, renegotiation),
						      "analogue data law=%u size=%u lookahead=%u renegotiation=%u confirmation=%u",
						      law, size, lookahead, renegotiation, confirmation);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated digital station exchange", test_generated_exchange },
		{ "full CPd ownership", test_full_cpd_ownership },
		{ "paired analogue/digital station matrix",
		  test_paired_station_matrix },
		{ "selected CPd filter handoff and payload", test_selected_filters },
		{ "analogue early control handoff", test_analog_early_control },
		{ "analogue retry at complete peer records", test_analog_retry_boundary },
		{ "compact station boundary", test_compact_boundary },
		{ "early CPu acknowledgement", test_early_cpu_acknowledgement },
		{ "short CPu phase guard", test_short_cpu_requires_renegotiation },
		{ "cleardown physical CP deadlines", test_cleardown_timing },
		{ "digital silence status agreement", test_silence_status },
		{ "analogue pause acknowledgement or Ed", test_analog_pause_confirmation },
		{ "analogue data acknowledgement or Ed", test_analog_data_confirmation },
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
