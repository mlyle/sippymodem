// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase2_info.h>

#include "test.h"

#include <string.h>

static void store_packed_bit(uint8_t *packed, size_t index, uint8_t bit)
{
	packed[index / 8u] |= (uint8_t)(bit << (7u - index % 8u));
}

static bool test_base_info0_frame(void)
{
	static const uint8_t payload[] = { 0xff, 0x84, 0x00 };
	static const uint8_t expected[] = {
		0xff, 0x72, 0xff, 0x84, 0x47, 0xec, 0xf8,
	};
	struct sippymodem_v34_phase2_info_generator generator;
	uint8_t encoded[ARRAY_LENGTH(expected)] = { 0 };
	size_t frame_bit_count =
		sippymodem_v34_phase2_info_frame_bit_count(17);

	CHECK(frame_bit_count == 53 &&
	      sippymodem_v34_phase2_info_generator_init(
		      &generator, payload, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      sippymodem_v34_phase2_info_generator_is_valid(&generator) &&
	      generator.crc == UINT16_C(0x8fd9),
	      "base INFO0 fixture did not initialize");
	for (size_t index = 0; index < frame_bit_count; ++index) {
		uint8_t bit = UINT8_MAX;
		bool complete = true;

		CHECK(sippymodem_v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_OK &&
		      bit <= 1 && complete == (index + 1u == frame_bit_count),
		      "base INFO0 bit changed at %zu", index);
		store_packed_bit(encoded, index, bit);
	}
	CHECK(memcmp(encoded, expected, sizeof(expected)) == 0 &&
	      generator.bit_index == frame_bit_count &&
	      sippymodem_v34_phase2_info_generator_is_valid(&generator),
	      "base INFO0 wire image changed");
	{
		struct sippymodem_v34_phase2_info_generator completed = generator;
		uint8_t bit = UINT8_MAX;
		bool complete = false;

		CHECK(sippymodem_v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
		      memcmp(&generator, &completed, sizeof(generator)) == 0 &&
		      bit == UINT8_MAX && !complete,
		      "completed INFO0 frame emitted an extra bit");
	}
	return true;
}

static bool test_crc_and_bounds(void)
{
	static const uint8_t maximum_payload[] = {
		0xa5, 0x5a, 0xc3, 0x3c, 0xf0,
		0x0f, 0x96, 0x69, 0x80, 0x00,
	};
	static const uint8_t one_bit[] = { 0x80 };
	struct sippymodem_v34_phase2_info_generator generator;
	uint16_t crc = 0;
	size_t frame_bit_count = sippymodem_v34_phase2_info_frame_bit_count(
		SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS);

	CHECK(sippymodem_v34_phase2_info_crc(NULL, 0, &crc) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      crc == UINT16_C(0xffff) &&
	      sippymodem_v34_phase2_info_crc(one_bit, 1, &crc) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      crc == UINT16_C(0xfffe) &&
	      sippymodem_v34_phase2_info_crc(
		      maximum_payload,
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS, &crc) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      crc == UINT16_C(0x93d5) &&
	      frame_bit_count ==
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_FRAME_BITS &&
	      sippymodem_v34_phase2_info_frame_bit_count(
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) == 0,
	      "Phase 2 INFO CRC or bounds changed");
	CHECK(sippymodem_v34_phase2_info_generator_init(
		      &generator, maximum_payload,
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK,
	      "maximum Phase 2 INFO fixture did not initialize");
	for (size_t index = 0; index < frame_bit_count; ++index) {
		uint8_t bit;
		bool complete;

		CHECK(sippymodem_v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_OK &&
		      complete == (index + 1u == frame_bit_count),
		      "maximum Phase 2 INFO ended at %zu", index);
	}
	CHECK(sippymodem_v34_phase2_info_generator_reset(&generator) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      generator.bit_index == 0 &&
	      generator.crc == UINT16_C(0x93d5),
	      "Phase 2 INFO reset changed owned payload state");
	return true;
}

static bool test_validation(void)
{
	static const uint8_t payload[] = { 0xff, 0x84, 0xff };
	struct sippymodem_v34_phase2_info_generator generator;
	struct sippymodem_v34_phase2_info_generator saved;
	uint8_t bit = UINT8_MAX;
	bool complete = true;
	uint16_t crc = UINT16_MAX;

	memset(&generator, 0xa5, sizeof(generator));
	saved = generator;
	CHECK(sippymodem_v34_phase2_info_generator_init(
		      NULL, payload, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_generator_init(
		      &generator, NULL, 1) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_generator_init(
		      &generator, payload,
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0 &&
	      sippymodem_v34_phase2_info_crc(NULL, 1, &crc) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_crc(payload, 17, NULL) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      crc == UINT16_MAX,
	      "Phase 2 INFO initialization accepted invalid input");

	CHECK(sippymodem_v34_phase2_info_generator_init(
		      &generator, payload, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK,
	      "Phase 2 INFO validation fixture did not initialize");
	saved = generator;
	CHECK(sippymodem_v34_phase2_info_next_bit(NULL, &bit, &complete) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_next_bit(
		      &generator, NULL, &complete) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_next_bit(
		      &generator, &bit, NULL) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0 &&
	      bit == UINT8_MAX && complete,
	      "Phase 2 INFO emission mutated state on invalid input");

	generator.payload[2] |= 1;
	CHECK(!sippymodem_v34_phase2_info_generator_is_valid(&generator) &&
	      sippymodem_v34_phase2_info_generator_reset(&generator) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT,
	      "Phase 2 INFO accepted noncanonical payload padding");
	generator = saved;
	generator.crc ^= 1;
	CHECK(!sippymodem_v34_phase2_info_generator_is_valid(&generator),
	      "Phase 2 INFO accepted a corrupt CRC");
	generator = saved;
	generator.bit_index =
		sippymodem_v34_phase2_info_frame_bit_count(17) + 1u;
	CHECK(!sippymodem_v34_phase2_info_generator_is_valid(&generator) &&
	      !sippymodem_v34_phase2_info_generator_is_valid(NULL) &&
	      strcmp(sippymodem_v34_phase2_info_result_name(
		     SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_phase2_info_result_name(
		     (enum sippymodem_v34_phase2_info_result)2),
		     "unknown") == 0,
	      "Phase 2 INFO accepted corrupt state or result");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "base INFO0 frame", test_base_info0_frame },
		{ "CRC and bounds", test_crc_and_bounds },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
