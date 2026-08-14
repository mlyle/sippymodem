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

static bool test_law_validation(void)
{
	CHECK(g711_law_is_valid(G711_LAW_PCMU), "PCMU law was rejected");
	CHECK(g711_law_is_valid(G711_LAW_PCMA), "PCMA law was rejected");
	CHECK(!g711_law_is_valid((enum g711_law)-1),
	      "negative law was accepted");
	CHECK(!g711_law_is_valid((enum g711_law)2),
	      "out-of-range law was accepted");
	return true;
}

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
			g711_encode_pcmu(samples[index].linear);
		CHECK(encoded == samples[index].encoded,
		      "PCMU encoded %d as %#02x, expected %#02x",
		      samples[index].linear, encoded, samples[index].encoded);
		CHECK(g711_decode_pcmu(encoded) ==
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
			g711_encode_pcma(samples[index].linear);
		CHECK(encoded == samples[index].encoded,
		      "PCMA encoded %d as %#02x, expected %#02x",
		      samples[index].linear, encoded, samples[index].encoded);
		CHECK(g711_decode_pcma(encoded) ==
			      samples[index].decoded,
		      "PCMA decoded %#02x incorrectly", encoded);
	}
	return true;
}

static bool test_pcmu_code_space(void)
{
	for (unsigned int code = 0; code <= UINT8_MAX; ++code) {
		int16_t sample = g711_decode_pcmu((uint8_t)code);
		uint8_t encoded = g711_encode_pcmu(sample);
		uint8_t expected = code == 0x7f ? 0xff : (uint8_t)code;

		CHECK(encoded == expected,
		      "PCMU code %#02x round-tripped as %#02x", code, encoded);
		CHECK(g711_decode_pcmu((uint8_t)(code ^ 0x80)) ==
			      -sample,
		      "PCMU code %#02x has asymmetric sign", code);
	}
	return true;
}

static bool test_pcma_code_space(void)
{
	for (unsigned int code = 0; code <= UINT8_MAX; ++code) {
		int16_t sample = g711_decode_pcma((uint8_t)code);
		uint8_t encoded = g711_encode_pcma(sample);

		CHECK(encoded == code,
		      "PCMA code %#02x round-tripped as %#02x", code, encoded);
		CHECK(g711_decode_pcma((uint8_t)(code ^ 0x80)) ==
			      -sample,
		      "PCMA code %#02x has asymmetric sign", code);
	}
	return true;
}

static bool test_all_linear_samples(void)
{
	for (int32_t sample = INT16_MIN; sample <= INT16_MAX; ++sample) {
		uint8_t pcmu = g711_encode_pcmu((int16_t)sample);
		uint8_t pcma = g711_encode_pcma((int16_t)sample);
		int16_t pcmu_linear = g711_decode_pcmu(pcmu);
		int16_t pcma_linear = g711_decode_pcma(pcma);
		uint8_t pcmu_round_trip =
			g711_encode_pcmu(pcmu_linear);

		/* Both PCMU zero codewords decode to linear zero, so ordinary
		 * signed PCM necessarily canonicalizes negative zero to 0xff. */
		CHECK(pcmu_round_trip == pcmu ||
			      (pcmu == 0x7f && pcmu_round_trip == 0xff),
		      "PCMU encoding is unstable at %d", (int)sample);
		CHECK(g711_encode_pcma(pcma_linear) == pcma,
		      "PCMA encoding is unstable at %d", (int)sample);
	}
	return true;
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ? g711_encode_pcma(sample) :
					   g711_encode_pcmu(sample);
}

static bool test_ucode_space(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (unsigned int code = 0; code < 128; ++code) {
			for (unsigned int sign = 0; sign < 2; ++sign) {
				uint8_t octet = 0;
				int16_t sample = 0;
				uint8_t positive = law == G711_LAW_PCMA ?
					(uint8_t)(code ^ 0xd5u) :
					(uint8_t)~code;
				uint8_t expected = sign != 0 ? positive :
					(uint8_t)(positive ^ 0x80u);

				CHECK(g711_ucode_octet(
					      law, (uint8_t)code, sign != 0,
					      &octet) &&
					      octet == expected,
				      "law %u Ucode %u sign %u produced %#02x",
				      (unsigned int)law, code, sign, octet);
				CHECK(g711_ucode_sample(
					      law, (uint8_t)code, sign != 0,
					      &sample) &&
					      encode_sample(law, sample) == expected,
				      "law %u Ucode %u sign %u lost exact octet",
				      (unsigned int)law, code, sign);
			}
		}
	}
	return true;
}

static bool test_ucode_boundaries(void)
{
	static const struct {
		enum g711_law law;
		uint8_t ucode;
		uint8_t octet;
		int16_t sample;
	} cases[] = {
		{ G711_LAW_PCMU, 0, 0xff, 0 },
		{ G711_LAW_PCMU, 16, 0xef, 132 },
		{ G711_LAW_PCMU, 127, 0x80, 32124 },
		{ G711_LAW_PCMA, 0, 0xd5, 8 },
		{ G711_LAW_PCMA, 16, 0xc5, 264 },
		{ G711_LAW_PCMA, 127, 0xaa, 32256 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		uint8_t octet = 0;
		int16_t sample = 0;

		CHECK(g711_ucode_octet(
			      cases[index].law, cases[index].ucode, true,
			      &octet) &&
			      octet == cases[index].octet,
		      "Ucode boundary %zu has the wrong octet", index);
		CHECK(g711_ucode_sample(
			      cases[index].law, cases[index].ucode, true,
			      &sample) &&
			      sample == cases[index].sample,
		      "Ucode boundary %zu has the wrong level", index);
	}
	return true;
}

static bool test_ucode_rejection_is_transactional(void)
{
	uint8_t octet = 0xa5;
	int16_t sample = 1234;

	CHECK(!g711_ucode_octet(G711_LAW_PCMU, 128, true, &octet) &&
		      octet == 0xa5,
	      "invalid Ucode changed the octet output");
	CHECK(!g711_ucode_sample((enum g711_law)2, 0, true, &sample) &&
		      sample == 1234,
	      "invalid law changed the sample output");
	CHECK(!g711_ucode_octet(G711_LAW_PCMA, 0, true, NULL) &&
		      !g711_ucode_sample(G711_LAW_PCMA, 0, true, NULL),
	      "NULL Ucode outputs were accepted");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "law validation", test_law_validation },
		{ "PCMU known values", test_pcmu_known_values },
		{ "PCMA known values", test_pcma_known_values },
		{ "PCMU code space", test_pcmu_code_space },
		{ "PCMA code space", test_pcma_code_space },
		{ "all linear samples", test_all_linear_samples },
		{ "V.90 Ucode space", test_ucode_space },
		{ "V.90 Ucode boundaries", test_ucode_boundaries },
		{ "V.90 Ucode rejection", test_ucode_rejection_is_transactional },
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
