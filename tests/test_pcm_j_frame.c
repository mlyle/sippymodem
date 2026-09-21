// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "pcm_j_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t frame_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >> (7u - index % 8u)) & 1u);
}

static void flip_bit(uint8_t *frame, size_t index)
{
	frame[index / 8u] ^= (uint8_t)(1u << (7u - index % 8u));
}

static bool test_wire_image(void)
{
	static const uint8_t expected[PCM_J_FRAME_BYTES] = {
		0xff, 0xff, 0x96, 0x96, 0x8a, 0x81, 0x67, 0x34, 0xf0,
	};
	const uint32_t payload = UINT32_C(0xd02a5a5a);
	uint8_t frame[PCM_J_FRAME_BYTES];
	uint32_t decoded = 0;

	CHECK(PCM_J_FRAME_SYNC_BITS == 17 &&
	      PCM_J_FRAME_PAYLOAD_BITS == 32 && PCM_J_FRAME_BITS == 72 &&
	      PCM_J_FRAME_BYTES == 9 &&
	      pcm_j_frame_encode(payload, frame, sizeof(frame)) ==
		      PCM_J_FRAME_OK &&
	      memcmp(frame, expected, sizeof(frame)) == 0 &&
	      pcm_j_frame_decode(frame, sizeof(frame), &decoded) ==
		      PCM_J_FRAME_OK &&
	      decoded == payload,
	      "shared J envelope wire image changed");
	return true;
}

static bool test_generated_payloads(void)
{
	static const uint32_t payloads[] = {
		UINT32_C(0),
		UINT32_C(1),
		UINT32_C(0x80000000),
		UINT32_C(0xa5c33ca5),
		UINT32_MAX,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(payloads); ++index) {
		uint8_t frame[PCM_J_FRAME_BYTES];
		uint32_t decoded = ~payloads[index];

		CHECK(pcm_j_frame_encode(
			      payloads[index], frame, sizeof(frame)) ==
			      PCM_J_FRAME_OK &&
		      pcm_j_frame_decode(frame, sizeof(frame), &decoded) ==
			      PCM_J_FRAME_OK &&
		      decoded == payloads[index],
		      "payload 0x%08x did not round trip",
		      payloads[index]);
		for (size_t bit = 0; bit < PCM_J_FRAME_SYNC_BITS; ++bit)
			CHECK(frame_bit(frame, bit) == 1,
			      "payload %zu lost sync bit %zu", index, bit);
		CHECK(frame_bit(frame, 17) == 0 &&
		      frame_bit(frame, 34) == 0 &&
		      frame_bit(frame, 51) == 0 &&
		      frame_bit(frame, 68) == 0 &&
		      frame_bit(frame, 71) == 0,
		      "payload %zu changed fixed framing", index);
	}
	return true;
}

static bool test_rejection(void)
{
	static const size_t framing_bits[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
		17, 34, 51, 68,
	};
	uint8_t frame[PCM_J_FRAME_BYTES];
	uint8_t malformed[PCM_J_FRAME_BYTES];
	uint32_t decoded = UINT32_C(0x12345678);
	const uint32_t saved = decoded;

	CHECK(pcm_j_frame_encode(
		      UINT32_C(0xa5c33ca5), frame, sizeof(frame)) ==
		      PCM_J_FRAME_OK,
	      "rejection fixture did not encode");
	memcpy(malformed, frame, sizeof(malformed));
	flip_bit(malformed, 18);
	CHECK(pcm_j_frame_decode(malformed, sizeof(malformed), &decoded) ==
		      PCM_J_FRAME_CRC_MISMATCH &&
	      decoded == saved,
	      "corrupt payload was accepted or published");
	for (size_t index = 0; index < ARRAY_LENGTH(framing_bits); ++index) {
		memcpy(malformed, frame, sizeof(malformed));
		flip_bit(malformed, framing_bits[index]);
		CHECK(pcm_j_frame_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      PCM_J_FRAME_INVALID_FRAME &&
		      decoded == saved,
		      "malformed fixed bit %zu was accepted",
		      framing_bits[index]);
	}
	return true;
}

static bool test_contracts(void)
{
	uint8_t frame[PCM_J_FRAME_BYTES + 1u];
	uint8_t saved_frame[sizeof(frame)];
	uint32_t payload = UINT32_C(0x12345678);

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	CHECK(pcm_j_frame_encode(0, NULL, PCM_J_FRAME_BYTES) ==
		      PCM_J_FRAME_INVALID_ARGUMENT &&
	      pcm_j_frame_encode(0, frame, PCM_J_FRAME_BYTES - 1u) ==
		      PCM_J_FRAME_INVALID_ARGUMENT &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid encode changed output");
	CHECK(pcm_j_frame_decode(NULL, PCM_J_FRAME_BYTES, &payload) ==
		      PCM_J_FRAME_INVALID_ARGUMENT &&
	      pcm_j_frame_decode(frame, PCM_J_FRAME_BYTES, NULL) ==
		      PCM_J_FRAME_INVALID_ARGUMENT &&
	      pcm_j_frame_decode(
		      frame, PCM_J_FRAME_BYTES - 1u, &payload) ==
		      PCM_J_FRAME_INVALID_ARGUMENT &&
	      payload == UINT32_C(0x12345678) &&
	      strcmp(pcm_j_frame_result_name(PCM_J_FRAME_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(pcm_j_frame_result_name(
			     (enum pcm_j_frame_result)99), "unknown") == 0,
	      "decode contract or result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "wire image", test_wire_image },
		{ "generated payloads", test_generated_payloads },
		{ "rejection", test_rejection },
		{ "contracts", test_contracts },
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
