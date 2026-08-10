// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v8_framing.h>

#include "test.h"

#include <string.h>

static bool test_control_sequence(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };
	static const char expected[] =
		"1111111111" "0000001111"
		"0100000111" "0010101001";
	struct sippymodem_v8_control_generator generator;

	CHECK(strlen(expected) ==
		      SIPPYMODEM_V8_PREAMBLE_BITS +
			      ARRAY_LENGTH(information) *
				      SIPPYMODEM_V8_FRAMED_OCTET_BITS &&
	      sippymodem_v8_control_generator_init(
		      &generator, information, ARRAY_LENGTH(information)) ==
		      SIPPYMODEM_V8_FRAMING_OK,
	      "V.8 control generator did not initialize");
	for (size_t repeat = 0; repeat < 2; ++repeat) {
		for (size_t index = 0; index < strlen(expected); ++index) {
			bool complete = true;
			uint8_t bit = UINT8_MAX;

			CHECK(sippymodem_v8_control_next_bit(
				      &generator, &bit, &complete) ==
				      SIPPYMODEM_V8_FRAMING_OK &&
				      bit == (uint8_t)(expected[index] - '0') &&
				      complete == (index + 1u == strlen(expected)),
			      "V.8 control repeat %zu changed bit %zu",
			      repeat, index);
		}
		CHECK(generator.bit_index == 0,
		      "V.8 control repeat did not return to its boundary");
	}
	return true;
}

static bool test_owned_bounded_information(void)
{
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	struct sippymodem_v8_control_generator generator;
	bool complete = false;
	uint8_t bit = UINT8_MAX;

	for (size_t index = 0; index < ARRAY_LENGTH(information); ++index)
		information[index] = (uint8_t)(index * 17u);
	CHECK(sippymodem_v8_control_generator_init(
		      &generator, information, ARRAY_LENGTH(information)) ==
		      SIPPYMODEM_V8_FRAMING_OK,
	      "V.8 maximum control sequence did not initialize");
	memset(information, UINT8_MAX, sizeof(information));
	for (size_t index = 0;
	     index < SIPPYMODEM_V8_PREAMBLE_BITS +
			     SIPPYMODEM_V8_MAX_INFORMATION_OCTETS *
				     SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	     ++index) {
		CHECK(sippymodem_v8_control_next_bit(
			      &generator, &bit, &complete) ==
			      SIPPYMODEM_V8_FRAMING_OK &&
			      complete ==
				      (index + 1u ==
				       SIPPYMODEM_V8_PREAMBLE_BITS +
					       SIPPYMODEM_V8_MAX_INFORMATION_OCTETS *
						       SIPPYMODEM_V8_FRAMED_OCTET_BITS),
		      "V.8 maximum control sequence failed at bit %zu", index);
	}
	CHECK(generator.information[0] == 0 &&
		      generator.information[1] == 17 &&
		      generator.bit_index == 0,
	      "V.8 control generator did not own its bounded information");
	return true;
}

static bool test_cj_sequence(void)
{
	for (size_t index = 0; index < SIPPYMODEM_V8_CJ_BITS; ++index) {
		uint8_t bit = UINT8_MAX;
		bool expected = index == 9 || index == 19 || index == 29;

		CHECK(sippymodem_v8_cj_bit(index, &bit) ==
			      SIPPYMODEM_V8_FRAMING_OK &&
			      bit == expected,
		      "V.8 CJ changed bit %zu", index);
	}
	return true;
}

static bool test_validation(void)
{
	uint8_t too_many[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS + 1] = { 0 };
	uint8_t information = UINT8_C(0xc1);
	struct sippymodem_v8_control_generator generator;
	struct sippymodem_v8_control_generator saved;
	bool complete = true;
	uint8_t bit = UINT8_MAX;

	CHECK(sippymodem_v8_control_generator_init(
		      &generator, &information, 1) ==
		      SIPPYMODEM_V8_FRAMING_OK,
	      "V.8 validation fixture did not initialize");
	saved = generator;
	CHECK(sippymodem_v8_control_generator_init(NULL, &information, 1) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_control_generator_init(&generator, NULL, 1) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_control_generator_init(
		      &generator, &information, 0) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_control_generator_init(
		      &generator, too_many, ARRAY_LENGTH(too_many)) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0,
	      "V.8 control initialization accepted invalid input");
	generator.bit_index =
		SIPPYMODEM_V8_PREAMBLE_BITS +
		SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	saved = generator;
	CHECK(sippymodem_v8_control_next_bit(
		      &generator, &bit, &complete) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0 &&
	      bit == UINT8_MAX && complete &&
	      sippymodem_v8_control_next_bit(NULL, &bit, &complete) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_control_next_bit(&saved, NULL, &complete) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_control_next_bit(&saved, &bit, NULL) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_cj_bit(SIPPYMODEM_V8_CJ_BITS, &bit) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_cj_bit(0, NULL) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      strcmp(sippymodem_v8_framing_result_name(
			     SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "V.8 framing accepted invalid state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "control sequence", test_control_sequence },
		{ "owned bounded information", test_owned_bounded_information },
		{ "CJ sequence", test_cj_sequence },
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
