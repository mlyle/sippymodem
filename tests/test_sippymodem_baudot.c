// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_baudot.h>

#include "test.h"

#include <string.h>

enum {
	TEST_AMPLITUDE = 8192,
	TEST_MAX_BURST_SAMPLES = 6240,
};

static uint8_t queued_code(
	const struct sippymodem_baudot_transmitter *transmitter,
	size_t offset)
{
	size_t index = (transmitter->code_head + offset) %
		       SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY;

	return transmitter->codes[index];
}

static bool test_text_encoding_and_bounds(void)
{
	static const uint8_t text[] = { 'a', '1', ' ', '?', 1 };
	static const uint8_t expected_codes[] = {
		0x1f, 3, 0x1b, 23, 0x1f, 4, 0x1b, 25, 25,
	};
	struct sippymodem_baudot_transmitter transmitter;
	struct sippymodem_baudot_transmitter refresh;
	uint8_t large_text[512];
	size_t consumed;

	CHECK(sippymodem_baudot_transmitter_init(
		      &transmitter, SIPPYMODEM_BAUDOT_RATE_45) ==
		      SIPPYMODEM_BAUDOT_OK,
	      "Baudot transmitter did not initialize");
	CHECK(sippymodem_baudot_queue_text(
		      &transmitter, text, sizeof(text)) == sizeof(text) &&
		      sippymodem_baudot_pending_codes(&transmitter) ==
			      ARRAY_LENGTH(expected_codes),
	      "Baudot encoder did not consume its compact text fixture");
	for (size_t index = 0; index < ARRAY_LENGTH(expected_codes); ++index)
		CHECK(queued_code(&transmitter, index) == expected_codes[index],
		      "Baudot encoder changed code %zu", index);

	CHECK(sippymodem_baudot_transmitter_init(
		      &refresh, SIPPYMODEM_BAUDOT_RATE_45) ==
		      SIPPYMODEM_BAUDOT_OK,
	      "Baudot shift-refresh fixture did not initialize");
	memset(large_text, 'A', 73);
	CHECK(sippymodem_baudot_queue_text(&refresh, large_text, 73) == 73 &&
		      refresh.code_count == 75 &&
		      queued_code(&refresh, 0) == 0x1f &&
		      queued_code(&refresh, 73) == 0x1f,
	      "Baudot encoder omitted its periodic letters refresh");

	CHECK(sippymodem_baudot_transmitter_init(
		      &transmitter, SIPPYMODEM_BAUDOT_RATE_45) ==
		      SIPPYMODEM_BAUDOT_OK,
	      "Baudot bounded-queue fixture did not initialize");
	memset(large_text, 'A', sizeof(large_text));
	consumed = sippymodem_baudot_queue_text(
		&transmitter, large_text, 251);
	CHECK(consumed == 251 && transmitter.code_count == 255,
	      "Baudot text queue did not leave the expected final slot");
	CHECK(sippymodem_baudot_queue_text(
		      &transmitter, (const uint8_t *)"1", 1) == 0 &&
		      transmitter.code_count == 255 &&
		      transmitter.shift == SIPPYMODEM_BAUDOT_LETTERS &&
		      sippymodem_baudot_queue_text(
			      &transmitter, (const uint8_t *)"A", 1) == 1 &&
		      transmitter.code_count ==
			      SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY,
	      "Baudot queue partially committed a character at capacity");
	return true;
}

struct expected_tone_segment {
	uint8_t bit;
	size_t samples;
};

static bool check_transmit_cadence(
	enum sippymodem_baudot_rate rate,
	size_t expected_samples)
{
	struct sippymodem_baudot_transmitter transmitter;
	struct sippymodem_fsk_receiver detector;
	struct sippymodem_fsk_channel channel = {
		.sample_rate = SIPPYMODEM_BAUDOT_SAMPLE_RATE,
		.symbol_rate_numerator =
			rate == SIPPYMODEM_BAUDOT_RATE_50 ? 50u : 500u,
		.symbol_rate_denominator =
			rate == SIPPYMODEM_BAUDOT_RATE_50 ? 1u : 11u,
		.mark_frequency_hz = 1400,
		.space_frequency_hz = 1800,
		.detector_window = 64,
		.metric_smoothing_shift = 1,
	};
	int16_t samples[TEST_MAX_BURST_SAMPLES];
	uint16_t bit_samples =
		rate == SIPPYMODEM_BAUDOT_RATE_50 ? 160u : 176u;
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
		      sippymodem_baudot_transmitter_init(&transmitter, rate) ==
			      SIPPYMODEM_BAUDOT_OK &&
		      sippymodem_fsk_receiver_init(&detector, &channel) ==
			      SIPPYMODEM_FSK_OK,
	      "Baudot cadence fixture did not initialize");
	CHECK(sippymodem_baudot_queue_text(
		      &transmitter, (const uint8_t *)"A", 1) == 1 &&
		      sippymodem_baudot_generate(
			      &transmitter, samples, expected_samples) ==
			      SIPPYMODEM_BAUDOT_OK,
	      "Baudot cadence fixture did not generate its burst");
	CHECK(!sippymodem_baudot_transmitting(&transmitter) &&
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
				      sippymodem_fsk_receiver_detect(
					      &detector, samples[sample_offset],
					      &detected_bit, &ready) ==
					      SIPPYMODEM_FSK_OK,
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
		SIPPYMODEM_BAUDOT_RATE_45, 6240);
}

static bool test_50_baud_transmit_cadence(void)
{
	return check_transmit_cadence(
		SIPPYMODEM_BAUDOT_RATE_50, 6000);
}

static bool test_validation(void)
{
	struct sippymodem_baudot_transmitter transmitter;
	int16_t sample = 0;

	CHECK(sippymodem_baudot_transmitter_init(
		      NULL, SIPPYMODEM_BAUDOT_RATE_45) ==
		      SIPPYMODEM_BAUDOT_INVALID_ARGUMENT &&
		      sippymodem_baudot_transmitter_init(
			      &transmitter,
			      (enum sippymodem_baudot_rate)49) ==
			      SIPPYMODEM_BAUDOT_INVALID_ARGUMENT,
	      "Baudot transmitter accepted an invalid configuration");
	CHECK(sippymodem_baudot_transmitter_init(
		      &transmitter, SIPPYMODEM_BAUDOT_RATE_50) ==
		      SIPPYMODEM_BAUDOT_OK &&
		      sippymodem_baudot_generate(&transmitter, NULL, 0) ==
			      SIPPYMODEM_BAUDOT_OK &&
		      sippymodem_baudot_generate(&transmitter, NULL, 1) ==
			      SIPPYMODEM_BAUDOT_INVALID_ARGUMENT &&
		      sippymodem_baudot_queue_text(&transmitter, NULL, 1) == 0,
	      "Baudot transmitter mishandled an empty or missing buffer");
	transmitter.code_head = SIPPYMODEM_BAUDOT_TX_CODE_CAPACITY;
	CHECK(sippymodem_baudot_generate(&transmitter, &sample, 1) ==
		      SIPPYMODEM_BAUDOT_INVALID_ARGUMENT &&
		      strcmp(sippymodem_baudot_result_name(
			     SIPPYMODEM_BAUDOT_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "Baudot transmitter accepted corrupt queue state");
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
