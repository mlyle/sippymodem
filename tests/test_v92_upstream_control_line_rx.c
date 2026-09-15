// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_digital_rx.h>
#include <v92_e2u_rx.h>
#include <v92_upstream_control_line_rx.h>

#include "test.h"
#include "v92_phase4_record_fixture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 6000,
	STARTING_INPUT_SAMPLES = 4096,
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
		.acknowledges_cpd = true,
	};

	add_ucode(&parameters.transmit_constellations[0], 16u);
	add_ucode(&parameters.transmit_constellations[0], 72u);
	return parameters;
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
	      "control-line predecessor did not initialize");
	for (size_t index = 0;
	     index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t raw;
		struct qam_point input;

		if (index == 0u)
			raw = previous_sign != 0u ? -TEST_LU : TEST_LU;
		else
			CHECK(v92_pcm_phase4_trn2u_next(
				      transmitter, &raw) ==
				      V92_PCM_PHASE4_TRN2U_OK,
			      "control-line lookahead failed at %zu", index);
		input = (struct qam_point){
			.in_phase = polarity *
				line_sample(law, polarity, raw),
		};
		CHECK(qam_equalizer_push(
			      &equalizer, &input, &output, &ready) ==
			      QAM_EQUALIZER_OK,
		      "control-line equalizer fill failed at %zu", index);
	}
	CHECK(ready && qam_equalizer_adapt(
			       &equalizer, &output, 0.0) == QAM_EQUALIZER_OK,
	      "control-line equalizer did not become warm");
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
	CHECK(v92_pcm_phase3_cpt_line_handoff_is_valid(handoff),
	      "control-line predecessor handoff was invalid");
	return true;
}

static bool build_boundary(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity,
	struct v92_pcm_phase4_trn2u *transmitter,
	struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct v92_pcm_phase3_cpt_line_handoff cpt_handoff;
	struct v92_pcm_phase4_trn2u_line_rx training;

	CHECK(build_predecessor(
		      law, constellation, previous_sign, polarity,
		      transmitter, &cpt_handoff) &&
	      v92_pcm_phase4_trn2u_line_rx_init(
		      &training, &cpt_handoff, constellation, cpt_handoff.parameters.law) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_OK,
	      "control-line training receiver did not initialize");
	for (uint32_t symbol = 0;
	     symbol < V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
			      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     ++symbol) {
		int16_t raw;
		enum v92_pcm_phase4_trn2u_line_rx_result result;

		CHECK(v92_pcm_phase4_trn2u_next(transmitter, &raw) ==
			      V92_PCM_PHASE4_TRN2U_OK,
		      "control-line training failed at %u", symbol);
		result = v92_pcm_phase4_trn2u_line_rx_sample(
			&training, line_sample(law, polarity, raw));
		CHECK(result ==
			      (symbol + 1u ==
				       V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
					       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY ?
				       V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED :
				       V92_PCM_PHASE4_TRN2U_LINE_RX_OK),
		      "control-line boundary changed at %u: %s", symbol,
		      v92_pcm_phase4_trn2u_line_rx_result_name(result));
	}
	CHECK(transmitter->emitted_symbols ==
		      V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
	      v92_pcm_phase4_trn2u_line_rx_handoff(
		      &training, handoff) ==
		      V92_PCM_PHASE4_TRN2U_LINE_RX_OK &&
	      v92_pcm_phase4_trn2u_line_handoff_is_valid(handoff),
	      "control-line physical handoff failed");
	return true;
}

static bool suvu_parameters_equal(
	const struct v92_suvu_parameters *left,
	const struct v92_suvu_parameters *right)
{
	return left->prefilter_level_db_q2_2 ==
		       right->prefilter_level_db_q2_2 &&
	       left->request_silence == right->request_silence &&
	       left->wait_for_cpu == right->wait_for_cpu &&
	       left->acknowledges_cpd == right->acknowledges_cpd;
}

static bool cpu_parameters_equal(
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
	       memcmp(
		       left->transmit_constellations,
		       right->transmit_constellations,
		       sizeof(left->transmit_constellations)) == 0 &&
	       memcmp(
		       left->frame_constellation,
		       right->frame_constellation,
		       sizeof(left->frame_constellation)) == 0;
}

static bool receive_sample(
	struct v92_phase4_digital_rx *receiver,
	enum g711_law law, int8_t polarity, int16_t raw,
	struct v92_control_event *suvu_event,
	struct v92_control_event *cpu_event)
{
	struct v92_control_event event;
	enum v92_phase4_digital_rx_result result =
		v92_phase4_digital_rx_sample(
			receiver, line_sample(law, polarity, raw), &event);

	CHECK(result == V92_PHASE4_DIGITAL_RX_OK ||
	      result == V92_PHASE4_DIGITAL_RX_EVENT,
	      "control-line sample failed: %s",
	      v92_phase4_digital_rx_result_name(result));
	if (event.type == V92_CONTROL_EVENT_SUVU) {
		CHECK(suvu_event->type == V92_CONTROL_EVENT_NONE,
		      "control line emitted duplicate SUVu");
		*suvu_event = event;
	} else if (event.type == V92_CONTROL_EVENT_CPU) {
		CHECK(cpu_event->type == V92_CONTROL_EVENT_NONE,
		      "control line emitted duplicate CPu");
		*cpu_event = event;
	} else {
		CHECK(event.type == V92_CONTROL_EVENT_NONE,
		      "control line emitted unexpected event %u", event.type);
	}
	return true;
}

static bool run_records(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity, int16_t offset)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.request_silence = true,
		.wait_for_cpu = true,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_phase4_digital_rx receiver;
	struct v92_pcm_phase4_trn2u reference;
	struct v92_upstream_channel expected_channel = { 0 };
	struct v92_phase4_record_tx suvu_tx;
	struct v92_phase4_record_tx cpu_tx;
	struct v92_phase4_modulator modulator;
	struct v92_control_event suvu_event = { 0 };
	struct v92_control_event cpu_event = { 0 };
	uint64_t starting_samples;
	size_t cpu_symbols = 0u;

	CHECK(build_predecessor(
		      law, constellation, previous_sign, polarity,
		      &transmitter, &handoff) &&
	      v92_phase4_digital_rx_init(
		      &receiver, &handoff, constellation, handoff.parameters.law) ==
		      V92_PHASE4_DIGITAL_RX_OK,
	      "digital Phase 4 line did not initialize");
	CHECK(v92_pcm_phase4_trn2u_init(&reference, constellation, previous_sign, TEST_LU) ==
		      V92_PCM_PHASE4_TRN2U_OK, "channel reference did not initialize");
	for (uint32_t symbol = 0;
	     symbol < V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
			      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     ++symbol) {
		struct v92_control_event event = {
			.type = V92_CONTROL_EVENT_CPU,
		};
		enum v92_phase4_digital_rx_result result;
		int16_t raw;

		CHECK(v92_pcm_phase4_trn2u_next(&transmitter, &raw) ==
			      V92_PCM_PHASE4_TRN2U_OK,
		      "digital Phase 4 training failed at %u", symbol);
		result = v92_phase4_digital_rx_sample(
			&receiver, line_sample(law, polarity, raw) + polarity * offset, &event);
		CHECK(result ==
			      (symbol + 1u ==
				       V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS -
					       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY ?
				       V92_PHASE4_DIGITAL_RX_ALIGNED :
				       V92_PHASE4_DIGITAL_RX_OK) &&
		      event.type == V92_CONTROL_EVENT_NONE,
		      "digital Phase 4 boundary changed at %u: %s", symbol,
		      v92_phase4_digital_rx_result_name(result));
		int16_t target;

		CHECK(v92_pcm_phase4_trn2u_next(&reference, &target) == V92_PCM_PHASE4_TRN2U_OK &&
		      v92_upstream_channel_sample(&expected_channel, target,
			      line_sample(law, 1, target) +
			      (symbol < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY ? 0 : offset)),
		      "channel reference failed");
	}
	CHECK(receiver.channel.symbols == V92_UPSTREAM_CHANNEL_SYMBOLS -
		      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      !receiver.channel_distorted &&
	      memcmp(&receiver.channel, &expected_channel, sizeof(expected_channel)) == 0,
	      "physical handoff lost channel history or used equalized rather than raw samples");
	CHECK(transmitter.emitted_symbols ==
		      V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS &&
	      v92_phase4_digital_rx_is_receiving_control(&receiver) &&
	      v92_record_fixture_start_suvu(
		      &suvu_tx, &transmitter.modulator, &suvu),
	      "control-line record path did not initialize");
	starting_samples = receiver.signal.control.signal.line.input_samples;
	while (!v92_phase4_record_tx_is_complete(&suvu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_record_tx_next(&suvu_tx, &raw) ==
			      V92_PHASE4_RECORD_TX_OK &&
		      receive_sample(
			      &receiver, law, polarity, raw,
			      &suvu_event, &cpu_event),
		      "control-line SUVu generation failed");
		if (expected_channel.symbols < V92_UPSTREAM_CHANNEL_SYMBOLS) {
			int16_t target;

			CHECK(v92_pcm_phase4_trn2u_next(&reference, &target) == V92_PCM_PHASE4_TRN2U_OK &&
			      v92_upstream_channel_sample(&expected_channel, target,
				      line_sample(law, 1, target) + offset), "delayed channel reference failed");
		}
		CHECK(memcmp(&receiver.channel, &expected_channel, sizeof(expected_channel)) == 0,
		      "channel lost a delayed training symbol or included control data");
	}
	CHECK(suvu_event.type == V92_CONTROL_EVENT_NONE &&
	      receiver.channel.symbols == V92_UPSTREAM_CHANNEL_SYMBOLS &&
	      !receiver.channel_distorted &&
	      receiver.signal.control.signal.training_tail_remaining == 0u,
	      "control line exposed SUVu before its delayed decisions");
	CHECK(v92_phase4_record_tx_handoff(&suvu_tx, &modulator) ==
		      V92_PHASE4_RECORD_TX_OK &&
	      v92_record_fixture_start_cpu(&cpu_tx, &modulator, &cpu),
	      "control-line CPu continuation did not initialize");
	while (!v92_phase4_record_tx_is_complete(&cpu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_record_tx_next(&cpu_tx, &raw) ==
			      V92_PHASE4_RECORD_TX_OK &&
		      receive_sample(
			      &receiver, law, polarity, raw,
			      &suvu_event, &cpu_event),
			      "control-line CPu generation failed");
		cpu_symbols++;
		CHECK((cpu_symbols <
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
		       suvu_event.type == V92_CONTROL_EVENT_NONE) ||
		      (cpu_symbols >=
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
		       suvu_event.type == V92_CONTROL_EVENT_SUVU),
		      "control line exposed SUVu at CPu symbol %zu",
		      cpu_symbols);
	}
	CHECK(cpu_event.type == V92_CONTROL_EVENT_NONE,
	      "control line exposed CPu before its delayed decisions");
	CHECK(v92_record_fixture_start_cpu(&cpu_tx, NULL, &cpu),
	      "control-line CPu flush record did not start");
	for (size_t symbol = 0;
	     symbol < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++symbol) {
		int16_t raw;

		CHECK(v92_phase4_record_tx_next(&cpu_tx, &raw) ==
			      V92_PHASE4_RECORD_TX_OK &&
		      receive_sample(
			      &receiver, law, polarity, raw,
			      &suvu_event, &cpu_event),
			      "control-line delayed CPu failed at %zu", symbol);
		CHECK((symbol + 1u <
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
		       cpu_event.type == V92_CONTROL_EVENT_NONE) ||
		      (symbol + 1u ==
			       V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
		       cpu_event.type == V92_CONTROL_EVENT_CPU),
		      "control line exposed CPu at flush symbol %zu", symbol);
	}
	CHECK(suvu_event.type == V92_CONTROL_EVENT_SUVU &&
	      suvu_event.record.suvu.frame_result == V92_SUVU_OK &&
	      suvu_parameters_equal(
		      &suvu_event.record.suvu.parameters, &suvu) &&
	      cpu_event.type == V92_CONTROL_EVENT_CPU &&
	      cpu_event.record.cpu.frame_result == V92_CPU_OK &&
	      cpu_parameters_equal(
		      &cpu_event.record.cpu.parameters, &cpu) &&
	      receiver.signal.control.signal.training_tail_remaining == 0u &&
	      receiver.signal.control.signal.line.input_samples > starting_samples &&
	      v92_phase4_digital_rx_is_valid(&receiver) &&
	      v92_phase4_digital_rx_is_receiving_control(&receiver) &&
	      !v92_phase4_digital_rx_has_failed(&receiver),
	      "control-line records or retained state differed");
	CHECK(memcmp(&receiver.channel, &expected_channel, sizeof(expected_channel)) == 0,
	      "control records changed the completed training measurement");
	return true;
}

static bool test_generated_lines(void)
{
	return run_records(
		       G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1, 0) &&
	       run_records(
		       G711_LAW_PCMA, V92_CONSTELLATION_8_POINT, 1u, -1, 0) &&
	       /* Host conditioning can move samples within the same G.711 cell.
		* Retain those raw values without mistaking them for channel memory. */
	       run_records(G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1, 1) &&
	       run_records(G711_LAW_PCMA, V92_CONSTELLATION_8_POINT, 1u, -1, 1);
}

static bool run_e2u(
	enum g711_law law, enum v92_constellation_size constellation,
	bool extended, bool invalid_sequence)
{
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_handoff boundary;
	struct v92_upstream_control_line_rx control;
	struct v92_phase4_record_tx cpu_tx;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_phase4_modulator modulator;
	struct v92_e2u_rx receiver;
	struct v92_upstream_rx handoff;
	struct v92_upstream_rx unchanged;
	struct v92_control_event event = { 0 };
	uint8_t invalid_frame[5] = { 0 };
	unsigned int bits =
		v92_phase4_modulator_bits_per_symbol(constellation);
	size_t expected_symbols = extended ?
		V92_PHASE4_E2U_EXTENDED_SYMBOLS : V92_PHASE4_E2U_SYMBOLS;
	size_t physical_e2u_symbols = 0u;
	enum v92_e2u_rx_result final_result = V92_E2U_RX_OK;
	bool continuation_started = false;

	CHECK(build_boundary(
		      law, constellation, (uint8_t)(law == G711_LAW_PCMA),
		      law == G711_LAW_PCMA ? -1 : 1,
		      &transmitter, &boundary) &&
	      v92_upstream_control_line_rx_init(&control, &boundary) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_OK &&
	      v92_record_fixture_start_cpu(
		      &cpu_tx, &transmitter.modulator, &cpu),
	      "E2u predecessor did not initialize");
	while (!v92_phase4_record_tx_is_complete(&cpu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_record_tx_next(&cpu_tx, &raw) ==
			      V92_PHASE4_RECORD_TX_OK &&
		      v92_upstream_control_line_rx_sample(
			      &control,
			      line_sample(
				      law, law == G711_LAW_PCMA ? -1 : 1,
				      raw),
			      &event) == V92_UPSTREAM_CONTROL_LINE_RX_OK &&
		      event.type == V92_CONTROL_EVENT_NONE,
		      "E2u predecessor record stopped");
	}
	CHECK(v92_phase4_record_tx_handoff(&cpu_tx, &modulator) ==
		      V92_PHASE4_RECORD_TX_OK,
	      "E2u predecessor handoff failed");
	if (invalid_sequence) {
		size_t invalid_bits = expected_symbols * bits;
		size_t invalid_bytes = (invalid_bits + 7u) / 8u;

		memset(invalid_frame, 0xff, sizeof(invalid_frame));
		if (invalid_bits % 8u != 0u)
			invalid_frame[invalid_bytes - 1u] =
				(uint8_t)(UINT8_C(0xff) <<
					  (8u - invalid_bits % 8u));
		CHECK(v92_phase4_record_tx_init(
			      &e2u_tx, &modulator, invalid_frame,
			      sizeof(invalid_frame), invalid_bits) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "invalid E2u generator did not initialize");
	} else {
		CHECK(v92_phase4_record_tx_init_e2u(
			      &e2u_tx, &modulator, extended) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "E2u generator did not initialize");
	}
	while (event.type == V92_CONTROL_EVENT_NONE) {
		int16_t raw;
		enum v92_upstream_control_line_rx_result result;

		CHECK(!v92_phase4_record_tx_is_complete(&e2u_tx) &&
		      v92_phase4_record_tx_next(&e2u_tx, &raw) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "E2u ended before the delayed CPu boundary");
		physical_e2u_symbols++;
		result = v92_upstream_control_line_rx_sample(
			&control,
			line_sample(
				law, law == G711_LAW_PCMA ? -1 : 1, raw),
			&event);
		CHECK((result == V92_UPSTREAM_CONTROL_LINE_RX_OK ||
		       result == V92_UPSTREAM_CONTROL_LINE_RX_EVENT) &&
		      (event.type == V92_CONTROL_EVENT_NONE ||
		       event.type == V92_CONTROL_EVENT_CPU),
		      "E2u boundary produced an unexpected control event");
	}
	CHECK(event.type == V92_CONTROL_EVENT_CPU &&
	      event.record.cpu.frame_result == V92_CPU_OK &&
	      physical_e2u_symbols ==
		      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      control.signal.training_tail_remaining == 0u &&
	      v92_e2u_rx_init(&receiver, &control.signal, extended) ==
		      V92_E2U_RX_OK,
	      "E2u decision boundary did not follow CPu");
	while (final_result == V92_E2U_RX_OK) {
		int16_t raw;

		if (!v92_phase4_record_tx_is_complete(&e2u_tx)) {
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &raw) ==
				      V92_PHASE4_RECORD_TX_OK,
			      "E2u continuation stopped");
		} else {
			if (!continuation_started) {
				CHECK(v92_phase4_record_tx_handoff(
					      &e2u_tx, &modulator) ==
					      V92_PHASE4_RECORD_TX_OK,
				      "post-E2u handoff stopped");
				continuation_started = true;
			}
			CHECK(
			      v92_phase4_modulator_next(
				      &modulator, 1u, &raw) ==
				      V92_PHASE4_MODULATOR_OK,
			      "post-E2u delay fill stopped");
		}
		final_result = v92_e2u_rx_sample(
			&receiver,
			line_sample(
				law, law == G711_LAW_PCMA ? -1 : 1, raw));
	}
	CHECK(final_result == (invalid_sequence ? V92_E2U_RX_NO_MATCH :
					       V92_E2U_RX_COMPLETE) &&
	      v92_e2u_rx_is_valid(&receiver) &&
	      !v92_e2u_rx_has_failed(&receiver) &&
	      v92_e2u_rx_is_complete(&receiver) == !invalid_sequence,
	      "E2u result differed law=%u constellation=%u extended=%u "
	      "invalid=%u result=%s errors=%u correlation=%.0f",
	      (unsigned int)law, (unsigned int)constellation, extended,
	      invalid_sequence, v92_e2u_rx_result_name(final_result),
	      receiver.bit_errors, receiver.correlation);
	{
		struct v92_e2u_rx terminal = receiver;

		CHECK(v92_e2u_rx_sample(&receiver, 0) == final_result &&
		      memcmp(&receiver, &terminal, sizeof(receiver)) == 0,
		      "terminal E2u receiver was not stable");
	}
	unchanged = receiver.signal;
	memset(&handoff, 0xa5, sizeof(handoff));
	if (invalid_sequence) {
		struct v92_upstream_rx saved = handoff;

		CHECK(v92_e2u_rx_handoff(&receiver, &handoff) ==
			      V92_E2U_RX_INVALID_STATE &&
		      memcmp(&handoff, &saved, sizeof(handoff)) == 0,
		      "rejected E2u exported a line handoff");
	} else {
		CHECK(v92_e2u_rx_handoff(&receiver, &handoff) ==
			      V92_E2U_RX_OK &&
		      memcmp(&handoff, &unchanged, sizeof(handoff)) == 0,
		      "accepted E2u lost its line handoff");
	}
	return true;
}

static bool test_e2u(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};

	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (size_t constellation = 0u;
		     constellation < ARRAY_LENGTH(constellations);
		     ++constellation) {
			for (unsigned int extended = 0u; extended <= 1u;
			     ++extended) {
				CHECK(run_e2u(
					      (enum g711_law)law,
					      constellations[constellation],
					      extended != 0u, false) &&
				      run_e2u(
					      (enum g711_law)law,
					      constellations[constellation],
					      extended != 0u, true),
				      "E2u matrix stopped");
			}
		}
	}
	return true;
}

static bool test_e2u_contracts(void)
{
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_handoff boundary;
	struct v92_upstream_rx signal;
	struct v92_upstream_rx ready;
	struct v92_upstream_rx output;
	struct v92_upstream_rx output_saved;
	struct v92_e2u_rx receiver;
	struct v92_e2u_rx untouched;
	struct v92_e2u_rx saved;
	struct v92_upstream_decision decision = {
		.equalized_level = 123.0,
		.nearest_level = 123,
		.group = UINT8_C(0xa5),
	};
	struct v92_upstream_decision decision_saved;
	struct v92_upstream_rx ready_saved;
	int16_t expected_level = 123;
	static const char *const names[] = {
		"ok", "complete", "no match", "failed", "invalid argument",
		"invalid state", "numeric failure",
	};

	CHECK(build_boundary(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1,
		      &transmitter, &boundary) &&
	      v92_upstream_rx_init(&signal, &boundary) == V92_UPSTREAM_RX_OK,
	      "E2u contract boundary failed");
	ready = signal;
	ready.training_tail_remaining = 0u;
	ready_saved = ready;
	decision_saved = decision;
	CHECK(!v92_upstream_rx_expected_level(NULL, 0u, &expected_level) &&
	      !v92_upstream_rx_expected_level(&ready, 0u, NULL) &&
	      !v92_upstream_rx_expected_level(&ready, 4u, &expected_level) &&
	      expected_level == 123 &&
	      v92_upstream_rx_expected_level(&ready, 0u, &expected_level) &&
	      v92_upstream_rx_sample_known(
		      &ready, 0, 4u, &decision) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&ready, &ready_saved, sizeof(ready)) == 0 &&
	      memcmp(&decision, &decision_saved, sizeof(decision)) == 0,
	      "known upstream decision contracts differed");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(v92_e2u_rx_init(NULL, &ready, false) ==
		      V92_E2U_RX_INVALID_ARGUMENT &&
	      v92_e2u_rx_init(&untouched, NULL, false) ==
		      V92_E2U_RX_INVALID_ARGUMENT &&
	      v92_e2u_rx_init(&untouched, &signal, false) ==
		      V92_E2U_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved, sizeof(saved)) == 0 &&
	      v92_e2u_rx_init(&receiver, &ready, false) == V92_E2U_RX_OK,
	      "E2u initialization was not transactional");
	saved = receiver;
	memset(&output, 0xa5, sizeof(output));
	output_saved = output;
	CHECK(v92_e2u_rx_sample(NULL, 0) == V92_E2U_RX_INVALID_ARGUMENT &&
	      v92_e2u_rx_handoff(NULL, &output) ==
		      V92_E2U_RX_INVALID_ARGUMENT &&
	      v92_e2u_rx_handoff(&receiver, NULL) ==
		      V92_E2U_RX_INVALID_ARGUMENT &&
	      v92_e2u_rx_handoff(
		      &receiver,
		      (struct v92_upstream_rx *)(void *)&receiver) ==
		      V92_E2U_RX_INVALID_ARGUMENT &&
	      v92_e2u_rx_handoff(&receiver, &output) ==
		      V92_E2U_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &output_saved, sizeof(output)) == 0,
	      "E2u invalid calls changed state");
	receiver.failed = true;
	saved = receiver;
	CHECK(v92_e2u_rx_has_failed(&receiver) &&
	      v92_e2u_rx_sample(&receiver, 0) == V92_E2U_RX_FAILED &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "failed E2u receiver was not stable");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index) {
		CHECK(strcmp(
			      v92_e2u_rx_result_name(
				      (enum v92_e2u_rx_result)index),
			      names[index]) == 0,
		      "E2u result name %zu differed", index);
	}
	CHECK(strcmp(
		      v92_e2u_rx_result_name((enum v92_e2u_rx_result)99),
		      "unknown") == 0 &&
	      !v92_e2u_rx_is_valid(NULL) &&
	      !v92_e2u_rx_is_complete(NULL) &&
	      !v92_e2u_rx_has_failed(NULL),
	      "E2u validation contracts differed");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_handoff handoff;
	struct v92_pcm_phase4_trn2u_line_handoff invalid;
	struct v92_upstream_rx signal;
	struct v92_upstream_rx signal_untouched;
	struct v92_upstream_rx signal_saved;
	struct v92_upstream_decision decision = {
		.equalized_level = 123.0,
		.nearest_level = 123,
		.group = UINT8_C(0xa5),
	};
	struct v92_upstream_decision decision_before;
	struct v92_upstream_control_line_rx receiver;
	struct v92_upstream_control_line_rx untouched;
	struct v92_upstream_control_line_rx saved;
	struct v92_control_event event = { .type = V92_CONTROL_EVENT_CPU };
	struct v92_control_event event_before;
	static const char *const names[] = {
		"ok", "event", "failed", "invalid argument",
		"invalid state", "internal error",
	};

	CHECK(build_boundary(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT,
		      0u, 1, &transmitter, &handoff),
	      "control-line contract handoff failed");
	invalid = handoff;
	invalid.training_symbols++;
	memset(&signal_untouched, 0xa5, sizeof(signal_untouched));
	signal_saved = signal_untouched;
	CHECK(v92_upstream_rx_init(NULL, &handoff) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v92_upstream_rx_init(&signal_untouched, NULL) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v92_upstream_rx_init(&signal_untouched, &invalid) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&signal_untouched, &signal_saved, sizeof(signal_saved)) == 0 &&
	      v92_upstream_rx_init(&signal, &handoff) == V92_UPSTREAM_RX_OK,
	      "shared upstream initialization was not transactional");
	decision_before = decision;
	signal_saved = signal;
	CHECK(v92_upstream_rx_sample(NULL, 0, &decision) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v92_upstream_rx_sample(&signal, 0, NULL) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v92_upstream_rx_sample(
		      &signal, 0,
		      (struct v92_upstream_decision *)(void *)&signal) ==
		      V92_UPSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&signal, &signal_saved, sizeof(signal)) == 0 &&
	      memcmp(&decision, &decision_before, sizeof(decision)) == 0,
	      "shared upstream invalid calls changed state");
	{
		uint64_t starting_samples = signal.line.input_samples;
		uint8_t starting_tail = signal.training_tail_remaining;
		int16_t raw;

		CHECK(v92_pcm_phase4_trn2u_next(&transmitter, &raw) ==
			      V92_PCM_PHASE4_TRN2U_OK &&
		      v92_upstream_rx_sample(
			      &signal,
			      line_sample(G711_LAW_PCMU, 1, raw),
			      &decision) == V92_UPSTREAM_RX_OK &&
		      signal.line.input_samples == starting_samples + 1u &&
		      signal.training_tail_remaining == starting_tail - 1u &&
		      decision.group < 4u &&
		      v92_upstream_rx_is_valid(&signal) &&
		      !v92_upstream_rx_has_failed(&signal),
		      "shared upstream decision did not preserve the line");
	}
	signal.failed = true;
	signal_saved = signal;
	decision_before = decision;
	CHECK(v92_upstream_rx_has_failed(&signal) &&
	      v92_upstream_rx_sample(&signal, 0, &decision) ==
		      V92_UPSTREAM_RX_FAILED &&
	      memcmp(&signal, &signal_saved, sizeof(signal)) == 0 &&
	      memcmp(&decision, &decision_before, sizeof(decision)) == 0,
	      "failed shared upstream receiver was not stable");
	CHECK(strcmp(v92_upstream_rx_result_name(V92_UPSTREAM_RX_OK), "ok") == 0 &&
	      strcmp(v92_upstream_rx_result_name(V92_UPSTREAM_RX_FAILED),
		     "failed") == 0 &&
	      strcmp(v92_upstream_rx_result_name(V92_UPSTREAM_RX_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v92_upstream_rx_result_name(V92_UPSTREAM_RX_INVALID_STATE),
		     "invalid state") == 0 &&
	      strcmp(v92_upstream_rx_result_name(V92_UPSTREAM_RX_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v92_upstream_rx_result_name(
		     (enum v92_upstream_rx_result)99), "unknown") == 0 &&
	      !v92_upstream_rx_is_valid(NULL) &&
	      !v92_upstream_rx_has_failed(NULL),
	      "shared upstream validation contracts differed");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(v92_upstream_control_line_rx_init(NULL, &handoff) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT &&
	      v92_upstream_control_line_rx_init(&untouched, NULL) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT &&
	      v92_upstream_control_line_rx_init(&untouched, &invalid) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved, sizeof(saved)) == 0 &&
	      v92_upstream_control_line_rx_init(&receiver, &handoff) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_OK,
	      "control-line initialization was not transactional");
	event_before = event;
	saved = receiver;
	CHECK(v92_upstream_control_line_rx_sample(NULL, 0, &event) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT &&
	      v92_upstream_control_line_rx_sample(&receiver, 0, NULL) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "control-line null calls changed state");
	for (unsigned fault = 0; fault < 5; ++fault) {
		untouched = receiver;
		switch (fault) {
		case 0: untouched.cpu_repeat_symbols = 1u; break;
		case 1: untouched.cpu_repeat_symbols = UINT16_MAX - UINT16_MAX % V92_CPU_SYMBOLS_PER_FRAME; break;
		case 2: untouched.cpu_repeat_remaining = 1u; break;
		case 3: untouched.cpu_repeat_confirmed = true; break;
		case 4:
			untouched.cpu_repeat_symbols = V92_CPU_SYMBOLS_PER_FRAME;
			untouched.cpu_repeat_remaining = V92_CPU_SYMBOLS_PER_FRAME + 1u;
			break;
		}
		saved = untouched;
		CHECK(!v92_upstream_control_line_rx_is_valid(&untouched) &&
		      v92_upstream_control_line_rx_sample(&untouched, 0, &event) ==
			      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&untouched, &saved, sizeof(saved)) == 0 &&
		      memcmp(&event, &event_before, sizeof(event)) == 0,
		      "invalid CPu repetition clock changed state, fault %u", fault);
	}
	receiver.signal.failed = true;
	saved = receiver;
	CHECK(v92_upstream_control_line_rx_has_failed(&receiver) &&
	      v92_upstream_control_line_rx_sample(&receiver, 0, &event) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_FAILED &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "failed control line was not stable");
	receiver.signal.failed = false;
	receiver.signal.line.input_samples = UINT64_MAX;
	saved = receiver;
	CHECK(v92_upstream_control_line_rx_sample(&receiver, 0, &event) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "overflowing control line was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index) {
		CHECK(strcmp(
			      v92_upstream_control_line_rx_result_name(
				      (enum v92_upstream_control_line_rx_result)
					      index),
			      names[index]) == 0,
		      "control-line result name %zu differed", index);
	}
	CHECK(strcmp(
		      v92_upstream_control_line_rx_result_name(
			      (enum v92_upstream_control_line_rx_result)99),
		      "unknown") == 0 &&
	      !v92_upstream_control_line_rx_is_valid(NULL) &&
	      !v92_upstream_control_line_rx_has_failed(NULL),
	      "control-line validation contracts differed");
	return true;
}

static bool test_digital_receive_contracts(void)
{
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase3_cpt_line_handoff handoff;
	struct v92_pcm_phase3_cpt_line_handoff invalid;
	struct v92_pcm_phase3_cpt_line_handoff unrepresentable;
	struct v92_phase4_digital_rx receiver;
	struct v92_phase4_digital_rx untouched;
	struct v92_phase4_digital_rx saved;
	struct v92_control_event event = { .type = V92_CONTROL_EVENT_CPU };
	struct v92_control_event event_before;
	static const char *const names[] = {
		"ok", "aligned", "event", "failed", "invalid argument",
		"invalid state", "numeric failure",
	};

	CHECK(build_predecessor(
		      G711_LAW_PCMU, V92_CONSTELLATION_4_POINT,
		      0u, 1, &transmitter, &handoff),
	      "digital receive contract handoff failed");
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	invalid = handoff;
	invalid.input_advance_symbols = 0u;
	unrepresentable = handoff;
	unrepresentable.signal_level = 32768.0;
	CHECK(v92_phase4_digital_rx_init(
		      &untouched, &handoff, V92_CONSTELLATION_4_POINT, (enum g711_law)99) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_phase4_digital_rx_init(
		      NULL, &handoff, V92_CONSTELLATION_4_POINT, handoff.parameters.law) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_phase4_digital_rx_init(
		      &untouched, NULL, V92_CONSTELLATION_4_POINT, G711_LAW_PCMU) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_phase4_digital_rx_init(
		      &untouched, &invalid, V92_CONSTELLATION_4_POINT, invalid.parameters.law) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_line_handoff_is_valid(&unrepresentable) &&
	      v92_phase4_digital_rx_init(
		      &untouched, &unrepresentable,
		      V92_CONSTELLATION_4_POINT, unrepresentable.parameters.law) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_phase4_digital_rx_init(
		      &untouched, &handoff,
		      (enum v92_constellation_size)99, handoff.parameters.law) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved, sizeof(saved)) == 0 &&
	      v92_phase4_digital_rx_init(
		      &receiver, &handoff, V92_CONSTELLATION_4_POINT, handoff.parameters.law) ==
		      V92_PHASE4_DIGITAL_RX_OK &&
	      v92_phase4_digital_rx_is_valid(&receiver) &&
	      !v92_phase4_digital_rx_has_failed(&receiver) &&
	      !v92_phase4_digital_rx_is_receiving_control(&receiver),
	      "digital receive initialization was not transactional");
	event_before = event;
	saved = receiver;
	CHECK(v92_phase4_digital_rx_sample(NULL, 0, &event) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_phase4_digital_rx_sample(&receiver, 0, NULL) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      v92_phase4_digital_rx_sample(
		      &receiver, 0, (struct v92_control_event *)&receiver) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "digital receive invalid calls changed state");
	receiver.channel.symbols = 1u;
	struct v92_phase4_digital_rx inconsistent = receiver;

	CHECK(!v92_phase4_digital_rx_is_valid(&receiver) &&
	      v92_phase4_digital_rx_sample(&receiver, 0, &event) == V92_PHASE4_DIGITAL_RX_INVALID_STATE &&
	      memcmp(&receiver, &inconsistent, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "inconsistent channel extent was accepted");
	receiver = saved;
	receiver.channel_distorted = true;
	CHECK(!v92_phase4_digital_rx_is_valid(&receiver),
	      "unmeasured channel accepted distortion evidence");
	receiver = saved;
	receiver.failed = true;
	saved = receiver;
	CHECK(v92_phase4_digital_rx_has_failed(&receiver) &&
	      v92_phase4_digital_rx_sample(&receiver, 0, &event) ==
		      V92_PHASE4_DIGITAL_RX_FAILED &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "failed digital receive was not stable");
	receiver.failed = false;
	receiver.signal.trn2u.starting_input_samples = UINT64_MAX;
	receiver.signal.trn2u.input_samples = UINT64_MAX;
	CHECK(v92_phase4_digital_rx_is_valid(&receiver),
	      "overflow digital receive setup was invalid");
	saved = receiver;
	CHECK(v92_phase4_digital_rx_sample(&receiver, 0, &event) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "overflowing digital receive was not transactional");
	receiver.stage = (enum v92_phase4_digital_rx_stage)99;
	saved = receiver;
	CHECK(v92_phase4_digital_rx_sample(&receiver, 0, &event) ==
		      V92_PHASE4_DIGITAL_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid digital receive was not transactional");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index) {
		CHECK(strcmp(
			      v92_phase4_digital_rx_result_name(
				      (enum v92_phase4_digital_rx_result)index),
			      names[index]) == 0,
		      "digital receive result name %zu differed", index);
	}
	CHECK(strcmp(
		      v92_phase4_digital_rx_result_name(
			      (enum v92_phase4_digital_rx_result)99),
		      "unknown") == 0 &&
	      !v92_phase4_digital_rx_is_valid(NULL) &&
	      !v92_phase4_digital_rx_has_failed(NULL) &&
	      !v92_phase4_digital_rx_is_receiving_control(NULL),
	      "digital receive validation contracts differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated digital Phase 4 lines", test_generated_lines },
		{ "generated E2u boundaries", test_e2u },
		{ "E2u contracts", test_e2u_contracts },
		{ "control-line contracts", test_contracts },
		{ "digital receive contracts", test_digital_receive_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0u ? 0 : 1;
}
