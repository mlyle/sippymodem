// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_cpt_line_rx.h>
#include <v92_pcm_phase3_cpt_tx.h>

#include "binary_state_fixture.h"
#include "channel_bench.h"
#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	FINISH_BIT = 37,
	SAMPLE_LIMIT = 12000,
};

static bool receiver_states_equal(
	const struct v92_pcm_phase3_cpt_line_rx *left,
	const struct v92_pcm_phase3_cpt_line_rx *right)
{
	struct v92_pcm_phase3_cpt_line_rx left_snapshot, right_snapshot;

	memcpy(&left_snapshot, left, sizeof(left_snapshot));
	memcpy(&right_snapshot, right, sizeof(right_snapshot));
	binary_equalizer_snapshot(&left_snapshot.line, &left->line);
	binary_equalizer_snapshot(&right_snapshot.line, &right->line);
	return memcmp(&left_snapshot, &right_snapshot, sizeof(left_snapshot)) == 0;
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters make_parameters(enum g711_law law)
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
	const struct v92_cpt_parameters *first,
	const struct v92_cpt_parameters *second)
{
	uint8_t first_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	uint8_t second_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t first_bits = 0u;
	size_t second_bits = 0u;

	return v92_cpt_encode(
		       first, first_frame, sizeof(first_frame), &first_bits) ==
		       V92_CPT_OK &&
	       v92_cpt_encode(
		       second, second_frame, sizeof(second_frame),
		       &second_bits) == V92_CPT_OK &&
	       first_bits == second_bits &&
	       memcmp(first_frame, second_frame, sizeof(first_frame)) == 0;
}

static bool start_tail(
	struct pcm_binary_sign *tail,
	const struct v92_pcm_phase3_cpt_tx *transmitter)
{
	struct v34_scrambler reset;

	return v34_scrambler_init(&reset, V34_POLYNOMIAL_GPA) ==
		       V34_FRAMING_OK &&
	       pcm_binary_sign_init_from_scrambler(
		       tail, &reset, transmitter->coding.previous_sign) ==
		       PCM_BINARY_SIGN_OK;
}

static bool next_tail_symbol(
	struct pcm_binary_sign *tail, enum g711_law law,
	struct v90_pcm_symbol *symbol)
{
	uint8_t sign;

	return pcm_binary_sign_encode(tail, 1u, &sign) ==
		       PCM_BINARY_SIGN_OK &&
	       v90_pcm_symbol_make(symbol, law, TEST_UCODE, sign == 0u);
}

static bool check_line(
	enum g711_law law, bool impaired, unsigned damaged_descriptors)
{
	struct channel_bench_effect_spec effects[5] = {
		{
			.kind = CHANNEL_BENCH_DELAY,
			.parameters.delay.samples = 7u,
		},
		{
			.kind = CHANNEL_BENCH_GAIN,
			.parameters.gain.scale_q15 = -24576,
		},
		{
			.kind = CHANNEL_BENCH_ECHO,
			.parameters.echo = {
				.delay_samples = 2u,
				.gain_q15 = 8192,
			},
		},
		{
			.kind = CHANNEL_BENCH_NOISE,
			.parameters.noise = {
				.peak = 48u,
				.seed = UINT32_C(0x13579bdf),
			},
		},
		{
			.kind = law == G711_LAW_PCMA ?
					CHANNEL_BENCH_PCMA : CHANNEL_BENCH_PCMU,
		},
	};
	struct channel_bench channel = { 0 };
	struct v92_cpt_parameters parameters = make_parameters(law);
	struct v92_pcm_phase3_cpt_tx transmitter;
	struct v92_pcm_phase3_trn1u_line_rx training;
	struct v92_pcm_phase3_cpt_line_rx receiver;
	struct pcm_binary_sign tail;
	size_t frame_bits = v92_cpt_frame_bits(&parameters);
	uint64_t tail_symbols = 0u;
	bool receiver_started = false;
	bool tail_started = false;
	bool parameters_seen = false;
	bool repeat_seen = false;
	bool e1u_seen = false;
	unsigned rejected_records = 0u;

	CHECK(channel_bench_init(
		      &channel, impaired ? effects : &effects[4],
		      impaired ? ARRAY_LENGTH(effects) : 1u) == CHANNEL_BENCH_OK &&
	      v92_pcm_phase3_cpt_tx_init(
		      &transmitter, law, TEST_UCODE, &parameters) ==
		      V92_PCM_PHASE3_CPT_TX_OK &&
	      v92_pcm_phase3_trn1u_line_rx_init(
		      &training, impaired ? 0.125 : 0.0) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_OK,
	      "CPt line fixture did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT && !e1u_seen; ++index) {
		struct v90_pcm_symbol symbol;
		int16_t sample;

		if (transmitter.stage == V92_PCM_PHASE3_CPT_TX_TRN1U &&
		    transmitter.trn1u.emitted_symbols ==
			    V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS)
			CHECK(v92_pcm_phase3_cpt_tx_start(&transmitter) ==
				      V92_PCM_PHASE3_CPT_TX_OK,
			      "CPt line transmitter did not start");
		if (transmitter.stage == V92_PCM_PHASE3_CPT_TX_RECORD &&
		    transmitter.completed_records == 1u + damaged_descriptors &&
		    transmitter.frame_bit_index == FINISH_BIT &&
		    !transmitter.finish_requested)
			CHECK(v92_pcm_phase3_cpt_tx_request_finish(
				      &transmitter) ==
				      V92_PCM_PHASE3_CPT_TX_OK,
			      "CPt line transmitter did not request finish");
		if (!v92_pcm_phase3_cpt_tx_is_complete(&transmitter)) {
			CHECK(v92_pcm_phase3_cpt_tx_next(
				      &transmitter, &symbol) ==
				      V92_PCM_PHASE3_CPT_TX_OK,
			      "CPt line transmitter failed at %zu", index);
		} else {
			if (!tail_started) {
				CHECK(start_tail(&tail, &transmitter),
				      "TRN2u tail did not initialize");
				tail_started = true;
			}
			CHECK(next_tail_symbol(&tail, law, &symbol),
			      "TRN2u tail failed at %zu", index);
			tail_symbols++;
		}
		sample = symbol.sample;
		if (damaged_descriptors != 0u &&
		    index >= V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
		    index < V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS + V92_PHASE3_MARKER_BITS)
			sample = 0;
		if (damaged_descriptors == 2u &&
		    index == V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS +
			V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS + frame_bits + 80u)
			sample = -sample;
		CHECK(channel_bench_process(&channel, &sample, 1u) ==
			      CHANNEL_BENCH_OK,
		      "CPt line channel failed at %zu", index);
		if (!receiver_started) {
			enum v92_pcm_phase3_trn1u_line_rx_result result =
				v92_pcm_phase3_trn1u_line_rx_sample(
					&training, sample);

			CHECK(result == V92_PCM_PHASE3_TRN1U_LINE_RX_OK ||
			      result == V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED ||
			      result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED,
			      "CPt training line returned %s at %zu",
			      v92_pcm_phase3_trn1u_line_rx_result_name(result),
			      index);
			if (result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED) {
				struct v92_pcm_phase3_trn1u_line_handoff handoff;

				CHECK(v92_pcm_phase3_trn1u_line_rx_handoff(
					      &training, &handoff) ==
					      V92_PCM_PHASE3_TRN1U_LINE_RX_OK &&
				      v92_pcm_phase3_cpt_line_rx_init(
					      &receiver, &handoff) ==
					      V92_PCM_PHASE3_CPT_LINE_RX_OK,
				      "CPt line handoff failed");
				receiver_started = true;
			}
			continue;
		}
		{
			struct v92_pcm_phase3_cpt_rx_event event;
			enum v92_pcm_phase3_cpt_line_rx_result result =
				v92_pcm_phase3_cpt_line_rx_sample(
					&receiver, sample, &event);

			CHECK(result == V92_PCM_PHASE3_CPT_LINE_RX_SIGN ||
			      result == V92_PCM_PHASE3_CPT_LINE_RX_EVENT,
			      "CPt line receiver returned %s at %zu",
			      v92_pcm_phase3_cpt_line_rx_result_name(result), index);
			if (result == V92_PCM_PHASE3_CPT_LINE_RX_SIGN)
				continue;
			if (event.type == V92_PCM_PHASE3_CPT_RX_PARAMETERS) {
				CHECK(!parameters_seen &&
				      parameters_match(
					      &event.parameters, &parameters),
				      "CPt line parameters changed");
				parameters_seen = true;
			} else if (event.type == V92_PCM_PHASE3_CPT_RX_REPEAT) {
				CHECK(parameters_seen && !repeat_seen,
				      "CPt line repeat order changed");
				repeat_seen = true;
			} else if (event.type == V92_PCM_PHASE3_CPT_RX_E1U) {
				CHECK(repeat_seen, "CPt line E1u arrived early");
				e1u_seen = true;
			} else if (event.type == V92_PCM_PHASE3_CPT_RX_REJECTED) {
				CHECK(damaged_descriptors == 2u && !parameters_seen,
				      "CPt line rejected a valid record");
				rejected_records++;
			} else {
				CHECK(false, "CPt line emitted event %u",
				      (unsigned int)event.type);
			}
		}
	}
	CHECK(receiver_started && parameters_seen && repeat_seen && e1u_seen &&
	      rejected_records == (damaged_descriptors == 2u) &&
	      v92_pcm_phase3_cpt_line_rx_is_complete(&receiver) &&
	      !v92_pcm_phase3_cpt_line_rx_has_failed(&receiver) &&
	      v92_pcm_phase3_cpt_tx_is_complete(&transmitter) &&
	      receiver.processed_samples ==
		      V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS +
		      (2u + damaged_descriptors) * frame_bits +
			      V92_CPT_SYMBOLS_PER_FRAME &&
	      receiver.input_advance_symbols ==
		      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	      receiver.line.polarity == (impaired ? -1 : 1) &&
	      tail_symbols ==
		      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY +
			      (impaired ? 7u : 0u),
	      "CPt line pipeline differed: started=%u parameters=%u repeat=%u "
	      "e1u=%u complete=%u failed=%u tx_complete=%u processed=%llu/%zu "
	      "advance=%u polarity=%d/%d tail=%llu/%u",
	      receiver_started, parameters_seen, repeat_seen, e1u_seen,
	      v92_pcm_phase3_cpt_line_rx_is_complete(&receiver),
	      v92_pcm_phase3_cpt_line_rx_has_failed(&receiver),
	      v92_pcm_phase3_cpt_tx_is_complete(&transmitter),
	      (unsigned long long)receiver.processed_samples,
	      V92_PCM_PHASE3_CPT_PREAMBLE_SYMBOLS +
		      (2u + damaged_descriptors) * frame_bits +
		      V92_CPT_SYMBOLS_PER_FRAME,
	      receiver.input_advance_symbols, receiver.line.polarity,
	      impaired ? -1 : 1, (unsigned long long)tail_symbols,
	      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY +
		      (impaired ? 7u : 0u));
	{
		struct v92_cpt_parameters received;
		struct v92_pcm_phase3_cpt_line_handoff handoff;
		struct v92_pcm_phase3_cpt_line_handoff invalid;
		uint8_t previous_sign = 2u;
		struct v92_pcm_phase3_cpt_line_rx before = receiver;
		struct v92_pcm_phase3_cpt_rx_event event;
		struct v92_pcm_phase3_cpt_rx_event before_event;

		memset(&event, 0xa5, sizeof(event));
		before_event = event;
		CHECK(v92_pcm_phase3_cpt_line_rx_parameters(
			      &receiver, &received) &&
		      parameters_match(&received, &parameters) &&
		      v92_pcm_phase3_cpt_line_rx_previous_sign(
			      &receiver, &previous_sign) &&
		      previous_sign == transmitter.coding.previous_sign &&
		      v92_pcm_phase3_cpt_line_rx_sample(
			      &receiver, 0, &event) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_DONE &&
		      receiver_states_equal(&receiver, &before) &&
		      memcmp(&event, &before_event, sizeof(event)) == 0,
		      "complete CPt line receiver was not stable");
		CHECK(v92_pcm_phase3_cpt_line_rx_handoff(
			      &receiver, &handoff) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_OK &&
		      v92_pcm_phase3_cpt_line_handoff_is_valid(&handoff) &&
		      memcmp(
			      &handoff.equalizer, &receiver.line.equalizer,
			      sizeof(handoff.equalizer)) == 0 &&
		      parameters_match(&handoff.parameters, &parameters) &&
		      handoff.input_samples == receiver.input_samples &&
		      handoff.signal_level == receiver.line.magnitude &&
		      handoff.adaptation == receiver.line.adaptation &&
		      handoff.input_advance_symbols ==
			      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
		      handoff.previous_sign == transmitter.coding.previous_sign &&
		      handoff.polarity == receiver.line.polarity,
		      "CPt line handoff did not preserve the E1u boundary");
		invalid = handoff;
		invalid.previous_sign = 2u;
		CHECK(!v92_pcm_phase3_cpt_line_handoff_is_valid(NULL) &&
		      !v92_pcm_phase3_cpt_line_handoff_is_valid(&invalid),
		      "invalid CPt line handoff was accepted");
	}
	channel_bench_destroy(&channel);
	return true;
}

static bool make_handoff(
	struct v92_pcm_phase3_trn1u_line_handoff *handoff)
{
	struct v92_pcm_phase3_trn1u source;
	struct v92_pcm_phase3_trn1u_line_rx receiver;

	if (v92_pcm_phase3_trn1u_init(
		    &source, G711_LAW_PCMU, TEST_UCODE) !=
		    V92_PCM_PHASE3_TRN1U_OK ||
	    v92_pcm_phase3_trn1u_line_rx_init(&receiver, 0.0) !=
		    V92_PCM_PHASE3_TRN1U_LINE_RX_OK)
		return false;
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v90_pcm_symbol symbol;
		enum v92_pcm_phase3_trn1u_line_rx_result result;

		if (v92_pcm_phase3_trn1u_next(&source, &symbol) !=
			    V92_PCM_PHASE3_TRN1U_OK)
			return false;
		result = v92_pcm_phase3_trn1u_line_rx_sample(
			&receiver, symbol.sample);
		if (result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED)
			return v92_pcm_phase3_trn1u_line_rx_handoff(
				       &receiver, handoff) ==
			       V92_PCM_PHASE3_TRN1U_LINE_RX_OK;
		if (result != V92_PCM_PHASE3_TRN1U_LINE_RX_OK &&
		    result != V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED)
			return false;
	}
	return false;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_trn1u_line_handoff handoff;
	struct v92_pcm_phase3_trn1u_line_handoff invalid;
	struct v92_pcm_phase3_cpt_line_rx receiver;
	struct v92_pcm_phase3_cpt_line_rx before;
	struct v92_pcm_phase3_cpt_rx_event event;
	struct v92_pcm_phase3_cpt_rx_event before_event;
	struct v92_pcm_phase3_cpt_line_handoff output;
	struct v92_pcm_phase3_cpt_line_handoff before_output;
	static const char *const names[] = {
		"ok", "sign", "event", "done", "failed",
		"invalid argument", "invalid state", "numeric failure",
	};

	CHECK(make_handoff(&handoff), "CPt line contract handoff failed");
	invalid = handoff;
	invalid.input_advance_symbols = 0u;
	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(v92_pcm_phase3_cpt_line_rx_init(NULL, &handoff) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_line_rx_init(&receiver, NULL) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_line_rx_init(&receiver, &invalid) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      v92_pcm_phase3_cpt_line_rx_init(&receiver, &handoff) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_OK &&
	      v92_pcm_phase3_cpt_line_rx_is_valid(&receiver) &&
	      !v92_pcm_phase3_cpt_line_rx_is_complete(&receiver) &&
	      !v92_pcm_phase3_cpt_line_rx_has_failed(&receiver),
	      "CPt line initialization contracts failed");
	memset(&output, 0xa5, sizeof(output));
	before_output = output;
	CHECK(v92_pcm_phase3_cpt_line_rx_handoff(NULL, &output) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_line_rx_handoff(&receiver, NULL) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_line_rx_handoff(&receiver, &output) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_STATE &&
	      memcmp(&output, &before_output, sizeof(output)) == 0,
	      "rejected CPt line handoff changed its output");
	receiver.failed = true;
	CHECK(v92_pcm_phase3_cpt_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_cpt_line_rx_handoff(&receiver, &output) ==
		      V92_PCM_PHASE3_CPT_LINE_RX_FAILED &&
	      memcmp(&output, &before_output, sizeof(output)) == 0,
	      "failed CPt line receiver produced a handoff");
	receiver.failed = false;
	memset(&event, 0xa5, sizeof(event));
	before_event = event;
	before = receiver;
	CHECK(v92_pcm_phase3_cpt_line_rx_sample(NULL, 0, &event) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_line_rx_sample(&receiver, 0, NULL) ==
			      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_ARGUMENT &&
	      receiver_states_equal(&receiver, &before) &&
	      memcmp(&event, &before_event, sizeof(event)) == 0,
	      "rejected CPt line sample changed state");
	receiver.input_samples = UINT64_MAX;
	receiver.processed_samples =
		UINT64_MAX - receiver.starting_input_samples;
	before = receiver;
	CHECK(v92_pcm_phase3_cpt_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_cpt_line_rx_sample(&receiver, 0, &event) ==
		      V92_PCM_PHASE3_CPT_LINE_RX_INVALID_STATE &&
	      receiver_states_equal(&receiver, &before),
	      "overflowing CPt line sample changed state");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_cpt_line_rx_result_name(
				      (enum v92_pcm_phase3_cpt_line_rx_result)
					      index),
			      names[index]) == 0,
		      "CPt line result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_cpt_line_rx_result_name(
			      (enum v92_pcm_phase3_cpt_line_rx_result)-1),
		      "unknown") == 0,
	      "unknown CPt line result name differed");
	return true;
}

static bool test_exact_pcmu(void)
{
	return check_line(G711_LAW_PCMU, false, 0u);
}

static bool test_exact_pcma(void)
{
	return check_line(G711_LAW_PCMA, false, 0u);
}

static bool test_impaired_pcma(void)
{
	return check_line(G711_LAW_PCMA, true, 0u);
}

static bool test_lost_preamble(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned damaged = 1u; damaged <= 2u; ++damaged)
			CHECK(check_line(law, false, damaged),
			      "CPt did not recover after %u damaged descriptors", damaged);
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact PCMU line", test_exact_pcmu },
		{ "exact PCMA line", test_exact_pcma },
		{ "impaired PCMA line", test_impaired_pcma },
		{ "lost preamble, CRC and E1u", test_lost_preamble },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
