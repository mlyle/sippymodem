// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v21.h>
#include <v8_fsk.h>
#include <v8_menu.h>

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
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	if (codec == TEST_PCMA)
		return g711_decode_pcma(
			g711_encode_pcma(sample));
	return sample;
}

static bool channel_matches(
	const struct fsk_channel *actual,
	const struct fsk_channel *expected)
{
	return actual->sample_rate == expected->sample_rate &&
	       actual->symbol_rate_numerator ==
		       expected->symbol_rate_numerator &&
	       actual->symbol_rate_denominator ==
		       expected->symbol_rate_denominator &&
	       actual->mark_frequency_hz == expected->mark_frequency_hz &&
	       actual->space_frequency_hz == expected->space_frequency_hz;
}

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	hash ^= word & UINT16_C(0xff);
	hash *= UINT64_C(1099511628211);
	hash ^= word >> 8;
	return hash * UINT64_C(1099511628211);
}

static bool test_station_channels(void)
{
	struct v8_fsk caller;
	struct v8_fsk answerer;

	CHECK(v8_fsk_init(
		      &caller, V8_CALLING_STATION) ==
		      V8_FSK_OK &&
	      v8_fsk_init(
		      &answerer, V8_ANSWERING_STATION) ==
		      V8_FSK_OK,
	      "V.8 FSK stations did not initialize");
	CHECK(channel_matches(
		      &caller.receiver.channel, &v21_channel_2) &&
	      channel_matches(
		      &answerer.receiver.channel, &v21_channel_1) &&
	      caller.transmitter.mark_phase_step == 1004 &&
	      caller.transmitter.space_phase_step == 1208 &&
	      answerer.transmitter.mark_phase_step == 1690 &&
	      answerer.transmitter.space_phase_step == 1894 &&
	      caller.transmitter.gain == 0x3ead &&
	      answerer.transmitter.gain == 0x3ead,
	      "V.8 FSK stations selected the wrong V.21 channels");
	return true;
}

static bool test_transmit_oracle(void)
{
	static const uint64_t expected_hash[] = {
		UINT64_C(0x0946f673c1b431e4),
		UINT64_C(0xa87832e2a321791b),
	};
	static const unsigned int expected_periods[] = { 27, 27, 26 };

	for (enum v8_station_role role = V8_CALLING_STATION;
	     role <= V8_ANSWERING_STATION; ++role) {
		struct v8_fsk fsk;
		uint64_t hash = UINT64_C(14695981039346656037);
		unsigned int period = 0;
		unsigned int period_index = 0;

		CHECK(v8_fsk_init(&fsk, role) == V8_FSK_OK,
		      "V.8 FSK oracle role %u did not initialize", role);
		for (size_t index = 0; index < 512; ++index) {
			int16_t sample;
			bool consumed;

			CHECK(v8_fsk_transmit_sample(
				      &fsk, 1, &sample, &consumed) == V8_FSK_OK,
			      "V.8 FSK oracle role %u stopped at %zu",
			      role, index);
			hash = hash_word(hash, (uint16_t)sample);
			period++;
			if (consumed && period_index < ARRAY_LENGTH(expected_periods)) {
				CHECK(period == expected_periods[period_index] &&
					      v8_fsk_can_accept_bit(&fsk),
				      "V.8 FSK bit period %u was %u",
				      period_index, period);
				period = 0;
				period_index++;
			}
		}
		CHECK(hash == expected_hash[role] &&
			      period_index == ARRAY_LENGTH(expected_periods),
		      "V.8 FSK role %u wire hash changed: %#llx",
		      role, (unsigned long long)hash);
	}
	return true;
}

static bool menu_round_trip(
	enum v8_station_role source_role,
	enum test_codec codec)
{
	struct v8_control_generator generator;
	struct v8_menu_detector detector;
	struct v8_menu offered;
	struct v8_menu detected = { 0 };
	struct v8_fsk source;
	struct v8_fsk destination;
	struct v8_fsk blocked;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	uint8_t transmit_bit = 1;
	size_t information_count;
	unsigned int completed_sequences = 0;
	bool confirmed = false;

	CHECK(v8_call_menu_init(
		      &offered, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_menu_encode(
		      &offered, information, sizeof(information),
		      &information_count) == V8_MENU_OK &&
	      v8_control_generator_init(
		      &generator, information, information_count) ==
		      V8_FRAMING_OK &&
	      v8_menu_detector_init(&detector) ==
		      V8_MENU_OK &&
	      v8_fsk_init(&source, source_role) ==
		      V8_FSK_OK &&
	      v8_fsk_init(
		      &destination,
		      source_role == V8_CALLING_STATION ?
			      V8_ANSWERING_STATION :
			      V8_CALLING_STATION) ==
		      V8_FSK_OK,
	      "V.8 menu audio endpoints did not initialize");
	blocked = destination;

	for (size_t sample_index = 0;
	     sample_index < 3u * V21_SAMPLE_RATE && !confirmed;
	     ++sample_index) {
		int16_t sample;
		uint8_t receive_bit;
		bool bit_consumed;
		bool bit_ready;
		struct v8_line_conditioned_block block, saved_block;
		struct v8_line_conditioner saved_line;
		uint8_t blocked_bit = 0;
		bool ready, blocked_ready = false;

		if (v8_fsk_can_accept_bit(&source)) {
			bool sequence_complete;

			CHECK(v8_control_next_bit(
				      &generator, &transmit_bit,
				      &sequence_complete) ==
				      V8_FRAMING_OK,
			      "V.8 menu bit generation failed");
			if (sequence_complete)
				completed_sequences++;
		}
		CHECK(v8_fsk_transmit_sample(
			      &source, transmit_bit, &sample, &bit_consumed) ==
			      V8_FSK_OK &&
		      v8_fsk_receive_sample(
			      &destination, codec_round_trip(sample, codec),
			      &receive_bit, &bit_ready) ==
			      V8_FSK_OK,
		      "V.8 menu audio processing failed");
		CHECK(v8_line_conditioner_process_sample(&blocked.line,
			      codec_round_trip(sample, codec), &block, &ready) == V8_LINE_CONDITIONER_OK,
		      "shared FSK conditioning failed");
		if (ready) {
			memcpy(&saved_block, &block, sizeof(block));
			saved_line = blocked.line;
			CHECK(v8_fsk_receive_block(&blocked, &block, &blocked_bit, &blocked_ready) == V8_FSK_OK &&
			      memcmp(&saved_line, &blocked.line, sizeof(saved_line)) == 0 &&
			      memcmp(&saved_block, &block, sizeof(block)) == 0,
			      "FSK block processing advanced the line or modified input");
		}
		CHECK(blocked_bit == receive_bit && blocked_ready == bit_ready &&
		      memcmp(&blocked, &destination, sizeof(blocked)) == 0,
		      "FSK block processing changed samplewise state or decisions");
		if (bit_ready)
			CHECK(v8_menu_detect_bit(
				      &detector, receive_bit, &detected,
				      &confirmed) == V8_MENU_OK,
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
		if (!menu_round_trip(V8_CALLING_STATION, codec) ||
		    !menu_round_trip(V8_ANSWERING_STATION, codec))
			return false;
	return true;
}

static bool test_block_validation(void)
{
	struct v8_fsk fsk, saved;
	struct v8_line_conditioned_block block = { 0 }, saved_block = { 0 };
	uint8_t bit = 7;
	bool ready = true;

	CHECK(v8_fsk_init(&fsk, V8_CALLING_STATION) == V8_FSK_OK,
	      "FSK block validation fixture");
	saved = fsk;
	CHECK(v8_fsk_receive_block(NULL, &block, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, NULL, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, NULL, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, &bit, NULL) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, (void *)&fsk, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, (void *)&fsk, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, &bit, (void *)&fsk) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, (void *)&block, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, &bit, (void *)&block) == V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_block(&fsk, &block, (void *)&ready, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      memcmp(&block, &saved_block, sizeof(block)) == 0 && bit == 7 && ready,
	      "FSK block rejection changed state, input, or output");
	fsk.line.role = V8_ANSWERING_STATION;
	saved = fsk;
	CHECK(v8_fsk_receive_block(&fsk, &block, &bit, &ready) == V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 && bit == 7 && ready,
	      "FSK block accepted inconsistent line ownership");
	return true;
}

static bool test_validation_and_reset(void)
{
	struct v8_fsk fsk;
	struct v8_fsk saved;
	int16_t sample = 123;
	uint8_t bit = 7;
	bool ready = true;

	CHECK(v8_fsk_init(
		      &fsk, V8_CALLING_STATION) ==
		      V8_FSK_OK,
	      "V.8 FSK validation fixture did not initialize");
	fsk.role = (enum v8_station_role)2;
	saved = fsk;
	CHECK(v8_fsk_init(
		      NULL, V8_CALLING_STATION) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_init(
		      &fsk, (enum v8_station_role)2) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      !v8_fsk_can_accept_bit(&fsk) &&
	      v8_fsk_transmit_sample(
		      &fsk, 1, &sample, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      sample == 123 && ready &&
	      v8_fsk_receive_sample(
		      &fsk, 0, &bit, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      bit == 7 && ready &&
	      v8_fsk_reset(&fsk) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      strcmp(v8_fsk_result_name(
			     V8_FSK_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "V.8 FSK API accepted invalid state");

	CHECK(v8_fsk_init(
		      &fsk, V8_ANSWERING_STATION) ==
		      V8_FSK_OK,
	      "V.8 FSK valid-argument fixture did not initialize");
	saved = fsk;
	sample = 123;
	ready = true;
	CHECK(v8_fsk_transmit_sample(
		      &fsk, 2, &sample, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      sample == 123 && ready && memcmp(&fsk, &saved, sizeof(fsk)) == 0 &&
	      v8_fsk_transmit_sample(
		      &fsk, 1, NULL, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      v8_fsk_receive_sample(
		      &fsk, 0, NULL, &ready) ==
		      V8_FSK_INVALID_ARGUMENT &&
	      memcmp(&fsk, &saved, sizeof(fsk)) == 0,
	      "V.8 FSK API changed state after an invalid argument");

	CHECK(v8_fsk_transmit_sample(
		      &fsk, 1, &sample, &ready) == V8_FSK_OK &&
	      v8_fsk_reset(&fsk) == V8_FSK_OK &&
	      fsk.role == V8_ANSWERING_STATION &&
	      fsk.transmitter.carrier_phase == 0 &&
	      fsk.transmitter.bit_progress == 0 &&
	      fsk.transmitter.filter_history[33] == 0x1000 &&
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
		{ "transmit oracle", test_transmit_oracle },
		{ "menu audio round trip", test_menu_audio_round_trip },
		{ "block validation", test_block_validation },
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
