// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "pcm_phase3_digital_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct v90_dil_parameters zero_dil = {
	.sign_pattern_bits = 1,
	.training_pattern_bits = 1,
};

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
	struct pcm_phase3_digital_tx *transmitter,
	struct reference_scrambler *scrambler, uint8_t *last_sign,
	enum g711_law law, uint8_t training_ucode)
{
	uint32_t duration = transmitter->sequence.prefix.trn1d_target_symbols;
	uint8_t sd_ucode =
		(uint8_t)((training_ucode + 16u) & UINT8_C(0x7f));

	for (size_t index = 0; index < V90_PHASE3_SD_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % 6u;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(pcm_phase3_digital_tx_next(transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(&symbol, law, ucode, phase < 3u),
		      "Sd mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		size_t phase = index % 6u;
		uint8_t ucode = phase == 1u || phase == 4u ? 0 : sd_ucode;

		CHECK(pcm_phase3_digital_tx_next(transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(&symbol, law, ucode, phase >= 3u),
		      "Sd-bar mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
	}
	for (size_t index = 0;
	     index < duration; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t sign = reference_scramble(scrambler, 1);

		CHECK(pcm_phase3_digital_tx_next(transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      symbol_matches(
			      &symbol, law, training_ucode, sign != 0),
		      "TRN1d mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
		*last_sign = sign;
	}
	CHECK(transmitter->stage == PCM_PHASE3_DIGITAL_TX_JD &&
	      transmitter->bit_index == 0 &&
	      transmitter->jd_records == 0,
	      "configured TRN1d did not hand off to V.92 Jd");
	return true;
}

static uint8_t frame_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >> (7u - index % 8u)) & 1u);
}

static bool emit_checked_bit(
	struct pcm_phase3_digital_tx *transmitter,
	struct reference_scrambler *scrambler, uint8_t *last_sign,
	uint8_t input, enum g711_law law, uint8_t training_ucode,
	const char *name, size_t index)
{
	struct v90_pcm_symbol symbol;

	*last_sign ^= reference_scramble(scrambler, input);
	CHECK(pcm_phase3_digital_tx_next(transmitter, &symbol) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      symbol_matches(
		      &symbol, law, training_ucode, *last_sign != 0),
	      "%s mismatch for law %u at bit %zu", name,
	      (unsigned int)law, index);
	return true;
}

static bool emit_checked_scr(
	struct pcm_phase3_digital_tx *transmitter,
	struct reference_scrambler *scrambler, enum g711_law law,
	uint8_t training_ucode, size_t index)
{
	struct v90_pcm_symbol symbol;
	uint8_t sign = reference_scramble(scrambler, 1);

	CHECK(pcm_phase3_digital_tx_next(transmitter, &symbol) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      symbol_matches(
		      &symbol, law, training_ucode, sign != 0),
	      "SCR mismatch for law %u at symbol %zu",
	      (unsigned int)law, index);
	return true;
}

static bool run_timeline(enum g711_law law, bool nonzero_dil, uint32_t duration)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jp jp = jp_parameters();
	struct v92_jp jp_template = jp;
	struct pcm_phase3_digital_tx transmitter;
	struct reference_scrambler scrambler = { 0 };
	uint8_t jd_frame[V92_J_FRAME_BYTES];
	uint8_t jp_frame[V92_J_FRAME_BYTES];
	uint8_t last_sign = 0;
	const uint8_t training_ucode = 78;
	const struct v90_dil_parameters dil = {
		.sign_pattern = { 5 }, .sign_pattern_bits = 3,
		.training_pattern = { 1 }, .training_pattern_bits = 2,
		.segment_length_codes = { [2] = 1, [5] = 2 },
		.reference_ucodes = { [2] = 91, [5] = 91 },
		.training_ucodes = { 32, 80 }, .segment_count = 2,
	};

	jp_template.su_phase_extension_q16 = 0;
	CHECK(v92_jd_encode(&jd, jd_frame, sizeof(jd_frame)) == V92_J_OK &&
	      v92_jp_encode(&jp, jp_frame, sizeof(jp_frame)) == V92_J_OK &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, law, training_ucode, duration, &jd,
		      &jp_template, nonzero_dil ? &dil : &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      advance_exact_prefix(
		      &transmitter, &scrambler, &last_sign, law,
		      training_ucode) &&
	      pcm_phase3_digital_tx_set_v92_su_phase(
		      &transmitter, jp.su_phase_extension_q16) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      memcmp(transmitter.jp_frame, jp_frame, sizeof(jp_frame)) == 0,
	      "V.92 digital Phase 3 initialization failed for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < 13; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign,
			      frame_bit(jd_frame, index), law, training_ucode,
			      "Jd prefix", index),
		      "could not emit Jd prefix");
	CHECK(pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_JD &&
	      transmitter.finish_jd_requested &&
	      transmitter.bit_index == 13 &&
	      pcm_phase3_digital_tx_set_v92_su_phase(
		      &transmitter, UINT16_C(0x1234)) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE,
	      "mid-record Jd completion did not latch");
	{
		struct pcm_phase3_digital_tx saved = transmitter;

		CHECK(pcm_phase3_digital_tx_finish_jd(&transmitter) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
		      "repeated Jd completion was not idempotent");
	}
	for (size_t index = 13; index < V92_J_FRAME_BITS; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign,
			      frame_bit(jd_frame, index), law, training_ucode,
			      "Jd suffix", index),
		      "could not emit Jd suffix");
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_JP &&
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
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_JP &&
	      transmitter.jp_records == 1 && transmitter.bit_index == 19 &&
	      pcm_phase3_digital_tx_finish_jp(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK,
	      "mid-record Jp completion did not latch");
	for (size_t index = 19; index < V92_J_FRAME_BITS; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign,
			      frame_bit(jp_frame, index), law, training_ucode,
			      "Jp suffix", index),
		      "could not emit Jp suffix");
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_PRIME &&
	      transmitter.jp_records == 2 && transmitter.bit_index == 0,
	      "Jp did not finish on its record boundary");
	for (size_t index = 0; index < V92_PHASE3_JP_PRIME_BITS; ++index)
		CHECK(emit_checked_bit(
			      &transmitter, &scrambler, &last_sign, 0, law,
			      training_ucode, "Jp-prime", index),
		      "could not emit Jp-prime");
	if (nonzero_dil) {
		struct pcm_phase3_digital_tx boundary = transmitter;

		CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_DIL &&
		      pcm_phase3_digital_tx_finish_scr(&transmitter) ==
			      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
		      pcm_phase3_digital_tx_finish_dil(&boundary) ==
			      PCM_PHASE3_DIGITAL_TX_OK &&
		      boundary.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE,
		      "nonzero DIL entered SCR");
		/* Two complete 30-symbol cycles, then finish five symbols into
		 * the first segment. Sign/training patterns restart per segment. */
		for (size_t index = 0; index < 72; ++index) {
			struct v90_pcm_symbol symbol;
			size_t phase = index % 30;
			uint8_t training = phase < 12 ? 32 : 80;
			if (phase >= 12)
				phase -= 12;
			if (index == 65)
				CHECK(pcm_phase3_digital_tx_finish_dil(&transmitter) ==
					      PCM_PHASE3_DIGITAL_TX_OK &&
				      transmitter.stage == PCM_PHASE3_DIGITAL_TX_DIL,
				      "mid-segment DIL finish did not latch");
			CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
				      PCM_PHASE3_DIGITAL_TX_OK &&
			      symbol_matches(&symbol, law, phase % 2 == 0 ? training : 91,
				      phase % 3 != 1),
			      "V.92 DIL waveform differs at %zu", index);
		}
		CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
		      transmitter.scr_symbols == 0 &&
		      pcm_phase3_digital_tx_finish_scr(&transmitter) ==
			      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
		      pcm_phase3_digital_tx_finish_dil(&transmitter) == PCM_PHASE3_DIGITAL_TX_OK,
		      "nonzero DIL did not finish at its segment boundary");
		return true;
	}
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_SCR &&
	      transmitter.scr_symbols == 0,
	      "Jp-prime did not enter zero-DIL SCR");
	for (size_t index = 0; index < 8; ++index)
		CHECK(emit_checked_scr(
			      &transmitter, &scrambler, law,
			      training_ucode, index),
		      "could not emit SCR");
	CHECK(pcm_phase3_digital_tx_finish_scr(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_SCR,
	      "mid-interval SCR completion did not latch");
	for (size_t index = 8; index < 12; ++index)
		CHECK(emit_checked_scr(
			      &transmitter, &scrambler, law,
			      training_ucode, index),
		      "could not complete SCR");
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_COMPLETE &&
	      transmitter.scr_symbols == 12 &&
	      transmitter.sequence.signs.last_sign == last_sign &&
	      pcm_phase3_digital_tx_finish_scr(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK,
	      "SCR did not finish on a six-symbol boundary");
	{
		struct pcm_phase3_digital_tx saved = transmitter;
		struct v90_pcm_symbol symbol = { .octet = 0xa5 };
		struct v90_pcm_symbol saved_symbol = symbol;

		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_DONE &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
		      "completed transmitter changed state or output");
	}
	return true;
}

static bool test_timelines(void)
{
	static const uint32_t durations[] = {
		V90_PHASE3_TRN1D_MIN_SYMBOLS, V90_PHASE3_TRN1D_SYMBOLS,
	};
	for (size_t i = 0; i < ARRAY_LENGTH(durations); ++i)
		CHECK(run_timeline(G711_LAW_PCMU, false, durations[i]) &&
		      run_timeline(G711_LAW_PCMA, false, durations[i]) &&
		      run_timeline(G711_LAW_PCMU, true, durations[i]) &&
		      run_timeline(G711_LAW_PCMA, true, durations[i]),
		      "TRN1d duration %u changed the coding epoch", durations[i]);
	return true;
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

static bool test_minimum_and_boundary_records(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jp jp = jp_parameters();
	struct pcm_phase3_digital_tx transmitter;
	struct v90_pcm_symbol symbol;

	CHECK(pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 91, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      advance_to_jd(&transmitter) &&
	      pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_JD,
	      "early Jd completion did not require a record");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "minimum Jd record failed at bit %zu", index);
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_JP &&
	      pcm_phase3_digital_tx_finish_jp(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_JP,
	      "early Jp completion did not require a record");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "minimum Jp record failed at bit %zu", index);
	CHECK(transmitter.stage == PCM_PHASE3_DIGITAL_TX_PRIME,
	      "minimum Jp record did not advance to Jp-prime");

	CHECK(pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMA, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      advance_to_jd(&transmitter),
	      "boundary fixture did not reach Jd");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "boundary Jd record failed at bit %zu", index);
	CHECK(transmitter.jd_records == 1 && transmitter.bit_index == 0 &&
	      pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_JP,
	      "Jd boundary completion emitted an extra record");
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index)
		CHECK(pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
			      PCM_PHASE3_DIGITAL_TX_OK,
		      "boundary Jp record failed at bit %zu", index);
	CHECK(transmitter.jp_records == 1 && transmitter.bit_index == 0 &&
	      pcm_phase3_digital_tx_finish_jp(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_OK &&
	      transmitter.stage == PCM_PHASE3_DIGITAL_TX_PRIME,
	      "Jp boundary completion emitted an extra record");
	return true;
}

static bool test_validation(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jd bad_jd = jd;
	struct v92_jp jp = jp_parameters();
	struct v92_jp bad_jp = jp;
	struct pcm_phase3_digital_tx transmitter;
	struct pcm_phase3_digital_tx saved;
	struct v90_pcm_symbol symbol = { .sample = 1234, .octet = 0xa5 };
	struct v90_pcm_symbol saved_symbol = symbol;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	saved = transmitter;
	bad_jd.maximum_spectral_shaping_lookahead = 0;
	bad_jp.training_constellation = (enum v92_constellation_size)6;
	CHECK(pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, 0u, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS + 1u,
		      &jd, &jp, &zero_dil) == PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
	      "invalid TRN1d duration changed the transmitter");
	CHECK(pcm_phase3_digital_tx_init_v92(
		      NULL, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, (enum g711_law)2, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 66, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, NULL, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, NULL, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &bad_jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &bad_jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
	      "invalid initialization changed state");
	CHECK(pcm_phase3_digital_tx_init_v92(
		      &transmitter, G711_LAW_PCMU, 78, V90_PHASE3_TRN1D_MIN_SYMBOLS, &jd, &jp, &zero_dil) ==
		      PCM_PHASE3_DIGITAL_TX_OK,
	      "validation fixture did not initialize");
	saved = transmitter;
	CHECK(pcm_phase3_digital_tx_next(NULL, &symbol) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_next(&transmitter, NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_set_v92_su_phase(NULL, 0) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_set_v92_su_phase(&transmitter, 0) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      pcm_phase3_digital_tx_finish_jd(NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_finish_scr(NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_finish_jp(NULL) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      pcm_phase3_digital_tx_finish_jd(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      pcm_phase3_digital_tx_finish_jp(&transmitter) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      !pcm_phase3_digital_tx_is_valid(NULL),
	      "invalid operation changed state or output");
	transmitter.jd_frame[0] ^= UINT8_C(0x80);
	CHECK(!pcm_phase3_digital_tx_is_valid(&transmitter) &&
	      pcm_phase3_digital_tx_next(&transmitter, &symbol) ==
		      PCM_PHASE3_DIGITAL_TX_INVALID_ARGUMENT &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "transmitter accepted a corrupt Jd frame");
	transmitter = saved;
	transmitter.jp_frame[0] ^= UINT8_C(0x80);
	CHECK(!pcm_phase3_digital_tx_is_valid(&transmitter),
	      "transmitter accepted a corrupt Jp frame");
	CHECK(strcmp(pcm_phase3_digital_tx_result_name(
		     PCM_PHASE3_DIGITAL_TX_INTERNAL_ERROR),
		     "internal error") == 0 &&
	      strcmp(pcm_phase3_digital_tx_result_name(
		     (enum pcm_phase3_digital_tx_result)5),
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
