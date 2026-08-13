// SPDX-License-Identifier: BSD-3-Clause

#include <v92_su.h>

#include "test.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 4096,
	TEST_SU_LEVEL = 5017,
	PREFIX_SAMPLES = 47,
};

static const int8_t SU_SIGNS[V92_SU_PATTERN_SYMBOLS] = {
	1, 0, 1, -1, 0, -1,
};

static struct v92_jp jp(uint16_t extension)
{
	return (struct v92_jp){
		.su_phase_extension_q16 = extension,
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
	};
}

static bool prefill_delays(
	struct fractional_delay *actual,
	struct fractional_delay *expected)
{
	fractional_delay_init(actual);
	fractional_delay_init(expected);
	for (size_t index = 0; index < PREFIX_SAMPLES; ++index) {
		int16_t input = (int16_t)((int)(index * 193u % 7001u) - 3500);
		int16_t left;
		int16_t right;

		if (fractional_delay_process(actual, input, &left) !=
			    FRACTIONAL_DELAY_OK ||
		    fractional_delay_process(expected, input, &right) !=
			    FRACTIONAL_DELAY_OK ||
		    left != right)
			return false;
	}
	return true;
}

static bool check_symbol(
	struct v92_su *su, struct fractional_delay *actual,
	struct fractional_delay *expected, bool inverted,
	enum v92_su_result wanted, uint8_t *pattern_phase)
{
	int sign = SU_SIGNS[*pattern_phase];
	int16_t expected_output;
	int16_t actual_output = INT16_MIN;

	if (inverted)
		sign = -sign;
	*pattern_phase = (uint8_t)((*pattern_phase + 1u) %
		V92_SU_PATTERN_SYMBOLS);
	if (fractional_delay_process(
		    expected, (int16_t)(sign * TEST_SU_LEVEL),
		    &expected_output) != FRACTIONAL_DELAY_OK ||
	    v92_su_next(su, actual, &actual_output) != wanted)
		return false;
	if (expected->phase_q16 != su->phase_q16 &&
	    fractional_delay_set_phase(expected, su->phase_q16) !=
		    FRACTIONAL_DELAY_OK)
		return false;
	return actual_output == expected_output &&
	       su->pattern_phase == *pattern_phase &&
	       memcmp(actual, expected, sizeof(*actual)) == 0;
}

static bool reach_jp_wait(
	struct v92_su *su, struct fractional_delay *actual,
	struct fractional_delay *expected, uint8_t *pattern_phase)
{
	for (size_t index = 0; index < V92_SU_JD_RESPONSE_SYMBOLS; ++index)
		if (!check_symbol(
			    su, actual, expected, false, V92_SU_OK,
			    pattern_phase))
			return false;
	if (su->stage != V92_SU_JD_BAR_RESPONSE || su->stage_symbols != 0)
		return false;
	for (size_t index = 0; index < V92_SU_JD_BAR_INPUT_SYMBOLS; ++index) {
		enum v92_su_result wanted =
			index + 1u == V92_SU_JD_BAR_INPUT_SYMBOLS ?
				V92_SU_REACHED_JP_WAIT : V92_SU_OK;

		if (!check_symbol(
			    su, actual, expected, true, wanted,
			    pattern_phase))
			return false;
	}
	return su->stage == V92_SU_JP_WAIT && su->stage_symbols == 0 &&
	       su->phase_q16 == -FRACTIONAL_DELAY_HALF_SAMPLE_Q16 &&
	       (int64_t)V92_SU_JD_BAR_INPUT_SYMBOLS *
		       FRACTIONAL_DELAY_ONE_SAMPLE_Q16 + su->phase_q16 ==
		       24 * FRACTIONAL_DELAY_ONE_SAMPLE_Q16 +
		       FRACTIONAL_DELAY_HALF_SAMPLE_Q16;
}

static bool reach_jp_prime_wait(
	struct v92_su *su, struct fractional_delay *actual,
	struct fractional_delay *expected, uint8_t *pattern_phase,
	uint16_t extension)
{
	struct v92_jp parameters = jp(extension);

	if (v92_su_accept_jp(su, &parameters) != V92_SU_OK ||
	    su->stage != V92_SU_JP_BAR_RESPONSE)
		return false;
	for (size_t index = 0; index < V92_SU_JP_BAR_INPUT_SYMBOLS; ++index) {
		enum v92_su_result wanted =
			index + 1u == V92_SU_JP_BAR_INPUT_SYMBOLS ?
				V92_SU_REACHED_JP_PRIME_WAIT : V92_SU_OK;

		if (!check_symbol(
			    su, actual, expected, true, wanted,
			    pattern_phase))
			return false;
	}
	return su->stage == V92_SU_JP_PRIME_WAIT &&
	       su->stage_symbols == 0;
}

static bool test_continuous_su_timeline(void)
{
	struct fractional_delay actual;
	struct fractional_delay expected;
	struct v92_su su;
	uint8_t pattern_phase = 0;
	int32_t phase = 0;
	int16_t output = 1234;

	CHECK(prefill_delays(&actual, &expected) &&
	      v92_su_init(&su, TEST_LU) == V92_SU_OK &&
	      su.level == TEST_SU_LEVEL && v92_su_is_valid(&su),
	      "Su timeline did not initialize");
	CHECK(reach_jp_wait(
		      &su, &actual, &expected, &pattern_phase),
	      "Su fixed 24.5T response differed");
	for (size_t index = 0; index < 13; ++index)
		CHECK(check_symbol(
			      &su, &actual, &expected, false, V92_SU_OK,
			      &pattern_phase),
		      "Su Jp wait differed at %zu", index);
	CHECK(reach_jp_prime_wait(
		      &su, &actual, &expected, &pattern_phase, UINT16_C(0x80b0)),
	      "Su Jp response differed");
	CHECK(v92_su_get_phase(&su, &phase) && phase == 0x00b0,
	      "Su Jp extension selected phase %d", phase);
	for (size_t index = 0; index < 17; ++index)
		CHECK(check_symbol(
			      &su, &actual, &expected, false, V92_SU_OK,
			      &pattern_phase),
		      "Su Jp-prime wait differed at %zu", index);
	CHECK(v92_su_accept_jp_prime(&su, &actual, false) ==
		      V92_SU_COMPLETE &&
	      v92_su_is_complete(&su) &&
	      actual.phase_q16 == 0x00b0 &&
	      v92_su_next(&su, &actual, &output) == V92_SU_COMPLETE &&
	      output == 1234,
	      "analogue Su completion discarded phase or emitted data");
	return true;
}

static bool test_phase_extension_and_codec_grid(void)
{
	static const struct {
		uint16_t extension;
		int32_t phase_q16;
	} fixtures[] = {
		{ UINT16_C(0x0000), -INT32_C(0x8000) },
		{ UINT16_C(0x4000), -INT32_C(0x4000) },
		{ UINT16_C(0x8000), 0 },
		{ UINT16_C(0xc000), INT32_C(0x4000) },
		{ UINT16_C(0xffff), INT32_C(0x7fff) },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		struct fractional_delay actual;
		struct fractional_delay expected;
		struct v92_su su;
		uint8_t pattern_phase = 0;
		int32_t phase = INT32_MIN;
		bool restore_codec_grid = (fixture & 1u) != 0;

		CHECK(prefill_delays(&actual, &expected) &&
		      v92_su_init(&su, TEST_LU) == V92_SU_OK &&
		      reach_jp_wait(
			      &su, &actual, &expected, &pattern_phase) &&
		      reach_jp_prime_wait(
			      &su, &actual, &expected, &pattern_phase,
			      fixtures[fixture].extension) &&
		      v92_su_get_phase(&su, &phase) &&
		      phase == fixtures[fixture].phase_q16,
		      "extension 0x%04x selected phase %d instead of %d",
		      fixtures[fixture].extension, phase,
		      fixtures[fixture].phase_q16);
		CHECK(v92_su_accept_jp_prime(
			      &su, &actual, restore_codec_grid) ==
			      V92_SU_COMPLETE &&
		      actual.phase_q16 ==
			      (restore_codec_grid ? 0 :
			       fixtures[fixture].phase_q16),
		      "extension 0x%04x used the wrong continuation phase",
		      fixtures[fixture].extension);
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct fractional_delay delay;
	struct fractional_delay delay_before;
	struct v92_su su;
	struct v92_su su_before;
	struct v92_jp invalid_jp = jp(0x4000);
	struct v92_jp valid_jp = jp(0x4000);
	uint8_t pattern_phase = 0;
	int16_t output = 1234;

	fractional_delay_init(&delay);
	CHECK(v92_su_init(&su, TEST_LU) == V92_SU_OK,
	      "Su validation fixture did not initialize");
	su_before = su;
	delay_before = delay;
	invalid_jp.training_constellation = (enum v92_constellation_size)6;
	CHECK(v92_su_init(NULL, TEST_LU) == V92_SU_INVALID_ARGUMENT &&
	      v92_su_init(&su, 0) == V92_SU_INVALID_ARGUMENT &&
	      v92_su_init(&su, INT16_MIN) == V92_SU_INVALID_ARGUMENT &&
	      memcmp(&su, &su_before, sizeof(su)) == 0 &&
	      v92_su_next(NULL, &delay, &output) ==
		      V92_SU_INVALID_ARGUMENT &&
	      v92_su_next(&su, NULL, &output) ==
		      V92_SU_INVALID_ARGUMENT &&
	      v92_su_next(&su, &delay, NULL) ==
		      V92_SU_INVALID_ARGUMENT &&
	      v92_su_accept_jp(&su, &valid_jp) ==
		      V92_SU_INVALID_STATE &&
	      v92_su_accept_jp_prime(&su, &delay, false) ==
		      V92_SU_INVALID_STATE &&
	      memcmp(&su, &su_before, sizeof(su)) == 0 &&
	      memcmp(&delay, &delay_before, sizeof(delay)) == 0 &&
	      output == 1234,
	      "Su rejected-call transactionality failed");
	CHECK(reach_jp_wait(
		      &su, &delay, &delay_before, &pattern_phase),
	      "Su validation fixture did not reach Jp wait");
	su_before = su;
	CHECK(v92_su_accept_jp(&su, &invalid_jp) ==
		      V92_SU_INVALID_ARGUMENT &&
	      memcmp(&su, &su_before, sizeof(su)) == 0 &&
	      v92_su_accept_jp(&su, &valid_jp) == V92_SU_OK &&
	      v92_su_accept_jp(&su, &valid_jp) ==
		      V92_SU_INVALID_STATE,
	      "Su accepted malformed or duplicate Jp");
	su_before = su;
	delay_before = delay;
	delay.coefficients_q15[0]++;
	output = 1234;
	CHECK(v92_su_next(&su, &delay, &output) ==
		      V92_SU_INVALID_STATE &&
	      memcmp(&su, &su_before, sizeof(su)) == 0 &&
	      output == 1234,
	      "Su consumed a sample with invalid delay state");
	delay = delay_before;
	CHECK(strcmp(v92_su_stage_name(V92_SU_JP_WAIT), "Jp wait") == 0 &&
	      strcmp(v92_su_stage_name((enum v92_su_stage)99), "unknown") ==
		      0 &&
	      strcmp(v92_su_result_name(V92_SU_FILTER_ERROR),
		     "fractional delay failed") == 0 &&
	      strcmp(v92_su_result_name((enum v92_su_result)99), "unknown") ==
		      0,
	      "Su diagnostics were incomplete");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous Su timeline", test_continuous_su_timeline },
		{ "phase extension and codec grid",
		  test_phase_extension_and_codec_grid },
		{ "transactional validation", test_validation_is_transactional },
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
