// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "v90_jd.h"
#include "v90_phase3_sign_receiver.h"
#include "v90_phase3_signs.h"

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
		scrambler->history[delay] = scrambler->history[delay - 1];
	scrambler->history[0] = output;
	return output;
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ? g711_encode_pcma(sample) :
					   g711_encode_pcmu(sample);
}

static bool check_symbol(
	const struct v90_pcm_symbol *symbol, enum g711_law law,
	uint8_t ucode, uint8_t sign)
{
	uint8_t positive = law == G711_LAW_PCMA ?
				(uint8_t)(ucode ^ 0xd5u) :
				(uint8_t)~ucode;
	uint8_t expected = sign != 0 ? positive :
					  (uint8_t)(positive ^ 0x80u);

	return symbol->ucode == ucode &&
	       symbol->positive == (sign != 0) &&
	       symbol->octet == expected &&
	       encode_sample(law, symbol->sample) == expected;
}

static bool run_continuous_stream(enum g711_law law)
{
	struct v90_phase3_signs signs;
	struct reference_scrambler reference = { 0 };
	uint8_t last_sign = 0;

	CHECK(v90_phase3_signs_init(&signs, law, 78) ==
		      V90_PHASE3_SIGNS_OK,
	      "sign stream initialization failed for law %u",
	      (unsigned int)law);
	for (size_t index = 0;
	     index < V90_PHASE3_TRN1D_MIN_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t sign = reference_scramble(&reference, 1);

		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
			      check_symbol(&symbol, law, 78, sign),
		      "TRN1d mismatch for law %u at symbol %zu",
		      (unsigned int)law, index);
		last_sign = sign;
	}
	CHECK(v90_phase3_signs_begin_jd(&signs) == V90_PHASE3_SIGNS_OK &&
		      signs.phase == V90_PHASE3_SIGNS_JD,
	      "TRN1d did not hand off to Jd at its minimum boundary");
	for (size_t index = 0; index < 72; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t bit = (uint8_t)(((index * 5u) ^ (index >> 1u)) & 1u);
		uint8_t scrambled = reference_scramble(&reference, bit);

		last_sign ^= scrambled;
		CHECK(v90_phase3_signs_next_jd_bit(&signs, bit, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
			      check_symbol(&symbol, law, 78, last_sign),
		      "Jd mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	for (size_t index = 0; index < V90_PHASE3_JD_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t scrambled = reference_scramble(&reference, 0);

		last_sign ^= scrambled;
		CHECK(v90_phase3_signs_next_jd_bit(&signs, 0, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
			      check_symbol(&symbol, law, 78, last_sign),
		      "Jd-prime mismatch for law %u at bit %zu",
		      (unsigned int)law, index);
	}
	return true;
}

static bool test_continuous_streams(void)
{
	return run_continuous_stream(G711_LAW_PCMU) &&
	       run_continuous_stream(G711_LAW_PCMA);
}

static bool test_jd_boundary(void)
{
	struct v90_phase3_signs signs;
	struct v90_phase3_signs saved;
	struct v90_pcm_symbol symbol;

	CHECK(v90_phase3_signs_init(&signs, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SIGNS_OK,
	      "boundary stream initialization failed");
	for (size_t index = 0;
	     index < V90_PHASE3_TRN1D_MIN_SYMBOLS - 1u; ++index)
		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "boundary TRN1d generation failed");
	saved = signs;
	CHECK(v90_phase3_signs_begin_jd(&signs) ==
		      V90_PHASE3_SIGNS_INVALID_STATE &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0,
	      "early Jd handoff changed state");
	for (size_t index = 0; index < 2; ++index)
		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "unaligned boundary generation failed");
	saved = signs;
	CHECK(v90_phase3_signs_begin_jd(&signs) ==
		      V90_PHASE3_SIGNS_INVALID_STATE &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0,
	      "unaligned Jd handoff changed state");
	for (size_t index = 0; index < 5; ++index)
		CHECK(v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "aligned boundary generation failed");
	CHECK(signs.trn1d_symbols == 2046 &&
		      v90_phase3_signs_begin_jd(&signs) == V90_PHASE3_SIGNS_OK,
	      "extended aligned TRN1d did not hand off");
	return true;
}

static bool test_pcm_symbol_parsing(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (unsigned int octet = 0; octet <= UINT8_MAX; ++octet) {
			struct v90_pcm_symbol parsed;
			struct v90_pcm_symbol rebuilt;

			CHECK(v90_pcm_symbol_parse(
				      &parsed, law, (uint8_t)octet) &&
			      v90_pcm_symbol_make(
				      &rebuilt, law, parsed.ucode,
				      parsed.positive) &&
			      rebuilt.octet == octet &&
			      rebuilt.sample == parsed.sample &&
			      encode_sample(law, parsed.sample) == octet,
			      "PCM character %u did not round trip for law %u",
			      octet, (unsigned int)law);
		}
	}
	return true;
}

static uint8_t frame_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >>
			  (7u - index % 8u)) & 1u);
}

static bool jd_parameters_match(
	const struct v90_jd_parameters *left,
	const struct v90_jd_parameters *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static bool feed_trn1d(
	struct v90_phase3_signs *transmitter,
	struct v90_phase3_sign_receiver *receiver, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		struct v90_pcm_symbol symbol;
		enum v90_phase3_sign_receiver_result expected =
			receiver->trn1d_symbols + 1u ==
					V90_PHASE3_TRN1D_MIN_SYMBOLS ?
				V90_PHASE3_SIGN_RECEIVER_TRN1D_READY :
				V90_PHASE3_SIGN_RECEIVER_OK;

		CHECK(v90_phase3_signs_next_trn1d(
			      transmitter, &symbol) == V90_PHASE3_SIGNS_OK &&
			      v90_phase3_sign_receiver_receive(
				      receiver, symbol.octet) == expected,
		      "TRN1d receiver mismatch at symbol %zu", index);
	}
	return true;
}

static bool feed_jd_record(
	struct v90_phase3_signs *transmitter,
	struct v90_phase3_sign_receiver *receiver, const uint8_t *frame,
	enum v90_phase3_sign_receiver_result final_result)
{
	for (size_t index = 0; index < V90_JD_FRAME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		enum v90_phase3_sign_receiver_result result;

		CHECK(v90_phase3_signs_next_jd_bit(
			      transmitter, frame_bit(frame, index), &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "could not generate Jd bit %zu", index);
		result = v90_phase3_sign_receiver_receive(
			receiver, symbol.octet);
		CHECK(result == (index == V90_JD_FRAME_BITS - 1u ?
				       final_result :
				       V90_PHASE3_SIGN_RECEIVER_OK),
		      "Jd receiver returned %s at bit %zu",
		      v90_phase3_sign_receiver_result_name(result), index);
	}
	return true;
}

static bool run_sign_receiver(enum g711_law law)
{
	const struct v90_jd_parameters transmitted = {
		.data_rate_capability_mask = UINT32_C(0x2a1555),
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 3,
	};
	struct v90_jd_parameters received;
	struct v90_phase3_signs transmitter;
	struct v90_phase3_sign_receiver receiver;
	uint8_t frame[V90_JD_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(&transmitter, law, 78) ==
		      V90_PHASE3_SIGNS_OK &&
		      v90_phase3_sign_receiver_init(&receiver, law, 78) ==
			      V90_PHASE3_SIGN_RECEIVER_OK &&
		      v90_jd_encode(&transmitted, frame, sizeof(frame)) ==
			      V90_JD_OK &&
		      feed_trn1d(
			      &transmitter, &receiver,
			      V90_PHASE3_TRN1D_MIN_SYMBOLS),
	      "could not initialize sign receiver for law %u",
	      (unsigned int)law);
	CHECK(v90_phase3_sign_receiver_begin_jd_search(&receiver) ==
		      V90_PHASE3_SIGN_RECEIVER_OK &&
		      v90_phase3_signs_begin_jd(&transmitter) ==
			      V90_PHASE3_SIGNS_OK &&
		      feed_jd_record(
			      &transmitter, &receiver, frame,
			      V90_PHASE3_SIGN_RECEIVER_JD_DETECTED) &&
		      v90_phase3_sign_receiver_jd(&receiver, &received) &&
		      jd_parameters_match(&received, &transmitted),
	      "receiver did not recover the first Jd record for law %u",
	      (unsigned int)law);
	CHECK(feed_jd_record(
		      &transmitter, &receiver, frame,
		      V90_PHASE3_SIGN_RECEIVER_JD_REPEATED),
	      "receiver did not recover repeated Jd for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < V90_PHASE3_JD_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		enum v90_phase3_sign_receiver_result result;

		CHECK(v90_phase3_signs_next_jd_bit(
			      &transmitter, 0, &symbol) == V90_PHASE3_SIGNS_OK,
		      "could not generate Jd-prime bit %zu", index);
		result = v90_phase3_sign_receiver_receive(
			&receiver, symbol.octet);
		CHECK(result == (index == V90_PHASE3_JD_PRIME_BITS - 1u ?
				       V90_PHASE3_SIGN_RECEIVER_COMPLETE :
				       V90_PHASE3_SIGN_RECEIVER_OK),
		      "Jd-prime receiver returned %s at bit %zu",
		      v90_phase3_sign_receiver_result_name(result), index);
	}
	CHECK(v90_phase3_sign_receiver_is_complete(&receiver),
	      "sign receiver did not retain completion for law %u",
	      (unsigned int)law);
	{
		struct v90_phase3_sign_receiver saved = receiver;

		CHECK(v90_phase3_sign_receiver_receive(&receiver, 0xa5) ==
			      V90_PHASE3_SIGN_RECEIVER_COMPLETE &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "completed sign receiver consumed another character");
	}
	return true;
}

static bool test_sign_receiver_round_trip(void)
{
	return run_sign_receiver(G711_LAW_PCMU) &&
	       run_sign_receiver(G711_LAW_PCMA);
}

static bool test_sign_receiver_early_search(void)
{
	const struct v90_jd_parameters parameters = {
		.data_rate_capability_mask = UINT32_C(0x15555),
		.training_constellation = V34_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_16_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
	struct v90_phase3_signs transmitter;
	struct v90_phase3_sign_receiver receiver;
	uint8_t frame[V90_JD_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(
		      &transmitter, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SIGNS_OK &&
		      v90_phase3_sign_receiver_init(
			      &receiver, G711_LAW_PCMU, 91) ==
			      V90_PHASE3_SIGN_RECEIVER_OK &&
		      feed_trn1d(
			      &transmitter, &receiver,
			      V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
		      v90_phase3_sign_receiver_begin_jd_search(&receiver) ==
			      V90_PHASE3_SIGN_RECEIVER_OK,
	      "could not start early Jd search");
	for (size_t index = 0; index < 60; ++index) {
		struct v90_pcm_symbol symbol;

		CHECK(v90_phase3_signs_next_trn1d(
			      &transmitter, &symbol) == V90_PHASE3_SIGNS_OK &&
			      v90_phase3_sign_receiver_receive(
				      &receiver, symbol.octet) ==
				      V90_PHASE3_SIGN_RECEIVER_OK,
		      "early Jd search failed on remaining TRN1d at %zu", index);
	}
	CHECK(v90_phase3_signs_begin_jd(&transmitter) ==
		      V90_PHASE3_SIGNS_OK &&
		      v90_jd_encode(&parameters, frame, sizeof(frame)) == V90_JD_OK &&
		      feed_jd_record(
			      &transmitter, &receiver, frame,
			      V90_PHASE3_SIGN_RECEIVER_OK) &&
		      feed_jd_record(
			      &transmitter, &receiver, frame,
			      V90_PHASE3_SIGN_RECEIVER_JD_DETECTED),
	      "receiver did not resynchronize after an early Jd search");
	return true;
}

static bool test_sign_receiver_boundaries(void)
{
	struct v90_phase3_signs transmitter;
	struct v90_phase3_sign_receiver receiver;
	struct v90_phase3_sign_receiver saved;
	struct v90_pcm_symbol symbol;

	CHECK(v90_phase3_signs_init(
		      &transmitter, G711_LAW_PCMA, 78) ==
		      V90_PHASE3_SIGNS_OK &&
		      v90_phase3_sign_receiver_init(
			      &receiver, G711_LAW_PCMA, 78) ==
			      V90_PHASE3_SIGN_RECEIVER_OK &&
		      feed_trn1d(
			      &transmitter, &receiver,
			      V90_PHASE3_TRN1D_MIN_SYMBOLS - 1u),
	      "could not reach early TRN1d boundary");
	saved = receiver;
	CHECK(v90_phase3_sign_receiver_begin_jd_search(&receiver) ==
		      V90_PHASE3_SIGN_RECEIVER_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      feed_trn1d(&transmitter, &receiver, 2),
	      "early Jd search was not transactional");
	saved = receiver;
	CHECK(v90_phase3_sign_receiver_begin_jd_search(&receiver) ==
		      V90_PHASE3_SIGN_RECEIVER_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      feed_trn1d(&transmitter, &receiver, 5) &&
		      v90_phase3_sign_receiver_begin_jd_search(&receiver) ==
			      V90_PHASE3_SIGN_RECEIVER_OK,
	      "unaligned Jd-search boundary was not enforced");
	CHECK(v90_pcm_symbol_make(
		      &symbol, G711_LAW_PCMA, 79, true),
	      "could not build wrong-code character");
	saved = receiver;
	CHECK(v90_phase3_sign_receiver_receive(
		      &receiver, symbol.octet) ==
		      V90_PHASE3_SIGN_RECEIVER_INVALID_SEQUENCE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "wrong training code changed receiver state");
	return true;
}

static bool test_sign_receiver_validation(void)
{
	struct v90_phase3_sign_receiver receiver;
	struct v90_phase3_sign_receiver saved;
	struct v90_jd_parameters parameters;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v90_phase3_sign_receiver_init(
		      NULL, G711_LAW_PCMU, 78) ==
		      V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sign_receiver_init(
			      &receiver, (enum g711_law)2, 78) ==
			      V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sign_receiver_init(
			      &receiver, G711_LAW_PCMU, 66) ==
			      V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid sign-receiver initialization changed state");
	CHECK(v90_phase3_sign_receiver_receive(NULL, 0) ==
		      V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sign_receiver_begin_jd_search(NULL) ==
			      V90_PHASE3_SIGN_RECEIVER_INVALID_ARGUMENT &&
		      v90_phase3_sign_receiver_receive(&receiver, 0) ==
			      V90_PHASE3_SIGN_RECEIVER_INVALID_STATE &&
		      v90_phase3_sign_receiver_begin_jd_search(&receiver) ==
			      V90_PHASE3_SIGN_RECEIVER_INVALID_STATE &&
		      !v90_phase3_sign_receiver_jd(&receiver, &parameters) &&
		      !v90_phase3_sign_receiver_jd(NULL, &parameters) &&
		      !v90_phase3_sign_receiver_jd(&receiver, NULL) &&
		      !v90_phase3_sign_receiver_is_valid(&receiver) &&
		      !v90_phase3_sign_receiver_is_complete(&receiver) &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid sign-receiver operation changed state");
	CHECK(strcmp(v90_phase3_sign_receiver_result_name(
			     V90_PHASE3_SIGN_RECEIVER_JD_DETECTED),
		     "Jd detected") == 0 &&
		      strcmp(v90_phase3_sign_receiver_result_name(
			     (enum v90_phase3_sign_receiver_result)8),
			     "unknown") == 0,
	      "sign-receiver result names are wrong");
	return true;
}

static bool test_validation(void)
{
	struct v90_phase3_signs signs;
	struct v90_phase3_signs saved;
	struct v90_pcm_symbol symbol = {
		.ucode = 0xa5,
		.octet = 0x5a,
		.sample = 1234,
	};
	struct v90_pcm_symbol saved_symbol = symbol;
	struct v90_pcm_symbol invalid_symbol = symbol;

	memset(&signs, 0xa5, sizeof(signs));
	saved = signs;
	CHECK(v90_phase3_signs_init(
		      &signs, (enum g711_law)2, 78) ==
		      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_init(&signs, G711_LAW_PCMU, 66) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_init(NULL, G711_LAW_PCMU, 78) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0,
	      "invalid sign-stream initialization changed state");
	CHECK(v90_phase3_signs_next_trn1d(NULL, &symbol) ==
		      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_trn1d(&signs, NULL) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_trn1d(&signs, &symbol) ==
			      V90_PHASE3_SIGNS_INVALID_STATE &&
		      v90_phase3_signs_begin_jd(NULL) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_jd_bit(NULL, 0, &symbol) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      v90_phase3_signs_next_jd_bit(&signs, 2, &symbol) ==
			      V90_PHASE3_SIGNS_INVALID_ARGUMENT &&
		      memcmp(&signs, &saved, sizeof(signs)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
		      !v90_phase3_signs_is_valid(NULL),
	      "invalid sign-stream operation changed state or output");
	CHECK(!v90_pcm_symbol_make(
		      &invalid_symbol, (enum g711_law)2, 78, true) &&
		      !v90_pcm_symbol_make(
			      &invalid_symbol, G711_LAW_PCMU, 128, true) &&
		      !v90_pcm_symbol_make(NULL, G711_LAW_PCMU, 78, true) &&
		      !v90_pcm_symbol_parse(
			      &invalid_symbol, (enum g711_law)2, 0) &&
		      !v90_pcm_symbol_parse(NULL, G711_LAW_PCMU, 0) &&
		      memcmp(&invalid_symbol, &saved_symbol,
			     sizeof(invalid_symbol)) == 0,
	      "invalid PCM symbol construction changed output");
	CHECK(strcmp(v90_phase3_sign_result_name(
		     V90_PHASE3_SIGNS_INVALID_STATE), "invalid state") == 0 &&
		      strcmp(v90_phase3_sign_result_name(
			     (enum v90_phase3_sign_result)3), "unknown") == 0,
	      "sign-stream result names are wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "continuous TRN1d, Jd, and Jd-prime", test_continuous_streams },
		{ "Jd handoff boundary", test_jd_boundary },
		{ "exact PCM symbol parsing", test_pcm_symbol_parsing },
		{ "sign receiver round trip", test_sign_receiver_round_trip },
		{ "sign receiver early search", test_sign_receiver_early_search },
		{ "sign receiver boundaries", test_sign_receiver_boundaries },
		{ "sign receiver validation", test_sign_receiver_validation },
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
