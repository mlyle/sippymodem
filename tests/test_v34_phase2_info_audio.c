// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_info_audio.h>
#include <g711.h>
#include <phase.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_PCM_AMPLITUDE = 5320,
	TEST_PAYLOAD_BITS = 17,
	TEST_SAMPLE_LIMIT = 1000,
	TEST_TONE_SAMPLES = V34_PHASE2_SAMPLE_RATE / 10,
};

enum test_codec {
	TEST_CODEC_LINEAR,
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
	if (codec == TEST_CODEC_LINEAR)
		return sample;
	if (codec == TEST_CODEC_PCMU)
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	return g711_decode_pcma(
		g711_encode_pcma(sample));
}

static bool check_frame_recovery(
	enum v34_station_role transmit_role,
	enum test_codec codec, uint16_t receive_gain_q15,
	int16_t amplitude)
{
	static const uint8_t payload[] = { 0xa5, 0x5a, 0x80 };
	enum v34_station_role receive_role =
		v34_station_peer_role(transmit_role);
	struct v34_phase2_info_audio_transmitter transmitter;
	struct v34_phase2_info_audio_receiver receiver;
	struct v34_phase2_info_event event = { 0 };
	struct v34_phase2_info_event received_event = { 0 };
	struct v34_phase2_dpsk_measurement measurement;
	bool transmit_complete = false;
	bool frame_received = false;

	CHECK(v34_phase2_info_audio_transmitter_init(
		      &transmitter, transmit_role, amplitude, payload,
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
		/* Attenuate the physical signal before companding, not a parallel
		 * post-codec lane unavailable to the INFO receiver. */
		sample = (int16_t)((int32_t)sample * receive_gain_q15 / 32767);
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
	      "Phase 2 INFO payload changed for role %d codec %d gain %u",
	      transmit_role, codec, receive_gain_q15);
	measurement = v34_phase2_dpsk_receiver_measure(
		&receiver.modulation);
	CHECK(measurement.timing_locked && measurement.line_power > 1.0 &&
		      measurement.symbol_power > 1.0,
	      "Phase 2 INFO audio receiver omitted its signal measurement");
	return true;
}

static bool test_g711_frame_recovery(void)
{
	return check_frame_recovery(
		       V34_CALLING_STATION,
		       TEST_CODEC_PCMU, 32767, TEST_AMPLITUDE) &&
	       check_frame_recovery(
		       V34_ANSWERING_STATION,
		       TEST_CODEC_PCMU, 32767, TEST_AMPLITUDE) &&
	       check_frame_recovery(
		       V34_CALLING_STATION,
		       TEST_CODEC_PCMA, 32767, TEST_AMPLITUDE) &&
	       check_frame_recovery(
		       V34_ANSWERING_STATION,
		       TEST_CODEC_PCMA, 32767, TEST_AMPLITUDE);
}

static bool test_weak_frame_recovery(void)
{
	static const uint16_t gains[] = { 1600, 2048, 2400 };

	for (size_t gain = 0; gain < ARRAY_LENGTH(gains); ++gain)
		for (int role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			for (int codec = TEST_CODEC_LINEAR;
			     codec <= TEST_CODEC_PCMA; ++codec)
				/* Use the calibrated PCM call level, whose INFO energy
				 * exposes fixed differential quantization without AGC. */
				CHECK(check_frame_recovery(role, codec, gains[gain],
						  TEST_PCM_AMPLITUDE),
				      "weak INFO recovery failed");
	return true;
}

static bool test_nonframe_input(void)
{
	static const unsigned int noise_peaks[] = { 0, 400, 4000 };

	for (int role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role)
		for (int codec = TEST_CODEC_LINEAR;
		     codec <= TEST_CODEC_PCMA; ++codec)
			for (size_t noise = 0; noise < ARRAY_LENGTH(noise_peaks);
			     ++noise) {
				struct v34_phase2_info_audio_receiver receiver;
				uint32_t random = UINT32_C(0x7612d408);
				unsigned int noise_peak = noise_peaks[noise];

				CHECK(v34_phase2_info_audio_receiver_init(&receiver,
					role, TEST_PAYLOAD_BITS) ==
					V34_PHASE2_INFO_AUDIO_OK,
				      "nonframe INFO receiver did not initialize");
				for (size_t index = 0; index < 8000; ++index) {
					struct v34_phase2_info_event event;
					int16_t sample = 0;

					if (noise_peak != 0) {
						random = random * UINT32_C(1664525) +
							UINT32_C(1013904223);
						sample = (int16_t)((int32_t)(random %
							(2u * noise_peak + 1u)) -
							(int32_t)noise_peak);
					}
					CHECK(v34_phase2_info_audio_receiver_sample(
						&receiver, codec_round_trip(sample, codec),
						&event) == V34_PHASE2_INFO_AUDIO_OK &&
					      event.type != V34_PHASE2_INFO_FRAME_RECEIVED,
					      "nonframe input produced a valid INFO frame");
				}
				if (noise_peak == 400)
					CHECK(receiver.modulation.gain.gain > 0x0600,
					      "weak-noise control did not exercise gain increase");
			}
	return true;
}

static bool test_info_marks_detection(void)
{
	struct v34_phase2_dpsk_transmitter transmitter;
	struct v34_phase2_info_audio_receiver receiver;
	unsigned int detections = 0;

	CHECK(v34_phase2_dpsk_transmitter_init(
		      &transmitter, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE) == V34_PHASE2_DPSK_OK &&
	      v34_phase2_info_audio_receiver_init(
		      &receiver, V34_CALLING_STATION,
		      TEST_PAYLOAD_BITS) == V34_PHASE2_INFO_AUDIO_OK,
	      "INFO marks endpoints did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v34_phase2_info_event event;
		uint8_t marks_before = receiver.consecutive_mark_bits;
		int16_t sample;
		bool bit_accepted;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &transmitter, 1, &sample, &bit_accepted) ==
			      V34_PHASE2_DPSK_OK &&
		      v34_phase2_info_audio_receiver_sample(
			      &receiver, sample, &event) ==
			      V34_PHASE2_INFO_AUDIO_OK &&
		      event.type == V34_PHASE2_INFO_NO_EVENT,
		      "INFO marks path failed at sample %zu", index);
		if (!event.info_marks)
			continue;
		CHECK(marks_before == V34_PHASE2_INFO_MARKS_BITS - 1u &&
		      receiver.consecutive_mark_bits == 0,
		      "INFO marks qualified away from the exact bit boundary");
		if (++detections == 2u)
			break;
	}
	CHECK(detections == 2u &&
	      receiver.frame.stage == V34_PHASE2_INFO_SEARCH_SYNC,
	      "continuous INFO marks did not produce bounded indications");
	return true;
}

static bool test_carrier_loss_is_not_info_marks(void)
{
	for (int role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role) {
		for (int codec = TEST_CODEC_LINEAR;
		     codec <= TEST_CODEC_PCMA; ++codec) {
			struct v34_phase2_dpsk_transmitter transmitter;
			struct v34_phase2_info_audio_receiver receiver;
			const size_t quiet_begin = V34_PHASE2_SAMPLE_RATE;
			const size_t quiet_end = quiet_begin + 560u;
			unsigned int detections = 0;

			CHECK(v34_phase2_dpsk_transmitter_init(
				      &transmitter, role, TEST_PCM_AMPLITUDE) ==
				      V34_PHASE2_DPSK_OK &&
			      v34_phase2_info_audio_receiver_init(
				      &receiver, v34_station_peer_role(role),
				      TEST_PAYLOAD_BITS) == V34_PHASE2_INFO_AUDIO_OK,
			      "carrier-loss endpoints did not initialize");
			/* Tone, the full-retrain silence, then genuine INFOMARKS.
			 * Keep the same receiver and its recovered bit clock. */
			for (size_t index = 0;
			     index < quiet_end + TEST_SAMPLE_LIMIT; ++index) {
				struct v34_phase2_info_event event;
				int16_t sample;
				bool accepted;

				CHECK(v34_phase2_dpsk_transmitter_sample(
					      &transmitter, index >= quiet_end,
					      &sample, &accepted) == V34_PHASE2_DPSK_OK,
				      "carrier-loss transmitter stopped");
				if (index >= quiet_begin && index < quiet_end)
					sample = 0;
				CHECK(v34_phase2_info_audio_receiver_sample(
					      &receiver, codec_round_trip(sample, codec),
					      &event) == V34_PHASE2_INFO_AUDIO_OK &&
				      event.type == V34_PHASE2_INFO_NO_EVENT,
				      "carrier-loss receiver stopped or reported a frame");
				CHECK(index >= quiet_end || !event.info_marks,
				      "role %d codec %d carrier loss became INFOMARKS at %zu",
				      role, codec, index);
				detections += event.info_marks;
			}
			CHECK(detections >= 2u,
			      "role %d codec %d did not receive marks after silence",
			      role, codec);
		}
	}
	return true;
}

struct test_following_tone {
	uint32_t carrier_phase;
	uint32_t carrier_step;
	uint32_t guard_phase;
	uint32_t guard_step;
	int16_t carrier_amplitude;
	int16_t guard_amplitude;
};

static void initial_tone_init(
	struct test_following_tone *tone,
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	*tone = (struct test_following_tone){
		.carrier_step = transmitter->carrier.carrier_phase_step,
		.guard_step = transmitter->guard_phase_step,
		.carrier_amplitude = transmitter->carrier.amplitude,
		.guard_amplitude = transmitter->local_role ==
			V34_ANSWERING_STATION ? TEST_AMPLITUDE : 0,
	};
}

static void reversed_following_tone_init(
	struct test_following_tone *tone,
	const struct v34_phase2_dpsk_transmitter *transmitter)
{
	*tone = (struct test_following_tone){
		.carrier_phase = transmitter->carrier.carrier_phase +
			UINT32_C(0xc0000000) +
			(transmitter->point_inverted ? UINT32_C(0x80000000) : 0),
		.carrier_step = transmitter->carrier.carrier_phase_step,
		.guard_phase = transmitter->guard_phase,
		.guard_step = transmitter->guard_phase_step,
		.carrier_amplitude = transmitter->carrier.amplitude,
		.guard_amplitude = transmitter->local_role ==
			V34_ANSWERING_STATION ? TEST_AMPLITUDE : 0,
	};
}

static int16_t following_tone_sample(
	struct test_following_tone *tone)
{
	long mixed = lround(
		(double)tone->carrier_amplitude *
		sin(phase_radians_u32(tone->carrier_phase)));

	tone->carrier_phase += tone->carrier_step;
	if (tone->guard_amplitude != 0) {
		mixed += lround(
			(double)tone->guard_amplitude *
			sin(phase_radians_u32(tone->guard_phase)));
		tone->guard_phase += tone->guard_step;
	}
	return (int16_t)mixed;
}

struct handoff_measurement {
	size_t sync_to_transition_samples;
	size_t event_delay_samples;
	uint16_t reported_delay_samples;
};

static bool measure_handoff_delay(
	enum v34_station_role transmit_role, enum test_codec codec,
	size_t payload_bits, uint8_t payload_fill, size_t leading_samples,
	struct handoff_measurement *measurement)
{
	uint8_t payload[V34_PHASE2_INFO_MAX_PAYLOAD_BYTES] = { 0 };
	enum v34_station_role receive_role =
		v34_station_peer_role(transmit_role);
	struct v34_phase2_info_audio_transmitter transmitter;
	struct v34_phase2_info_audio_receiver receiver;
	struct test_following_tone tone = { 0 };
	size_t filter_tail_symbols = 0;
	size_t sync_sample = SIZE_MAX;
	size_t line_transition = SIZE_MAX;
	bool frame_complete = false;

	memset(payload, payload_fill, (payload_bits + 7u) / 8u);
	if (payload_bits % 8u != 0)
		payload[payload_bits / 8u] &=
			(uint8_t)(UINT8_MAX << (8u - payload_bits % 8u));
	CHECK(v34_phase2_info_audio_transmitter_init(
		      &transmitter, transmit_role, TEST_AMPLITUDE,
		      payload, payload_bits) == V34_PHASE2_INFO_AUDIO_OK &&
	      v34_phase2_info_audio_receiver_init(
		      &receiver, receive_role, payload_bits) ==
		      V34_PHASE2_INFO_AUDIO_OK,
	      "INFO handoff fixture did not initialize");
	initial_tone_init(&tone, &transmitter.modulation);
	for (size_t index = 0; index < TEST_TONE_SAMPLES + leading_samples;
	     ++index) {
		struct v34_phase2_info_event event;
		int16_t sample = following_tone_sample(&tone);

		CHECK(v34_phase2_info_audio_receiver_sample(
			      &receiver, codec_round_trip(sample, codec), &event) ==
			      V34_PHASE2_INFO_AUDIO_OK &&
		      event.type == V34_PHASE2_INFO_NO_EVENT,
		      "INFO handoff emitted during the initial tone");
	}
	CHECK(receiver.frame.stage == V34_PHASE2_INFO_SEARCH_SYNC,
	      "initial tone falsely synchronized an INFO frame");
	/* A repeated INFO sequence inherits the current following-tone phase. */
	transmitter.modulation.carrier.carrier_phase =
		tone.carrier_phase - UINT32_C(0x40000000);
	transmitter.modulation.guard_phase = tone.guard_phase;
	for (size_t index = TEST_TONE_SAMPLES + leading_samples;
	     index < TEST_TONE_SAMPLES + leading_samples + TEST_SAMPLE_LIMIT;
	     ++index) {
		struct v34_phase2_info_event event;
		enum v34_phase2_info_receive_stage stage_before =
			receiver.frame.stage;
		int16_t sample;

		if (!frame_complete) {
			bool completes;

			CHECK(v34_phase2_info_audio_transmitter_sample(
				      &transmitter, &sample, &completes) ==
				      V34_PHASE2_INFO_AUDIO_OK,
			      "INFO handoff transmitter failed");
			frame_complete = completes;
		} else if (filter_tail_symbols <
			   V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS) {
			bool bit_accepted;

			CHECK(v34_phase2_dpsk_transmitter_sample(
				      &transmitter.modulation, 0, &sample,
				      &bit_accepted) == V34_PHASE2_DPSK_OK,
			      "INFO handoff filter tail failed");
			if (bit_accepted && ++filter_tail_symbols ==
				    V34_PHASE2_INFO_FILTER_TAIL_SYMBOLS)
				reversed_following_tone_init(
					&tone, &transmitter.modulation);
		} else {
			if (line_transition == SIZE_MAX)
				line_transition = index;
			sample = following_tone_sample(&tone);
		}
		CHECK(v34_phase2_info_audio_receiver_sample(
			      &receiver, codec_round_trip(sample, codec), &event) ==
			      V34_PHASE2_INFO_AUDIO_OK &&
		      event.type != V34_PHASE2_INFO_FRAME_REJECTED,
		      "INFO handoff receiver rejected its frame");
		if (stage_before == V34_PHASE2_INFO_SEARCH_SYNC &&
		    receiver.frame.stage != V34_PHASE2_INFO_SEARCH_SYNC &&
		    sync_sample == SIZE_MAX)
			sync_sample = index;
		if (event.type == V34_PHASE2_INFO_FRAME_RECEIVED) {
			int16_t next_sample;
			struct v34_phase2_info_event next_event;

			CHECK(line_transition != SIZE_MAX && sync_sample != SIZE_MAX,
			      "INFO handoff completed before the following tone");
			*measurement = (struct handoff_measurement){
				.sync_to_transition_samples =
					line_transition - sync_sample,
				.event_delay_samples = index - line_transition,
				.reported_delay_samples =
				v34_phase2_info_audio_receiver_handoff_delay_samples(
					&receiver),
			};
			/* Completed lineage expires on the next input sample. */
			next_sample = following_tone_sample(&tone);
			CHECK(v34_phase2_info_audio_receiver_sample(
				      &receiver,
				      codec_round_trip(next_sample, codec), &next_event) ==
				      V34_PHASE2_INFO_AUDIO_OK &&
			      next_event.type == V34_PHASE2_INFO_NO_EVENT &&
			      v34_phase2_info_audio_receiver_handoff_delay_samples(
				      &receiver) == 0,
			      "completed INFO handoff lineage was not one-shot");
			return true;
		}
	}
	CHECK(false, "INFO handoff frame did not complete");
	return false;
}

static bool test_receiver_handoff_delay(void)
{
	static const struct {
		size_t payload_bits;
		size_t expected_sync_to_tone;
		size_t minimum_sync_to_tone;
		size_t maximum_sync_to_tone;
	} profiles[] = {
		{ 17, 443, 442, 445 },
		{ 30, 617, 615, 618 },
	};
	static const uint8_t payload_fills[] = {
		0x00, 0x55, 0xaa, 0xff,
	};

	for (size_t profile = 0; profile < ARRAY_LENGTH(profiles); ++profile) {
		size_t minimum = SIZE_MAX;
		size_t maximum = 0;

		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			for (enum test_codec codec = TEST_CODEC_LINEAR;
			     codec <= TEST_CODEC_PCMA; ++codec)
				for (size_t fill = 0;
				     fill < ARRAY_LENGTH(payload_fills); ++fill)
					/* Twenty samples cover all carrier and four-sample
					 * receiver-block alignments at 8 kHz. */
					for (size_t leading = 0; leading < 20; ++leading) {
						struct handoff_measurement measured;
						size_t expected_reported;
						size_t error;

						CHECK(measure_handoff_delay(
							      role, codec,
							      profiles[profile].payload_bits,
							      payload_fills[fill], leading,
							      &measured),
						      "INFO handoff fixture failed for %d/%d/%zu/%u/%zu",
						      role, codec,
						      profiles[profile].payload_bits,
						      payload_fills[fill], leading);
						expected_reported =
							measured.event_delay_samples +
							measured.sync_to_transition_samples -
							profiles[profile].expected_sync_to_tone;
						CHECK(measured.reported_delay_samples ==
							      expected_reported,
						      "INFO handoff lineage changed for %d/%d/%zu/%u/%zu: %u != %zu",
						      role, codec,
						      profiles[profile].payload_bits,
						      payload_fills[fill], leading,
						      measured.reported_delay_samples,
						      expected_reported);
						error = measured.reported_delay_samples >
								measured.event_delay_samples ?
							measured.reported_delay_samples -
								measured.event_delay_samples :
							measured.event_delay_samples -
								measured.reported_delay_samples;
						CHECK(error <= 2u,
						      "INFO handoff edge error was %zu samples for %d/%d/%zu/%u/%zu",
						      error, role, codec,
						      profiles[profile].payload_bits,
						      payload_fills[fill], leading);
						if (measured.sync_to_transition_samples < minimum)
							minimum = measured.sync_to_transition_samples;
						if (measured.sync_to_transition_samples > maximum)
							maximum = measured.sync_to_transition_samples;
					}
		CHECK(minimum == profiles[profile].minimum_sync_to_tone &&
		      maximum == profiles[profile].maximum_sync_to_tone,
		      "INFO sync-to-tone bound changed for %zu bits: %zu..%zu",
		      profiles[profile].payload_bits, minimum, maximum);
	}
	return true;
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
	CHECK(v34_phase2_info_audio_receiver_handoff_delay_samples(
		      &receiver) == 0,
	      "idle Phase 2 INFO receiver exposed handoff lineage");
	receiver.handoff_delay_samples =
		V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES + 1u;
	CHECK(!v34_phase2_info_audio_receiver_is_valid(&receiver) &&
	      v34_phase2_info_audio_receiver_handoff_delay_samples(
		      &receiver) == 0,
	      "Phase 2 INFO receiver accepted an unbounded handoff delay");
	receiver.handoff_delay_samples =
		V34_PHASE2_INFO_SYNC_PIPELINE_DELAY_SAMPLES;
	receiver.consecutive_mark_bits = V34_PHASE2_INFO_MARKS_BITS;
	CHECK(!v34_phase2_info_audio_receiver_is_valid(&receiver),
	      "Phase 2 INFO receiver accepted an unbounded mark run");
	receiver.consecutive_mark_bits = 0;
	memcpy(receiver_snapshot, &receiver, sizeof(receiver_snapshot));
	CHECK(v34_phase2_info_audio_receiver_sample(
		      &receiver, 0, NULL) ==
		      V34_PHASE2_INFO_AUDIO_INVALID_ARGUMENT &&
		      memcmp(receiver_snapshot, &receiver, sizeof(receiver_snapshot)) ==
			      0 &&
		      v34_phase2_info_audio_receiver_init(
			      &receiver, V34_CALLING_STATION,
			      TEST_PAYLOAD_BITS) ==
			      V34_PHASE2_INFO_AUDIO_OK &&
		      receiver.handoff_delay_samples == 0,
	      "Phase 2 INFO audio receiver validation or reinit failed");
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
		{ "weak physical frame recovery", test_weak_frame_recovery },
		{ "silence and noise rejection", test_nonframe_input },
		{ "INFO marks detection", test_info_marks_detection },
		{ "carrier loss is not INFO marks", test_carrier_loss_is_not_info_marks },
		{ "receiver handoff delay", test_receiver_handoff_delay },
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
