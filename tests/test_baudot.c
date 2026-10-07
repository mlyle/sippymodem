// SPDX-License-Identifier: BSD-3-Clause

#include <baudot.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 8192,
	TEST_MAX_BURST_SAMPLES = 6240,
	TEST_MANUAL_SAMPLE_CAPACITY = 8000,
};

static const double TEST_TWO_PI = 6.283185307179586476925286766559;

static uint8_t queued_code(
	const struct baudot_transmitter *transmitter,
	size_t offset)
{
	size_t index = (transmitter->code_head + offset) %
		       BAUDOT_TX_CODE_CAPACITY;

	return transmitter->codes[index];
}

static bool test_text_encoding_and_bounds(void)
{
	static const uint8_t text[] = { 'a', '1', ' ', '?', 1 };
	static const uint8_t expected_codes[] = {
		0x1f, 3, 0x1b, 23, 0x1f, 4, 0x1b, 25, 25,
	};
	struct baudot_transmitter transmitter;
	struct baudot_transmitter refresh;
	uint8_t large_text[512];
	size_t consumed;

	CHECK(baudot_transmitter_init(
		      &transmitter, BAUDOT_RATE_45) ==
		      BAUDOT_OK,
	      "Baudot transmitter did not initialize");
	CHECK(baudot_queue_text(
		      &transmitter, text, sizeof(text)) == sizeof(text) &&
		      baudot_pending_codes(&transmitter) ==
			      ARRAY_LENGTH(expected_codes),
	      "Baudot encoder did not consume its compact text fixture");
	for (size_t index = 0; index < ARRAY_LENGTH(expected_codes); ++index)
		CHECK(queued_code(&transmitter, index) == expected_codes[index],
		      "Baudot encoder changed code %zu", index);

	CHECK(baudot_transmitter_init(
		      &refresh, BAUDOT_RATE_45) ==
		      BAUDOT_OK,
	      "Baudot shift-refresh fixture did not initialize");
	memset(large_text, 'A', 73);
	CHECK(baudot_queue_text(&refresh, large_text, 73) == 73 &&
		      refresh.code_count == 75 &&
		      queued_code(&refresh, 0) == 0x1f &&
		      queued_code(&refresh, 73) == 0x1f,
	      "Baudot encoder omitted its periodic letters refresh");

	CHECK(baudot_transmitter_init(
		      &transmitter, BAUDOT_RATE_45) ==
		      BAUDOT_OK,
	      "Baudot bounded-queue fixture did not initialize");
	memset(large_text, 'A', sizeof(large_text));
	consumed = baudot_queue_text(
		&transmitter, large_text, 251);
	CHECK(consumed == 251 && transmitter.code_count == 255,
	      "Baudot text queue did not leave the expected final slot");
	CHECK(baudot_queue_text(
		      &transmitter, (const uint8_t *)"1", 1) == 0 &&
		      transmitter.code_count == 255 &&
		      transmitter.shift == BAUDOT_LETTERS &&
		      baudot_queue_text(
			      &transmitter, (const uint8_t *)"A", 1) == 1 &&
		      transmitter.code_count ==
			      BAUDOT_TX_CODE_CAPACITY,
	      "Baudot queue partially committed a character at capacity");
	return true;
}

struct expected_tone_segment {
	uint8_t bit;
	size_t samples;
};

static bool check_transmit_cadence(
	enum baudot_rate rate,
	size_t expected_samples)
{
	struct baudot_transmitter transmitter;
	struct fsk_receiver detector;
	struct fsk_channel channel = {
		.sample_rate = BAUDOT_SAMPLE_RATE,
		.symbol_rate_numerator =
			rate == BAUDOT_RATE_50 ? 50u : 500u,
		.symbol_rate_denominator =
			rate == BAUDOT_RATE_50 ? 1u : 11u,
		.mark_frequency_hz = 1400,
		.space_frequency_hz = 1800,
		.detector_window = 64,
		.metric_smoothing_shift = 1,
	};
	int16_t samples[TEST_MAX_BURST_SAMPLES];
	uint16_t bit_samples =
		rate == BAUDOT_RATE_50 ? 160u : 176u;
	uint16_t stop_samples =
		(uint16_t)(bit_samples + bit_samples / 2u);
	struct expected_tone_segment segments[] = {
		{ 1, 1200 },
		{ 0, bit_samples },
		{ 1, 5u * bit_samples + stop_samples },
		{ 0, bit_samples },
		{ 1, 2u * bit_samples },
		{ 0, 3u * bit_samples },
		{ 1, stop_samples + 2400u },
	};
	size_t sample_offset = 0;

	CHECK(expected_samples <= ARRAY_LENGTH(samples) &&
		      baudot_transmitter_init(&transmitter, rate) ==
			      BAUDOT_OK &&
		      fsk_receiver_init(&detector, &channel) ==
			      FSK_OK,
	      "Baudot cadence fixture did not initialize");
	CHECK(baudot_queue_text(
		      &transmitter, (const uint8_t *)"A", 1) == 1 &&
		      baudot_generate(
			      &transmitter, samples, expected_samples) ==
			      BAUDOT_OK,
	      "Baudot cadence fixture did not generate its burst");
	CHECK(!baudot_transmitting(&transmitter) &&
		      transmitter.generated_tone_samples == expected_samples &&
		      transmitter.transmitted_codes == 2 &&
		      transmitter.code_count == 0,
	      "Baudot burst length or initial letters code was wrong");

	for (size_t segment = 0; segment < ARRAY_LENGTH(segments); ++segment) {
		uint8_t detected_bit = 0;
		bool ready = false;

		for (size_t index = 0; index < segments[segment].samples;
		     ++index) {
			CHECK(sample_offset < expected_samples &&
				      samples[sample_offset] >= -TEST_AMPLITUDE &&
				      samples[sample_offset] <= TEST_AMPLITUDE &&
				      fsk_receiver_detect(
					      &detector, samples[sample_offset],
					      &detected_bit, &ready) ==
					      FSK_OK,
			      "Baudot waveform was invalid at sample %zu",
			      sample_offset);
			sample_offset++;
		}
		CHECK(ready && detected_bit == segments[segment].bit,
		      "Baudot framing tone was wrong at segment %zu", segment);
	}
	CHECK(sample_offset == expected_samples,
	      "Baudot framing segments did not cover the exact burst");
	return true;
}

static bool test_45_baud_transmit_cadence(void)
{
	return check_transmit_cadence(
		BAUDOT_RATE_45, 6240);
}

static bool test_50_baud_transmit_cadence(void)
{
	return check_transmit_cadence(
		BAUDOT_RATE_50, 6000);
}

static bool check_receive_loopback(
	enum baudot_rate rate,
	unsigned int attenuation)
{
	static const uint8_t input_text[] = "hello 123?\r\n";
	static const uint8_t expected_text[] = "HELLO 123?\r\n";
	struct baudot_transmitter transmitter;
	struct baudot_receiver receiver;
	int16_t samples[BAUDOT_BLOCK_SAMPLES];
	uint8_t decoded[32];
	uint8_t received[sizeof(expected_text)] = { 0 };
	size_t received_count = 0;
	unsigned int blocks = 0;

	CHECK(attenuation != 0 &&
		      baudot_transmitter_init(&transmitter, rate) ==
			      BAUDOT_OK &&
		      baudot_receiver_init(&receiver, rate) ==
			      BAUDOT_OK &&
		      baudot_queue_text(
			      &transmitter, input_text,
			      sizeof(input_text) - 1u) ==
			      sizeof(input_text) - 1u,
	      "Baudot receive loopback did not initialize");
	while (baudot_transmitting(&transmitter) &&
	       blocks++ < 400) {
		size_t decoded_count;

		CHECK(baudot_generate(
		      &transmitter, samples, ARRAY_LENGTH(samples)) ==
			      BAUDOT_OK,
		      "Baudot loopback transmitter failed");
		if (attenuation != 1)
			for (size_t index = 0; index < ARRAY_LENGTH(samples);
			     ++index)
				samples[index] =
					(int16_t)(samples[index] /
						  (int)attenuation);
		CHECK(baudot_receive(
		      &receiver, samples, ARRAY_LENGTH(samples)) ==
			      BAUDOT_OK,
		      "Baudot loopback receiver failed");
		decoded_count = baudot_drain_text(
			&receiver, decoded, sizeof(decoded));
		CHECK(received_count + decoded_count <= sizeof(received),
		      "Baudot loopback decoded excess text");
		memcpy(received + received_count, decoded, decoded_count);
		received_count += decoded_count;
	}
	CHECK(!baudot_transmitting(&transmitter) &&
		      received_count == sizeof(expected_text) - 1u &&
		      memcmp(received, expected_text,
			     sizeof(expected_text) - 1u) == 0 &&
		      receiver.framing_errors == 0 &&
		      receiver.receive_overruns == 0,
	      "Baudot loopback changed text at rate %u with attenuation %u",
	      (unsigned int)rate, attenuation);
	return true;
}

static bool test_receive_loopback(void)
{
	return check_receive_loopback(BAUDOT_RATE_45, 1) &&
	       check_receive_loopback(BAUDOT_RATE_50, 1) &&
	       check_receive_loopback(BAUDOT_RATE_45, 64);
}

static size_t append_test_tone(
	int16_t *samples,
	size_t offset,
	size_t capacity,
	unsigned int frequency_hz,
	size_t count,
	double *phase)
{
	for (size_t index = 0; index < count && offset < capacity; ++index) {
		samples[offset++] = (int16_t)lround(sin(*phase) * 12000.0);
		*phase += TEST_TWO_PI * (double)frequency_hz /
			  BAUDOT_SAMPLE_RATE;
		if (*phase >= TEST_TWO_PI)
			*phase -= TEST_TWO_PI;
	}
	return offset;
}

static bool check_receive_deviation(
	enum baudot_rate rate,
	int frequency_percent,
	int timing_delta_samples,
	bool preamble)
{
	struct baudot_receiver receiver;
	int16_t samples[TEST_MANUAL_SAMPLE_CAPACITY] = { 0 };
	uint8_t decoded[2] = { 0 };
	double phase = 0.321;
	unsigned int nominal =
		rate == BAUDOT_RATE_50 ? 160u : 176u;
	unsigned int bit_samples =
		(unsigned int)((int)nominal + timing_delta_samples);
	unsigned int mark_hz =
		(unsigned int)(1400 * (100 + frequency_percent) / 100);
	unsigned int space_hz =
		(unsigned int)(1800 * (100 + frequency_percent) / 100);
	uint8_t code = 3;
	size_t count = preamble ? 0 : 400;

	CHECK(baudot_receiver_init(&receiver, rate) ==
		      BAUDOT_OK,
	      "Baudot deviation receiver did not initialize");
	if (preamble)
		count = append_test_tone(
			samples, count, ARRAY_LENGTH(samples), mark_hz, 1200,
			&phase);
	count = append_test_tone(
		samples, count, ARRAY_LENGTH(samples), space_hz, bit_samples,
		&phase);
	for (unsigned int bit = 0; bit < 5; ++bit)
		count = append_test_tone(
			samples, count, ARRAY_LENGTH(samples),
			(code >> bit) & 1u ? mark_hz : space_hz,
			bit_samples, &phase);
	count = append_test_tone(
		samples, count, ARRAY_LENGTH(samples), mark_hz,
		bit_samples * 3u / 2u + 400u, &phase);
	CHECK(count < ARRAY_LENGTH(samples) &&
		      baudot_receive(&receiver, samples, count) ==
			      BAUDOT_OK &&
		      baudot_drain_text(
			      &receiver, decoded, sizeof(decoded)) == 1 &&
		      decoded[0] == 'A' && receiver.framing_errors == 0,
	      "Baudot deviation failed at rate %u, frequency %+d%%, timing %+d",
	      (unsigned int)rate, frequency_percent, timing_delta_samples);
	return true;
}

static bool test_receive_tolerance(void)
{
	return check_receive_deviation(
		       BAUDOT_RATE_45, -4, -3, true) &&
	       check_receive_deviation(
		       BAUDOT_RATE_45, 4, 3, true) &&
	       check_receive_deviation(
		       BAUDOT_RATE_50, -4, -3, true) &&
	       check_receive_deviation(
		       BAUDOT_RATE_50, 4, 3, true) &&
	       check_receive_deviation(
		       BAUDOT_RATE_45, 0, 0, false);
}

static bool test_receive_queue_bound(void)
{
	struct baudot_transmitter transmitter;
	struct baudot_receiver receiver;
	int16_t samples[BAUDOT_BLOCK_SAMPLES];
	uint8_t text[130];
	uint8_t decoded[BAUDOT_RX_CHARACTER_CAPACITY];
	unsigned int blocks = 0;

	memset(text, 'A', sizeof(text));
	CHECK(baudot_transmitter_init(
		      &transmitter, BAUDOT_RATE_45) ==
		      BAUDOT_OK &&
		      baudot_receiver_init(
			      &receiver, BAUDOT_RATE_45) ==
			      BAUDOT_OK &&
		      baudot_queue_text(
			      &transmitter, text, sizeof(text)) == sizeof(text),
	      "Baudot receive-bound fixture did not initialize");
	while (baudot_transmitting(&transmitter) &&
	       blocks++ < 1400) {
		CHECK(baudot_generate(
		      &transmitter, samples, ARRAY_LENGTH(samples)) ==
			      BAUDOT_OK &&
		      baudot_receive(
			      &receiver, samples, ARRAY_LENGTH(samples)) ==
			      BAUDOT_OK,
		      "Baudot receive-bound loopback failed");
	}
	baudot_receiver_reset(&receiver);
	CHECK(!baudot_transmitting(&transmitter) &&
		      baudot_pending_text(&receiver) ==
			      BAUDOT_RX_CHARACTER_CAPACITY &&
		      receiver.character_count ==
		      BAUDOT_RX_CHARACTER_CAPACITY &&
		      receiver.received_characters ==
			      BAUDOT_RX_CHARACTER_CAPACITY &&
		      receiver.receive_overruns == 2 &&
		      baudot_drain_text(
			      &receiver, decoded, sizeof(decoded)) ==
			      sizeof(decoded),
	      "Baudot receive queue did not account for overflow exactly");
	for (size_t index = 0; index < sizeof(decoded); ++index)
		CHECK(decoded[index] == 'A',
		      "Baudot receive queue changed character %zu", index);
	return true;
}

static bool test_validation(void)
{
	struct baudot_transmitter transmitter;
	struct baudot_receiver receiver;
	int16_t sample = 0;

	CHECK(baudot_transmitter_init(
		      NULL, BAUDOT_RATE_45) ==
		      BAUDOT_INVALID_ARGUMENT &&
		      baudot_transmitter_init(
			      &transmitter,
			      (enum baudot_rate)49) ==
			      BAUDOT_INVALID_ARGUMENT,
	      "Baudot transmitter accepted an invalid configuration");
	CHECK(baudot_transmitter_init(
		      &transmitter, BAUDOT_RATE_50) ==
		      BAUDOT_OK &&
		      baudot_generate(&transmitter, NULL, 0) ==
			      BAUDOT_OK &&
		      baudot_generate(&transmitter, NULL, 1) ==
			      BAUDOT_INVALID_ARGUMENT &&
		      baudot_queue_text(&transmitter, NULL, 1) == 0,
	      "Baudot transmitter mishandled an empty or missing buffer");
	transmitter.code_head = BAUDOT_TX_CODE_CAPACITY;
	CHECK(baudot_generate(&transmitter, &sample, 1) ==
		      BAUDOT_INVALID_ARGUMENT,
	      "Baudot transmitter accepted corrupt queue state");
	CHECK(baudot_receiver_init(
		      NULL, BAUDOT_RATE_45) ==
		      BAUDOT_INVALID_ARGUMENT &&
		      baudot_receiver_init(
			      &receiver, BAUDOT_RATE_45) ==
			      BAUDOT_OK &&
		      baudot_receive(&receiver, NULL, 0) ==
			      BAUDOT_OK &&
		      baudot_receive(&receiver, NULL, 1) ==
			      BAUDOT_INVALID_ARGUMENT &&
		      baudot_drain_text(&receiver, NULL, 1) == 0,
	      "Baudot receiver accepted an invalid buffer");
	receiver.character_head = BAUDOT_RX_CHARACTER_CAPACITY;
	CHECK(baudot_receive(&receiver, &sample, 1) ==
		      BAUDOT_INVALID_ARGUMENT,
	      "Baudot receiver accepted corrupt queue state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "text encoding and bounds", test_text_encoding_and_bounds },
		{ "45.45-baud transmit cadence",
		  test_45_baud_transmit_cadence },
		{ "50-baud transmit cadence", test_50_baud_transmit_cadence },
		{ "receive loopback", test_receive_loopback },
		{ "receive tolerance", test_receive_tolerance },
		{ "receive queue bound", test_receive_queue_bound },
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
