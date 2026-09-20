// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_trn1u_line_rx.h>

#include "channel_bench.h"
#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	TEST_LIMIT = 4096,
};

static uint8_t symbol_sign(const struct v90_pcm_symbol *symbol)
{
	return symbol->positive ? 0u : 1u;
}

static bool check_channel(
	enum g711_law law,
	const struct channel_bench_effect_spec *effects,
	size_t effect_count, double adaptation, int expected_polarity)
{
	struct channel_bench bench = { 0 };
	struct v92_pcm_phase3_trn1u source;
	struct v92_pcm_phase3_trn1u_line_rx receiver;
	struct v92_pcm_phase3_trn1u_line_handoff handoff;
	uint32_t source_symbols = 0u;
	uint32_t alignment_events = 0u;
	uint8_t expected_previous_sign = 2u;
	uint32_t expected_history = 0u;
	bool trained = false;

	CHECK(channel_bench_init(&bench, effects, effect_count) ==
		      CHANNEL_BENCH_OK &&
	      v92_pcm_phase3_trn1u_init(&source, law, TEST_UCODE) ==
		      V92_PCM_PHASE3_TRN1U_OK &&
	      v92_pcm_phase3_trn1u_line_rx_init(&receiver, adaptation) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_OK,
	      "TRN1u line fixture did not initialize");
	for (size_t index = 0; index < TEST_LIMIT; ++index) {
		struct v90_pcm_symbol symbol;
		int16_t sample;
		enum v92_pcm_phase3_trn1u_line_rx_result result;

		CHECK(v92_pcm_phase3_trn1u_next(&source, &symbol) ==
			      V92_PCM_PHASE3_TRN1U_OK,
		      "TRN1u source failed at %zu", index);
		source_symbols++;
		if (source_symbols == V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS) {
			expected_previous_sign = symbol_sign(&symbol);
			expected_history = source.scrambler.history;
		}
		sample = symbol.sample;
		CHECK(channel_bench_process(&bench, &sample, 1u) ==
			      CHANNEL_BENCH_OK,
		      "TRN1u channel failed at %zu", index);
		result = v92_pcm_phase3_trn1u_line_rx_sample(
			&receiver, sample);
		alignment_events +=
			result == V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED;
		CHECK(result == V92_PCM_PHASE3_TRN1U_LINE_RX_OK ||
		      result == V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED ||
		      result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED,
		      "TRN1u line receiver returned %s at %zu",
		      v92_pcm_phase3_trn1u_line_rx_result_name(result), index);
		if (result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED) {
			trained = true;
			break;
		}
	}
	CHECK(trained && alignment_events == 1u &&
	      v92_pcm_phase3_trn1u_line_rx_is_complete(&receiver) &&
	      receiver.output_symbols == V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS &&
	      receiver.training_errors < 64u &&
	      receiver.polarity == expected_polarity,
	      "TRN1u line receiver did not acquire law %u: trained=%u "
	      "alignments=%u stage=%u outputs=%u errors=%u polarity=%d",
	      (unsigned int)law, trained, alignment_events,
	      (unsigned int)receiver.stage, receiver.output_symbols,
	      receiver.training_errors, receiver.polarity);
	CHECK(v92_pcm_phase3_trn1u_line_rx_handoff(
		      &receiver, &handoff) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_OK &&
	      v92_pcm_phase3_trn1u_line_handoff_is_valid(&handoff) &&
	      handoff.input_advance_symbols ==
		      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	      handoff.input_samples == source_symbols &&
	      handoff.sync.training.previous_sign == expected_previous_sign &&
	      handoff.sync.training.scrambler.history == expected_history,
	      "TRN1u line handoff lost coding or equalizer delay");
	{
		struct v92_pcm_phase3_trn1u_line_rx before = receiver;

		CHECK(v92_pcm_phase3_trn1u_line_rx_sample(&receiver, 0) ==
			      V92_PCM_PHASE3_TRN1U_LINE_RX_DONE &&
		      memcmp(&receiver, &before, sizeof(receiver)) == 0,
		      "complete TRN1u line receiver consumed another sample");
	}
	channel_bench_destroy(&bench);
	return true;
}

static bool test_exact_laws(void)
{
	CHECK(check_channel(G711_LAW_PCMU, NULL, 0u, 0.0, 1),
	      "exact PCMU TRN1u failed");
	CHECK(check_channel(G711_LAW_PCMA, NULL, 0u, 0.0, 1),
	      "exact PCMA TRN1u failed");
	return true;
}

static bool test_impaired_channel(void)
{
	static const struct channel_bench_effect_spec effects[] = {
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
				.peak = 64u,
				.seed = UINT32_C(0x12345678),
			},
		},
		{
			.kind = CHANNEL_BENCH_PCMA,
		},
	};

	CHECK(check_channel(
		      G711_LAW_PCMA, effects, ARRAY_LENGTH(effects),
		      0.125, -1),
	      "impaired PCMA TRN1u failed");
	return true;
}

static bool test_weak_signal_rejection(void)
{
	struct v92_pcm_phase3_trn1u source;
	struct v92_pcm_phase3_trn1u_line_rx receiver;

	CHECK(v92_pcm_phase3_trn1u_init(
		      &source, G711_LAW_PCMU, TEST_UCODE) ==
		      V92_PCM_PHASE3_TRN1U_OK &&
	      v92_pcm_phase3_trn1u_line_rx_init(&receiver, 0.0) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_OK,
	      "weak TRN1u fixture did not initialize");
	for (size_t index = 0; index < 256u; ++index) {
		struct v90_pcm_symbol symbol;
		int16_t weak;

		CHECK(v92_pcm_phase3_trn1u_next(&source, &symbol) ==
			      V92_PCM_PHASE3_TRN1U_OK,
		      "weak TRN1u source failed");
		weak = (int16_t)(symbol.sample / 64);
		CHECK(v92_pcm_phase3_trn1u_line_rx_sample(
			      &receiver, weak) ==
			      V92_PCM_PHASE3_TRN1U_LINE_RX_OK,
		      "weak TRN1u sample failed");
	}
	CHECK(receiver.stage == V92_PCM_PHASE3_TRN1U_LINE_RX_STAGE_HUNTING &&
	      receiver.output_symbols == 0u,
	      "weak TRN1u falsely acquired");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_trn1u_line_rx receiver;
	struct v92_pcm_phase3_trn1u_line_rx before;
	struct v92_pcm_phase3_trn1u_line_handoff handoff;
	struct v92_pcm_phase3_trn1u_line_handoff before_handoff;
	static const char *const names[] = {
		"ok", "aligned", "trained", "done", "failed",
		"invalid argument", "invalid state", "numeric failure",
	};

	CHECK(v92_pcm_phase3_trn1u_line_rx_init(NULL, 0.0) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_line_rx_init(&receiver, NAN) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_line_rx_init(&receiver, -0.1) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_line_rx_init(&receiver, 1.1) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_trn1u_line_rx_init(&receiver, 0.125) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_OK &&
	      v92_pcm_phase3_trn1u_line_rx_is_valid(&receiver) &&
	      !v92_pcm_phase3_trn1u_line_rx_is_complete(&receiver),
	      "TRN1u line initialization contracts failed");
	for (unsigned int fault = 0u; fault < 4u; ++fault) {
		struct v92_pcm_phase3_trn1u_line_rx invalid = receiver;
		size_t index = fault % 2u == 0u ? 0u :
			V92_PCM_PHASE3_TRN1U_LINE_EQUALIZER_TAPS - 1u;

		invalid.expected[index] = fault < 2u ? 2u : UINT8_MAX;
		before = invalid;
		CHECK(!v92_pcm_phase3_trn1u_line_rx_is_valid(&invalid) &&
		      v92_pcm_phase3_trn1u_line_rx_sample(&invalid, 0) ==
			      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&invalid, &before, sizeof(invalid)) == 0,
		      "nonbinary TRN1u reference at %zu was not rejected "
		      "transactionally (fault %u)", index, fault);
	}
	memset(&handoff, 0xa5, sizeof(handoff));
	before_handoff = handoff;
	CHECK(v92_pcm_phase3_trn1u_line_rx_handoff(
		      &receiver, &handoff) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_STATE &&
	      memcmp(&handoff, &before_handoff, sizeof(handoff)) == 0 &&
	      !v92_pcm_phase3_trn1u_line_handoff_is_valid(NULL),
	      "premature TRN1u handoff changed output");
	receiver.window_count =
		V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS + 1u;
	before = receiver;
	CHECK(!v92_pcm_phase3_trn1u_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_trn1u_line_rx_sample(&receiver, 0) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "corrupt TRN1u line state was not rejected transactionally");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_trn1u_line_rx_result_name(
				      (enum v92_pcm_phase3_trn1u_line_rx_result)
					      index),
			      names[index]) == 0,
		      "TRN1u line result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_trn1u_line_rx_result_name(
			      (enum v92_pcm_phase3_trn1u_line_rx_result)-1),
		      "unknown") == 0,
	      "unknown TRN1u line result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact PCMU and PCMA lines", test_exact_laws },
		{ "impaired line", test_impaired_channel },
		{ "weak signal rejection", test_weak_signal_rejection },
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
