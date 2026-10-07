// SPDX-License-Identifier: BSD-3-Clause

#include <v8_framing.h>

#include "test.h"

#include <string.h>

static bool test_control_sequence(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };
	static const char expected[] =
		"1111111111" "0000001111"
		"0100000111" "0010101001";
	struct v8_control_generator generator;

	CHECK(strlen(expected) ==
		      V8_PREAMBLE_BITS +
			      ARRAY_LENGTH(information) *
				      V8_FRAMED_OCTET_BITS &&
	      v8_control_generator_init(
		      &generator, information, ARRAY_LENGTH(information)) ==
		      V8_FRAMING_OK,
	      "V.8 control generator did not initialize");
	for (size_t repeat = 0; repeat < 2; ++repeat) {
		for (size_t index = 0; index < strlen(expected); ++index) {
			bool complete = true;
			uint8_t bit = UINT8_MAX;

			CHECK(v8_control_next_bit(
				      &generator, &bit, &complete) ==
				      V8_FRAMING_OK &&
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
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	struct v8_control_generator generator;
	bool complete = false;
	uint8_t bit = UINT8_MAX;

	for (size_t index = 0; index < ARRAY_LENGTH(information); ++index)
		information[index] = (uint8_t)(index * 17u);
	CHECK(v8_control_generator_init(
		      &generator, information, ARRAY_LENGTH(information)) ==
		      V8_FRAMING_OK,
	      "V.8 maximum control sequence did not initialize");
	memset(information, UINT8_MAX, sizeof(information));
	for (size_t index = 0;
	     index < V8_PREAMBLE_BITS +
			     V8_MAX_INFORMATION_OCTETS *
				     V8_FRAMED_OCTET_BITS;
	     ++index) {
		CHECK(v8_control_next_bit(
			      &generator, &bit, &complete) ==
			      V8_FRAMING_OK &&
			      complete ==
				      (index + 1u ==
				       V8_PREAMBLE_BITS +
					       V8_MAX_INFORMATION_OCTETS *
						       V8_FRAMED_OCTET_BITS),
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
	for (size_t index = 0; index < V8_CJ_BITS; ++index) {
		uint8_t bit = UINT8_MAX;
		bool expected = index == 9 || index == 19 || index == 29;

		CHECK(v8_cj_bit(index, &bit) ==
			      V8_FRAMING_OK &&
			      bit == expected,
		      "V.8 CJ changed bit %zu", index);
	}
	return true;
}

static bool test_control_recovery(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };
	size_t sequence_bits =
		V8_PREAMBLE_BITS +
		ARRAY_LENGTH(information) * V8_FRAMED_OCTET_BITS;

	for (size_t leading = 0; leading <= V8_PREAMBLE_BITS;
	     ++leading) {
		struct v8_control_generator generator;
		struct v8_control_receiver receiver;
		size_t octet_count = 0;
		unsigned int starts = 0;

		CHECK(v8_control_generator_init(
			      &generator, information,
			      ARRAY_LENGTH(information)) ==
			      V8_FRAMING_OK &&
		      v8_control_receiver_init(&receiver) ==
			      V8_FRAMING_OK,
		      "V.8 recovery endpoints did not initialize");
		for (size_t index = 0; index < leading; ++index) {
			struct v8_control_event event;

			CHECK(v8_control_receive_bit(
				      &receiver, (uint8_t)(index & 1u), &event) ==
				      V8_FRAMING_OK &&
				      event.type == V8_CONTROL_NO_EVENT,
			      "V.8 receiver reported leading noise");
		}
		for (size_t index = 0; index < 2u * sequence_bits; ++index) {
			struct v8_control_event event;
			bool complete;
			uint8_t bit;

			CHECK(v8_control_next_bit(
				      &generator, &bit, &complete) ==
				      V8_FRAMING_OK &&
			      v8_control_receive_bit(
				      &receiver, bit, &event) ==
				      V8_FRAMING_OK,
			      "V.8 control recovery failed at bit %zu", index);
			if (event.type == V8_CONTROL_SEQUENCE_START) {
				starts++;
			} else if (event.type ==
				   V8_CONTROL_INFORMATION_OCTET) {
				CHECK(event.octet ==
					      information[octet_count %
							  ARRAY_LENGTH(information)],
				      "V.8 receiver changed information octet %zu",
				      octet_count);
				octet_count++;
			}
		}
		CHECK(starts == 2 &&
			      octet_count == 2u * ARRAY_LENGTH(information),
		      "V.8 receiver lost alignment after %zu leading bits",
		      leading);
	}
	return true;
}

static bool test_clipped_control_preamble(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };

	for (size_t clipped = 1; clipped <= V8_SYNC_MARK_BITS - 2u;
	     ++clipped) {
		struct v8_control_generator generator;
		struct v8_control_receiver receiver;
		size_t octet_count = 0;
		unsigned int starts = 0;

		CHECK(v8_control_generator_init(
			      &generator, information,
			      ARRAY_LENGTH(information)) == V8_FRAMING_OK &&
		      v8_control_receiver_init(&receiver) == V8_FRAMING_OK,
		      "V.8 clipped-preamble endpoints did not initialize");
		for (size_t index = 0; index < clipped; ++index) {
			bool complete;
			uint8_t bit;

			CHECK(v8_control_next_bit(
				      &generator, &bit, &complete) == V8_FRAMING_OK &&
			      bit == 1 && !complete,
			      "V.8 clipped a non-mark preamble bit");
		}
		for (;;) {
			struct v8_control_event event;
			bool complete;
			uint8_t bit;

			CHECK(v8_control_next_bit(
				      &generator, &bit, &complete) == V8_FRAMING_OK &&
			      v8_control_receive_bit(
				      &receiver, bit, &event) == V8_FRAMING_OK,
			      "V.8 clipped-preamble receive failed");
			if (event.type == V8_CONTROL_SEQUENCE_START)
				starts++;
			else if (event.type == V8_CONTROL_INFORMATION_OCTET) {
				CHECK(octet_count < ARRAY_LENGTH(information) &&
				      event.octet == information[octet_count],
				      "V.8 clipped preamble changed octet %zu",
				      octet_count);
				octet_count++;
			}
			if (complete)
				break;
		}
		CHECK(starts == 1 && octet_count == ARRAY_LENGTH(information),
		      "V.8 lost a preamble clipped by %zu marks", clipped);
	}
	return true;
}

static bool test_control_reacquisition(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };
	struct v8_control_generator generator;
	struct v8_control_receiver receiver;
	size_t sequence_bits =
		V8_PREAMBLE_BITS +
		ARRAY_LENGTH(information) * V8_FRAMED_OCTET_BITS;
	size_t octet_count = 0;
	unsigned int starts = 0;

	CHECK(v8_control_generator_init(
		      &generator, information, ARRAY_LENGTH(information)) ==
		      V8_FRAMING_OK &&
	      v8_control_receiver_init(&receiver) ==
		      V8_FRAMING_OK,
	      "V.8 reacquisition endpoints did not initialize");
	for (size_t index = 0; index < 2u * sequence_bits; ++index) {
		struct v8_control_event event;
		bool complete;
		uint8_t bit;

		CHECK(v8_control_next_bit(
			      &generator, &bit, &complete) ==
			      V8_FRAMING_OK,
		      "V.8 reacquisition generation failed");
		if (index == V8_PREAMBLE_BITS +
				     V8_FRAMED_OCTET_BITS - 1u)
			bit = 0;
		CHECK(v8_control_receive_bit(
			      &receiver, bit, &event) ==
			      V8_FRAMING_OK,
		      "V.8 reacquisition failed at bit %zu", index);
		if (event.type == V8_CONTROL_SEQUENCE_START)
			starts++;
		else if (event.type ==
			 V8_CONTROL_INFORMATION_OCTET) {
			CHECK(octet_count < ARRAY_LENGTH(information) &&
				      event.octet == information[octet_count],
			      "V.8 reacquisition changed octet %zu", octet_count);
			octet_count++;
		}
	}
	CHECK(starts == 2 && octet_count == ARRAY_LENGTH(information),
	      "V.8 receiver did not discard and reacquire a damaged sequence");
	return true;
}

static bool test_cj_detection(void)
{
	for (size_t leading = 0;
	     leading < V8_FRAMED_OCTET_BITS; ++leading) {
		struct v8_cj_detector detector;
		bool detected = false;

		CHECK(v8_cj_detector_init(&detector) ==
			      V8_FRAMING_OK,
		      "V.8 CJ detector did not initialize");
		for (size_t index = 0; index < leading; ++index)
			CHECK(v8_cj_detect_bit(
				      &detector, (uint8_t)(index & 1u),
				      &detected) == V8_FRAMING_OK &&
				      !detected,
			      "V.8 CJ detector accepted leading noise");
		for (size_t index = 0; index < V8_CJ_BITS; ++index) {
			uint8_t bit;

			CHECK(v8_cj_bit(index, &bit) ==
				      V8_FRAMING_OK &&
			      v8_cj_detect_bit(
				      &detector, bit, &detected) ==
				      V8_FRAMING_OK &&
			      detected == (index + 1u == V8_CJ_BITS),
			      "V.8 CJ alignment %zu changed at bit %zu",
			      leading, index);
		}
	}
	{
		struct v8_cj_detector detector;
		bool detected = false;

		CHECK(v8_cj_detector_init(&detector) ==
			      V8_FRAMING_OK,
		      "V.8 damaged CJ detector did not initialize");
		for (size_t pass = 0; pass < 2; ++pass) {
			for (size_t index = 0; index < V8_CJ_BITS;
			     ++index) {
				uint8_t bit;

				CHECK(v8_cj_bit(index, &bit) ==
					      V8_FRAMING_OK,
				      "V.8 CJ fixture generation failed");
				if (pass == 0 && index == 19)
					bit = 0;
				CHECK(v8_cj_detect_bit(
					      &detector, bit, &detected) ==
					      V8_FRAMING_OK,
				      "V.8 damaged CJ processing failed");
			}
			CHECK(detected == (pass == 1),
			      "V.8 CJ detector did not reject then reacquire");
		}
	}
	return true;
}

static bool test_validation(void)
{
	uint8_t too_many[V8_MAX_INFORMATION_OCTETS + 1] = { 0 };
	uint8_t information = UINT8_C(0xc1);
	struct v8_control_generator generator;
	struct v8_control_generator saved;
	struct v8_control_receiver receiver;
	struct v8_control_receiver saved_receiver;
	struct v8_control_event event = {
		.type = V8_CONTROL_INFORMATION_OCTET,
		.octet = UINT8_C(0xa5),
	};
	struct v8_control_event saved_event = event;
	struct v8_cj_detector detector;
	struct v8_cj_detector saved_detector;
	bool complete = true;
	bool detected = true;
	uint8_t bit = UINT8_MAX;

	CHECK(v8_control_generator_init(
		      &generator, &information, 1) ==
		      V8_FRAMING_OK,
	      "V.8 validation fixture did not initialize");
	saved = generator;
	CHECK(v8_control_generator_init(NULL, &information, 1) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_control_generator_init(&generator, NULL, 1) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_control_generator_init(
		      &generator, &information, 0) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_control_generator_init(
		      &generator, too_many, ARRAY_LENGTH(too_many)) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0,
	      "V.8 control initialization accepted invalid input");
	generator.bit_index =
		V8_PREAMBLE_BITS +
		V8_FRAMED_OCTET_BITS;
	saved = generator;
	CHECK(v8_control_next_bit(
		      &generator, &bit, &complete) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&generator, &saved, sizeof(generator)) == 0 &&
	      bit == UINT8_MAX && complete &&
	      v8_control_next_bit(NULL, &bit, &complete) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_control_next_bit(&saved, NULL, &complete) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_control_next_bit(&saved, &bit, NULL) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_cj_bit(V8_CJ_BITS, &bit) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_cj_bit(0, NULL) ==
		      V8_FRAMING_INVALID_ARGUMENT,
	      "V.8 framing accepted invalid state");
	CHECK(v8_control_receiver_init(&receiver) ==
		      V8_FRAMING_OK &&
	      v8_cj_detector_init(&detector) ==
		      V8_FRAMING_OK,
	      "V.8 receive validation fixtures did not initialize");
	receiver.sync_bit_count = V8_PREAMBLE_BITS + 1u;
	detector.matched_octets = V8_CJ_OCTETS + 1u;
	saved_receiver = receiver;
	saved_detector = detector;
	CHECK(v8_control_receive_bit(&receiver, 0, &event) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      event.type == saved_event.type && event.octet == saved_event.octet &&
	      v8_control_receive_bit(NULL, 0, &event) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_control_receive_bit(&saved_receiver, 2, &event) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      v8_cj_detect_bit(&detector, 0, &detected) ==
		      V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved_detector, sizeof(detector)) == 0 &&
	      detected &&
	      v8_cj_detect_bit(NULL, 0, &detected) ==
		      V8_FRAMING_INVALID_ARGUMENT,
	      "V.8 receive framing accepted invalid state");
	v8_control_receiver_reset(&receiver);
	v8_cj_detector_reset(&detector);
	CHECK(receiver.sync_bit_count == 0 && !receiver.receiving &&
		      detector.window_bit_count == 0 && !detector.detected,
	      "V.8 receive framing reset retained state");
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
		{ "control recovery", test_control_recovery },
		{ "clipped control preamble", test_clipped_control_preamble },
		{ "control reacquisition", test_control_reacquisition },
		{ "CJ detection", test_cj_detection },
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
