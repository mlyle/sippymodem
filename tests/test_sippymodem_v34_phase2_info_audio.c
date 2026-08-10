// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase2_info_audio.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_PAYLOAD_BITS = 17,
	TEST_SAMPLE_LIMIT = 1000,
};

static bool build_expected_bits(
	const uint8_t *payload, uint8_t *bits, size_t *bit_count)
{
	struct sippymodem_v34_phase2_info_generator generator;
	size_t count = sippymodem_v34_phase2_info_frame_bit_count(
		TEST_PAYLOAD_BITS);

	CHECK(sippymodem_v34_phase2_info_generator_init(
		      &generator, payload, TEST_PAYLOAD_BITS) ==
		      SIPPYMODEM_V34_PHASE2_INFO_OK,
	      "expected Phase 2 frame did not initialize");
	for (size_t index = 0; index < count; ++index) {
		bool complete;

		CHECK(sippymodem_v34_phase2_info_next_bit(
			      &generator, &bits[index], &complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_OK &&
			      complete == (index + 1u == count),
		      "expected Phase 2 frame ended at bit %zu", index);
	}
	*bit_count = count;
	return true;
}

static bool check_station(
	enum sippymodem_v34_phase2_station_role role)
{
	static const uint8_t payload[] = { 0xa5, 0x5a, 0x80 };
	struct sippymodem_v34_phase2_info_audio_transmitter transmitter;
	uint8_t expected[SIPPYMODEM_V34_PHASE2_INFO_MAX_FRAME_BITS];
	size_t expected_count;
	size_t observed_count = 0;
	size_t sample_count;
	bool saw_reference = false;
	bool complete = false;

	CHECK(build_expected_bits(payload, expected, &expected_count) &&
		      sippymodem_v34_phase2_info_audio_transmitter_init(
			      &transmitter, role, TEST_AMPLITUDE, payload,
			      TEST_PAYLOAD_BITS) ==
			      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO audio transmitter did not initialize");
	CHECK(sippymodem_v34_phase2_info_audio_transmitter_is_valid(
		      &transmitter) &&
		      transmitter.frame.bit_index == 0 &&
		      transmitter.modulation.initial_point_pending,
	      "Phase 2 INFO audio transmitter began in a corrupt state");

	for (sample_count = 0; sample_count < TEST_SAMPLE_LIMIT;
	     ++sample_count) {
		size_t previous_bit_index = transmitter.frame.bit_index;
		bool reference_was_pending =
			transmitter.modulation.initial_point_pending;
		bool previous_inversion =
			transmitter.modulation.point_inverted;
		int16_t sample;

		CHECK(sippymodem_v34_phase2_info_audio_transmitter_sample(
			      &transmitter, &sample, &complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK,
		      "Phase 2 INFO audio failed at sample %zu", sample_count);
		if (reference_was_pending &&
		    !transmitter.modulation.initial_point_pending) {
			CHECK(transmitter.frame.bit_index == previous_bit_index &&
				      !complete,
			      "arbitrary reference point consumed a frame bit");
			saw_reference = true;
		}
		if (transmitter.frame.bit_index != previous_bit_index) {
			CHECK(transmitter.frame.bit_index ==
				      previous_bit_index + 1u &&
				      previous_bit_index == observed_count &&
				      transmitter.modulation.point_inverted ==
					      (previous_inversion ^
					       (expected[observed_count] != 0)),
			      "DPSK phase changed incorrectly for INFO bit %zu",
			      observed_count);
			observed_count++;
			CHECK(complete == (observed_count == expected_count),
			      "Phase 2 INFO completion moved away from its final bit");
		} else {
			CHECK(!complete,
			      "Phase 2 INFO completed without accepting a bit");
		}
		if (complete) {
			sample_count++;
			break;
		}
	}
	CHECK(saw_reference && observed_count == expected_count && complete &&
		      transmitter.frame_complete &&
		      sippymodem_v34_phase2_info_audio_transmitter_is_valid(
			      &transmitter),
	      "Phase 2 INFO audio omitted part of its one-shot sequence");
	CHECK(sample_count > expected_count * 12u &&
		      sample_count < (expected_count + 2u) * 15u,
	      "Phase 2 INFO audio used an implausible 600-bit/s duration");

	{
		unsigned char snapshot[sizeof(transmitter)];
		int16_t sample = 123;
		bool reported_complete = false;

		memcpy(snapshot, &transmitter, sizeof(snapshot));
		CHECK(sippymodem_v34_phase2_info_audio_transmitter_sample(
			      &transmitter, &sample, &reported_complete) ==
			      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
			      memcmp(snapshot, &transmitter, sizeof(snapshot)) == 0 &&
			      sample == 123 && !reported_complete,
		      "completed Phase 2 INFO audio emitted an extra sample");
	}
	return true;
}

static bool test_bidirectional_transmission(void)
{
	return check_station(SIPPYMODEM_V34_PHASE2_CALLING_STATION) &&
	       check_station(SIPPYMODEM_V34_PHASE2_ANSWERING_STATION);
}

static bool test_validation(void)
{
	static const uint8_t payload[] = { 0xa5, 0x5a, 0x80 };
	struct sippymodem_v34_phase2_info_audio_transmitter transmitter;
	unsigned char snapshot[sizeof(transmitter)];
	int16_t sample = 321;
	bool complete = true;

	CHECK(sippymodem_v34_phase2_info_audio_transmitter_init(
		      NULL, SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      TEST_AMPLITUDE, payload, TEST_PAYLOAD_BITS) ==
		      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      sippymodem_v34_phase2_info_audio_transmitter_init(
			      &transmitter,
			      SIPPYMODEM_V34_PHASE2_CALLING_STATION,
			      TEST_AMPLITUDE, payload,
			      SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
			      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT,
	      "Phase 2 INFO audio accepted an invalid configuration");
	CHECK(sippymodem_v34_phase2_info_audio_transmitter_init(
		      &transmitter, SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      TEST_AMPLITUDE, payload, TEST_PAYLOAD_BITS) ==
		      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO audio validation fixture did not initialize");
	transmitter.modulation.carrier.symbol_phase_units =
		transmitter.modulation.carrier.symbol_period_units;
	CHECK(!sippymodem_v34_phase2_info_audio_transmitter_is_valid(
		      &transmitter),
	      "Phase 2 INFO audio accepted corrupt modulation state");
	memcpy(snapshot, &transmitter, sizeof(snapshot));
	CHECK(sippymodem_v34_phase2_info_audio_transmitter_sample(
		      &transmitter, &sample, &complete) ==
		      SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      memcmp(snapshot, &transmitter, sizeof(snapshot)) == 0 &&
		      sample == 321 && complete,
	      "invalid Phase 2 INFO audio mutated state or outputs");
	CHECK(strcmp(sippymodem_v34_phase2_info_audio_result_name(
			     SIPPYMODEM_V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "Phase 2 INFO audio returned an unstable result name");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional transmission", test_bidirectional_transmission },
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
