// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "v92_phase3_digital_tx.h"

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

static struct v92_jd jd_parameters(void)
{
	return (struct v92_jd){
		.data_rate_capability_mask = UINT32_C(0x002aaaaa),
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static struct v92_jp jp_parameters(void)
{
	return (struct v92_jp){
		.su_phase_extension_q16 = UINT16_C(0x9234),
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
	};
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

static bool advance_exact_prefix(
	struct v92_phase3_digital_tx *transmitter,
	struct reference_scrambler *scrambler, uint8_t *last_sign,
	enum g711_law law, uint8_t training_ucode)
{
	uint8_t sd_ucode =
		(uint8_t)((training_ucode + 16u) & UINT8_C(0x7f));

	for (size_t index = 0; index < V90_PHASE3_SD_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % 6u;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(v92_phase3_digital_tx_next(transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(&symbol, law, ucode, phase < 3u),
		      "Sd mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % 6u;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(v92_phase3_digital_tx_next(transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(&symbol, law, ucode, phase >= 3u),
		      "Sd-bar mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	for (size_t index = 0;
	     index < V90_PHASE3_TRN1D_MIN_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t sign = reference_scramble(scrambler, 1);

		CHECK(v92_phase3_digital_tx_next(transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode, sign != 0),
		      "TRN1d mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
		*last_sign = sign;
	}
	CHECK(transmitter->stage == V92_PHASE3_DIGITAL_TX_JD &&
	      transmitter->bit_index == 0 &&
	      transmitter->jd_records == 0,
	      "minimum TRN1d did not hand off to V.92 Jd");
	return true;
}

static uint8_t frame_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >> (7u - index % 8u)) & 1u);
}

static bool emit_checked_bit(
	struct v92_phase3_digital_tx *transmitter,
	struct reference_scrambler *scrambler, uint8_t *last_sign,
	uint8_t input, enum g711_law law, uint8_t training_ucode,
	const char *name, size_t index)
{
	struct v90_pcm_symbol symbol;

	*last_sign ^= reference_scramble(scrambler, input);
	CHECK(v92_phase3_digital_tx_next(transmitter, &symbol) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      symbol_matches(
		      &symbol, law, training_ucode, *last_sign != 0),
	      "%s mismatch for law %u at bit %zu", name,
	      (unsigned int)law, index);
	return true;
}

static bool run_timeline(enum g711_law law)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jp jp = jp_parameters();
	struct v92_phase3_digital_tx transmitter;
	struct reference_scrambler scrambler = { 0 };
	uint8_t jd_frame[V92_J_FRAME_BYTES];
	uint8_t jp_frame[V92_J_FRAME_BYTES];
	uint8_t last_sign = 0;
	const uint8_t training_ucode = 78;

	CHECK(v92_jd_encode(&jd, jd_frame, sizeof(jd_frame)) == V92_J_OK &&
	      v92_jp_encode(&jp, jp_frame, sizeof(jp_frame)) == V92_J_OK &&
	      v92_phase3_digital_tx_init(
		      &transmitter, law, training_ucode, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      advance_exact_prefix(
		      &transmitter, &scrambler, &last_sign, law,
		      training_ucode),
	      "V.92 digital Phase 3 initialization failed for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < 13; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign,
			      frame_bit(jd_frame, index), law, training_ucode,
			      "Jd prefix", index),
		      "could not emit Jd prefix");
	CHECK(v92_phase3_digital_tx_finish_jd(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == V92_PHASE3_DIGITAL_TX_JD &&
	      transmitter.finish_jd_requested &&
	      transmitter.bit_index == 13,
	      "mid-record Jd completion did not latch");
	{
		struct v92_phase3_digital_tx saved = transmitter;

		CHECK(v92_phase3_digital_tx_finish_jd(&transmitter) ==
			      V92_PHASE3_DIGITAL_TX_OK &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
		      "repeated Jd completion was not idempotent");
	}
	for (size_t index = 13; index < V92_J_FRAME_BITS; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign,
			      frame_bit(jd_frame, index), law, training_ucode,
			      "Jd suffix", index),
		      "could not emit Jd suffix");
	CHECK(transmitter.stage == V92_PHASE3_DIGITAL_TX_JP &&
	      transmitter.jd_records == 1 && transmitter.jp_records == 0 &&
	      transmitter.bit_index == 0,
	      "Jd did not finish on its record boundary");
	for (size_t record = 0; record < 2; ++record) {
		size_t limit = record == 0 ? V92_J_FRAME_BITS : 19;

		for (size_t index = 0; index < limit; ++index)
			CHECK(emit_checked_bit(
				      &transmitter, &scrambler, &last_sign,
				      frame_bit(jp_frame, index), law,
				      training_ucode, "Jp", index),
			      "could not emit Jp record");
	}
	CHECK(transmitter.stage == V92_PHASE3_DIGITAL_TX_JP &&
	      transmitter.jp_records == 1 && transmitter.bit_index == 19 &&
	      v92_phase3_digital_tx_finish_jp(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_OK,
	      "mid-record Jp completion did not latch");
	for (size_t index = 19; index < V92_J_FRAME_BITS; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign,
			      frame_bit(jp_frame, index), law, training_ucode,
			      "Jp suffix", index),
		      "could not emit Jp suffix");
	CHECK(transmitter.stage == V92_PHASE3_DIGITAL_TX_JP_PRIME &&
	      transmitter.jp_records == 2 && transmitter.bit_index == 0,
	      "Jp did not finish on its record boundary");
	for (size_t index = 0; index < V92_PHASE3_JP_PRIME_BITS; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign, 0, law,
			      training_ucode, "Jp-prime", index),
		      "could not emit Jp-prime");
	CHECK(transmitter.stage == V92_PHASE3_DIGITAL_TX_COMPLETE,
	      "Jp-prime did not complete Phase 3 transmission");
	{
		struct v92_phase3_digital_tx saved = transmitter;
		struct v90_pcm_symbol symbol = { .octet = 0xa5 };
		struct v90_pcm_symbol saved_symbol = symbol;

		CHECK(v92_phase3_digital_tx_next(&transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_DONE &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
		      "completed transmitter changed state or output");
	}
	return true;
}

static bool test_timelines(void)
{
	return run_timeline(G711_LAW_PCMU) && run_timeline(G711_LAW_PCMA);
}

static bool advance_to_jd(struct v92_phase3_digital_tx *transmitter)
{
	struct v90_pcm_symbol symbol;

	while (transmitter->stage != V92_PHASE3_DIGITAL_TX_JD)
		CHECK(v92_phase3_digital_tx_next(transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK,
		      "could not advance transmitter to Jd");
	return true;
}

static bool test_minimum_and_boundary_records(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jp jp = jp_parameters();
	struct v92_phase3_digital_tx transmitter;
	struct v90_pcm_symbol symbol;

	CHECK(v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 91, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      advance_to_jd(&transmitter) &&
	      v92_phase3_digital_tx_finish_jd(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == V92_PHASE3_DIGITAL_TX_JD,
	      "early Jd completion did not require a record");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(v92_phase3_digital_tx_next(&transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK,
		      "minimum Jd record failed at bit %zu", index);
	CHECK(transmitter.stage == V92_PHASE3_DIGITAL_TX_JP &&
	      v92_phase3_digital_tx_finish_jp(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == V92_PHASE3_DIGITAL_TX_JP,
	      "early Jp completion did not require a record");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(v92_phase3_digital_tx_next(&transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK,
		      "minimum Jp record failed at bit %zu", index);
	CHECK(transmitter.stage == V92_PHASE3_DIGITAL_TX_JP_PRIME,
	      "minimum Jp record did not advance to Jp-prime");

	CHECK(v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMA, 78, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      advance_to_jd(&transmitter),
	      "boundary fixture did not reach Jd");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(v92_phase3_digital_tx_next(&transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK,
		      "boundary Jd record failed at bit %zu", index);
	CHECK(transmitter.jd_records == 1 && transmitter.bit_index == 0 &&
	      v92_phase3_digital_tx_finish_jd(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == V92_PHASE3_DIGITAL_TX_JP,
	      "Jd boundary completion emitted an extra record");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(v92_phase3_digital_tx_next(&transmitter, &symbol) ==
			      V92_PHASE3_DIGITAL_TX_OK,
		      "boundary Jp record failed at bit %zu", index);
	CHECK(transmitter.jp_records == 1 && transmitter.bit_index == 0 &&
	      v92_phase3_digital_tx_finish_jp(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == V92_PHASE3_DIGITAL_TX_JP_PRIME,
	      "Jp boundary completion emitted an extra record");
	return true;
}

static bool test_validation(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jd bad_jd = jd;
	struct v92_jp jp = jp_parameters();
	struct v92_jp bad_jp = jp;
	struct v92_phase3_digital_tx transmitter;
	struct v92_phase3_digital_tx saved;
	struct v90_pcm_symbol symbol = { .sample = 1234, .octet = 0xa5 };
	struct v90_pcm_symbol saved_symbol = symbol;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	saved = transmitter;
	bad_jd.maximum_spectral_shaping_lookahead = 0;
	bad_jp.training_constellation = (enum v92_constellation_size)6;
	CHECK(v92_phase3_digital_tx_init(
		      NULL, G711_LAW_PCMU, 78, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_init(
		      &transmitter, (enum g711_law)2, 78, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 66, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 78, NULL, &jp) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 78, &jd, NULL) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 78, &bad_jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 78, &jd, &bad_jp) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
	      "invalid initialization changed state");
	CHECK(v92_phase3_digital_tx_init(
		      &transmitter, G711_LAW_PCMU, 78, &jd, &jp) ==
		      V92_PHASE3_DIGITAL_TX_OK,
	      "validation fixture did not initialize");
	saved = transmitter;
	CHECK(v92_phase3_digital_tx_next(NULL, &symbol) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_next(&transmitter, NULL) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_finish_jd(NULL) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_finish_jp(NULL) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      v92_phase3_digital_tx_finish_jd(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      v92_phase3_digital_tx_finish_jp(&transmitter) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      !v92_phase3_digital_tx_is_valid(NULL),
	      "invalid operation changed state or output");
	transmitter.jd_frame[0] ^= UINT8_C(0x80);
	CHECK(!v92_phase3_digital_tx_is_valid(&transmitter) &&
	      v92_phase3_digital_tx_next(&transmitter, &symbol) ==
		      V92_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "transmitter accepted a corrupt Jd frame");
	transmitter = saved;
	transmitter.jp_frame[0] ^= UINT8_C(0x80);
	CHECK(!v92_phase3_digital_tx_is_valid(&transmitter),
	      "transmitter accepted a corrupt Jp frame");
	CHECK(strcmp(v92_phase3_digital_tx_result_name(
		     V92_PHASE3_DIGITAL_TX_INTERNAL_ERROR),
		     "internal error") == 0 &&
	      strcmp(v92_phase3_digital_tx_result_name(
		     (enum v92_phase3_digital_tx_result)5),
		     "unknown") == 0,
	      "result names changed");
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
		{ "minimum and boundary records",
		  test_minimum_and_boundary_records },
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
