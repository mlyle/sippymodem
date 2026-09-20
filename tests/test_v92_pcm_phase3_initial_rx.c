// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_initial_rx.h>
#include <v92_pcm_phase3_initial_tx.h>

#include "channel_bench.h"
#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	SAMPLE_LIMIT = 16384,
};

static struct v92_pcm_info1a info1a(uint8_t md_units)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 192,
		.maximum_section_coefficients = 128,
		.local_md_length_276_symbol_units = md_units,
		.training_ucode = 103,
	};
}

static bool next_source_symbol(
	struct v92_pcm_phase3_initial_tx *transmitter,
	const struct v90_pcm_symbol *md_symbol,
	struct v90_pcm_symbol *symbol)
{
	const struct v90_pcm_symbol *md = NULL;

	if (transmitter->stage == V92_PCM_PHASE3_INITIAL_TX_OPENING &&
	    transmitter->sequence.opening.stage == V92_PCM_PHASE3_OPENING_MD)
		md = md_symbol;
	return v92_pcm_phase3_initial_tx_next(transmitter, md, symbol) ==
	       V92_PCM_PHASE3_INITIAL_TX_OK;
}

static bool check_line(
	enum g711_law law, uint8_t md_units, uint32_t rate_mask,
	const struct channel_bench_effect_spec *effects,
	size_t effect_count, double adaptation, int expected_polarity,
	uint32_t channel_delay)
{
	struct channel_bench channel = { 0 };
	struct v92_pcm_info1a selection = info1a(md_units);
	struct v92_pcm_phase3_initial_tx transmitter;
	struct v92_pcm_phase3_initial_rx receiver;
	struct v90_pcm_symbol md_symbol;
	struct v92_pcm_dil_parameters decoded = { 0 };
	unsigned int alignment_events = 0u;
	unsigned int training_events = 0u;
	unsigned int synchronization_events = 0u;
	bool detected = false;

	CHECK(channel_bench_init(&channel, effects, effect_count) ==
		      CHANNEL_BENCH_OK &&
	      v90_pcm_symbol_make(&md_symbol, law, 17u, false) &&
	      v92_pcm_phase3_initial_tx_init(
		      &transmitter, law, TEST_UCODE, &selection, rate_mask,
		      V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS, V90_DIL_PROFILE_NONE) ==
		      V92_PCM_PHASE3_INITIAL_TX_OK &&
	      v92_pcm_phase3_initial_rx_init(&receiver, adaptation) ==
		      V92_PCM_PHASE3_INITIAL_RX_OK,
	      "initial receive fixture did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT && !detected; ++index) {
		struct v90_pcm_symbol symbol;
		int16_t sample;
		enum v92_pcm_phase3_initial_rx_result result;

		CHECK(next_source_symbol(&transmitter, &md_symbol, &symbol),
		      "initial transmitter failed at %zu", index);
		sample = symbol.sample;
		CHECK(channel_bench_process(&channel, &sample, 1u) ==
			      CHANNEL_BENCH_OK,
		      "initial receive channel failed at %zu", index);
		result = v92_pcm_phase3_initial_rx_sample(&receiver, sample);
		CHECK(result == V92_PCM_PHASE3_INITIAL_RX_OK ||
		      result == V92_PCM_PHASE3_INITIAL_RX_ALIGNED ||
		      result == V92_PCM_PHASE3_INITIAL_RX_TRAINED ||
		      result == V92_PCM_PHASE3_INITIAL_RX_SYNCHRONIZED ||
		      result == V92_PCM_PHASE3_INITIAL_RX_REJECTED ||
		      result == V92_PCM_PHASE3_INITIAL_RX_DETECTED,
		      "initial receiver returned %s at %zu",
		      v92_pcm_phase3_initial_rx_result_name(result), index);
		alignment_events += result == V92_PCM_PHASE3_INITIAL_RX_ALIGNED;
		training_events += result == V92_PCM_PHASE3_INITIAL_RX_TRAINED;
		synchronization_events +=
			result == V92_PCM_PHASE3_INITIAL_RX_SYNCHRONIZED;
		detected = result == V92_PCM_PHASE3_INITIAL_RX_DETECTED;
	}
	CHECK(detected && alignment_events == 1u && training_events == 1u &&
	      synchronization_events == 1u &&
	      v92_pcm_phase3_initial_rx_is_complete(&receiver) &&
	      !v92_pcm_phase3_initial_rx_has_failed(&receiver) &&
	      v92_pcm_phase3_initial_rx_parameters(&receiver, &decoded) &&
	      decoded.upstream_rate_mask == rate_mask &&
	      decoded.downstream.segment_count == 0u &&
	      transmitter.stage == V92_PCM_PHASE3_INITIAL_TX_JA &&
	      receiver.sequence.ja.processed_samples ==
		      V92_PCM_PHASE3_JA_MIN_SYMBOLS &&
	      receiver.sequence.ja.line.polarity == expected_polarity &&
	      transmitter.sequence.ja.emitted_symbols ==
		      receiver.sequence.ja.processed_samples +
			      receiver.sequence.ja.input_advance_symbols +
			      channel_delay,
	      "initial receive differed: detected=%u events=%u/%u/%u "
	      "mask=%05x/%05x processed=%llu/%u polarity=%d/%d "
	      "source=%llu delay=%u",
	      detected, alignment_events, training_events,
	      synchronization_events, (unsigned int)decoded.upstream_rate_mask,
	      (unsigned int)rate_mask,
	      (unsigned long long)receiver.sequence.ja.processed_samples,
	      V92_PCM_PHASE3_JA_MIN_SYMBOLS,
	      receiver.sequence.ja.line.polarity, expected_polarity,
	      (unsigned long long)transmitter.sequence.ja.emitted_symbols,
	      channel_delay);
	{
		struct v92_pcm_phase3_initial_rx before = receiver;

		CHECK(v92_pcm_phase3_initial_rx_sample(&receiver, 0) ==
			      V92_PCM_PHASE3_INITIAL_RX_DONE &&
		      memcmp(&receiver, &before, sizeof(receiver)) == 0,
		      "complete initial receiver consumed another sample");
	}
	channel_bench_destroy(&channel);
	return true;
}

static bool test_exact_laws(void)
{
	static const struct channel_bench_effect_spec pcmu = {
		.kind = CHANNEL_BENCH_PCMU,
	};
	static const struct channel_bench_effect_spec pcma = {
		.kind = CHANNEL_BENCH_PCMA,
	};

	CHECK(check_line(
		      G711_LAW_PCMU, 0u, 0u, &pcmu, 1u, 0.0, 1, 0u),
	      "exact PCMU initial receive failed");
	CHECK(check_line(
		      G711_LAW_PCMA, 1u, V92_PCM_DIL_RATE_MASK_ALL,
		      &pcma, 1u, 0.0, 1, 0u),
	      "exact PCMA initial receive failed");
	return true;
}

static bool test_impaired_line(void)
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
				.peak = 48u,
				.seed = UINT32_C(0x2468ace0),
			},
		},
		{
			.kind = CHANNEL_BENCH_PCMA,
		},
	};

	CHECK(check_line(
		      G711_LAW_PCMA, 1u, UINT32_C(0x52491), effects,
		      ARRAY_LENGTH(effects), 0.125, -1, 7u),
	      "impaired PCMA initial receive failed");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_initial_rx receiver;
	struct v92_pcm_phase3_initial_rx before;
	struct v92_pcm_dil_parameters sentinel = {
		.upstream_rate_mask = UINT32_C(0xa5a5a5a5),
	};
	struct v92_pcm_dil_parameters parameters = sentinel;
	static const char *const stages[] = {
		"TRN1u", "Ja", "complete",
	};
	static const char *const results[] = {
		"ok", "aligned", "trained", "synchronized", "rejected",
		"detected", "done", "failed", "invalid argument",
		"internal error",
	};

	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(v92_pcm_phase3_initial_rx_init(NULL, 0.0) ==
		      V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_rx_init(&receiver, NAN) ==
		      V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_rx_init(&receiver, -0.1) ==
		      V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_initial_rx_init(&receiver, 1.1) ==
		      V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid initial receive initialization changed its destination");
	CHECK(v92_pcm_phase3_initial_rx_init(&receiver, 0.125) ==
		      V92_PCM_PHASE3_INITIAL_RX_OK &&
	      v92_pcm_phase3_initial_rx_is_valid(&receiver) &&
	      !v92_pcm_phase3_initial_rx_is_complete(&receiver) &&
	      !v92_pcm_phase3_initial_rx_has_failed(&receiver) &&
	      !v92_pcm_phase3_initial_rx_parameters(&receiver, &parameters) &&
	      memcmp(&parameters, &sentinel, sizeof(parameters)) == 0,
	      "initial receive contract instance did not initialize");
	before = receiver;
	CHECK(v92_pcm_phase3_initial_rx_sample(NULL, 0) ==
		      V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid initial receive call changed state");
	receiver.sequence.trn1u.window_count =
		V92_PCM_PHASE3_TRN1U_LINE_SYNC_SYMBOLS + 1u;
	before = receiver;
	CHECK(!v92_pcm_phase3_initial_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_initial_rx_sample(&receiver, 0) ==
		      V92_PCM_PHASE3_INITIAL_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      !v92_pcm_phase3_initial_rx_is_complete(NULL) &&
	      !v92_pcm_phase3_initial_rx_has_failed(NULL),
	      "corrupt initial receive state was not rejected");
	for (size_t index = 0; index < ARRAY_LENGTH(stages); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_initial_rx_stage_name(
				      (enum v92_pcm_phase3_initial_rx_stage)index),
			      stages[index]) == 0,
		      "initial receive stage name %zu differed", index);
	for (size_t index = 0; index < ARRAY_LENGTH(results); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_initial_rx_result_name(
				      (enum v92_pcm_phase3_initial_rx_result)index),
			      results[index]) == 0,
		      "initial receive result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_initial_rx_stage_name(
			      (enum v92_pcm_phase3_initial_rx_stage)-1),
		      "unknown") == 0 &&
	      strcmp(
		      v92_pcm_phase3_initial_rx_result_name(
			      (enum v92_pcm_phase3_initial_rx_result)-1),
		      "unknown") == 0,
	      "unknown initial receive names differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact PCMU and PCMA streams", test_exact_laws },
		{ "impaired stream", test_impaired_line },
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
