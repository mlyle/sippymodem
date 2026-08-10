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

static bool test_control_recovery(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };
	size_t sequence_bits =
		SIPPYMODEM_V8_PREAMBLE_BITS +
		ARRAY_LENGTH(information) * SIPPYMODEM_V8_FRAMED_OCTET_BITS;

	for (size_t leading = 0; leading <= SIPPYMODEM_V8_PREAMBLE_BITS;
	     ++leading) {
		struct sippymodem_v8_control_generator generator;
		struct sippymodem_v8_control_receiver receiver;
		size_t octet_count = 0;
		unsigned int starts = 0;

		CHECK(sippymodem_v8_control_generator_init(
			      &generator, information,
			      ARRAY_LENGTH(information)) ==
			      SIPPYMODEM_V8_FRAMING_OK &&
		      sippymodem_v8_control_receiver_init(&receiver) ==
			      SIPPYMODEM_V8_FRAMING_OK,
		      "V.8 recovery endpoints did not initialize");
		for (size_t index = 0; index < leading; ++index) {
			struct sippymodem_v8_control_event event;

			CHECK(sippymodem_v8_control_receive_bit(
				      &receiver, (uint8_t)(index & 1u), &event) ==
				      SIPPYMODEM_V8_FRAMING_OK &&
				      event.type == SIPPYMODEM_V8_CONTROL_NO_EVENT,
			      "V.8 receiver reported leading noise");
		}
		for (size_t index = 0; index < 2u * sequence_bits; ++index) {
			struct sippymodem_v8_control_event event;
			bool complete;
			uint8_t bit;

			CHECK(sippymodem_v8_control_next_bit(
				      &generator, &bit, &complete) ==
				      SIPPYMODEM_V8_FRAMING_OK &&
			      sippymodem_v8_control_receive_bit(
				      &receiver, bit, &event) ==
				      SIPPYMODEM_V8_FRAMING_OK,
			      "V.8 control recovery failed at bit %zu", index);
			if (event.type == SIPPYMODEM_V8_CONTROL_SEQUENCE_START) {
				starts++;
			} else if (event.type ==
				   SIPPYMODEM_V8_CONTROL_INFORMATION_OCTET) {
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

static bool test_control_reacquisition(void)
{
	static const uint8_t information[] = { UINT8_C(0xc1), UINT8_C(0x2a) };
	struct sippymodem_v8_control_generator generator;
	struct sippymodem_v8_control_receiver receiver;
	size_t sequence_bits =
		SIPPYMODEM_V8_PREAMBLE_BITS +
		ARRAY_LENGTH(information) * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	size_t octet_count = 0;
	unsigned int starts = 0;

	CHECK(sippymodem_v8_control_generator_init(
		      &generator, information, ARRAY_LENGTH(information)) ==
		      SIPPYMODEM_V8_FRAMING_OK &&
	      sippymodem_v8_control_receiver_init(&receiver) ==
		      SIPPYMODEM_V8_FRAMING_OK,
	      "V.8 reacquisition endpoints did not initialize");
	for (size_t index = 0; index < 2u * sequence_bits; ++index) {
		struct sippymodem_v8_control_event event;
		bool complete;
		uint8_t bit;

		CHECK(sippymodem_v8_control_next_bit(
			      &generator, &bit, &complete) ==
			      SIPPYMODEM_V8_FRAMING_OK,
		      "V.8 reacquisition generation failed");
		if (index == SIPPYMODEM_V8_PREAMBLE_BITS +
				     SIPPYMODEM_V8_FRAMED_OCTET_BITS - 1u)
			bit = 0;
		CHECK(sippymodem_v8_control_receive_bit(
			      &receiver, bit, &event) ==
			      SIPPYMODEM_V8_FRAMING_OK,
		      "V.8 reacquisition failed at bit %zu", index);
		if (event.type == SIPPYMODEM_V8_CONTROL_SEQUENCE_START)
			starts++;
		else if (event.type ==
			 SIPPYMODEM_V8_CONTROL_INFORMATION_OCTET) {
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
	     leading < SIPPYMODEM_V8_FRAMED_OCTET_BITS; ++leading) {
		struct sippymodem_v8_cj_detector detector;
		bool detected = false;

		CHECK(sippymodem_v8_cj_detector_init(&detector) ==
			      SIPPYMODEM_V8_FRAMING_OK,
		      "V.8 CJ detector did not initialize");
		for (size_t index = 0; index < leading; ++index)
			CHECK(sippymodem_v8_cj_detect_bit(
				      &detector, (uint8_t)(index & 1u),
				      &detected) == SIPPYMODEM_V8_FRAMING_OK &&
				      !detected,
			      "V.8 CJ detector accepted leading noise");
		for (size_t index = 0; index < SIPPYMODEM_V8_CJ_BITS; ++index) {
			uint8_t bit;

			CHECK(sippymodem_v8_cj_bit(index, &bit) ==
				      SIPPYMODEM_V8_FRAMING_OK &&
			      sippymodem_v8_cj_detect_bit(
				      &detector, bit, &detected) ==
				      SIPPYMODEM_V8_FRAMING_OK &&
			      detected == (index + 1u == SIPPYMODEM_V8_CJ_BITS),
			      "V.8 CJ alignment %zu changed at bit %zu",
			      leading, index);
		}
	}
	{
		struct sippymodem_v8_cj_detector detector;
		bool detected = false;

		CHECK(sippymodem_v8_cj_detector_init(&detector) ==
			      SIPPYMODEM_V8_FRAMING_OK,
		      "V.8 damaged CJ detector did not initialize");
		for (size_t pass = 0; pass < 2; ++pass) {
			for (size_t index = 0; index < SIPPYMODEM_V8_CJ_BITS;
			     ++index) {
				uint8_t bit;

				CHECK(sippymodem_v8_cj_bit(index, &bit) ==
					      SIPPYMODEM_V8_FRAMING_OK,
				      "V.8 CJ fixture generation failed");
				if (pass == 0 && index == 19)
					bit = 0;
				CHECK(sippymodem_v8_cj_detect_bit(
					      &detector, bit, &detected) ==
					      SIPPYMODEM_V8_FRAMING_OK,
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
	uint8_t too_many[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS + 1] = { 0 };
	uint8_t information = UINT8_C(0xc1);
	struct sippymodem_v8_control_generator generator;
	struct sippymodem_v8_control_generator saved;
	struct sippymodem_v8_control_receiver receiver;
	struct sippymodem_v8_control_receiver saved_receiver;
	struct sippymodem_v8_control_event event = {
		.type = SIPPYMODEM_V8_CONTROL_INFORMATION_OCTET,
		.octet = UINT8_C(0xa5),
	};
	struct sippymodem_v8_control_event saved_event = event;
	struct sippymodem_v8_cj_detector detector;
	struct sippymodem_v8_cj_detector saved_detector;
	bool complete = true;
	bool detected = true;
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
	CHECK(sippymodem_v8_control_receiver_init(&receiver) ==
		      SIPPYMODEM_V8_FRAMING_OK &&
	      sippymodem_v8_cj_detector_init(&detector) ==
		      SIPPYMODEM_V8_FRAMING_OK,
	      "V.8 receive validation fixtures did not initialize");
	receiver.sync_bit_count = SIPPYMODEM_V8_PREAMBLE_BITS + 1u;
	detector.matched_octets = SIPPYMODEM_V8_CJ_OCTETS + 1u;
	saved_receiver = receiver;
	saved_detector = detector;
	CHECK(sippymodem_v8_control_receive_bit(&receiver, 0, &event) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0 &&
	      sippymodem_v8_control_receive_bit(NULL, 0, &event) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_control_receive_bit(&saved_receiver, 2, &event) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      sippymodem_v8_cj_detect_bit(&detector, 0, &detected) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved_detector, sizeof(detector)) == 0 &&
	      detected &&
	      sippymodem_v8_cj_detect_bit(NULL, 0, &detected) ==
		      SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT,
	      "V.8 receive framing accepted invalid state");
	sippymodem_v8_control_receiver_reset(&receiver);
	sippymodem_v8_cj_detector_reset(&detector);
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
