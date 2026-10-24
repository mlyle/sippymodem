// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_ja.h>
#include <v92_pcm_phase3_ja_line_rx.h>

#include "binary_state_fixture.h"
#include "channel_bench.h"
#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_UCODE = 91,
	SAMPLE_LIMIT = 8192,
};

static bool receiver_states_equal(
	const struct v92_pcm_phase3_ja_line_rx *left,
	const struct v92_pcm_phase3_ja_line_rx *right)
{
	struct v92_pcm_phase3_ja_line_rx left_snapshot, right_snapshot;

	memcpy(&left_snapshot, left, sizeof(left_snapshot));
	memcpy(&right_snapshot, right, sizeof(right_snapshot));
	binary_equalizer_snapshot(&left_snapshot.line, &left->line);
	binary_equalizer_snapshot(&right_snapshot.line, &right->line);
	return memcmp(&left_snapshot, &right_snapshot, sizeof(left_snapshot)) == 0;
}

static bool next_source_symbol(
	struct v92_pcm_phase3_trn1u *trn1u,
	struct v92_pcm_phase3_ja_tx *ja, bool *ja_started,
	uint32_t rate_mask, struct v90_pcm_symbol *symbol)
{
	if (!*ja_started &&
	    trn1u->emitted_symbols == V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS) {
		if (v92_pcm_phase3_ja_tx_init(ja, trn1u, rate_mask) !=
		    V92_PCM_PHASE3_JA_OK)
			return false;
		*ja_started = true;
	}
	if (*ja_started)
		return v92_pcm_phase3_ja_tx_next(ja, symbol) ==
		       V92_PCM_PHASE3_JA_OK;
	return v92_pcm_phase3_trn1u_next(trn1u, symbol) ==
	       V92_PCM_PHASE3_TRN1U_OK;
}

static bool check_line(
	enum g711_law law, uint32_t rate_mask,
	const struct channel_bench_effect_spec *effects,
	size_t effect_count, double adaptation, int expected_polarity,
	uint32_t channel_delay, unsigned damaged_descriptors)
{
	struct channel_bench channel = { 0 };
	struct v92_pcm_phase3_trn1u trn1u;
	struct v92_pcm_phase3_ja_tx ja;
	struct v92_pcm_phase3_trn1u_line_rx training;
	struct v92_pcm_phase3_ja_line_rx receiver;
	uint64_t ja_symbols = 0u, processed_samples = 0u;
	struct v92_pcm_dil_parameters decoded = { 0 };
	unsigned int synchronization_events = 0u;
	bool ja_started = false;
	bool receiver_started = false;
	bool detected = false;

	CHECK(channel_bench_init(&channel, effects, effect_count) ==
		      CHANNEL_BENCH_OK &&
	      v92_pcm_phase3_trn1u_init(&trn1u, law, TEST_UCODE) ==
		      V92_PCM_PHASE3_TRN1U_OK &&
	      v92_pcm_phase3_trn1u_line_rx_init(&training, adaptation) ==
		      V92_PCM_PHASE3_TRN1U_LINE_RX_OK,
	      "Ja line fixture did not initialize");
	for (size_t index = 0; index < SAMPLE_LIMIT && !detected; ++index) {
		struct v90_pcm_symbol symbol;
		int16_t sample;

		CHECK(next_source_symbol(
			      &trn1u, &ja, &ja_started, rate_mask, &symbol),
		      "Ja line source failed at %zu", index);
		ja_symbols += ja_started;
		sample = symbol.sample;
		/* Lose the one-time preamble and the first descriptor's sync.
		 * Later descriptors carry only the ordinary frame sync. */
		if (damaged_descriptors != 0u && ja_started &&
		    ja_symbols <= V92_PHASE3_MARKER_BITS)
			sample = 0;
		if (damaged_descriptors == 2u &&
		    ja_symbols == V92_PCM_PHASE3_JA_MIN_SYMBOLS + 81u)
			sample = -sample;
		CHECK(channel_bench_process(&channel, &sample, 1u) ==
			      CHANNEL_BENCH_OK,
		      "Ja line channel failed at %zu", index);
		if (!receiver_started) {
			enum v92_pcm_phase3_trn1u_line_rx_result result =
				v92_pcm_phase3_trn1u_line_rx_sample(
					&training, sample);

			CHECK(result == V92_PCM_PHASE3_TRN1U_LINE_RX_OK ||
			      result == V92_PCM_PHASE3_TRN1U_LINE_RX_ALIGNED ||
			      result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED,
			      "Ja training line returned %s at %zu",
			      v92_pcm_phase3_trn1u_line_rx_result_name(result),
			      index);
			if (result == V92_PCM_PHASE3_TRN1U_LINE_RX_TRAINED) {
				struct v92_pcm_phase3_trn1u_line_handoff handoff;

				CHECK(v92_pcm_phase3_trn1u_line_rx_handoff(
					      &training, &handoff) ==
					      V92_PCM_PHASE3_TRN1U_LINE_RX_OK &&
				      v92_pcm_phase3_ja_line_rx_init(
					      &receiver, &handoff) ==
					      V92_PCM_PHASE3_JA_LINE_RX_OK,
				      "Ja line handoff failed");
				receiver_started = true;
			}
			continue;
		}
		{
			enum v92_pcm_phase3_ja_line_rx_result result =
				v92_pcm_phase3_ja_line_rx_sample(
					&receiver, sample);

			CHECK(result == V92_PCM_PHASE3_JA_LINE_RX_OK ||
			      result == V92_PCM_PHASE3_JA_LINE_RX_SYNCHRONIZED ||
			      result == V92_PCM_PHASE3_JA_LINE_RX_REJECTED ||
			      result == V92_PCM_PHASE3_JA_LINE_RX_DETECTED,
			      "Ja line receiver returned %s at %zu",
			      v92_pcm_phase3_ja_line_rx_result_name(result), index);
			++processed_samples;
			CHECK(receiver.input_samples == receiver.starting_input_samples +
				      processed_samples,
			      "Ja line input clock did not advance once");
			synchronization_events +=
				result == V92_PCM_PHASE3_JA_LINE_RX_SYNCHRONIZED;
			detected = result == V92_PCM_PHASE3_JA_LINE_RX_DETECTED;
		}
	}
	CHECK(receiver_started && detected && synchronization_events == 1u &&
	      v92_pcm_phase3_ja_line_rx_is_complete(&receiver) &&
	      !v92_pcm_phase3_ja_line_rx_has_failed(&receiver) &&
	      v92_pcm_phase3_ja_line_rx_parameters(&receiver, &decoded) &&
	      decoded.upstream_rate_mask == rate_mask &&
	      decoded.downstream.segment_count == 0u &&
	      processed_samples == V92_PCM_PHASE3_JA_MIN_SYMBOLS +
		      damaged_descriptors * V92_PCM_DIL_ZERO_DESCRIPTOR_BITS &&
	      receiver.input_advance_symbols ==
		      V92_PCM_PHASE3_TRN1U_LINE_DECISION_DELAY &&
	      receiver.line.polarity == expected_polarity &&
	      ja_symbols == processed_samples +
			    receiver.input_advance_symbols + channel_delay,
	      "Ja line differed: started=%u detected=%u sync=%u mask=%05x/%05x "
	      "processed=%llu/%u polarity=%d/%d source=%llu delay=%u",
	      receiver_started, detected, synchronization_events,
	      (unsigned int)decoded.upstream_rate_mask, (unsigned int)rate_mask,
	      (unsigned long long)processed_samples,
	      V92_PCM_PHASE3_JA_MIN_SYMBOLS +
		damaged_descriptors * V92_PCM_DIL_ZERO_DESCRIPTOR_BITS,
	      receiver.line.polarity,
	      expected_polarity, (unsigned long long)ja_symbols, channel_delay);
	{
		struct v92_pcm_phase3_ja_line_rx before = receiver;

		CHECK(v92_pcm_phase3_ja_line_rx_sample(&receiver, 0) ==
			      V92_PCM_PHASE3_JA_LINE_RX_DONE &&
		      receiver_states_equal(&receiver, &before),
		      "complete Ja line receiver consumed another sample");
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
		      G711_LAW_PCMU, 0u, &pcmu, 1u, 0.0, 1, 0u, 0u),
	      "exact PCMU Ja line failed");
	CHECK(check_line(
		      G711_LAW_PCMA, V92_PCM_DIL_RATE_MASK_ALL,
		      &pcma, 1u, 0.0, 1, 0u, 0u),
	      "exact PCMA Ja line failed");
	return true;
}

static bool test_lost_preamble(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct channel_bench_effect_spec codec = {
			.kind = law == G711_LAW_PCMU ?
				CHANNEL_BENCH_PCMU : CHANNEL_BENCH_PCMA,
		};

		for (unsigned damaged = 1u; damaged <= 2u; ++damaged)
			CHECK(check_line(law, V92_PCM_DIL_RATE_MASK_ALL,
				&codec, 1u, 0.0, 1, 0u, damaged),
			      "lost Ja preamble did not recover after %u descriptors", damaged);
	}
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
				.seed = UINT32_C(0x13579bdf),
			},
		},
		{
			.kind = CHANNEL_BENCH_PCMA,
		},
	};

	CHECK(check_line(
		      G711_LAW_PCMA, UINT32_C(0x52491), effects,
		      ARRAY_LENGTH(effects), 0.125, -1, 7u, 0u),
	      "impaired PCMA Ja line failed");
	return true;
}

static bool prepare_handoff(
	struct v92_pcm_phase3_trn1u_line_handoff *handoff)
{
	struct v92_pcm_phase3_trn1u source;
	struct v92_pcm_phase3_trn1u_line_rx receiver;

	if (handoff == NULL ||
	    v92_pcm_phase3_trn1u_init(
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
	struct v92_pcm_phase3_ja_line_rx receiver;
	struct v92_pcm_phase3_ja_line_rx before;
	struct v92_pcm_dil_parameters sentinel = {
		.upstream_rate_mask = UINT32_C(0xa5a5a5a5),
	};
	struct v92_pcm_dil_parameters parameters = sentinel;
	static const char *const names[] = {
		"ok", "synchronized", "rejected", "detected", "done",
		"failed", "invalid argument", "invalid state",
		"numeric failure",
	};

	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(prepare_handoff(&handoff),
	      "Ja line contract handoff did not initialize");
	invalid = handoff;
	invalid.polarity = 0;
	CHECK(v92_pcm_phase3_ja_line_rx_init(NULL, &handoff) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ja_line_rx_init(&receiver, NULL) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_ja_line_rx_init(&receiver, &invalid) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid Ja line initialization changed its destination");
	CHECK(v92_pcm_phase3_ja_line_rx_init(&receiver, &handoff) ==
		      V92_PCM_PHASE3_JA_LINE_RX_OK &&
	      v92_pcm_phase3_ja_line_rx_is_valid(&receiver) &&
	      !v92_pcm_phase3_ja_line_rx_is_complete(&receiver) &&
	      !v92_pcm_phase3_ja_line_rx_has_failed(&receiver) &&
	      !v92_pcm_phase3_ja_line_rx_parameters(&receiver, &parameters) &&
	      memcmp(&parameters, &sentinel, sizeof(parameters)) == 0,
	      "Ja line initialization/getter contract failed");
	before = receiver;
	CHECK(v92_pcm_phase3_ja_line_rx_sample(NULL, 0) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT &&
	      receiver_states_equal(&receiver, &before),
	      "invalid Ja line call changed state");
	CHECK(receiver.starting_input_samples != 0u,
	      "JA line fixture needs a nonzero handoff clock");
	uint64_t origin = receiver.starting_input_samples;
	receiver.starting_input_samples = receiver.input_samples + 1u;
	before = receiver;
	CHECK(!v92_pcm_phase3_ja_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_ja_line_rx_sample(&receiver, 0) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT &&
	      receiver_states_equal(&receiver, &before),
	      "JA line accepted an input clock before its handoff");
	receiver.starting_input_samples = origin;
	receiver.input_samples = UINT64_MAX - 1u;
	CHECK(v92_pcm_phase3_ja_line_rx_sample(&receiver, 0) ==
		      V92_PCM_PHASE3_JA_LINE_RX_OK &&
	      receiver.input_samples == UINT64_MAX &&
	      receiver.starting_input_samples == origin,
	      "JA line rejected the last representable input sample");

	before = receiver;
	CHECK(v92_pcm_phase3_ja_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_ja_line_rx_sample(&receiver, 0) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_STATE &&
	      receiver_states_equal(&receiver, &before),
	      "Ja line counter overflow was not transactional");
	receiver.stage = (enum v92_pcm_phase3_ja_line_rx_stage)-1;
	before = receiver;
	CHECK(!v92_pcm_phase3_ja_line_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_ja_line_rx_sample(&receiver, 0) ==
		      V92_PCM_PHASE3_JA_LINE_RX_INVALID_ARGUMENT &&
	      receiver_states_equal(&receiver, &before) &&
	      !v92_pcm_phase3_ja_line_rx_is_complete(NULL) &&
	      !v92_pcm_phase3_ja_line_rx_has_failed(NULL),
	      "corrupt Ja line state was not rejected transactionally");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_ja_line_rx_result_name(
				      (enum v92_pcm_phase3_ja_line_rx_result)
					      index),
			      names[index]) == 0,
		      "Ja line result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_ja_line_rx_result_name(
			      (enum v92_pcm_phase3_ja_line_rx_result)-1),
		      "unknown") == 0,
	      "unknown Ja line result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact PCMU and PCMA lines", test_exact_laws },
		{ "impaired line", test_impaired_line },
		{ "lost preamble and CRC rejection", test_lost_preamble },
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
