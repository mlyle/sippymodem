// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_phase2.h>
#include <pcm_phase2_info1_handoff.h>
#include <v34_phase2_info_audio.h>
#include <v90_phase2_initial_probe.h>
#include <pcm_phase2_info1_exchange.h>
#include <v90_phase2_probe_turnaround.h>
#include <v90_phase2_reciprocal_probe.h>
#include <pcm_phase3_handoff.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_LINE_DELAY_SAMPLES = 13,
	TEST_SAMPLE_LIMIT = 12000,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

enum test_info1_mode {
	TEST_INFO1_V90,
	TEST_INFO1_V92_CONVENTIONAL,
	TEST_INFO1_V92_PCM,
};

struct test_delay_line {
	int16_t samples[TEST_LINE_DELAY_SAMPLES];
	size_t cursor;
};

struct test_endpoint {
	struct pcm_phase2_ranging ranging;
	struct v90_phase2_initial_probe probe;
	bool probing;
	bool analog;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	return codec == TEST_CODEC_PCMU ?
		       g711_decode_pcmu(g711_encode_pcmu(sample)) :
		       g711_decode_pcma(g711_encode_pcma(sample));
}

static int16_t delay_line_receive(const struct test_delay_line *line)
{
	return line->samples[line->cursor];
}

static void delay_line_transmit(
	struct test_delay_line *line, int16_t sample)
{
	line->samples[line->cursor] = sample;
	line->cursor = (line->cursor + 1u) % TEST_LINE_DELAY_SAMPLES;
}

static struct v90_info0 analog_capabilities(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
	};
}

static struct v90_info0d digital_capabilities(enum test_codec codec)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.high_carrier_3000_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = codec == TEST_CODEC_PCMU ?
				   V90_PCM_MU_LAW : V90_PCM_A_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static struct v34_info1c digital_info1(void)
{
	struct v34_info1c info1d = {
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units = -23,
		},
	};

	for (size_t index = 0; index < V34_SYMBOL_RATE_COUNT; ++index)
		info1d.answer_transmit[index] =
			(struct v34_info1_probe_result){
				.high_carrier = (index & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)(2u * index),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(6u + index),
			};
	return info1d;
}

static struct v90_info1a analog_info1(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 0,
		.training_ucode = 91,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3200,
		.frequency_offset_1_50_hz_units = 31,
	};
}

static struct v92_info1d v92_digital_info1(enum test_info1_mode mode)
{
	struct v92_info1d info1d = {
		.v34 = digital_info1(),
		.pcm_upstream_supported = mode == TEST_INFO1_V92_PCM,
	};

	info1d.v34.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
	return info1d;
}

static struct v92_pcm_info1a pcm_analog_info1(void)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_Z1_P1_Z2,
		.maximum_total_coefficients = 320,
		.maximum_section_coefficients = 192,
		.local_md_length_276_symbol_units = 4,
		.training_ucode = 91,
	};
}

static bool info1d_equal(
	const struct v34_info1c *left, const struct v34_info1c *right)
{
	uint8_t left_payload[V34_INFO1C_PAYLOAD_BYTES];
	uint8_t right_payload[V34_INFO1C_PAYLOAD_BYTES];

	return v34_info1c_encode(left, left_payload, sizeof(left_payload)) ==
		       V34_INFO1_OK &&
	       v34_info1c_encode(right, right_payload, sizeof(right_payload)) ==
		       V34_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool info1a_equal(
	const struct v90_info1a *left, const struct v90_info1a *right)
{
	uint8_t left_payload[V90_INFO1A_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO1A_PAYLOAD_BYTES];

	return v90_info1a_encode(left, left_payload, sizeof(left_payload)) ==
		       V90_INFO1_OK &&
	       v90_info1a_encode(right, right_payload, sizeof(right_payload)) ==
		       V90_INFO1_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool v92_info1d_equal(
	const struct v92_info1d *left, const struct v92_info1d *right)
{
	uint8_t left_payload[V92_INFO1D_PAYLOAD_BYTES];
	uint8_t right_payload[V92_INFO1D_PAYLOAD_BYTES];

	return v92_info1d_encode(left, left_payload, sizeof(left_payload)) ==
		       V92_INFO1D_OK &&
	       v92_info1d_encode(right, right_payload, sizeof(right_payload)) ==
		       V92_INFO1D_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool pcm_info1a_equal(
	const struct v92_pcm_info1a *left,
	const struct v92_pcm_info1a *right)
{
	uint8_t left_payload[V92_PCM_INFO1A_PAYLOAD_BYTES];
	uint8_t right_payload[V92_PCM_INFO1A_PAYLOAD_BYTES];

	return v92_pcm_info1a_encode(
		       left, left_payload, sizeof(left_payload)) ==
		       V92_PCM_INFO1A_OK &&
	       v92_pcm_info1a_encode(
		       right, right_payload, sizeof(right_payload)) ==
		       V92_PCM_INFO1A_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool check_pcm_info1d_retry(
	const struct pcm_phase2_info1_exchange *initial,
	const uint8_t peer_payload[V90_INFO1A_PAYLOAD_BYTES])
{
	struct pcm_phase2_info1_exchange late = *initial;
	struct pcm_phase2_info1_exchange recovery = *initial;
	struct v34_phase2_info_audio_transmitter peer;
	struct phase2_info1_exchange repeated = initial->control;
	struct v34_phase2_dpsk_transmitter marks;
	struct pcm_phase2_info1_event event;
	bool frame_complete = false;
	bool late_received = false;
	bool retrain_requested = false;
	bool retry_sent = false;

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t transmit;

		CHECK(pcm_phase2_info1_sample(
			      &late, 0, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM late INFO1a fixture failed before recovery");
		if (event.kind == PCM_PHASE2_INFO1_EVENT_FAILED)
			break;
	}
	CHECK(late.control.phase == PHASE2_INFO1_FAILED &&
	      phase2_info1_recover(&late.control) == PHASE2_INFO1_OK &&
	      pcm_phase2_info1_is_valid(&late),
	      "PCM late INFO1a fixture did not arm at the first deadline");
	/* A checksum-valid but unusable reply may arrive after timeout. It
	 * must not advance the expired deadline or prevent a later good reply. */
	for (unsigned int attempt = 0; attempt < 2u; ++attempt) {
		const uint8_t invalid_payload[V90_INFO1A_PAYLOAD_BYTES] = {
			[2] = UINT8_C(0x03), [3] = UINT8_C(0x80),
		};
		uint32_t deadline = late.control.peer_deadline_elapsed_samples;
		bool rejected = false;

		CHECK(v34_phase2_info_audio_transmitter_init(
			      &peer, V34_ANSWERING_STATION, TEST_AMPLITUDE,
			      invalid_payload, V90_INFO1A_PAYLOAD_BITS) ==
			      V34_PHASE2_INFO_AUDIO_OK,
		      "late invalid INFO1a transmitter did not initialize");
		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			int16_t received = 0;
			int16_t transmit;
			bool complete;

			if (!peer.frame_complete)
				CHECK(v34_phase2_info_audio_transmitter_sample(
					      &peer, &received, &complete) ==
					      V34_PHASE2_INFO_AUDIO_OK,
				      "late invalid INFO1a transmitter failed");
			CHECK(pcm_phase2_info1_sample(
				      &late, received, &transmit, &event) ==
				      PHASE2_INFO1_OK,
			      "late invalid INFO1a caused an internal error");
			if (event.kind == PCM_PHASE2_INFO1_EVENT_PEER_FRAME_REJECTED) {
				rejected = true;
				break;
			}
		}
		CHECK(rejected && late.control.phase == PHASE2_INFO1_RECOVERING &&
		      late.control.peer_deadline_elapsed_samples == deadline &&
		      late.info1d_resends == 0u && !late.analog_record_stored &&
		      pcm_phase2_info1_retry_pending(&late),
		      "late invalid INFO1a lost the recoverable exchange");
	}
	CHECK(v34_phase2_info_audio_transmitter_init(
		      &peer, V34_ANSWERING_STATION, TEST_AMPLITUDE,
		      peer_payload, V90_INFO1A_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_OK,
	      "PCM late valid INFO1a transmitter did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t received = 0;
		int16_t transmit;

		if (!frame_complete)
			CHECK(v34_phase2_info_audio_transmitter_sample(
			      &peer, &received, &frame_complete) ==
			      V34_PHASE2_INFO_AUDIO_OK,
			      "PCM late INFO1a transmitter failed at %zu", index);
		CHECK(pcm_phase2_info1_sample(
			      &late, received, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM late INFO1a receiver failed at sample %zu", index);
		CHECK(late.info1d_resends == 0u,
		      "late INFO1a prefix was mistaken for INFOMARKSa");
		if (event.kind == PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			late_received = true;
			break;
		}
	}
	CHECK(late_received && late.analog_record_stored &&
	      late.control.phase == PHASE2_INFO1_SILENCE,
	      "late INFO1a did not complete the original exchange");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t transmit;

		CHECK(pcm_phase2_info1_sample(
			      &recovery, 0, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM INFO1d retry fixture failed before recovery");
		if (event.kind == PCM_PHASE2_INFO1_EVENT_FAILED)
			break;
	}
	CHECK(recovery.control.phase == PHASE2_INFO1_FAILED &&
	      phase2_info1_recover(&recovery.control) == PHASE2_INFO1_OK &&
	      pcm_phase2_info1_retry_pending(&recovery) &&
	      v34_phase2_dpsk_transmitter_init(
		      &marks, V34_ANSWERING_STATION, TEST_AMPLITUDE) ==
		      V34_PHASE2_DPSK_OK,
	      "PCM INFO1d retry did not arm at the first deadline");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t received;
		int16_t transmit;
		bool bit_accepted;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &marks, 1, &received, &bit_accepted) ==
			      V34_PHASE2_DPSK_OK &&
		      pcm_phase2_info1_sample(
			      &recovery, received, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM INFOMARKSa retry failed at sample %zu", index);
		if (recovery.info1d_resends != 0u) {
			CHECK(transmit == 0 &&
			      event.kind == PCM_PHASE2_INFO1_EVENT_NONE,
			      "PCM INFO1d retry consumed its marker interval");
			break;
		}
	}
	CHECK(recovery.info1d_resends == 1u &&
	      recovery.control.phase == PHASE2_INFO1_TRANSMITTING &&
	      recovery.protocol == initial->protocol &&
	      !pcm_phase2_info1_retry_pending(&recovery),
	      "PCM INFO1d retry did not start from INFOMARKSa");
	frame_complete = false;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t transmit;
		int16_t expected;
		struct phase2_info1_event repeated_event;

		CHECK(pcm_phase2_info1_sample(
			      &recovery, 0, &transmit, &event) ==
			      PHASE2_INFO1_OK &&
		      phase2_info1_sample(
			      &repeated, 0, &expected, &repeated_event) ==
			      PHASE2_INFO1_OK &&
		      transmit == expected,
		      "PCM repeated INFO1d failed at sample %zu", index);
		if (event.kind == PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT) {
			frame_complete = repeated_event.kind ==
				PHASE2_INFO1_EVENT_LOCAL_FIRST_SENT;
			retry_sent = true;
			break;
		}
	}
	CHECK(retry_sent && frame_complete && recovery.info1d_resends == 1u,
	      "PCM repeated INFO1d did not complete exactly once");
	/* A response to the resend must still select the requested protocol and
	 * upstream waveform. Keep the original copy for retry exhaustion below. */
	late = recovery;
	late_received = false;
	frame_complete = false;
	CHECK(v34_phase2_info_audio_transmitter_init(
		      &peer, V34_ANSWERING_STATION, TEST_AMPLITUDE,
		      peer_payload, V90_INFO1A_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_OK,
	      "PCM INFO1a response to resend did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t received = 0;
		int16_t transmit;

		if (!frame_complete)
			CHECK(v34_phase2_info_audio_transmitter_sample(
				      &peer, &received, &frame_complete) ==
				      V34_PHASE2_INFO_AUDIO_OK,
			      "PCM INFO1a response stopped at %zu", index);
		CHECK(pcm_phase2_info1_sample(
			      &late, received, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM INFO1a response failed at %zu", index);
		if (event.kind == PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			late_received = true;
			break;
		}
	}
	CHECK(late_received && late.analog_record_stored &&
	      late.protocol == initial->protocol && late.info1d_resends == 1u &&
	      late.control.phase == PHASE2_INFO1_SILENCE &&
	      memcmp(event.frame_event.payload, peer_payload,
		     V90_INFO1A_PAYLOAD_BYTES) == 0,
	      "PCM INFO1a response did not complete the repeated exchange");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t transmit;

		CHECK(pcm_phase2_info1_sample(
			      &recovery, 0, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM retry exhaustion failed at sample %zu", index);
		if (event.kind == PCM_PHASE2_INFO1_EVENT_FAILED)
			break;
	}
	CHECK(recovery.control.phase == PHASE2_INFO1_FAILED &&
	      phase2_info1_recover(&recovery.control) == PHASE2_INFO1_OK &&
	      recovery.control.phase == PHASE2_INFO1_RECOVERING &&
	      recovery.info1d_resends == 1u &&
	      !pcm_phase2_info1_retry_pending(&recovery) &&
	      v34_phase2_dpsk_transmitter_init(
		      &marks, V34_ANSWERING_STATION, TEST_AMPLITUDE) ==
		      V34_PHASE2_DPSK_OK,
	      "PCM INFO1d retry did not expose its exhausted recovery");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t received;
		int16_t transmit;
		bool bit_accepted;

		CHECK(v34_phase2_dpsk_transmitter_sample(
			      &marks, 1, &received, &bit_accepted) ==
			      V34_PHASE2_DPSK_OK &&
		      pcm_phase2_info1_sample(
			      &recovery, received, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM exhausted INFOMARKSa failed at sample %zu", index);
		if (event.kind == PCM_PHASE2_INFO1_EVENT_RETRAIN_REQUESTED) {
			retrain_requested = true;
			break;
		}
	}
	CHECK(retrain_requested && recovery.info1d_resends == 1u &&
	      recovery.control.phase == PHASE2_INFO1_RECOVERING,
	      "persistent INFOMARKSa did not request full retrain");
	return true;
}

static bool check_v34_fallback_info1a(
	const struct v90_phase2_reciprocal_probe *digital_probe,
	enum test_info1_mode mode, const uint8_t *payload)
{
	struct pcm_phase2_info1_exchange digital;
	struct v34_phase2_info_audio_transmitter peer;
	struct v34_info1c local = digital_info1();
	struct v92_info1d local_v92 = v92_digital_info1(mode);
	struct pcm_phase2_info1_event event;
	struct v34_info1a decoded;
	uint8_t retained_payload[V34_INFO1A_PAYLOAD_BYTES];
	bool local_sent = false;
	bool peer_received = false;
	bool frame_complete = false;

	CHECK(v34_info1a_decode(
		      payload, V34_INFO1A_PAYLOAD_BYTES, V34_INFO1A_PAYLOAD_BITS,
		      &decoded) == V34_INFO1_OK &&
	      (mode == TEST_INFO1_V90 ? pcm_phase2_info1_init_v90_digital(
		      &digital, digital_probe, &local) :
	       pcm_phase2_info1_init_v92_digital(
		      &digital, digital_probe, &local_v92)) == PHASE2_INFO1_OK,
	      "captured V.34 fallback INFO1a fixture was invalid");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t transmit;

		CHECK(pcm_phase2_info1_sample(
			      &digital, 0, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM INFO1d did not reach its fallback receive boundary");
		if (event.kind == PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT) {
			local_sent = true;
			break;
		}
	}
	CHECK(local_sent &&
	      v34_phase2_info_audio_transmitter_init(
		      &peer, V34_ANSWERING_STATION, TEST_AMPLITUDE,
		      payload, V34_INFO1A_PAYLOAD_BITS) ==
		      V34_PHASE2_INFO_AUDIO_OK,
	      "PCM fallback peer did not start at the INFO1a boundary");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		int16_t received = 0;
		int16_t transmit;

		if (!frame_complete)
			CHECK(v34_phase2_info_audio_transmitter_sample(
				      &peer, &received, &frame_complete) ==
				      V34_PHASE2_INFO_AUDIO_OK,
			      "V.34 fallback peer failed at sample %zu", index);
		CHECK(pcm_phase2_info1_sample(
			      &digital, received, &transmit, &event) ==
			      PHASE2_INFO1_OK,
		      "PCM fallback receiver failed at sample %zu", index);
		if (event.kind == PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			peer_received = true;
			break;
		}
	}
	CHECK(peer_received && digital.analog_record_stored &&
	      digital.analog.kind == PCM_INFO1A_V34_FALLBACK &&
	      v34_info1a_encode(
		      &digital.analog.record.v34_fallback, retained_payload,
		      sizeof(retained_payload)) == V34_INFO1_OK &&
	      memcmp(retained_payload, payload, sizeof(retained_payload)) == 0,
	      "captured V.34 INFO1a was not retained as a typed fallback");
	return true;
}

static bool check_info1_exchange(
	enum test_codec codec, struct test_delay_line *analog_to_digital,
	struct test_delay_line *digital_to_analog,
	const struct v90_phase2_reciprocal_probe *analog_probe,
	const struct v90_phase2_reciprocal_probe *digital_probe,
	enum test_info1_mode mode)
{
	struct v34_info1c v90_info1d = digital_info1();
	struct v92_info1d v92_info1d = v92_digital_info1(mode);
	struct v90_info1a conventional = analog_info1();
	struct v92_pcm_info1a pcm = pcm_analog_info1();
	struct pcm_phase2_info1_exchange analog;
	struct pcm_phase2_info1_exchange digital;
	struct pcm_phase2_info1_exchange untouched;
	struct pcm_phase2_info1_exchange untouched_before;
	uint8_t expected_digital[V92_INFO1D_PAYLOAD_BYTES];
	uint8_t expected_analog[V92_PCM_INFO1A_PAYLOAD_BYTES];
	enum pcm_protocol protocol = mode == TEST_INFO1_V90 ?
					 PCM_PROTOCOL_V90 : PCM_PROTOCOL_V92;
	enum pcm_upstream_mode upstream = mode == TEST_INFO1_V92_PCM ?
					 PCM_UPSTREAM_PCM : PCM_UPSTREAM_V34;
	enum pcm_info1a_kind expected_kind = upstream == PCM_UPSTREAM_PCM ?
					 PCM_INFO1A_PCM : PCM_INFO1A_CONVENTIONAL;
	size_t digital_sent = SIZE_MAX;
	size_t analog_received = SIZE_MAX;
	size_t analog_sent = SIZE_MAX;
	size_t digital_received = SIZE_MAX;
	size_t analog_silence_calls = 0;
	size_t digital_silence_calls = 0;
	uint16_t digital_silence_start = 0;
	bool initialized;
	bool wrong_role_rejected;

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_before = untouched;
	if (mode == TEST_INFO1_V90) {
		wrong_role_rejected = pcm_phase2_info1_init_v90_analog(
			&untouched, digital_probe) ==
			PHASE2_INFO1_INVALID_ARGUMENT &&
			pcm_phase2_info1_init_v90_digital(
				&untouched, analog_probe, &v90_info1d) ==
				PHASE2_INFO1_INVALID_ARGUMENT;
		initialized = pcm_phase2_info1_init_v90_analog(
			&analog, analog_probe) == PHASE2_INFO1_OK &&
			pcm_phase2_info1_init_v90_digital(
				&digital, digital_probe, &v90_info1d) ==
				PHASE2_INFO1_OK &&
			v34_info1c_encode(
				&v90_info1d, expected_digital,
				sizeof(expected_digital)) == V34_INFO1_OK;
	} else {
		wrong_role_rejected = pcm_phase2_info1_init_v92_analog(
			&untouched, digital_probe) ==
			PHASE2_INFO1_INVALID_ARGUMENT &&
			pcm_phase2_info1_init_v92_digital(
				&untouched, analog_probe, &v92_info1d) ==
				PHASE2_INFO1_INVALID_ARGUMENT;
		initialized = pcm_phase2_info1_init_v92_analog(
			&analog, analog_probe) == PHASE2_INFO1_OK &&
			pcm_phase2_info1_init_v92_digital(
				&digital, digital_probe, &v92_info1d) ==
				PHASE2_INFO1_OK &&
			v92_info1d_encode(
				&v92_info1d, expected_digital,
				sizeof(expected_digital)) == V92_INFO1D_OK;
	}
	initialized = initialized &&
		(mode == TEST_INFO1_V92_PCM ?
			 v92_pcm_info1a_encode(
				 &pcm, expected_analog,
				 sizeof(expected_analog)) == V92_PCM_INFO1A_OK :
			 v90_info1a_encode(
				 &conventional, expected_analog,
				 sizeof(expected_analog)) == V90_INFO1_OK);
	CHECK(wrong_role_rejected &&
	      memcmp(&untouched, &untouched_before, sizeof(untouched)) == 0,
	      "wrong-role PCM INFO1 mode %d mutated output state", mode);
	CHECK(initialized && analog.protocol == protocol &&
	      digital.protocol == protocol &&
	      analog.control.nominal_amplitude == TEST_AMPLITUDE &&
	      digital.control.nominal_amplitude == TEST_AMPLITUDE &&
	      digital.control.transmitter.frame.payload_bit_count ==
		      V34_INFO1C_PAYLOAD_BITS &&
	      memcmp(
		      digital.control.transmitter.frame.payload,
		      expected_digital, sizeof(expected_digital)) == 0,
	      "PCM INFO1 mode %d did not preserve its exact handoff", mode);
	CHECK(check_pcm_info1d_retry(&digital, expected_analog),
	      "PCM INFO1d mode %d recovery policy changed", mode);
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct pcm_phase2_info1_event analog_event;
		struct pcm_phase2_info1_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;
		bool analog_silencing = analog.control.phase ==
			PHASE2_INFO1_SILENCE;
		bool digital_silencing = digital.control.phase ==
			PHASE2_INFO1_SILENCE;

		CHECK(pcm_phase2_info1_sample(
			      &analog, delay_line_receive(digital_to_analog),
			      &analog_transmit, &analog_event) == PHASE2_INFO1_OK &&
		      pcm_phase2_info1_sample(
			      &digital, delay_line_receive(analog_to_digital),
			      &digital_transmit, &digital_event) == PHASE2_INFO1_OK,
		      "PCM INFO1 mode %d failed for codec %d at sample %zu",
		      mode, codec, index);
		CHECK(analog_event.protocol == protocol &&
		      digital_event.protocol == protocol,
		      "PCM INFO1 event lost protocol %d", protocol);
		delay_line_transmit(
			analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		if (digital_event.kind ==
		    PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1D_SENT)
			digital_sent = index;
		if (analog_event.kind ==
		    PCM_PHASE2_INFO1_EVENT_PEER_INFO1D_RECEIVED) {
			struct pcm_phase2_info1_exchange before = analog;
			bool peer_matches =
				analog_event.peer_record.digital.protocol == protocol &&
				(mode == TEST_INFO1_V90 ?
					 info1d_equal(
						 &analog_event.peer_record.digital
							  .record.v90,
						 &v90_info1d) :
					 v92_info1d_equal(
						 &analog_event.peer_record.digital
							  .record.v92,
						 &v92_info1d));
			bool replied;

			analog_received = index;
			if (mode == TEST_INFO1_V92_PCM) {
				struct v92_pcm_info1a invalid = pcm;

				invalid.maximum_total_coefficients =
					V92_PCM_INFO1A_MIN_TOTAL_COEFFICIENTS - 1;
				replied = pcm_phase2_info1_reply_pcm(
						  &analog, &invalid) ==
						  PHASE2_INFO1_INVALID_ARGUMENT &&
					  memcmp(
						  &analog, &before,
						  sizeof(analog)) == 0 &&
					  pcm_phase2_info1_reply_pcm(
						  &analog, &pcm) == PHASE2_INFO1_OK;
			} else {
				struct v90_info1a invalid = conventional;

				invalid.training_ucode =
					V90_INFO1A_MIN_TRAINING_UCODE - 1;
				replied = pcm_phase2_info1_reply_conventional(
						  &analog, &invalid) ==
						  PHASE2_INFO1_INVALID_ARGUMENT &&
					  memcmp(
						  &analog, &before,
						  sizeof(analog)) == 0 &&
					  pcm_phase2_info1_reply_pcm(
						  &analog, &pcm) ==
						  PHASE2_INFO1_INVALID_ARGUMENT &&
					  memcmp(
						  &analog, &before,
						  sizeof(analog)) == 0 &&
					  pcm_phase2_info1_reply_conventional(
						  &analog, &conventional) ==
						  PHASE2_INFO1_OK;
			}
			CHECK(peer_matches && replied &&
			      analog.analog.kind == expected_kind &&
			      memcmp(
				      analog.control.transmitter.frame.payload,
				      expected_analog,
				      sizeof(expected_analog)) == 0 &&
			      analog.control.transmitter.modulation.carrier
					      .carrier_phase +
					      UINT32_C(0x40000000) ==
				      analog.control.signal_a_tone.carrier_phase &&
			      analog.control.transmitter.modulation.guard_phase ==
				      analog.control.signal_a_tone.guard_phase,
			      "analog modem did not select INFO1 mode %d phase-continuously",
			      mode);
		}
		if (analog_event.kind ==
		    PCM_PHASE2_INFO1_EVENT_LOCAL_INFO1A_SENT) {
			analog_sent = index;
			CHECK(analog.control.phase == PHASE2_INFO1_SILENCE &&
			      analog.control.silence_samples == 0,
			      "analog modem did not begin INFO1a silence at its line edge");
		}
		if (digital_event.kind ==
		    PCM_PHASE2_INFO1_EVENT_PEER_INFO1A_RECEIVED) {
			digital_received = index;
			digital_silence_start = digital.control.silence_samples;
			CHECK(digital_event.peer_record.analog.kind == expected_kind &&
			      (upstream == PCM_UPSTREAM_V34 ?
				       info1a_equal(
					       &digital_event.peer_record.analog
							.record.conventional,
					       &conventional) :
				       pcm_info1a_equal(
					       &digital_event.peer_record.analog
							.record.pcm,
					       &pcm)) &&
			      digital.control.phase == PHASE2_INFO1_SILENCE &&
			      digital_silence_start ==
				      v34_phase2_info_audio_receiver_handoff_delay_samples(
					      &digital.control.receiver),
			      "digital modem decoded a different INFO1 mode %d selection",
			      mode);
		}
		if (analog_silencing) {
			analog_silence_calls++;
			CHECK(analog_transmit == 0,
			      "analog modem emitted a post-INFO1a signal");
		}
		if (digital_silencing) {
			digital_silence_calls++;
			CHECK(digital_transmit == 0,
			      "digital modem emitted a post-INFO1a signal");
		}
		if (analog.control.phase == PHASE2_INFO1_COMPLETE &&
		    digital.control.phase == PHASE2_INFO1_COMPLETE)
			break;
	}
	CHECK(digital_sent < analog_received && analog_received < analog_sent &&
	      analog_sent < digital_received &&
	      analog.digital_record_stored && digital.digital_record_stored &&
	      analog.analog_record_stored && digital.analog_record_stored &&
	      analog.protocol == protocol && digital.protocol == protocol &&
	      analog.analog.kind == expected_kind &&
	      digital.analog.kind == expected_kind &&
	      analog.control.silence_samples ==
		      PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES &&
	      digital.control.silence_samples ==
		      PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES &&
	      analog_silence_calls ==
		      PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES &&
	      digital_silence_start + digital_silence_calls ==
		      PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES &&
	      (protocol == PCM_PROTOCOL_V90 ?
		       info1d_equal(
			       &analog.digital.record.v90, &v90_info1d) &&
		       info1d_equal(
			       &digital.digital.record.v90, &v90_info1d) :
		       v92_info1d_equal(
			       &analog.digital.record.v92, &v92_info1d) &&
		       v92_info1d_equal(
			       &digital.digital.record.v92, &v92_info1d)) &&
	      (upstream == PCM_UPSTREAM_V34 ?
		       info1a_equal(
			       &analog.analog.record.conventional,
			       &conventional) &&
		       info1a_equal(
			       &digital.analog.record.conventional,
			       &conventional) :
		       pcm_info1a_equal(&analog.analog.record.pcm, &pcm) &&
		       pcm_info1a_equal(&digital.analog.record.pcm, &pcm)) &&
	      pcm_phase2_info1_is_valid(&analog) &&
	      pcm_phase2_info1_is_valid(&digital),
	      "PCM INFO1 mode %d order or retained selection changed", mode);
	return true;
}

static bool endpoint_sample(
	struct test_endpoint *endpoint, int16_t received_sample,
	int16_t *transmit_sample,
	struct phase2_initial_probe_event *probe_event)
{
	if (endpoint->probing)
		return v90_phase2_initial_probe_sample(
			       &endpoint->probe, received_sample,
			       transmit_sample, probe_event) ==
		       PHASE2_INITIAL_PROBE_OK;
	{
		struct pcm_phase2_ranging_event event;
		enum phase2_ranging_event_kind ready =
			endpoint->analog ? PHASE2_PROBE_TRANSMIT_READY :
					   PHASE2_PROBE_RECEIVE_READY;

		if (pcm_phase2_ranging_sample(
			    &endpoint->ranging, received_sample,
			    transmit_sample, &event) != PCM_PHASE2_RANGING_OK)
			return false;
		*probe_event = (struct phase2_initial_probe_event){ 0 };
		if (event.kind == ready) {
			if (v90_phase2_initial_probe_init(
				    &endpoint->probe, &endpoint->ranging) !=
			    PHASE2_INITIAL_PROBE_OK)
				return false;
			endpoint->probing = true;
		}
	}
	return true;
}

static bool check_initial_probe(enum test_codec codec)
{
	struct v90_info0 analog_info = analog_capabilities();
	struct v90_info0d digital_info = digital_capabilities(codec);
	struct test_endpoint analog = { .analog = true };
	struct test_endpoint digital = { 0 };
	struct test_delay_line analog_to_digital = { 0 };
	struct test_delay_line digital_to_analog = { 0 };
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v90_phase2_probe_turnaround analog_turnaround;
	struct v90_phase2_probe_turnaround digital_turnaround;
	struct v90_phase2_reciprocal_probe analog_reciprocal;
	struct v90_phase2_reciprocal_probe digital_reciprocal;
	uint32_t transmitted_l2_samples = 0;
	bool analog_l2_started = false;
	bool analog_probe_complete = false;
	bool digital_probe_complete = false;
	bool tone_pair_active = false;

	CHECK(pcm_phase2_ranging_init_analog(
		      &analog.ranging, TEST_AMPLITUDE, &analog_info) ==
		      PCM_PHASE2_RANGING_OK &&
	      pcm_phase2_ranging_init_digital(
		      &digital.ranging, TEST_AMPLITUDE, &digital_info) ==
		      PCM_PHASE2_RANGING_OK,
	      "V.90 initial-probe endpoints did not initialize");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_initial_probe_event analog_event;
		struct phase2_initial_probe_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;

		CHECK(endpoint_sample(
			      &analog, delay_line_receive(&digital_to_analog),
			      &analog_transmit, &analog_event) &&
		      endpoint_sample(
			      &digital, delay_line_receive(&analog_to_digital),
			      &digital_transmit, &digital_event),
		      "V.90 initial probe failed for codec %d at sample %zu",
		      codec, index);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));

		if (analog_event.transmit_event.kind ==
		    V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED)
			analog_l2_started = true;
		if (analog_event.kind ==
		    PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE) {
			analog_probe_complete = true;
			transmitted_l2_samples =
				analog_event.transmit_event.l2_samples;
		}
		if (digital_event.kind ==
		    PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE)
			digital_probe_complete = true;
		if (digital_event.tone_event.kind == V34_PHASE2_TONE_STARTED)
			tone_pair_active = true;
		if (analog.probing && digital.probing &&
		    analog.probe.control.phase ==
			    PHASE2_INITIAL_PROBE_SIGNAL_A_TONE &&
		    digital.probe.control.phase == PHASE2_INITIAL_PROBE_SIGNAL_B_TONE &&
		    tone_pair_active)
			break;
	}

	CHECK(analog.probing && digital.probing &&
	      v90_phase2_initial_probe_is_valid(&analog.probe) &&
	      v90_phase2_initial_probe_is_valid(&digital.probe) &&
	      analog_l2_started && analog_probe_complete &&
	      digital_probe_complete && tone_pair_active,
	      "V.90 first probe did not complete both role handoffs");
	CHECK(transmitted_l2_samples >= V34_PHASE2_PROBE_L1_SAMPLES &&
	      transmitted_l2_samples <
		      (uint32_t)V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
		      analog.probe.ranging.control.round_trip_delay_samples,
	      "V.90 analog modem ended L2 outside its bounded window");
	CHECK(v90_phase2_initial_probe_measure(
		      &digital.probe, &measurement) ==
		      PHASE2_INITIAL_PROBE_OK &&
	      measurement.l1.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples >= V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l1.harmonics[0].mean_square > 0.0 &&
	      measurement.l2.harmonics[0].mean_square > 0.0 &&
	      v90_phase2_initial_probe_measure(
		      &analog.probe, &measurement) ==
		      PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "V.90 digital probe measurement was unavailable or malformed");
	{
		struct v90_phase2_initial_probe invalid = analog.probe;
		struct v90_phase2_initial_probe before;
		struct phase2_initial_probe_event event = {
			.kind = PHASE2_INITIAL_PROBE_EVENT_FAILED,
		};
		int16_t output = 1234;

		invalid.ranging.info0_exchange.transport.info_receiver.modulation
			.detector_output = NAN;
		before = invalid;
		CHECK(v90_phase2_initial_probe_sample(
			      &invalid, 0, &output, &event) ==
			      PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
		      memcmp(&invalid, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_INITIAL_PROBE_EVENT_FAILED,
		      "invalid V.90 probe state mutated state or outputs");
	}
	CHECK(v90_phase2_probe_turnaround_init(
		      &analog_turnaround, &analog.probe) ==
		      PHASE2_PROBE_TURNAROUND_OK &&
	      v90_phase2_probe_turnaround_init(
		      &digital_turnaround, &digital.probe) ==
		      PHASE2_PROBE_TURNAROUND_OK,
	      "V.90 probe-turnaround endpoints did not initialize");
	{
		bool analog_ready = false;
		bool digital_ready = false;

		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			struct phase2_probe_turnaround_event analog_event;
			struct phase2_probe_turnaround_event digital_event;
			int16_t analog_transmit;
			int16_t digital_transmit;

			CHECK(v90_phase2_probe_turnaround_sample(
				      &analog_turnaround,
				      delay_line_receive(&digital_to_analog),
				      &analog_transmit, &analog_event) ==
				      PHASE2_PROBE_TURNAROUND_OK &&
			      v90_phase2_probe_turnaround_sample(
				      &digital_turnaround,
				      delay_line_receive(&analog_to_digital),
				      &digital_transmit, &digital_event) ==
				      PHASE2_PROBE_TURNAROUND_OK,
			      "V.90 probe turnaround failed for codec %d at sample %zu",
			      codec, index);
			delay_line_transmit(
				&analog_to_digital,
				codec_round_trip(analog_transmit, codec));
			delay_line_transmit(
				&digital_to_analog,
				codec_round_trip(digital_transmit, codec));
			analog_ready |= analog_event.kind ==
				PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY;
			digital_ready |= digital_event.kind ==
				PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY;
			if (analog_ready && digital_ready)
				break;
		}
		CHECK(analog_ready && digital_ready &&
		      analog_turnaround.control.phase ==
			      PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY &&
		      digital_turnaround.control.phase ==
			      PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY &&
		      analog_turnaround.control
				      .signal_a_reversal_elapsed_samples != 0 &&
		      v90_phase2_probe_turnaround_is_valid(
			      &analog_turnaround) &&
		      v90_phase2_probe_turnaround_is_valid(
			      &digital_turnaround),
		      "V.90 probe turnaround did not complete both role handoffs");
	}
	CHECK(v90_phase2_reciprocal_probe_init(
		      &analog_reciprocal, &analog_turnaround) ==
		      PHASE2_RECIPROCAL_PROBE_OK &&
	      v90_phase2_reciprocal_probe_init(
		      &digital_reciprocal, &digital_turnaround) ==
		      PHASE2_RECIPROCAL_PROBE_OK,
	      "V.90 reciprocal-probe endpoints did not initialize");
	{
		bool analog_measured = false;
		bool digital_acknowledged = false;

		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			struct phase2_reciprocal_probe_event analog_event;
			struct phase2_reciprocal_probe_event digital_event;
			int16_t analog_transmit;
			int16_t digital_transmit;

			CHECK(v90_phase2_reciprocal_probe_sample(
				      &analog_reciprocal,
				      delay_line_receive(&digital_to_analog),
				      &analog_transmit, &analog_event) ==
				      PHASE2_RECIPROCAL_PROBE_OK &&
			      v90_phase2_reciprocal_probe_sample(
				      &digital_reciprocal,
				      delay_line_receive(&analog_to_digital),
				      &digital_transmit, &digital_event) ==
				      PHASE2_RECIPROCAL_PROBE_OK,
			      "V.90 reciprocal probe failed for codec %d at sample %zu",
			      codec, index);
			delay_line_transmit(
				&analog_to_digital,
				codec_round_trip(analog_transmit, codec));
			delay_line_transmit(
				&digital_to_analog,
				codec_round_trip(digital_transmit, codec));
			analog_measured |= analog_event.kind ==
				PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED;
			digital_acknowledged |= digital_event.kind ==
				PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED;
			if (analog_measured && digital_acknowledged)
				break;
		}
		CHECK(analog_measured && digital_acknowledged &&
		      analog_reciprocal.control.phase ==
			      PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE &&
		      digital_reciprocal.control.phase ==
			      PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE &&
		      analog_reciprocal.control
				      .signal_a_reversal_elapsed_samples >
			      analog_turnaround.control
				      .signal_a_reversal_elapsed_samples &&
		      v90_phase2_reciprocal_probe_is_valid(
			      &analog_reciprocal) &&
		      v90_phase2_reciprocal_probe_is_valid(
			      &digital_reciprocal),
		      "V.90 reciprocal probe did not complete both role handoffs");
	}
	CHECK(v90_phase2_reciprocal_probe_measure(
		      &analog_reciprocal, &measurement) ==
		      PHASE2_RECIPROCAL_PROBE_OK &&
	      measurement.l1.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples == V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l1.harmonics[0].mean_square > 0.0 &&
	      measurement.l2.harmonics[0].mean_square > 0.0 &&
	      v90_phase2_reciprocal_probe_measure(
		      &digital_reciprocal, &measurement) ==
		      PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT,
		      "V.90 analog reciprocal-probe measurement was unavailable");
	{
		struct test_delay_line analog_start = analog_to_digital;
		struct test_delay_line digital_start = digital_to_analog;
		/* Compact on-wire fallback replies: the latter two were rejected
		 * during V.92 recovery despite valid V.34 rates and checksums. */
		static const uint8_t fallback_payloads[][V34_INFO1A_PAYLOAD_BYTES] = {
			{ 0x00, 0x06, 0x4f, 0x60, 0x04 },
			{ 0xa0, 0x07, 0x2c, 0x90, 0x04 },
			{ 0xa0, 0x05, 0x5f, 0x60, 0x04 },
		};

		for (enum test_info1_mode mode = TEST_INFO1_V90;
		     mode <= TEST_INFO1_V92_PCM; ++mode) {
			struct test_delay_line analog_line = analog_start;
			struct test_delay_line digital_line = digital_start;

			CHECK(check_info1_exchange(
				      codec, &analog_line, &digital_line,
				      &analog_reciprocal, &digital_reciprocal,
				      mode),
			      "PCM INFO1 mode %d failed for codec %d",
			      mode, codec);
			for (size_t index = 0; index < sizeof(fallback_payloads) /
						     sizeof(fallback_payloads[0]); ++index)
				CHECK(check_v34_fallback_info1a(
					      &digital_reciprocal, mode, fallback_payloads[index]),
				      "captured V.34 fallback %zu failed for codec %d mode %d",
				      index, codec, mode);
		}
	}

	{
		struct v90_phase2_probe_turnaround before;
		struct phase2_probe_turnaround_event event = {
			.kind = PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
		};
		int16_t output = 1234;

		analog_turnaround.initial_probe.ranging.info0_exchange.transport
			.info_receiver
			.modulation.detector_output = NAN;
		before = analog_turnaround;
		CHECK(v90_phase2_probe_turnaround_sample(
			      &analog_turnaround, 0, &output, &event) ==
			      PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT &&
		      memcmp(&analog_turnaround, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
		      "invalid V.90 turnaround mutated state or outputs");
	}
	{
		struct v90_phase2_reciprocal_probe before;
		struct phase2_reciprocal_probe_event event = {
			.kind = PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
		};
		int16_t output = 1234;

		analog_reciprocal.turnaround.initial_probe.ranging.info0_exchange
			.transport.info_receiver.modulation.detector_output = NAN;
		before = analog_reciprocal;
		CHECK(v90_phase2_reciprocal_probe_sample(
			      &analog_reciprocal, 0, &output, &event) ==
			      PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT &&
		      memcmp(&analog_reciprocal, &before, sizeof(before)) == 0 &&
		      output == 1234 &&
		      event.kind == PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
		      "invalid V.90 reciprocal probe mutated state or outputs");
	}
	return true;
}

static bool check_phase2_controller(
	enum test_codec codec, enum test_info1_mode mode, unsigned short_offer)
{
	struct v90_info0 analog_info = analog_capabilities();
	struct v90_info0d digital_info = digital_capabilities(codec);
	struct v92_info0a v92_analog_info = {
		.v90 = analog_info,
		.v92_capable = true,
		.short_phase2_requested = short_offer == 1u,
	};
	struct v92_info0d v92_digital_info = {
		.v90 = digital_info,
		.v92_capable = true,
		.short_phase2_requested = short_offer == 2u,
	};
	struct v34_info1c v90_info1d = digital_info1();
	struct v92_info1d v92_info1d = v92_digital_info1(mode);
	struct v90_info1a conventional_info1a = analog_info1();
	struct v92_pcm_info1a pcm_info1a = pcm_analog_info1();
	struct pcm_phase2 analog;
	struct pcm_phase2 digital;
	struct test_delay_line analog_to_digital = { 0 };
	struct test_delay_line digital_to_analog = { 0 };
	struct v34_phase2_probe_receiver_measurement analog_measurement;
	struct v34_phase2_probe_receiver_measurement digital_measurement;
	struct v90_info0 peer_info0a;
	struct v90_info0d peer_info0d;
	struct v92_info0a peer_v92_info0a;
	struct v92_info0d peer_v92_info0d;
	struct v34_info1c peer_info1d;
	struct v92_info1d peer_v92_info1d;
	struct v90_info1a peer_info1a;
	struct v92_pcm_info1a peer_pcm_info1a;
	struct v34_phase2_probe_receiver_measurement untouched_measurement;
	struct v34_phase2_probe_receiver_measurement untouched_before;
	struct pcm_info1_selection untouched_selection;
	struct pcm_info1_selection untouched_selection_before;
	struct pcm_phase3_entry untouched_entry;
	struct pcm_phase3_entry untouched_entry_before;
	bool analog_initial = false;
	bool digital_initial = false;
	uint16_t analog_initial_round_trip = 0;
	uint16_t digital_initial_round_trip = 0;
	bool digital_measured_initial = false;
	bool analog_turnaround = false;
	bool digital_turnaround = false;
	bool analog_reciprocal = false;
	bool digital_reciprocal = false;
	bool analog_ready = false;
	bool digital_ready = false;
	size_t digital_sent = SIZE_MAX;
	size_t analog_received = SIZE_MAX;
	size_t analog_sent = SIZE_MAX;
	size_t digital_received = SIZE_MAX;
	enum pcm_protocol protocol = mode == TEST_INFO1_V90 ?
					 PCM_PROTOCOL_V90 : PCM_PROTOCOL_V92;
	enum pcm_upstream_mode upstream = mode == TEST_INFO1_V92_PCM ?
					 PCM_UPSTREAM_PCM : PCM_UPSTREAM_V34;
	bool initialized;

	if (mode == TEST_INFO1_V90)
		initialized = pcm_phase2_init_v90_analog(
			      &analog, TEST_AMPLITUDE, &analog_info) ==
			      PCM_PHASE2_OK &&
			pcm_phase2_init_v90_digital(
				&digital, TEST_AMPLITUDE, &digital_info) ==
				PCM_PHASE2_OK;
	else
		initialized = pcm_phase2_init_v92_analog(
			      &analog, TEST_AMPLITUDE, &v92_analog_info) ==
			      PCM_PHASE2_OK &&
			pcm_phase2_init_v92_digital(
				&digital, TEST_AMPLITUDE, &v92_digital_info) ==
				PCM_PHASE2_OK;
	CHECK(initialized,
	      "PCM Phase 2 mode %d controllers did not initialize", mode);
	memset(&untouched_measurement, 0xa5, sizeof(untouched_measurement));
	untouched_before = untouched_measurement;
	CHECK(pcm_phase2_measure_remote_probe(
		      &analog, &untouched_measurement) ==
		      PCM_PHASE2_INVALID_STATE &&
	      memcmp(
		      &untouched_measurement, &untouched_before,
		      sizeof(untouched_measurement)) == 0,
	      "premature PCM probe access mutated its destination");
	memset(&untouched_selection, 0xa5, sizeof(untouched_selection));
	untouched_selection_before = untouched_selection;
	CHECK(pcm_phase2_info1_handoff(
		      &analog, &untouched_selection) ==
		      PCM_INFO1_INVALID_RECORD &&
	      memcmp(
		      &untouched_selection, &untouched_selection_before,
		      sizeof(untouched_selection)) == 0 &&
	      pcm_phase2_info1_handoff(NULL, &untouched_selection) ==
		      PCM_INFO1_INVALID_ARGUMENT &&
	      pcm_phase2_info1_handoff(&analog, NULL) ==
		      PCM_INFO1_INVALID_ARGUMENT,
	      "rejected PCM INFO1 handoff mutated its destination");
	memset(&untouched_entry, 0xa5, sizeof(untouched_entry));
	untouched_entry_before = untouched_entry;
	CHECK(pcm_phase3_entry_from_phase2(
		      &untouched_entry, &analog,
		      codec == TEST_CODEC_PCMU ? G711_LAW_PCMU :
						 G711_LAW_PCMA) ==
		      PCM_PHASE3_ENTRY_INVALID_STATE &&
	      memcmp(
		      &untouched_entry, &untouched_entry_before,
		      sizeof(untouched_entry)) == 0,
	      "premature PCM Phase 3 entry mutated its destination");
	for (size_t index = 0; index < 4u * TEST_SAMPLE_LIMIT; ++index) {
		struct pcm_phase2_event analog_event;
		struct pcm_phase2_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;

		CHECK(pcm_phase2_sample(
			      &analog, delay_line_receive(&digital_to_analog),
			      &analog_transmit, &analog_event) == PCM_PHASE2_OK &&
		      pcm_phase2_sample(
			      &digital, delay_line_receive(&analog_to_digital),
			      &digital_transmit, &digital_event) == PCM_PHASE2_OK,
		      "PCM Phase 2 mode %d failed for codec %d at sample %zu",
		      mode, codec, index);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		if (analog_event.kind ==
		    PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
			analog_initial = true;
			analog_initial_round_trip =
				analog_event.round_trip_delay_samples;
		}
		if (digital_event.kind ==
		    PCM_PHASE2_EVENT_INITIAL_PROBE_STARTED) {
			digital_initial = true;
			digital_initial_round_trip =
				digital_event.round_trip_delay_samples;
		}
		digital_measured_initial |= digital_event.kind ==
			PCM_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED;
		analog_turnaround |= analog_event.kind ==
			PCM_PHASE2_EVENT_PROBE_TURNAROUND_STARTED;
		digital_turnaround |= digital.stage == PCM_PHASE2_PROBE_TURNAROUND;
		analog_reciprocal |= analog_event.kind ==
			PCM_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED;
		digital_reciprocal |= digital_event.kind ==
			PCM_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED;
		analog_ready |= analog_event.kind ==
			PCM_PHASE2_EVENT_READY_FOR_INFO1;
		digital_ready |= digital_event.kind ==
			PCM_PHASE2_EVENT_READY_FOR_INFO1;
		CHECK(analog_event.kind != PCM_PHASE2_EVENT_FAILED &&
		      digital_event.kind != PCM_PHASE2_EVENT_FAILED,
		      "PCM Phase 2 mode %d reported a protocol failure", mode);
		if (analog_ready && digital_ready)
			break;
	}
	CHECK(analog_initial && digital_initial &&
	      analog_initial_round_trip != 0 &&
	      digital_initial_round_trip != 0 && digital_measured_initial &&
	      analog_turnaround && digital_turnaround && analog_reciprocal &&
	      digital_reciprocal && analog_ready && digital_ready &&
	      pcm_phase2_is_ready_for_info1(&analog) &&
	      pcm_phase2_is_ready_for_info1(&digital) &&
	      !pcm_phase2_has_failed(&analog) &&
	      !pcm_phase2_has_failed(&digital),
	      "PCM Phase 2 mode %d lost a probe handoff", mode);
	{
		struct pcm_phase2 recovery = digital;
		bool started = mode == TEST_INFO1_V90 ?
			pcm_phase2_start_v90_digital_info1(
				&recovery, &v90_info1d) == PCM_PHASE2_OK :
			pcm_phase2_start_v92_digital_info1(
				&recovery, &v92_info1d) == PCM_PHASE2_OK;
		bool recovering = false;

		CHECK(started,
		      "PCM Phase 2 mode %d did not start recovery test", mode);
		for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
			struct pcm_phase2_event event;
			int16_t transmit;

			CHECK(pcm_phase2_sample(
				      &recovery, 0, &transmit, &event) ==
				      PCM_PHASE2_OK,
			      "isolated PCM INFO1 mode %d failed at %zu",
			      mode, index);
			if (event.kind ==
			    PCM_PHASE2_EVENT_INFO1_RECOVERY_STARTED) {
				recovering = true;
				break;
			}
		}
		CHECK(recovering && !pcm_phase2_has_failed(&recovery) &&
		      recovery.state.info1.control.phase ==
			      PHASE2_INFO1_RECOVERING &&
		      pcm_phase2_info1_retry_pending(&recovery.state.info1),
		      "PCM Phase 2 mode %d did not recover at the INFO1 deadline",
		      mode);
		for (size_t index = 0; index < V34_PHASE2_SAMPLE_RATE; ++index) {
			struct pcm_phase2_event event;
			int16_t transmit;

			CHECK(pcm_phase2_sample(
				      &recovery, 0, &transmit, &event) ==
					      PCM_PHASE2_OK &&
			      transmit == 0 &&
			      event.kind == PCM_PHASE2_EVENT_NONE &&
			      !pcm_phase2_has_failed(&recovery),
			      "PCM INFO1 recovery mode %d was not stable at %zu",
			      mode, index);
		}
	}
	{
		struct pcm_startup_selection analog_startup;
		struct pcm_startup_selection digital_startup;
		struct pcm_phase2 before = digital;
		bool wrong_protocol_rejected = mode == TEST_INFO1_V90 ?
			pcm_phase2_start_v92_digital_info1(
				&digital, &v92_info1d) ==
				PCM_PHASE2_INVALID_STATE :
			pcm_phase2_start_v90_digital_info1(
				&digital, &v90_info1d) ==
				PCM_PHASE2_INVALID_STATE;
		bool unchanged_after_rejection =
			memcmp(&digital, &before, sizeof(digital)) == 0;
		bool digital_started = mode == TEST_INFO1_V90 ?
			pcm_phase2_start_v90_digital_info1(
				&digital, &v90_info1d) == PCM_PHASE2_OK :
			pcm_phase2_start_v92_digital_info1(
				&digital, &v92_info1d) == PCM_PHASE2_OK;

		CHECK(pcm_phase2_startup_selection(
			      &analog, &analog_startup) == PCM_PHASE2_OK &&
		      pcm_phase2_startup_selection(
			      &before, &digital_startup) == PCM_PHASE2_OK &&
		      analog_startup.protocol == protocol &&
		      digital_startup.protocol == protocol &&
		      analog_startup.phase2 == PCM_PHASE2_FULL &&
		      digital_startup.phase2 == PCM_PHASE2_FULL &&
		      wrong_protocol_rejected &&
		      unchanged_after_rejection,
		      "PCM Phase 2 mode %d did not nail its protocol", mode);
		CHECK(pcm_phase2_start_analog_info1(&analog) == PCM_PHASE2_OK &&
		      digital_started,
		      "PCM Phase 2 mode %d did not enter INFO1", mode);
	}
	for (size_t index = 0; index < 4u * TEST_SAMPLE_LIMIT; ++index) {
		struct pcm_phase2_event analog_event;
		struct pcm_phase2_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;

		CHECK(pcm_phase2_sample(
			      &analog, delay_line_receive(&digital_to_analog),
			      &analog_transmit, &analog_event) == PCM_PHASE2_OK &&
		      pcm_phase2_sample(
			      &digital, delay_line_receive(&analog_to_digital),
			      &digital_transmit, &digital_event) == PCM_PHASE2_OK,
		      "owned PCM INFO1 mode %d failed for codec %d at sample %zu",
		      mode, codec, index);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		if (digital_event.kind == PCM_PHASE2_EVENT_LOCAL_INFO1D_SENT)
			digital_sent = index;
		if (analog_event.kind == PCM_PHASE2_EVENT_PEER_INFO1D_RECEIVED) {
			analog_received = index;
			CHECK(pcm_phase2_peer_info1d(&analog, &peer_info1d) ==
				      PCM_PHASE2_OK &&
			      info1d_equal(
				      &peer_info1d,
				      mode == TEST_INFO1_V90 ? &v90_info1d :
							       &v92_info1d.v34) &&
			      (mode == TEST_INFO1_V90 ||
			       (pcm_phase2_peer_v92_info1d(
					&analog, &peer_v92_info1d) ==
						PCM_PHASE2_OK &&
				v92_info1d_equal(
					&peer_v92_info1d, &v92_info1d))) &&
			      (mode == TEST_INFO1_V92_PCM ?
				       pcm_phase2_reply_pcm(
					       &analog, &pcm_info1a) ==
					       PCM_PHASE2_OK :
				       pcm_phase2_reply_conventional(
					       &analog,
					       &conventional_info1a) ==
					       PCM_PHASE2_OK),
			      "analog controller did not answer INFO1 mode %d", mode);
		}
		if (analog_event.kind == PCM_PHASE2_EVENT_LOCAL_INFO1A_SENT) {
			analog_sent = index;
			CHECK(pcm_phase2_is_ready_for_phase3(&analog) &&
			      analog.state.info1.control.phase == PHASE2_INFO1_SILENCE &&
			      analog.state.info1.control.silence_samples == 0,
			      "PCM Phase 2 mode %d used the wrong local INFO1a "
			      "handoff boundary",
			      mode);
		}
		if (digital_event.kind == PCM_PHASE2_EVENT_PEER_INFO1A_RECEIVED) {
			digital_received = index;
			CHECK(pcm_phase2_is_ready_for_phase3(&digital) &&
			      digital.state.info1.control.phase == PHASE2_INFO1_SILENCE,
			      "PCM Phase 2 mode %d used the wrong peer INFO1a "
			      "handoff boundary",
			      mode);
		}
		CHECK(analog_event.kind != PCM_PHASE2_EVENT_FAILED &&
		      digital_event.kind != PCM_PHASE2_EVENT_FAILED,
		      "owned PCM INFO1 mode %d reported a failure", mode);
		if (pcm_phase2_is_ready_for_phase3(&analog) &&
		    pcm_phase2_is_ready_for_phase3(&digital))
			break;
	}
	{
		bool peer_analog_matches = mode == TEST_INFO1_V92_PCM ?
			pcm_phase2_peer_pcm_info1a(
				&digital, &peer_pcm_info1a) == PCM_PHASE2_OK &&
				pcm_info1a_equal(
					&peer_pcm_info1a, &pcm_info1a) :
			pcm_phase2_peer_info1a(
				&digital, &peer_info1a) == PCM_PHASE2_OK &&
				info1a_equal(
					&peer_info1a, &conventional_info1a);
		bool handoff_boundary_matches =
			analog.state.info1.control.phase == PHASE2_INFO1_SILENCE &&
			analog.state.info1.control.silence_samples <
				PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES &&
			digital.state.info1.control.phase == PHASE2_INFO1_SILENCE &&
			digital.state.info1.control.silence_samples <
				PHASE2_INFO1_POST_INFO1A_SILENCE_SAMPLES;

		CHECK(digital_sent < analog_received &&
		      analog_received < analog_sent &&
		      analog_sent < digital_received &&
		      pcm_phase2_is_ready_for_phase3(&analog) &&
		      pcm_phase2_is_ready_for_phase3(&digital) &&
		      peer_analog_matches && handoff_boundary_matches,
		      "PCM Phase 2 mode %d did not hand off INFO1 in order", mode);
	}
	{
		struct pcm_info1_selection analog_selection;
		struct pcm_info1_selection digital_selection;
		bool digital_records_match;
		bool analog_records_match;

		CHECK(pcm_phase2_info1_handoff(
			      &analog, &analog_selection) == PCM_INFO1_OK &&
		      pcm_phase2_info1_handoff(
			      &digital, &digital_selection) == PCM_INFO1_OK,
		      "PCM Phase 2 mode %d did not hand off INFO1", mode);

		digital_records_match = mode == TEST_INFO1_V90 ?
			info1d_equal(
				&analog_selection.digital.record.v90,
				&v90_info1d) &&
				info1d_equal(
					&digital_selection.digital.record.v90,
					&v90_info1d) :
			v92_info1d_equal(
				&analog_selection.digital.record.v92,
				&v92_info1d) &&
				v92_info1d_equal(
					&digital_selection.digital.record.v92,
					&v92_info1d);
		analog_records_match = mode == TEST_INFO1_V92_PCM ?
			pcm_info1a_equal(
				&analog_selection.analog.pcm, &pcm_info1a) &&
				pcm_info1a_equal(
					&digital_selection.analog.pcm,
					&pcm_info1a) :
			info1a_equal(
				&analog_selection.analog.conventional,
				&conventional_info1a) &&
				info1a_equal(
					&digital_selection.analog.conventional,
					&conventional_info1a);

		CHECK(pcm_info1_selection_is_valid(&analog_selection) &&
		      pcm_info1_selection_is_valid(&digital_selection) &&
		      analog_selection.startup.protocol == protocol &&
		      digital_selection.startup.protocol == protocol &&
		      analog_selection.startup.phase2 == PCM_PHASE2_FULL &&
		      digital_selection.startup.phase2 == PCM_PHASE2_FULL &&
		      analog_selection.upstream == upstream &&
		      digital_selection.upstream == upstream &&
		      digital_records_match && analog_records_match,
		      "PCM Phase 2 mode %d lost its INFO1 decision", mode);
	}
	{
		struct pcm_phase3_entry analog_entry;
		struct pcm_phase3_entry digital_entry;
		enum g711_law law = codec == TEST_CODEC_PCMU ?
					    G711_LAW_PCMU : G711_LAW_PCMA;
		bool parameters_match;

		CHECK(pcm_phase3_entry_from_phase2(
			      &analog_entry, &analog, law) ==
			      PCM_PHASE3_ENTRY_OK &&
		      pcm_phase3_entry_from_phase2(
			      &digital_entry, &digital, law) ==
			      PCM_PHASE3_ENTRY_OK &&
		      pcm_phase3_entry_is_valid(&analog_entry) &&
		      pcm_phase3_entry_is_valid(&digital_entry),
		      "PCM Phase 2 mode %d did not form Phase 3 entries", mode);
		parameters_match = upstream == PCM_UPSTREAM_PCM ?
			pcm_info1a_equal(
				&analog_entry.selection.analog.pcm,
				&pcm_info1a) &&
				pcm_info1a_equal(
					&digital_entry.selection.analog.pcm,
					&pcm_info1a) :
			analog_entry.selection.analog.conventional.training_ucode ==
				conventional_info1a.training_ucode &&
				digital_entry.selection.analog.conventional
					.training_ucode ==
					conventional_info1a.training_ucode &&
				analog_entry.conventional_profile.symbol_rate ==
					conventional_info1a.upstream_symbol_rate &&
				digital_entry.conventional_profile.symbol_rate ==
					conventional_info1a.upstream_symbol_rate;
		CHECK(analog_entry.local_role == V90_ANALOG_MODEM &&
		      digital_entry.local_role == V90_DIGITAL_MODEM &&
		      analog_entry.law == law && digital_entry.law == law &&
		      analog_entry.selection.startup.protocol == protocol &&
		      digital_entry.selection.startup.protocol == protocol &&
		      analog_entry.selection.upstream == upstream &&
		      digital_entry.selection.upstream == upstream &&
		      parameters_match,
		      "PCM Phase 3 entry lost mode %d branch selection", mode);
	}
	{
		struct pcm_phase2_event event = {
			.kind = PCM_PHASE2_EVENT_FAILED,
		};
		int16_t output = 1234;

		CHECK(pcm_phase2_sample(&analog, 0, &output, &event) ==
			      PCM_PHASE2_OK &&
		      output == 0 && event.kind == PCM_PHASE2_EVENT_NONE &&
		      pcm_phase2_is_ready_for_phase3(&analog),
		      "completed PCM Phase 2 mode %d was not stable", mode);
	}
	CHECK(pcm_phase2_peer_info0d(&analog, &peer_info0d) == PCM_PHASE2_OK &&
	      pcm_phase2_peer_info0a(&digital, &peer_info0a) == PCM_PHASE2_OK &&
	      peer_info0d.pcm_law == digital_info.pcm_law &&
	      peer_info0d.nominal_transmit_power ==
		      digital_info.nominal_transmit_power &&
	      peer_info0a.symbol_rate_2743_supported ==
		      analog_info.symbol_rate_2743_supported &&
	      peer_info0a.maximum_symbol_rate_difference_steps ==
		      analog_info.maximum_symbol_rate_difference_steps &&
	      (mode == TEST_INFO1_V90 ||
	       (pcm_phase2_peer_v92_info0d(
			&analog, &peer_v92_info0d) == PCM_PHASE2_OK &&
		pcm_phase2_peer_v92_info0a(
			&digital, &peer_v92_info0a) == PCM_PHASE2_OK &&
		peer_v92_info0d.v92_capable && peer_v92_info0a.v92_capable)),
	      "PCM Phase 2 mode %d lost a typed INFO0 record", mode);
	CHECK(pcm_phase2_measure_remote_probe(
		      &analog, &analog_measurement) == PCM_PHASE2_OK &&
	      pcm_phase2_measure_remote_probe(
		      &digital, &digital_measurement) == PCM_PHASE2_OK &&
	      v34_phase2_probe_measurement_is_valid(&analog_measurement) &&
	      v34_phase2_probe_measurement_is_valid(&digital_measurement) &&
	      analog_measurement.l1.harmonics[0].mean_square > 0.0 &&
	      digital_measurement.l1.harmonics[0].mean_square > 0.0,
	      "PCM Phase 2 mode %d lost a probe measurement", mode);
	return true;
}

static bool check_v92_fallback(enum test_codec codec)
{
	struct v92_info0a analog_info = {
		.v90 = analog_capabilities(),
		.v92_capable = true,
	};
	struct v90_info0d digital_info = digital_capabilities(codec);
	struct pcm_phase2 analog;
	struct pcm_phase2 digital;
	struct pcm_phase2 untouched;
	struct pcm_phase2 untouched_before;
	struct test_delay_line analog_to_digital = { 0 };
	struct test_delay_line digital_to_analog = { 0 };
	struct pcm_startup_selection analog_startup;
	struct pcm_startup_selection digital_startup;
	struct v92_info0d peer_v92_info0d;
	bool selected = false;

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_before = untouched;
	analog_info.short_phase2_requested = true;
	analog_info.v92_capable = false;
	CHECK(pcm_phase2_init_v92_analog(
		      &untouched, TEST_AMPLITUDE, &analog_info) ==
		      PCM_PHASE2_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_before, sizeof(untouched)) == 0,
	      "invalid V.92 short capabilities mutated the controller");
	analog_info.short_phase2_requested = false;
	analog_info.v92_capable = true;
	CHECK(pcm_phase2_init_v92_analog(
		      &analog, TEST_AMPLITUDE, &analog_info) == PCM_PHASE2_OK &&
	      pcm_phase2_init_v90_digital(
		      &digital, TEST_AMPLITUDE, &digital_info) == PCM_PHASE2_OK,
	      "asymmetric V.92 offer did not initialize");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct pcm_phase2_event analog_event;
		struct pcm_phase2_event digital_event;
		int16_t analog_transmit;
		int16_t digital_transmit;

		CHECK(pcm_phase2_sample(
			      &analog, delay_line_receive(&digital_to_analog),
			      &analog_transmit, &analog_event) == PCM_PHASE2_OK &&
		      pcm_phase2_sample(
			      &digital, delay_line_receive(&analog_to_digital),
			      &digital_transmit, &digital_event) == PCM_PHASE2_OK,
		      "V.92 fallback failed at sample %zu", index);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		selected = pcm_phase2_startup_selection(
				   &analog, &analog_startup) == PCM_PHASE2_OK &&
			   pcm_phase2_startup_selection(
				   &digital, &digital_startup) == PCM_PHASE2_OK;
		if (selected)
			break;
	}
	CHECK(selected && analog_startup.protocol == PCM_PROTOCOL_V90 &&
	      digital_startup.protocol == PCM_PROTOCOL_V90 &&
	      analog_startup.phase2 == PCM_PHASE2_FULL &&
	      digital_startup.phase2 == PCM_PHASE2_FULL &&
	      analog.info0_protocol == PCM_PROTOCOL_V92 &&
	      digital.info0_protocol == PCM_PROTOCOL_V90 &&
	      pcm_phase2_peer_v92_info0d(
		      &analog, &peer_v92_info0d) == PCM_PHASE2_OK &&
	      !peer_v92_info0d.v92_capable,
	      "V.92 offer did not negotiate one shared V.90 fallback");
	return true;
}

static bool test_pcm_initial_probe(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		CHECK(check_initial_probe(codec),
		      "PCM initial probe failed for codec %d", codec);
		for (enum test_info1_mode mode = TEST_INFO1_V90;
		     mode <= TEST_INFO1_V92_PCM; ++mode)
			CHECK(check_phase2_controller(codec, mode, 0),
			      "PCM Phase 2 mode %d failed for codec %d",
			      mode, codec);
		CHECK(check_v92_fallback(codec),
		      "V.92 fallback failed for codec %d", codec);
		for (unsigned offer = 1; offer <= 2; ++offer)
			for (enum test_info1_mode mode = TEST_INFO1_V92_CONVENTIONAL;
			     mode <= TEST_INFO1_V92_PCM; ++mode)
				CHECK(check_phase2_controller(codec, mode, offer),
				      "unilateral short offer %u mode %d codec %d", offer, mode, codec);
	}
	return true;
}

int main(void)
{
	const char *name = "PCM protocol and codec matrix through INFO1";

	fprintf(stderr, "[ RUN      ] %s\n", name);
	if (!test_pcm_initial_probe()) {
		fprintf(stderr, "[  FAILED  ] %s\n", name);
		return 1;
	}
	fprintf(stderr, "[       OK ] %s\n", name);
	return 0;
}
