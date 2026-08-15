// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_cpu_tx.h>
#include <v92_phase4_suvu_tx.h>
#include <v92_upstream_control_line_rx.h>

#include "test.h"

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

static bool build_boundary(
	enum g711_law law, enum v92_constellation_size constellation,
	uint8_t previous_sign, int8_t polarity,
	struct v92_pcm_phase4_trn2u *transmitter,
	struct v92_pcm_phase4_trn2u_line_handoff *handoff)
{
	struct qam_point reference = { .in_phase = TEST_LU };
	struct qam_equalizer equalizer;
	struct qam_point output = { 0 };
	struct v92_pcm_phase3_cpt_line_handoff cpt_handoff;
	struct v92_pcm_phase4_trn2u_line_rx training;
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
	cpt_handoff = (struct v92_pcm_phase3_cpt_line_handoff){
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
	CHECK(v92_pcm_phase4_trn2u_line_rx_init(
		      &training, &cpt_handoff, constellation) ==
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
	struct v92_upstream_control_line_rx *receiver,
	enum g711_law law, int8_t polarity, int16_t raw,
	struct v92_control_event *suvu_event,
	struct v92_control_event *cpu_event)
{
	struct v92_control_event event;
	enum v92_upstream_control_line_rx_result result =
		v92_upstream_control_line_rx_sample(
			receiver, line_sample(law, polarity, raw), &event);

	CHECK(result == V92_UPSTREAM_CONTROL_LINE_RX_OK ||
	      result == V92_UPSTREAM_CONTROL_LINE_RX_EVENT,
	      "control-line sample failed: %s",
	      v92_upstream_control_line_rx_result_name(result));
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
	uint8_t previous_sign, int8_t polarity)
{
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.request_silence = true,
		.wait_for_cpu = true,
	};
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_handoff handoff;
	struct v92_upstream_control_line_rx receiver;
	struct v92_phase4_suvu_tx suvu_tx;
	struct v92_phase4_cpu_tx cpu_tx;
	struct v92_phase4_modulator modulator;
	struct v92_control_event suvu_event = { 0 };
	struct v92_control_event cpu_event = { 0 };
	uint64_t starting_samples;
	size_t cpu_symbols = 0u;

	CHECK(build_boundary(
		      law, constellation, previous_sign, polarity,
		      &transmitter, &handoff) &&
	      v92_upstream_control_line_rx_init(&receiver, &handoff) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_OK &&
	      v92_phase4_suvu_tx_init(
		      &suvu_tx, &transmitter, false, &suvu) ==
		      V92_PHASE4_SUVU_TX_OK,
	      "control-line record path did not initialize");
	starting_samples = receiver.line.input_samples;
	while (!v92_phase4_suvu_tx_record_is_complete(&suvu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_suvu_tx_next(&suvu_tx, &raw) ==
			      V92_PHASE4_SUVU_TX_OK &&
		      receive_sample(
			      &receiver, law, polarity, raw,
			      &suvu_event, &cpu_event),
		      "control-line SUVu generation failed");
	}
	CHECK(suvu_event.type == V92_CONTROL_EVENT_NONE &&
	      receiver.training_tail_remaining == 0u,
	      "control line exposed SUVu before its delayed decisions");
	CHECK(v92_phase4_suvu_tx_handoff(&suvu_tx, &modulator) ==
		      V92_PHASE4_SUVU_TX_OK &&
	      v92_phase4_cpu_tx_init(&cpu_tx, &modulator, &cpu) ==
		      V92_PHASE4_CPU_TX_OK,
	      "control-line CPu continuation did not initialize");
	while (!v92_phase4_cpu_tx_record_is_complete(&cpu_tx)) {
		int16_t raw;

		CHECK(v92_phase4_cpu_tx_next(&cpu_tx, &raw) ==
			      V92_PHASE4_CPU_TX_OK &&
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
	CHECK(v92_phase4_cpu_tx_start_next_record(&cpu_tx, &cpu) ==
		      V92_PHASE4_CPU_TX_OK,
	      "control-line CPu flush record did not start");
	for (size_t symbol = 0;
	     symbol < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++symbol) {
		int16_t raw;

		CHECK(v92_phase4_cpu_tx_next(&cpu_tx, &raw) ==
			      V92_PHASE4_CPU_TX_OK &&
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
	      receiver.training_tail_remaining == 0u &&
	      receiver.line.input_samples > starting_samples &&
	      v92_upstream_control_line_rx_is_valid(&receiver) &&
	      !v92_upstream_control_line_rx_has_failed(&receiver),
	      "control-line records or retained state differed");
	return true;
}

static bool test_generated_lines(void)
{
	return run_records(
		       G711_LAW_PCMU, V92_CONSTELLATION_4_POINT, 0u, 1) &&
	       run_records(
		       G711_LAW_PCMA, V92_CONSTELLATION_8_POINT, 1u, -1);
}

static bool test_contracts(void)
{
	struct v92_pcm_phase4_trn2u transmitter;
	struct v92_pcm_phase4_trn2u_line_handoff handoff;
	struct v92_pcm_phase4_trn2u_line_handoff invalid;
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
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	invalid = handoff;
	invalid.training_symbols++;
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
	receiver.failed = true;
	saved = receiver;
	CHECK(v92_upstream_control_line_rx_has_failed(&receiver) &&
	      v92_upstream_control_line_rx_sample(&receiver, 0, &event) ==
		      V92_UPSTREAM_CONTROL_LINE_RX_FAILED &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "failed control line was not stable");
	receiver.failed = false;
	receiver.line.input_samples = UINT64_MAX;
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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated four- and eight-point records", test_generated_lines },
		{ "contracts", test_contracts },
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
