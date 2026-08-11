// SPDX-License-Identifier: BSD-3-Clause

#include <conditioning.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_FIXED_SYMBOLS = V32_CONDITIONING_S_SYMBOLS +
			     V32_CONDITIONING_S_BAR_SYMBOLS,
};

static uint8_t direct_trn_quadrant(uint8_t scrambled)
{
	return (uint8_t)(scrambled ^ (scrambled >> 1));
}

static bool check_conditioning_sequence(
	enum v32_transmitter_role role,
	const uint8_t expected_start[15])
{
	struct v32_conditioning_transmitter transmitter;
	struct v32_symbol_encoder reference;
	struct v32_symbol_encoder final_state;
	uint8_t expected_prefix[15];
	uint8_t quadrant;

	CHECK(v32_conditioning_transmitter_init(
		      &transmitter, role,
		      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      V32_CONDITIONING_OK &&
		      v32_symbol_encoder_init(&reference, role, 0) ==
			      V32_FRAMING_OK,
	      "V.32 conditioning transmitter did not initialize for role %d",
	      role);
	for (uint16_t symbol = 0;
	     symbol < V32_CONDITIONING_S_SYMBOLS; ++symbol) {
		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK &&
			      quadrant == (uint8_t)(symbol & 1u),
		      "V.32 S changed at symbol %u/%d", symbol, role);
	}
	for (uint16_t symbol = 0;
	     symbol < V32_CONDITIONING_S_BAR_SYMBOLS; ++symbol) {
		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK &&
			      quadrant == (uint8_t)(2u + (symbol & 1u)),
		      "V.32 S-bar changed at symbol %u/%d", symbol, role);
	}
	for (uint16_t symbol = 0;
	     symbol < V32_CONDITIONING_TRN_MIN_SYMBOLS; ++symbol) {
		uint8_t expected_quadrant;
		uint8_t scrambled;

		CHECK(v32_scramble_dibit(
			      &reference.scrambler, 3, &scrambled) ==
			      V32_FRAMING_OK,
		      "V.32 TRN reference stopped at symbol %u/%d", symbol,
		      role);
		if (symbol < V32_CONDITIONING_TRN_PREFIX_SYMBOLS) {
			expected_quadrant =
				(scrambled & 2u) != 0 ? 2u : 0u;
			reference.phase_quadrant = expected_quadrant;
		} else {
			expected_quadrant = direct_trn_quadrant(scrambled);
			reference.phase_quadrant = expected_quadrant;
		}
		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK &&
			      quadrant == expected_quadrant,
		      "V.32 TRN changed at symbol %u/%d", symbol, role);
		if (symbol < ARRAY_LENGTH(expected_prefix))
			expected_prefix[symbol] = scrambled;
	}
	CHECK(memcmp(expected_prefix, expected_start, sizeof(expected_prefix)) ==
		      0,
	      "V.32 normative scrambler start changed for role %d", role);
	CHECK(v32_conditioning_transmitter_is_complete(
		      &transmitter) &&
		      transmitter.emitted_symbols ==
			      TEST_FIXED_SYMBOLS +
				      V32_CONDITIONING_TRN_MIN_SYMBOLS &&
		      v32_conditioning_transmitter_framing_state(
			      &transmitter, &final_state) ==
			      V32_CONDITIONING_OK &&
		      final_state.phase_quadrant == reference.phase_quadrant &&
		      final_state.scrambler.history == reference.scrambler.history,
	      "V.32 conditioning final framing state changed for role %d", role);
	quadrant = UINT8_MAX;
	CHECK(v32_conditioning_transmitter_next(
		      &transmitter, &quadrant) ==
		      V32_CONDITIONING_COMPLETE &&
		      quadrant == UINT8_MAX,
	      "completed V.32 conditioning transmitter changed its output");
	return true;
}

static bool test_calling_sequence(void)
{
	static const uint8_t expected_start[15] = {
		3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3,
	};

	return check_conditioning_sequence(
		V32_CALLING_TRANSMITTER, expected_start);
}

static bool test_answering_sequence(void)
{
	static const uint8_t expected_start[15] = {
		3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3,
	};

	return check_conditioning_sequence(
		V32_ANSWERING_TRANSMITTER, expected_start);
}

static bool test_maximum_trn_length(void)
{
	struct v32_conditioning_transmitter transmitter;
	uint8_t quadrant = UINT8_MAX;
	uint32_t total = TEST_FIXED_SYMBOLS +
			 V32_CONDITIONING_TRN_MAX_SYMBOLS;

	CHECK(v32_conditioning_transmitter_init(
		      &transmitter, V32_CALLING_TRANSMITTER,
		      V32_CONDITIONING_TRN_MAX_SYMBOLS) ==
		      V32_CONDITIONING_OK,
	      "maximum V.32 TRN transmitter did not initialize");
	for (uint32_t symbol = 0; symbol < total; ++symbol)
		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK &&
			      quadrant < QPSK_QUADRANT_COUNT,
		      "maximum V.32 TRN stopped at symbol %u", symbol);
	CHECK(v32_conditioning_transmitter_is_complete(
		      &transmitter),
	      "maximum V.32 TRN did not complete");
	return true;
}

static bool test_validation(void)
{
	struct v32_conditioning_transmitter transmitter;
	struct v32_conditioning_transmitter saved;
	struct v32_symbol_encoder encoder = { 0 };
	uint8_t quadrant = UINT8_MAX;

	CHECK(v32_conditioning_transmitter_init(
		      &transmitter, V32_CALLING_TRANSMITTER,
		      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      V32_CONDITIONING_OK,
	      "V.32 validation transmitter did not initialize");
	saved = transmitter;
	CHECK(v32_conditioning_transmitter_init(
		      NULL, V32_CALLING_TRANSMITTER,
		      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      V32_CONDITIONING_INVALID_ARGUMENT &&
		      v32_conditioning_transmitter_init(
			      &transmitter,
			      (enum v32_transmitter_role)99,
			      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      V32_CONDITIONING_INVALID_ARGUMENT &&
		      v32_conditioning_transmitter_init(
			      &transmitter,
			      V32_CALLING_TRANSMITTER,
			      V32_CONDITIONING_TRN_MIN_SYMBOLS -
				      1) ==
			      V32_CONDITIONING_INVALID_ARGUMENT &&
		      v32_conditioning_transmitter_init(
			      &transmitter,
			      V32_CALLING_TRANSMITTER,
			      V32_CONDITIONING_TRN_MAX_SYMBOLS +
				      1) ==
			      V32_CONDITIONING_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
	      "V.32 conditioning initialization changed state on rejection");
	CHECK(v32_conditioning_transmitter_next(
		      &transmitter, NULL) ==
		      V32_CONDITIONING_INVALID_ARGUMENT &&
		      v32_conditioning_transmitter_framing_state(
			      &transmitter, &encoder) ==
			      V32_CONDITIONING_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
		      quadrant == UINT8_MAX,
	      "V.32 conditioning rejected call changed state or output");
	transmitter.emitted_symbols = UINT16_MAX;
	CHECK(!v32_conditioning_transmitter_is_valid(&transmitter) &&
		      !v32_conditioning_transmitter_is_complete(
			      &transmitter) &&
		      v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_INVALID_ARGUMENT &&
		      quadrant == UINT8_MAX,
	      "V.32 conditioning accepted corrupt state");
	CHECK(strcmp(
		      v32_conditioning_result_name(
			      V32_CONDITIONING_OK),
		      "ok") == 0 &&
		      strcmp(
			      v32_conditioning_result_name(
				      (enum v32_conditioning_result)99),
			      "unknown") == 0,
	      "V.32 conditioning result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "calling sequence", test_calling_sequence },
		{ "answering sequence", test_answering_sequence },
		{ "maximum TRN length", test_maximum_trn_length },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
