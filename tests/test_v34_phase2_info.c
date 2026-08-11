// SPDX-License-Identifier: BSD-3-Clause

#include <v34_info0.h>
#include <v34_phase2_info.h>

#include "test.h"

#include <string.h>

static void store_packed_bit(uint8_t *packed, size_t index, uint8_t bit)
{
	packed[index / 8u] |= (uint8_t)(bit << (7u - index % 8u));
}

static struct v34_info0 base_info0(bool acknowledged)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 0,
		.cme_modem = false,
		.constellation_1664_supported = true,
		.transmit_clock_source =
			V34_INFO0_CLOCK_INTERNAL,
		.acknowledges_peer_info0 = acknowledged,
	};
}

static bool info0_equal(
	const struct v34_info0 *left,
	const struct v34_info0 *right)
{
	return left->symbol_rate_2743_supported ==
		       right->symbol_rate_2743_supported &&
	       left->symbol_rate_2800_supported ==
		       right->symbol_rate_2800_supported &&
	       left->symbol_rate_3429_supported ==
		       right->symbol_rate_3429_supported &&
	       left->low_carrier_3000_supported ==
		       right->low_carrier_3000_supported &&
	       left->high_carrier_3000_supported ==
		       right->high_carrier_3000_supported &&
	       left->low_carrier_3200_supported ==
		       right->low_carrier_3200_supported &&
	       left->high_carrier_3200_supported ==
		       right->high_carrier_3200_supported &&
	       left->symbol_rate_3429_transmit_permitted ==
		       right->symbol_rate_3429_transmit_permitted &&
	       left->transmit_power_reduction_supported ==
		       right->transmit_power_reduction_supported &&
	       left->maximum_symbol_rate_difference_steps ==
		       right->maximum_symbol_rate_difference_steps &&
	       left->cme_modem == right->cme_modem &&
	       left->constellation_1664_supported ==
		       right->constellation_1664_supported &&
	       left->transmit_clock_source == right->transmit_clock_source &&
	       left->acknowledges_peer_info0 ==
		       right->acknowledges_peer_info0;
}

struct receive_capture {
	struct v34_phase2_info_event last;
	size_t received_bit;
	size_t rejected_bit;
	unsigned int received;
	unsigned int rejected;
};

static bool feed_frame(
	struct v34_phase2_info_receiver *receiver,
	const uint8_t *payload, size_t payload_bit_count, size_t flipped_bit,
	struct receive_capture *capture)
{
	struct v34_phase2_info_generator generator;
	size_t frame_bit_count =
		v34_phase2_info_frame_bit_count(payload_bit_count);

	CHECK(receiver != NULL && capture != NULL && frame_bit_count != 0 &&
	      v34_phase2_info_generator_init(
		      &generator, payload, payload_bit_count) ==
		      V34_PHASE2_INFO_OK,
	      "receive feed fixture did not initialize");
	for (size_t index = 0; index < frame_bit_count; ++index) {
		struct v34_phase2_info_event event;
		uint8_t bit;
		bool complete;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      V34_PHASE2_INFO_OK,
		      "receive feed generation failed at %zu", index);
		if (index == flipped_bit)
			bit ^= 1;
		CHECK(v34_phase2_info_receive_bit(
			      receiver, bit, &event) ==
			      V34_PHASE2_INFO_OK,
		      "receive feed failed at %zu", index);
		if (event.type == V34_PHASE2_INFO_FRAME_RECEIVED) {
			capture->received++;
			capture->received_bit = index;
			capture->last = event;
		} else if (event.type ==
			   V34_PHASE2_INFO_FRAME_REJECTED) {
			capture->rejected++;
			capture->rejected_bit = index;
		}
	}
	return true;
}

static bool test_base_info0_frame(void)
{
	static const uint8_t expected[] = {
		0xf7, 0x2f, 0xf8, 0x44, 0x7e, 0xcf, 0x80,
	};
	struct v34_info0 info0 = base_info0(false);
	struct v34_phase2_info_generator generator;
	uint8_t payload[V34_INFO0_PAYLOAD_BYTES];
	uint8_t encoded[ARRAY_LENGTH(expected)] = { 0 };
	size_t frame_bit_count =
		v34_phase2_info_frame_bit_count(
			V34_INFO0_PAYLOAD_BITS);

	CHECK(V34_INFO0_PAYLOAD_BITS == 17 &&
	      V34_PHASE2_INFO_LEADING_FILL_BITS == 4 &&
	      V34_PHASE2_INFO_SYNC_BITS == 8 &&
	      V34_PHASE2_INFO_PREFIX_BITS == 12 &&
	      v34_info0_encode(
		      &info0, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      frame_bit_count == 49 &&
	      v34_phase2_info_generator_init(
		      &generator, payload,
		      V34_INFO0_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_OK &&
	      v34_phase2_info_generator_is_valid(&generator) &&
	      generator.crc == UINT16_C(0x8fd9),
	      "base INFO0 fixture did not initialize");
	for (size_t index = 0; index < frame_bit_count; ++index) {
		uint8_t bit = UINT8_MAX;
		bool complete = true;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      V34_PHASE2_INFO_OK &&
		      bit <= 1 && complete == (index + 1u == frame_bit_count),
		      "base INFO0 bit changed at %zu", index);
		store_packed_bit(encoded, index, bit);
	}
	CHECK(memcmp(encoded, expected, sizeof(expected)) == 0 &&
	      generator.bit_index == frame_bit_count &&
	      v34_phase2_info_generator_is_valid(&generator),
	      "base INFO0 wire image changed");
	{
		struct v34_phase2_info_generator completed = generator;
		uint8_t bit = UINT8_MAX;
		bool complete = false;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      V34_PHASE2_INFO_INVALID_ARGUMENT &&
		      memcmp(&generator, &completed, sizeof(generator)) == 0 &&
		      bit == UINT8_MAX && !complete,
		      "completed INFO0 frame emitted an extra bit");
	}
	return true;
}

static bool test_info0_codec(void)
{
	static const uint8_t expected[] = { 0x8d, 0x59, 0x80 };
	struct v34_info0 selected = {
		.symbol_rate_2743_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.maximum_symbol_rate_difference_steps = 5,
		.cme_modem = true,
		.transmit_clock_source =
			V34_INFO0_CLOCK_EXTERNAL,
		.acknowledges_peer_info0 = true,
	};
	struct v34_info0 decoded;
	uint8_t payload[4] = { 0xa5, 0xa5, 0xa5, 0xa5 };

	CHECK(v34_info0_is_valid(&selected) &&
	      v34_info0_encode(
		      &selected, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      memcmp(payload, expected, sizeof(expected)) == 0 &&
	      payload[3] == 0xa5,
	      "INFO0 fields changed transmission order");
	CHECK(v34_info0_decode(
		      payload, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_OK &&
	      info0_equal(&decoded, &selected),
	      "INFO0 fields did not round trip");

	selected = base_info0(true);
	CHECK(v34_info0_encode(
		      &selected, payload, sizeof(payload)) ==
		      V34_INFO0_OK &&
	      payload[0] == 0xff && payload[1] == 0x84 &&
	      payload[2] == 0x80 &&
	      v34_info0_decode(
		      payload, V34_INFO0_PAYLOAD_BYTES,
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_OK &&
	      info0_equal(&decoded, &selected),
	      "base INFO0 acknowledgment did not round trip");
	return true;
}

static bool test_info0_validation(void)
{
	struct v34_info0 valid = base_info0(false);
	struct v34_info0 invalid = valid;
	struct v34_info0 decoded = base_info0(true);
	struct v34_info0 saved_decoded = decoded;
	uint8_t payload[] = { 0xff, 0x84, 0x00, 0xa5 };
	uint8_t saved_payload[ARRAY_LENGTH(payload)];

	memcpy(saved_payload, payload, sizeof(payload));
	invalid.maximum_symbol_rate_difference_steps =
		V34_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS + 1u;
	CHECK(!v34_info0_is_valid(&invalid) &&
	      v34_info0_encode(
		      &invalid, payload, sizeof(payload)) ==
		      V34_INFO0_INVALID_CAPABILITIES &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO0 accepted an invalid symbol-rate difference");
	invalid = valid;
	invalid.transmit_clock_source =
		(enum v34_info0_clock_source)3;
	CHECK(!v34_info0_is_valid(&invalid) &&
	      v34_info0_encode(
		      &invalid, payload, sizeof(payload)) ==
		      V34_INFO0_INVALID_CAPABILITIES &&
	      v34_info0_encode(
		      NULL, payload, sizeof(payload)) ==
		      V34_INFO0_INVALID_ARGUMENT &&
	      v34_info0_encode(
		      &valid, NULL, sizeof(payload)) ==
		      V34_INFO0_INVALID_ARGUMENT &&
	      v34_info0_encode(
		      &valid, payload,
		      V34_INFO0_PAYLOAD_BYTES - 1u) ==
		      V34_INFO0_INVALID_ARGUMENT &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO0 encoding mutated output after invalid input");

	CHECK(v34_info0_decode(
		      NULL, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_INVALID_ARGUMENT &&
	      v34_info0_decode(
		      payload, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS, NULL) ==
		      V34_INFO0_INVALID_ARGUMENT &&
	      v34_info0_decode(
		      payload, V34_INFO0_PAYLOAD_BYTES - 1u,
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_INVALID_ARGUMENT &&
	      v34_info0_decode(
		      payload, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS - 1u, &decoded) ==
		      V34_INFO0_INVALID_PAYLOAD &&
	      info0_equal(&decoded, &saved_decoded),
	      "INFO0 decoding mutated output after invalid input");

	payload[2] = 1;
	CHECK(v34_info0_decode(
		      payload, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_INVALID_PAYLOAD,
	      "INFO0 accepted noncanonical payload padding");
	payload[2] = 0;
	payload[1] = 0xb4;
	CHECK(v34_info0_decode(
		      payload, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_INVALID_PAYLOAD,
	      "INFO0 accepted reserved symbol-rate difference");
	payload[1] = 0x87;
	CHECK(v34_info0_decode(
		      payload, sizeof(payload),
		      V34_INFO0_PAYLOAD_BITS, &decoded) ==
		      V34_INFO0_INVALID_PAYLOAD &&
	      info0_equal(&decoded, &saved_decoded) &&
	      !v34_info0_is_valid(NULL) &&
	      strcmp(v34_info0_result_name(
		     V34_INFO0_INVALID_CAPABILITIES),
		     "invalid capabilities") == 0 &&
	      strcmp(v34_info0_result_name(
		     V34_INFO0_INVALID_PAYLOAD),
		     "invalid payload") == 0 &&
	      strcmp(v34_info0_result_name(
		     (enum v34_info0_result)4),
		     "unknown") == 0,
	      "INFO0 accepted a reserved clock source or result");
	return true;
}

static bool test_crc_and_bounds(void)
{
	static const uint8_t maximum_payload[] = {
		0xa5, 0x5a, 0xc3, 0x3c, 0xf0,
		0x0f, 0x96, 0x69, 0x80, 0x00,
	};
	static const uint8_t one_bit[] = { 0x80 };
	struct v34_phase2_info_generator generator;
	uint16_t crc = 0;
	size_t frame_bit_count = v34_phase2_info_frame_bit_count(
		V34_PHASE2_INFO_MAX_PAYLOAD_BITS);

	CHECK(v34_phase2_info_crc(NULL, 0, &crc) ==
		      V34_PHASE2_INFO_OK &&
	      crc == UINT16_C(0xffff) &&
	      v34_phase2_info_crc(one_bit, 1, &crc) ==
		      V34_PHASE2_INFO_OK &&
	      crc == UINT16_C(0xfffe) &&
	      v34_phase2_info_crc(
		      maximum_payload,
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS, &crc) ==
		      V34_PHASE2_INFO_OK &&
	      crc == UINT16_C(0x93d5) &&
	      frame_bit_count ==
		      V34_PHASE2_INFO_MAX_FRAME_BITS &&
	      v34_phase2_info_frame_bit_count(
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) == 0,
	      "Phase 2 INFO CRC or bounds changed");
	CHECK(v34_phase2_info_generator_init(
		      &generator, maximum_payload,
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_OK,
	      "maximum Phase 2 INFO fixture did not initialize");
	for (size_t index = 0; index < frame_bit_count; ++index) {
		uint8_t bit;
		bool complete;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bit, &complete) ==
			      V34_PHASE2_INFO_OK &&
		      complete == (index + 1u == frame_bit_count),
		      "maximum Phase 2 INFO ended at %zu", index);
	}
	CHECK(v34_phase2_info_generator_reset(&generator) ==
		      V34_PHASE2_INFO_OK &&
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
	struct v34_phase2_info_receiver receiver;
	struct receive_capture capture = { 0 };

	CHECK(v34_phase2_info_receiver_init(&receiver, 17) ==
		      V34_PHASE2_INFO_OK,
	      "base INFO0 receiver did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(noise); ++index) {
		struct v34_phase2_info_event event;

		CHECK(v34_phase2_info_receive_bit(
			      &receiver, noise[index], &event) ==
			      V34_PHASE2_INFO_OK &&
		      event.type == V34_PHASE2_INFO_NO_EVENT,
		      "noise produced a Phase 2 INFO event at %zu", index);
	}
	CHECK(feed_frame(&receiver, base_payload, 17, SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 0 &&
	      capture.received_bit ==
		      v34_phase2_info_frame_bit_count(17) - 1u &&
	      capture.last.payload_bit_count == 17 &&
	      memcmp(capture.last.payload, base_payload, 2) == 0 &&
	      capture.last.payload[2] == 0 &&
	      v34_phase2_info_receiver_is_valid(&receiver) &&
	      receiver.stage == V34_PHASE2_INFO_SEARCH_SYNC,
	      "base INFO0 frame did not round trip");

	memset(&capture, 0, sizeof(capture));
	CHECK(v34_phase2_info_receiver_init(
		      &receiver,
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_OK &&
	      feed_frame(
		      &receiver, maximum_payload,
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS,
		      SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 0 &&
	      capture.last.payload_bit_count ==
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS &&
	      memcmp(capture.last.payload, maximum_payload,
		     sizeof(maximum_payload)) == 0,
	      "maximum Phase 2 INFO frame did not round trip");

	memset(&capture, 0, sizeof(capture));
	CHECK(v34_phase2_info_receiver_init(&receiver, 0) ==
		      V34_PHASE2_INFO_OK &&
	      feed_frame(&receiver, NULL, 0, SIZE_MAX, &capture) &&
	      capture.received == 1 && capture.rejected == 0 &&
	      capture.last.payload_bit_count == 0,
	      "empty Phase 2 INFO frame did not round trip");
	return true;
}

static bool test_receive_reacquisition(void)
{
	static const uint8_t payload[] = { 0xff, 0x84, 0x00 };
	struct v34_phase2_info_receiver receiver;
	struct receive_capture capture = { 0 };
	size_t frame_bit_count =
		v34_phase2_info_frame_bit_count(17);

	CHECK(v34_phase2_info_receiver_init(&receiver, 17) ==
		      V34_PHASE2_INFO_OK &&
	      feed_frame(
		      &receiver, payload, 17,
		      V34_PHASE2_INFO_PREFIX_BITS + 3u, &capture) &&
	      capture.received == 0 && capture.rejected == 1 &&
	      capture.rejected_bit ==
		      V34_PHASE2_INFO_PREFIX_BITS + 17u +
			      V34_PHASE2_INFO_CRC_BITS - 1u &&
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
	struct v34_phase2_info_receiver receiver;
	struct v34_phase2_info_receiver saved;
	struct v34_phase2_info_event event = {
		.type = V34_PHASE2_INFO_FRAME_RECEIVED,
		.payload = { 0xa5 },
		.payload_bit_count = 1,
	};
	struct v34_phase2_info_event saved_event = event;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase2_info_receiver_init(NULL, 17) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_receiver_init(
		      &receiver,
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "receiver initialization accepted an invalid bound");
	CHECK(v34_phase2_info_receiver_init(&receiver, 17) ==
		      V34_PHASE2_INFO_OK,
	      "receiver validation fixture did not initialize");
	saved = receiver;
	CHECK(v34_phase2_info_receive_bit(
		      NULL, 0, &event) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_receive_bit(
		      &receiver, 2, &event) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_receive_bit(
		      &receiver, 0, NULL) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "receiver mutated state on invalid input");

	receiver.stage =
		(enum v34_phase2_info_receive_stage)4;
	CHECK(!v34_phase2_info_receiver_is_valid(&receiver) &&
	      v34_phase2_info_receiver_reset(&receiver) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT,
	      "receiver accepted a corrupt stage");
	receiver = saved;
	receiver.crc ^= 1;
	CHECK(!v34_phase2_info_receiver_is_valid(&receiver),
	      "receiver accepted a corrupt CRC state");
	receiver = saved;
	receiver.sync_window = UINT16_C(0x1000);
	CHECK(!v34_phase2_info_receiver_is_valid(&receiver),
	      "receiver accepted an oversized sync window");
	receiver = saved;
	CHECK(v34_phase2_info_receive_bit(
		      &receiver, 1, &event) ==
		      V34_PHASE2_INFO_OK &&
	      v34_phase2_info_receiver_reset(&receiver) ==
		      V34_PHASE2_INFO_OK &&
	      receiver.sync_window == 0 &&
	      receiver.stage == V34_PHASE2_INFO_SEARCH_SYNC &&
	      v34_phase2_info_receiver_is_valid(&receiver) &&
	      !v34_phase2_info_receiver_is_valid(NULL),
	      "receiver reset did not clear acquisition state");
	return true;
}

static bool test_validation(void)
{
	static const uint8_t payload[] = { 0xff, 0x84, 0xff };
	struct v34_phase2_info_generator generator;
	struct v34_phase2_info_generator saved;
	uint8_t bit = UINT8_MAX;
	bool complete = true;
	uint16_t crc = UINT16_MAX;

	memset(&generator, 0xa5, sizeof(generator));
	saved = generator;
	CHECK(v34_phase2_info_generator_init(
		      NULL, payload, 17) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_generator_init(
		      &generator, NULL, 1) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_generator_init(
		      &generator, payload,
		      V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0 &&
	      v34_phase2_info_crc(NULL, 1, &crc) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_crc(payload, 17, NULL) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      crc == UINT16_MAX,
	      "Phase 2 INFO initialization accepted invalid input");

	CHECK(v34_phase2_info_generator_init(
		      &generator, payload, 17) ==
		      V34_PHASE2_INFO_OK,
	      "Phase 2 INFO validation fixture did not initialize");
	saved = generator;
	CHECK(v34_phase2_info_next_bit(NULL, &bit, &complete) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_next_bit(
		      &generator, NULL, &complete) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      v34_phase2_info_next_bit(
		      &generator, &bit, NULL) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0 &&
	      bit == UINT8_MAX && complete,
	      "Phase 2 INFO emission mutated state on invalid input");

	generator.payload[2] |= 1;
	CHECK(!v34_phase2_info_generator_is_valid(&generator) &&
	      v34_phase2_info_generator_reset(&generator) ==
		      V34_PHASE2_INFO_INVALID_ARGUMENT,
	      "Phase 2 INFO accepted noncanonical payload padding");
	generator = saved;
	generator.crc ^= 1;
	CHECK(!v34_phase2_info_generator_is_valid(&generator),
	      "Phase 2 INFO accepted a corrupt CRC");
	generator = saved;
	generator.bit_index =
		v34_phase2_info_frame_bit_count(17) + 1u;
	CHECK(!v34_phase2_info_generator_is_valid(&generator) &&
	      !v34_phase2_info_generator_is_valid(NULL) &&
	      strcmp(v34_phase2_info_result_name(
		     V34_PHASE2_INFO_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v34_phase2_info_result_name(
		     (enum v34_phase2_info_result)2),
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
		{ "INFO0 codec", test_info0_codec },
		{ "INFO0 validation", test_info0_validation },
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
