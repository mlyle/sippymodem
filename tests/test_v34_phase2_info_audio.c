// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_info_audio.h>
#include <g711.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_PAYLOAD_BITS = 17,
	TEST_SAMPLE_LIMIT = 1000,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

static bool build_expected_bits(
	const uint8_t *payload, uint8_t *bits, size_t *bit_count)
{
	struct v34_phase2_info_generator generator;
	size_t count = v34_phase2_info_frame_bit_count(
		TEST_PAYLOAD_BITS);

	CHECK(v34_phase2_info_generator_init(
		      &generator, payload, TEST_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_OK,
	      "expected Phase 2 frame did not initialize");
	for (size_t index = 0; index < count; ++index) {
		bool complete;

		CHECK(v34_phase2_info_next_bit(
			      &generator, &bits[index], &complete) ==
			      V34_PHASE2_INFO_OK &&
			      complete == (index + 1u == count),
		      "expected Phase 2 frame ended at bit %zu", index);
	}
	*bit_count = count;
	return true;
}

static bool check_station(
	enum v34_station_role role)
{
	static const uint8_t payload[] = { 0xa5, 0x5a, 0x80 };
	struct v34_phase2_info_audio_transmitter transmitter;
	uint8_t expected[V34_PHASE2_INFO_MAX_FRAME_BITS];
	size_t expected_count;
	size_t observed_count = 0;
	size_t sample_count;
	bool saw_reference = false;
	bool complete = false;

	CHECK(build_expected_bits(payload, expected, &expected_count) &&
		      v34_phase2_info_audio_transmitter_init(
			      &transmitter, role, TEST_AMPLITUDE, payload,
			      TEST_PAYLOAD_BITS) ==
			      V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO audio transmitter did not initialize");
	CHECK(v34_phase2_info_audio_transmitter_is_valid(
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

		CHECK(v34_phase2_info_audio_transmitter_sample(
			      &transmitter, &sample, &complete) ==
			      V34_PHASE2_INFO_AUDIO_OK,
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
		      v34_phase2_info_audio_transmitter_is_valid(
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
		CHECK(v34_phase2_info_audio_transmitter_sample(
			      &transmitter, &sample, &reported_complete) ==
			      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
			      memcmp(snapshot, &transmitter, sizeof(snapshot)) == 0 &&
			      sample == 123 && !reported_complete,
		      "completed Phase 2 INFO audio emitted an extra sample");
	}
	return true;
}

static bool test_bidirectional_transmission(void)
{
	return check_station(V34_CALLING_STATION) &&
	       check_station(V34_ANSWERING_STATION);
}

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	if (codec == TEST_CODEC_PCMU)
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	return g711_decode_pcma(
		g711_encode_pcma(sample));
}

static bool check_frame_recovery(
	enum v34_station_role transmit_role,
	enum test_codec codec)
{
	static const uint8_t payload[] = { 0xa5, 0x5a, 0x80 };
	enum v34_station_role receive_role =
		transmit_role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION :
			V34_CALLING_STATION;
	struct v34_phase2_info_audio_transmitter transmitter;
	struct v34_phase2_info_audio_receiver receiver;
	struct v34_phase2_info_event event = { 0 };
	struct v34_phase2_info_event received_event = { 0 };
	struct v34_phase2_dpsk_measurement measurement;
	bool transmit_complete = false;
	bool frame_received = false;

	CHECK(v34_phase2_info_audio_transmitter_init(
		      &transmitter, transmit_role, TEST_AMPLITUDE, payload,
		      TEST_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_OK &&
		      v34_phase2_info_audio_receiver_init(
			      &receiver, receive_role, TEST_PAYLOAD_BITS) ==
			      V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO G.711 endpoints did not initialize");
	for (size_t index = 0; index < 7; ++index) {
		int16_t silence = codec_round_trip(0, codec);

		CHECK(v34_phase2_info_audio_receiver_sample(
			      &receiver, silence, &event) ==
			      V34_PHASE2_INFO_AUDIO_OK &&
			      event.type == V34_PHASE2_INFO_NO_EVENT,
		      "Phase 2 INFO receiver emitted an event during silence");
	}
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t sample;

		if (!transmit_complete) {
			CHECK(v34_phase2_info_audio_transmitter_sample(
				      &transmitter, &sample,
				      &transmit_complete) ==
				      V34_PHASE2_INFO_AUDIO_OK,
			      "Phase 2 INFO G.711 transmitter failed at %zu",
			      index);
		} else {
			bool bit_accepted;

			/* Preserve the carrier while the receive filter delivers its
			 * final delayed decisions. The Phase 2 controller will own the
			 * corresponding transition to the following carrier tone. */
			CHECK(v34_phase2_dpsk_transmitter_sample(
				      &transmitter.modulation, 0, &sample,
				      &bit_accepted) ==
				      V34_PHASE2_DPSK_OK,
			      "Phase 2 post-INFO carrier failed at %zu", index);
		}
		sample = codec_round_trip(sample, codec);
		CHECK(v34_phase2_info_audio_receiver_sample(
			      &receiver, sample, &event) ==
			      V34_PHASE2_INFO_AUDIO_OK,
		      "Phase 2 INFO G.711 receiver failed at %zu", index);
		CHECK(event.type != V34_PHASE2_INFO_FRAME_REJECTED,
		      "Phase 2 INFO receiver rejected role %d codec %d at %zu",
		      (int)transmit_role, (int)codec, index);
		if (event.type == V34_PHASE2_INFO_FRAME_RECEIVED) {
			frame_received = true;
			received_event = event;
		}
		if (transmit_complete && frame_received)
			break;
	}
	CHECK(transmit_complete && frame_received &&
		      received_event.payload_bit_count == TEST_PAYLOAD_BITS &&
		      memcmp(received_event.payload, payload,
			     sizeof(payload)) == 0,
	      "Phase 2 INFO payload changed across G.711 audio");
	measurement =
		v34_phase2_info_audio_receiver_measure(&receiver);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "Phase 2 INFO audio receiver omitted its signal measurement");
	return true;
}

static bool test_g711_frame_recovery(void)
{
	return check_frame_recovery(
		       V34_CALLING_STATION,
		       TEST_CODEC_PCMU) &&
	       check_frame_recovery(
		       V34_ANSWERING_STATION,
		       TEST_CODEC_PCMU) &&
	       check_frame_recovery(
		       V34_CALLING_STATION,
		       TEST_CODEC_PCMA) &&
	       check_frame_recovery(
		       V34_ANSWERING_STATION,
		       TEST_CODEC_PCMA);
}

static bool test_validation(void)
{
	static const uint8_t payload[] = { 0xa5, 0x5a, 0x80 };
	struct v34_phase2_info_audio_transmitter transmitter;
	struct v34_phase2_info_audio_receiver receiver;
	unsigned char snapshot[sizeof(transmitter)];
	unsigned char receiver_snapshot[sizeof(receiver)];
	struct v34_phase2_info_event event = {
		.type = V34_PHASE2_INFO_FRAME_REJECTED,
	};
	int16_t sample = 321;
	bool complete = true;

	CHECK(v34_phase2_info_audio_transmitter_init(
		      NULL, V34_CALLING_STATION,
		      TEST_AMPLITUDE, payload, TEST_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      v34_phase2_info_audio_transmitter_init(
			      &transmitter,
			      V34_CALLING_STATION,
			      TEST_AMPLITUDE, payload,
			      V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
			      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT,
	      "Phase 2 INFO audio accepted an invalid configuration");
	CHECK(v34_phase2_info_audio_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      TEST_AMPLITUDE, payload, TEST_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO audio validation fixture did not initialize");
	transmitter.modulation.carrier.symbol_phase_units =
		transmitter.modulation.carrier.symbol_period_units;
	CHECK(!v34_phase2_info_audio_transmitter_is_valid(
		      &transmitter),
	      "Phase 2 INFO audio accepted corrupt modulation state");
	memcpy(snapshot, &transmitter, sizeof(snapshot));
	CHECK(v34_phase2_info_audio_transmitter_sample(
		      &transmitter, &sample, &complete) ==
		      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      memcmp(snapshot, &transmitter, sizeof(snapshot)) == 0 &&
		      sample == 321 && complete,
	      "invalid Phase 2 INFO audio mutated state or outputs");
	CHECK(strcmp(v34_phase2_info_audio_result_name(
			     V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT),
		     "invalid argument") == 0,
	      "Phase 2 INFO audio returned an unstable result name");
	CHECK(v34_phase2_info_audio_receiver_init(
		      NULL, V34_CALLING_STATION,
		      TEST_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      v34_phase2_info_audio_receiver_init(
			      &receiver,
			      V34_CALLING_STATION,
			      V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 1u) ==
			      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      v34_phase2_info_audio_receiver_init(
			      &receiver,
			      V34_CALLING_STATION,
			      TEST_PAYLOAD_BITS) ==
			      V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO audio receiver validation fixture failed");
	memcpy(receiver_snapshot, &receiver, sizeof(receiver_snapshot));
	CHECK(v34_phase2_info_audio_receiver_sample(
		      &receiver, 0, NULL) ==
		      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      memcmp(receiver_snapshot, &receiver, sizeof(receiver_snapshot)) ==
			      0 &&
		      v34_phase2_info_audio_receiver_reset(&receiver) ==
			      V34_PHASE2_INFO_AUDIO_OK,
	      "Phase 2 INFO audio receiver validation or reset failed");
	receiver.modulation.detector_output = NAN;
	memcpy(receiver_snapshot, &receiver, sizeof(receiver_snapshot));
	CHECK(v34_phase2_info_audio_receiver_sample(
		      &receiver, 0, &event) ==
		      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      memcmp(receiver_snapshot, &receiver, sizeof(receiver_snapshot)) ==
			      0 &&
		      event.type == V34_PHASE2_INFO_FRAME_REJECTED,
	      "corrupt Phase 2 INFO receiver mutated state or outputs");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional transmission", test_bidirectional_transmission },
		{ "G.711 frame recovery", test_g711_frame_recovery },
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
