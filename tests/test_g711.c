// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>

struct codec_sample {
	int16_t linear;
	uint8_t encoded;
	int16_t decoded;
};

static bool test_pcmu_known_values(void)
{
	static const struct codec_sample samples[] = {
		{ 0, 0xff, 0 },
		{ -1, 0x7f, 0 },
		{ 4, 0xfe, 8 },
		{ -4, 0x7e, -8 },
		{ 1000, 0xce, 988 },
		{ -1000, 0x4e, -988 },
		{ 32124, 0x80, 32124 },
		{ -32124, 0x00, -32124 },
		{ INT16_MAX, 0x80, 32124 },
		{ INT16_MIN, 0x00, -32124 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
		uint8_t encoded =
			sippymodem_g711_encode_pcmu(samples[index].linear);
		CHECK(encoded == samples[index].encoded,
		      "PCMU encoded %d as %#02x, expected %#02x",
		      samples[index].linear, encoded, samples[index].encoded);
		CHECK(sippymodem_g711_decode_pcmu(encoded) ==
			      samples[index].decoded,
		      "PCMU decoded %#02x incorrectly", encoded);
	}
	return true;
}

static bool test_pcma_known_values(void)
{
	static const struct codec_sample samples[] = {
		{ 0, 0xd5, 8 },
		{ -1, 0x55, -8 },
		{ 8, 0xd5, 8 },
		{ -8, 0x55, -8 },
		{ 16, 0xd4, 24 },
		{ -17, 0x54, -24 },
		{ 1000, 0xfa, 1008 },
		{ -1000, 0x7a, -1008 },
		{ INT16_MAX, 0xaa, 32256 },
		{ INT16_MIN, 0x2a, -32256 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(samples); ++index) {
		uint8_t encoded =
			sippymodem_g711_encode_pcma(samples[index].linear);
		CHECK(encoded == samples[index].encoded,
		      "PCMA encoded %d as %#02x, expected %#02x",
		      samples[index].linear, encoded, samples[index].encoded);
		CHECK(sippymodem_g711_decode_pcma(encoded) ==
			      samples[index].decoded,
		      "PCMA decoded %#02x incorrectly", encoded);
	}
	return true;
}

static bool test_pcmu_code_space(void)
{
	for (unsigned int code = 0; code <= UINT8_MAX; ++code) {
		int16_t sample = sippymodem_g711_decode_pcmu((uint8_t)code);
		uint8_t encoded = sippymodem_g711_encode_pcmu(sample);
		uint8_t expected = code == 0x7f ? 0xff : (uint8_t)code;

		CHECK(encoded == expected,
		      "PCMU code %#02x round-tripped as %#02x", code, encoded);
		CHECK(sippymodem_g711_decode_pcmu((uint8_t)(code ^ 0x80)) ==
			      -sample,
		      "PCMU code %#02x has asymmetric sign", code);
	}
	return true;
}

static bool test_pcma_code_space(void)
{
	for (unsigned int code = 0; code <= UINT8_MAX; ++code) {
		int16_t sample = sippymodem_g711_decode_pcma((uint8_t)code);
		uint8_t encoded = sippymodem_g711_encode_pcma(sample);

		CHECK(encoded == code,
		      "PCMA code %#02x round-tripped as %#02x", code, encoded);
		CHECK(sippymodem_g711_decode_pcma((uint8_t)(code ^ 0x80)) ==
			      -sample,
		      "PCMA code %#02x has asymmetric sign", code);
	}
	return true;
}

static bool test_all_linear_samples(void)
{
	for (int32_t sample = INT16_MIN; sample <= INT16_MAX; ++sample) {
		uint8_t pcmu = sippymodem_g711_encode_pcmu((int16_t)sample);
		uint8_t pcma = sippymodem_g711_encode_pcma((int16_t)sample);
		int16_t pcmu_linear = sippymodem_g711_decode_pcmu(pcmu);
		int16_t pcma_linear = sippymodem_g711_decode_pcma(pcma);
		uint8_t pcmu_round_trip =
			sippymodem_g711_encode_pcmu(pcmu_linear);

		/* Both PCMU zero codewords decode to linear zero, so ordinary
		 * signed PCM necessarily canonicalizes negative zero to 0xff. */
		CHECK(pcmu_round_trip == pcmu ||
			      (pcmu == 0x7f && pcmu_round_trip == 0xff),
		      "PCMU encoding is unstable at %d", (int)sample);
		CHECK(sippymodem_g711_encode_pcma(pcma_linear) == pcma,
		      "PCMA encoding is unstable at %d", (int)sample);
	}
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "PCMU known values", test_pcmu_known_values },
		{ "PCMA known values", test_pcma_known_values },
		{ "PCMU code space", test_pcmu_code_space },
		{ "PCMA code space", test_pcma_code_space },
		{ "all linear samples", test_all_linear_samples },
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
