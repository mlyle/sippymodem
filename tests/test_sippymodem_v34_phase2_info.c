// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase2_info.h>

#include "test.h"

#include <string.h>

static void store_packed_bit(uint8_t *packed, size_t index, uint8_t bit)
{
	packed[index / 8u] |= (uint8_t)(bit << (7u - index % 8u));
}

struct receive_capture {
	struct sippymodem_v34_phase2_info_event last;
	size_t received_bit;
	size_t rejected_bit;
	unsigned int received;
	unsigned int rejected;
};

static bool feed_frame(
	struct sippymodem_v34_phase2_info_receiver *receiver,
	const uint8_t *payload, size_t payload_bit_count, size_t flipped_bit,
	struct receive_capture *capture)
{
	struct sippymodem_v34_phase2_info_generator generator;
	size_t frame_bit_count =
		sippymodem_v34_phase2_info_frame_bit_count(payload_bit_count);

	CHECK(receiver != NULL && capture != NULL && frame_bit_count != 0 &&
	      sippymodem_v34_phase2_info_generator_init(
		      &generator, payload, payload_bit_count) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK,
	      "receive feed fixture did not initialize");
	for (size_t index = 0; index < frame_bit_count; ++index) {
		struct sippymodem_v34_phase2_info_event event;
		uint8_t bit;
		bool complete;

		CHECK(sippymodem_v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_OK,
		      "receive feed generation failed at %zu", index);
		if (index == flipped_bit)
			bit ^= 1;
		CHECK(sippymodem_v34_phase2_info_receive_bit(
			      receiver, bit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INFO_OK,
		      "receive feed failed at %zu", index);
		if (event.type == SIPPYMODEM_V34_PHASE2_INFO_FRAME_RECEIVED) {
			capture->received++;
			capture->received_bit = index;
			capture->last = event;
		} else if (event.type ==
			   SIPPYMODEM_V34_PHASE2_INFO_FRAME_REJECTED) {
			capture->rejected++;
			capture->rejected_bit = index;
		}
	}
	return true;
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

static bool test_receive_round_trip(void)
{
	static const uint8_t base_payload[] = { 0xff, 0x84, 0x00 };
	static const uint8_t maximum_payload[] = {
		0xa5, 0x5a, 0xc3, 0x3c, 0xf0,
		0x0f, 0x96, 0x69, 0x80, 0xa8,
	};
	static const uint8_t noise[] = { 0, 1, 1, 0, 1, 0, 0, 1, 0 };
	struct sippymodem_v34_phase2_info_receiver receiver;
	struct receive_capture capture = { 0 };

	CHECK(sippymodem_v34_phase2_info_receiver_init(&receiver, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK,
	      "base INFO0 receiver did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(noise); ++index) {
		struct sippymodem_v34_phase2_info_event event;

		CHECK(sippymodem_v34_phase2_info_receive_bit(
			      &receiver, noise[index], &event) ==
			      SIPPYMODEM_V34_PHASE2_INFO_OK &&
		      event.type == SIPPYMODEM_V34_PHASE2_INFO_NO_EVENT,
		      "noise produced a Phase 2 INFO event at %zu", index);
	}
	CHECK(feed_frame(&receiver, base_payload, 17, SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 0 &&
	      capture.received_bit ==
		      sippymodem_v34_phase2_info_frame_bit_count(17) - 1u &&
	      capture.last.payload_bit_count == 17 &&
	      memcmp(capture.last.payload, base_payload, 2) == 0 &&
	      capture.last.payload[2] == 0 &&
	      sippymodem_v34_phase2_info_receiver_is_valid(&receiver) &&
	      receiver.stage == SIPPYMODEM_V34_PHASE2_INFO_SEARCH_SYNC,
	      "base INFO0 frame did not round trip");

	memset(&capture, 0, sizeof(capture));
	CHECK(sippymodem_v34_phase2_info_receiver_init(
		      &receiver,
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      feed_frame(
		      &receiver, maximum_payload,
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS,
		      SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 0 &&
	      capture.last.payload_bit_count ==
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS &&
	      memcmp(capture.last.payload, maximum_payload,
		     sizeof(maximum_payload)) == 0,
	      "maximum Phase 2 INFO frame did not round trip");

	memset(&capture, 0, sizeof(capture));
	CHECK(sippymodem_v34_phase2_info_receiver_init(&receiver, 0) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      feed_frame(&receiver, NULL, 0, SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 0 &&
	      capture.last.payload_bit_count == 0,
	      "empty Phase 2 INFO frame did not round trip");
	return true;
}

static bool test_receive_reacquisition(void)
{
	static const uint8_t payload[] = { 0xff, 0x84, 0x00 };
	struct sippymodem_v34_phase2_info_receiver receiver;
	struct receive_capture capture = { 0 };
	size_t frame_bit_count =
		sippymodem_v34_phase2_info_frame_bit_count(17);

	CHECK(sippymodem_v34_phase2_info_receiver_init(&receiver, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      feed_frame(
		      &receiver, payload, 17,
		      SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS + 3u, &capture) &&
	      capture.received == 0 && capture.rejected == 1 &&
	      capture.rejected_bit ==
		      SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS + 17u +
			      SIPPYMODEM_V34_PHASE2_INFO_CRC_BITS - 1u &&
	      feed_frame(&receiver, payload, 17, SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 1 &&
	      capture.received_bit == frame_bit_count - 1u,
	      "receiver did not reacquire after a CRC rejection");
	CHECK(feed_frame(
		      &receiver, payload, 17, frame_bit_count - 1u,
		      &capture) &&
	      capture.received == 1 && capture.rejected == 2 &&
	      capture.rejected_bit == frame_bit_count - 1u &&
	      feed_frame(&receiver, payload, 17, SIZE_MAX, &capture) &&
	      capture.received == 2 && capture.rejected == 2 &&
	      capture.last.payload_bit_count == 17 &&
	      memcmp(capture.last.payload, payload, 2) == 0,
	      "receiver did not reacquire after an end-marker rejection");
	return true;
}

static bool test_receiver_validation(void)
{
	struct sippymodem_v34_phase2_info_receiver receiver;
	struct sippymodem_v34_phase2_info_receiver saved;
	struct sippymodem_v34_phase2_info_event event = {
		.type = SIPPYMODEM_V34_PHASE2_INFO_FRAME_RECEIVED,
		.payload = { 0xa5 },
		.payload_bit_count = 1,
	};
	struct sippymodem_v34_phase2_info_event saved_event = event;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(sippymodem_v34_phase2_info_receiver_init(NULL, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_receiver_init(
		      &receiver,
		      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "receiver initialization accepted an invalid bound");
	CHECK(sippymodem_v34_phase2_info_receiver_init(&receiver, 17) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK,
	      "receiver validation fixture did not initialize");
	saved = receiver;
	CHECK(sippymodem_v34_phase2_info_receive_bit(
		      NULL, 0, &event) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_receive_bit(
		      &receiver, 2, &event) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_info_receive_bit(
		      &receiver, 0, NULL) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "receiver mutated state on invalid input");

	receiver.stage =
		(enum sippymodem_v34_phase2_info_receive_stage)4;
	CHECK(!sippymodem_v34_phase2_info_receiver_is_valid(&receiver) &&
	      sippymodem_v34_phase2_info_receiver_reset(&receiver) ==
		      SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT,
	      "receiver accepted a corrupt stage");
	receiver = saved;
	receiver.crc ^= 1;
	CHECK(!sippymodem_v34_phase2_info_receiver_is_valid(&receiver),
	      "receiver accepted a corrupt CRC state");
	receiver = saved;
	CHECK(sippymodem_v34_phase2_info_receive_bit(
		      &receiver, 1, &event) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      sippymodem_v34_phase2_info_receiver_reset(&receiver) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK &&
	      receiver.sync_window == 0 &&
	      receiver.stage == SIPPYMODEM_V34_PHASE2_INFO_SEARCH_SYNC &&
	      sippymodem_v34_phase2_info_receiver_is_valid(&receiver) &&
	      !sippymodem_v34_phase2_info_receiver_is_valid(NULL),
	      "receiver reset did not clear acquisition state");
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
		{ "receive round trip", test_receive_round_trip },
		{ "receive reacquisition", test_receive_reacquisition },
		{ "receiver validation", test_receiver_validation },
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
