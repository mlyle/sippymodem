// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_g711.h>
#include <v21.h>
#include <sippymodem_v8_fsk.h>
#include <sippymodem_v8_menu.h>

#include "test.h"

#include <string.h>

enum test_codec {
	TEST_LINEAR,
	TEST_PCMU,
	TEST_PCMA,
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	if (codec == TEST_PCMU)
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	if (codec == TEST_PCMA)
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	return sample;
}

static bool channel_matches(
	const struct sippymodem_fsk_channel *actual,
	const struct sippymodem_fsk_channel *expected)
{
	return actual->sample_rate == expected->sample_rate &&
	       actual->symbol_rate_numerator ==
		       expected->symbol_rate_numerator &&
	       actual->symbol_rate_denominator ==
		       expected->symbol_rate_denominator &&
	       actual->mark_frequency_hz == expected->mark_frequency_hz &&
	       actual->space_frequency_hz == expected->space_frequency_hz;
}

static bool test_station_channels(void)
{
	struct sippymodem_v8_fsk caller;
	struct sippymodem_v8_fsk answerer;

	CHECK(sippymodem_v8_fsk_init(
		      &caller, SIPPYMODEM_V8_CALLING_STATION) ==
		      SIPPYMODEM_V8_FSK_OK &&
	      sippymodem_v8_fsk_init(
		      &answerer, SIPPYMODEM_V8_ANSWERING_STATION) ==
		      SIPPYMODEM_V8_FSK_OK,
	      "V.8 FSK stations did not initialize");
	CHECK(channel_matches(
		      &caller.transmitter.channel,
		      &sippymodem_v21_channel_1) &&
	      channel_matches(
		      &caller.receiver.channel, &sippymodem_v21_channel_2) &&
	      channel_matches(
		      &answerer.transmitter.channel,
		      &sippymodem_v21_channel_2) &&
	      channel_matches(
		      &answerer.receiver.channel, &sippymodem_v21_channel_1),
	      "V.8 FSK stations selected the wrong V.21 channels");
	return true;
}

static bool menu_round_trip(
	enum sippymodem_v8_station_role source_role,
	enum test_codec codec)
{
	struct sippymodem_v8_control_generator generator;
	struct sippymodem_v8_menu_detector detector;
	struct sippymodem_v8_menu offered;
	struct sippymodem_v8_menu detected = { 0 };
	struct sippymodem_v8_fsk source;
	struct sippymodem_v8_fsk destination;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	uint8_t transmit_bit = 1;
	size_t information_count;
	unsigned int completed_sequences = 0;
	bool confirmed = false;

	CHECK(sippymodem_v8_call_menu_init(
		      &offered, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_encode(
		      &offered, information, sizeof(information),
		      &information_count) == SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_control_generator_init(
		      &generator, information, information_count) ==
		      SIPPYMODEM_V8_FRAMING_OK &&
	      sippymodem_v8_menu_detector_init(&detector) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_fsk_init(&source, source_role) ==
		      SIPPYMODEM_V8_FSK_OK &&
	      sippymodem_v8_fsk_init(
		      &destination,
		      source_role == SIPPYMODEM_V8_CALLING_STATION ?
			      SIPPYMODEM_V8_ANSWERING_STATION :
			      SIPPYMODEM_V8_CALLING_STATION) ==
		      SIPPYMODEM_V8_FSK_OK,
	      "V.8 menu audio endpoints did not initialize");

	for (size_t sample_index = 0;
	     sample_index < 3u * SIPPYMODEM_V21_SAMPLE_RATE && !confirmed;
	     ++sample_index) {
		int16_t sample;
		uint8_t receive_bit;
		bool bit_consumed;
		bool bit_ready;

		if (sippymodem_v8_fsk_can_accept_bit(&source)) {
			bool sequence_complete;

			CHECK(sippymodem_v8_control_next_bit(
				      &generator, &transmit_bit,
				      &sequence_complete) ==
				      SIPPYMODEM_V8_FRAMING_OK,
			      "V.8 menu bit generation failed");
			if (sequence_complete)
				completed_sequences++;
		}
		CHECK(sippymodem_v8_fsk_transmit_sample(
			      &source, transmit_bit, &sample, &bit_consumed) ==
			      SIPPYMODEM_V8_FSK_OK &&
		      sippymodem_v8_fsk_receive_sample(
			      &destination, codec_round_trip(sample, codec),
			      &receive_bit, &bit_ready) ==
			      SIPPYMODEM_V8_FSK_OK,
		      "V.8 menu audio processing failed");
		if (bit_ready)
			CHECK(sippymodem_v8_menu_detect_bit(
				      &detector, receive_bit, &detected,
				      &confirmed) == SIPPYMODEM_V8_MENU_OK,
			      "V.8 menu bit recovery failed");
	}
	CHECK(confirmed && completed_sequences >= 2 &&
		      detected.call_function == offered.call_function &&
		      detected.modulations == offered.modulations &&
		      detected.modulation_octets == offered.modulation_octets &&
		      detected.lapm == offered.lapm,
	      "V.8 role %d codec %d did not recover the repeated menu",
	      source_role, codec);
	return true;
}

static bool test_menu_audio_round_trip(void)
{
	for (enum test_codec codec = TEST_LINEAR; codec <= TEST_PCMA; ++codec)
		if (!menu_round_trip(SIPPYMODEM_V8_CALLING_STATION, codec) ||
		    !menu_round_trip(SIPPYMODEM_V8_ANSWERING_STATION, codec))
			return false;
	return true;
}

static bool test_validation_and_reset(void)
{
	struct sippymodem_v8_fsk fsk;
	struct sippymodem_v8_fsk saved;
	int16_t sample = 123;
	uint8_t bit = 7;
	bool ready = true;

	CHECK(sippymodem_v8_fsk_init(
		      &fsk, SIPPYMODEM_V8_CALLING_STATION) ==
		      SIPPYMODEM_V8_FSK_OK,
	      "V.8 FSK validation fixture did not initialize");
	fsk.role = (enum sippymodem_v8_station_role)2;
	saved = fsk;
	CHECK(sippymodem_v8_fsk_init(
		      NULL, SIPPYMODEM_V8_CALLING_STATION) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      sippymodem_v8_fsk_init(
		      &fsk, (enum sippymodem_v8_station_role)2) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      !sippymodem_v8_fsk_can_accept_bit(&fsk) &&
	      sippymodem_v8_fsk_transmit_sample(
		      &fsk, 1, &sample, &ready) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      sample == 123 && ready &&
	      sippymodem_v8_fsk_receive_sample(
		      &fsk, 0, &bit, &ready) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      bit == 7 && ready &&
	      sippymodem_v8_fsk_reset(&fsk) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      strcmp(sippymodem_v8_fsk_result_name(
			     SIPPYMODEM_V8_FSK_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "V.8 FSK API accepted invalid state");

	CHECK(sippymodem_v8_fsk_init(
		      &fsk, SIPPYMODEM_V8_ANSWERING_STATION) ==
		      SIPPYMODEM_V8_FSK_OK,
	      "V.8 FSK valid-argument fixture did not initialize");
	saved = fsk;
	sample = 123;
	ready = true;
	CHECK(sippymodem_v8_fsk_transmit_sample(
		      &fsk, 2, &sample, &ready) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      sample == 123 && ready && memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      sippymodem_v8_fsk_transmit_sample(
		      &fsk, 1, NULL, &ready) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      sippymodem_v8_fsk_receive_sample(
		      &fsk, 0, NULL, &ready) ==
		      SIPPYMODEM_V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0,
	      "V.8 FSK API changed state after an invalid argument");

	CHECK(sippymodem_v8_fsk_transmit_sample(
		      &fsk, 1, &sample, &ready) == SIPPYMODEM_V8_FSK_OK &&
	      sippymodem_v8_fsk_reset(&fsk) == SIPPYMODEM_V8_FSK_OK &&
	      fsk.role == SIPPYMODEM_V8_ANSWERING_STATION &&
	      fsk.transmitter.tones.carrier_phase == 0 &&
	      fsk.receiver.history_count == 0,
	      "V.8 FSK reset did not preserve role and clear stream state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "station channels", test_station_channels },
		{ "menu audio round trip", test_menu_audio_round_trip },
		{ "validation and reset", test_validation_and_reset },
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
