// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "pcm_phase3_digital_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct reference_scrambler {
	uint8_t history[23];
};

static uint8_t reference_scramble(
	struct reference_scrambler *scrambler, uint8_t input)
{
	uint8_t output = input ^ scrambler->history[17] ^
			 scrambler->history[22];

	for (size_t delay = 22; delay > 0; --delay)
		scrambler->history[delay] = scrambler->history[delay - 1u];
	scrambler->history[0] = output;
	return output;
}

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = V90_JD_RATE_CAPABILITY_MASK,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static const struct v90_dil_parameters zero_dil = {
	.sign_pattern_bits = 1,
	.training_pattern_bits = 1,
};

static struct v90_dil_parameters nonzero_dil(void)
{
	struct v90_dil_parameters generated = {
		.sign_pattern = { 0x05 },
		.training_pattern = { 0x06 },
		.segment_count = 2,
		.sign_pattern_bits = 3,
		.training_pattern_bits = 4,
	};

	generated.training_ucodes[0] = 17;
	generated.training_ucodes[1] = 112;
	generated.reference_ucodes[1] = 16;
	generated.reference_ucodes[7] = 120;
	generated.segment_length_codes[7] = 1;
	return generated;
}

static bool symbol_matches(
	const struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, bool positive)
{
	uint8_t octet;

	return g711_ucode_octet(law, ucode, positive, &octet) &&
	       symbol->ucode == ucode && symbol->positive == positive &&
	       symbol->octet == octet &&
	       (law == G711_LAW_PCMU ?
			g711_encode_pcmu(symbol->sample) :
			g711_encode_pcma(symbol->sample)) == octet;
}

static bool run_timeline(enum g711_law law)
{
	struct v90_jd_parameters parameters = jd_parameters();
	struct pcm_phase3_digital_tx transmitter;
	struct reference_scrambler scrambler = { 0 };
	uint8_t frame[V90_JD_FRAME_BYTES];
	uint8_t last_sign = 0;
	const uint8_t training_ucode = 78;
	const uint8_t sd_ucode =
		(uint8_t)((training_ucode + 16u) & UINT8_C(0x7f));

	CHECK(v90_jd_encode(&parameters, frame, sizeof(frame)) == V90_JD_OK &&
	      pcm_phase3_digital_tx_init_v90(
		      &transmitter, law, training_ucode, &parameters,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_PREFIX &&
	      transmitter.sequence.prefix.stage ==
		      PCM_PHASE3_DIGITAL_PREFIX_SD,
	      "digital Phase 3 initialization failed for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < V90_PHASE3_SD_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % 6u;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(&symbol, law, ucode, phase < 3u),
		      "Sd mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_PREFIX &&
	      transmitter.sequence.prefix.stage ==
		      PCM_PHASE3_DIGITAL_PREFIX_SD_BAR,
	      "Sd did not hand off to Sd-bar");
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % 6u;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(&symbol, law, ucode, phase >= 3u),
		      "Sd-bar mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_PREFIX &&
	      transmitter.sequence.prefix.stage ==
		      PCM_PHASE3_DIGITAL_PREFIX_TRN1D,
	      "Sd-bar did not hand off to TRN1d");
	for (size_t index = 0;
	     index < V90_PHASE3_TRN1D_MIN_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t sign = reference_scramble(&scrambler, 1);

		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode, sign != 0),
		      "TRN1d mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
		last_sign = sign;
	}
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_JD &&
		      transmitter.bit_index == 0 &&
		      transmitter.jd_records == 0,
	      "minimum TRN1d did not hand off to Jd");
	for (size_t index = 0; index < 13; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t bit = (uint8_t)(
			(frame[index / 8u] >> (7u - index % 8u)) & 1u);

		last_sign ^= reference_scramble(&scrambler, bit);
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode,
			      last_sign != 0),
		      "Jd prefix mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	CHECK(pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_JD &&
	      transmitter.finish_jd_requested &&
	      transmitter.bit_index == 13,
	      "mid-record finish request did not latch");
	{
		struct pcm_phase3_digital_tx saved = transmitter;

		CHECK(pcm_phase3_digital_tx_finish_jd(&transmitter) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
		      "repeated finish request was not idempotent");
	}
	for (size_t index = 13; index < V90_JD_FRAME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t bit = (uint8_t)(
			(frame[index / 8u] >> (7u - index % 8u)) & 1u);

		last_sign ^= reference_scramble(&scrambler, bit);
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode,
			      last_sign != 0),
		      "Jd suffix mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_PRIME &&
		      transmitter.jd_records == 1 &&
		      transmitter.bit_index == 0,
	      "Jd did not finish on its record boundary");
	for (size_t index = 0; index < V90_PHASE3_JD_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;

		last_sign ^= reference_scramble(&scrambler, 0);
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode,
			      last_sign != 0),
		      "Jd-prime mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE,
	      "Jd-prime did not complete Phase 3 transmission");
	{
		struct pcm_phase3_digital_tx saved = transmitter;
		struct v90_pcm_symbol symbol = { .octet = 0xa5 };
		struct v90_pcm_symbol saved_symbol = symbol;

		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_DONE &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
		      "completed Phase 3 transmitter changed state or output");
	}
	return true;
}

static bool test_timelines(void)
{
	return run_timeline(G711_LAW_PCMU) &&
	       run_timeline(G711_LAW_PCMA);
}

static bool advance_to_jd(struct pcm_phase3_digital_tx *transmitter)
{
	struct v90_pcm_symbol symbol;

	while (transmitter->stage != PCM_PHASE3_DIGITAL_TX_JD)
		CHECK(pcm_phase3_digital_tx_next(transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "could not advance transmitter to Jd");
	return true;
}

static bool run_dil_timeline(enum g711_law law)
{
	struct v90_jd_parameters parameters = jd_parameters();
	struct v90_dil_parameters dil = nonzero_dil();
	struct pcm_phase3_digital_tx transmitter;
	struct v90_dil_sequence reference;
	struct v90_pcm_symbol actual;
	struct v90_pcm_symbol expected;
	size_t remaining = 0;

	CHECK(pcm_phase3_digital_tx_init_v90(
		      &transmitter, law, 78, &parameters, &dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      advance_to_jd(&transmitter) &&
	      pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK,
	      "nonzero DIL fixture did not reach Jd for law %u",
	      (unsigned int)law);
	for (size_t index = 0;
	     index < V90_JD_FRAME_BITS + V90_PHASE3_JD_PRIME_BITS; ++index)
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &actual) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "nonzero DIL prefix stopped for law %u at %zu",
		      (unsigned int)law, index);
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_DIL &&
	      v90_dil_sequence_init(&reference, &dil, law) ==
		      V90_DIL_SEQUENCE_OK,
	      "Jd-prime did not enter DIL for law %u", (unsigned int)law);
	for (size_t index = 0; index < 8u; ++index) {
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &actual) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      v90_dil_sequence_next(&reference, &expected) ==
			      V90_DIL_SEQUENCE_OK &&
		      symbol_matches(
			      &actual, law, expected.ucode, expected.positive),
		      "DIL symbol %zu differed for law %u", index,
		      (unsigned int)law);
	}
	CHECK(pcm_phase3_digital_tx_finish_dil(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      v90_dil_sequence_request_finish(&reference) ==
		      V90_DIL_SEQUENCE_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_DIL,
	      "mid-segment DIL finish did not latch for law %u",
	      (unsigned int)law);
	while (!v90_dil_sequence_is_complete(&reference)) {
		CHECK(remaining++ < V90_DIL_MAX_SEGMENT_SYMBOLS &&
		      pcm_phase3_digital_tx_next(&transmitter, &actual) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      v90_dil_sequence_next(&reference, &expected) ==
			      V90_DIL_SEQUENCE_OK &&
		      symbol_matches(
			      &actual, law, expected.ucode, expected.positive),
		      "finishing DIL differed for law %u at %zu",
		      (unsigned int)law, remaining);
	}
	CHECK(remaining == 10u &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	      pcm_phase3_digital_tx_finish_dil(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK,
	      "DIL did not finish at its segment boundary for law %u",
	      (unsigned int)law);
	return true;
}

static bool test_dil_timelines(void)
{
	return run_dil_timeline(G711_LAW_PCMU) &&
	       run_dil_timeline(G711_LAW_PCMA);
}

static bool test_record_boundary_finish(void)
{
	struct v90_jd_parameters parameters = jd_parameters();
	struct pcm_phase3_digital_tx transmitter;
	struct pcm_phase3_digital_tx saved;
	struct v90_pcm_symbol symbol;

	CHECK(pcm_phase3_digital_tx_init_v90(
		      &transmitter, G711_LAW_PCMU, 91, &parameters,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      advance_to_jd(&transmitter),
	      "record-boundary fixture did not reach Jd");
	saved = transmitter;
	CHECK(transmitter.protocol == PCM_PROTOCOL_V90 &&
	      pcm_phase3_digital_tx_set_v92_su_phase(
		      &transmitter, UINT16_C(0x1234)) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      pcm_phase3_digital_tx_finish_jp(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      pcm_phase3_digital_tx_finish_scr(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
	      "V.90 transmitter entered the V.92-only Jp branch");
	for (size_t index = 0; index < V90_JD_FRAME_BITS; ++index)
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "whole Jd record failed at bit %zu", index);
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_JD &&
		      transmitter.jd_records == 1 &&
		      transmitter.bit_index == 0 &&
	      pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_PRIME,
	      "boundary finish emitted an unnecessary Jd record");
	return true;
}

static bool test_validation(void)
{
	struct v90_jd_parameters valid = jd_parameters();
	struct v90_jd_parameters invalid = valid;
	struct pcm_phase3_digital_tx transmitter;
	struct pcm_phase3_digital_tx saved;
	struct v90_pcm_symbol symbol = { .sample = 1234, .octet = 0xa5 };
	struct v90_pcm_symbol saved_symbol = symbol;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	saved = transmitter;
	invalid.maximum_spectral_shaping_lookahead = 0;
	CHECK(pcm_phase3_digital_tx_init_v90(
		      NULL, G711_LAW_PCMU, 78, &valid, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v90(
		      &transmitter, (enum g711_law)2, 78, &valid,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v90(
		      &transmitter, G711_LAW_PCMU, 66, &valid,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v90(
		      &transmitter, G711_LAW_PCMU, 78, NULL,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v90(
		      &transmitter, G711_LAW_PCMU, 78, &valid, NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v90(
		      &transmitter, G711_LAW_PCMU, 78, &invalid,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "invalid digital Phase 3 initialization changed state");
	CHECK(pcm_phase3_digital_tx_init_v90(
		      &transmitter, G711_LAW_PCMU, 78, &valid,
		      &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK,
	      "validation fixture did not initialize");
	saved = transmitter;
	CHECK(pcm_phase3_digital_tx_next(NULL, &symbol) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_next(&transmitter, NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_finish_jd(NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_finish_dil(NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      pcm_phase3_digital_tx_finish_dil(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      !pcm_phase3_digital_tx_is_valid(NULL),
	      "invalid digital Phase 3 operation changed state or output");
	transmitter.jd_frame[0] = 0;
	CHECK(!pcm_phase3_digital_tx_is_valid(&transmitter) &&
	      pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "digital Phase 3 accepted a corrupt owned Jd record");
	CHECK(strcmp(pcm_phase3_digital_tx_result_name(
		     PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR),
		     "internal error") == 0 &&
	      strcmp(pcm_phase3_digital_tx_result_name(
		     (enum pcm_phase3_digital_tx_result)5),
		     "unknown") == 0,
	      "digital Phase 3 result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact PCMU and PCMA timelines", test_timelines },
		{ "record-boundary finish", test_record_boundary_finish },
		{ "descriptor-driven DIL timelines", test_dil_timelines },
		{ "validation and transactionality", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
