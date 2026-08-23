// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_phase2_info0_exchange.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_SAMPLE_LIMIT = 12000,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

enum test_info0_mode {
	TEST_INFO0_V90,
	TEST_INFO0_V92_FULL,
	TEST_INFO0_V92_SHORT,
};

struct exchange_observation {
	size_t info_received;
	size_t info_rejected;
	size_t tone_started;
	union pcm_info0_record peer_info0;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	return codec == TEST_CODEC_PCMU ?
		       g711_decode_pcmu(g711_encode_pcmu(sample)) :
		       g711_decode_pcma(g711_encode_pcma(sample));
}

static struct v90_info0 analog_capabilities(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
		.acknowledges_peer_info0 = true,
	};
}

static struct v90_info0d digital_capabilities(enum test_codec codec)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.high_carrier_3000_supported = true,
			.low_carrier_3200_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
			.acknowledges_peer_info0 = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = codec == TEST_CODEC_PCMU ?
				   V90_PCM_MU_LAW : V90_PCM_A_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static bool analog_equal(
	const struct v90_info0 *left, const struct v90_info0 *right)
{
	struct v90_info0 expected = *right;
	uint8_t left_payload[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO0A_PAYLOAD_BYTES];

	expected.acknowledges_peer_info0 = left->acknowledges_peer_info0;
	return v90_info0a_encode(left, left_payload, sizeof(left_payload)) ==
		       V90_INFO0_OK &&
	       v90_info0a_encode(&expected, right_payload, sizeof(right_payload)) ==
		       V90_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool digital_equal(
	const struct v90_info0d *left, const struct v90_info0d *right)
{
	struct v90_info0d expected = *right;
	uint8_t left_payload[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO0D_PAYLOAD_BYTES];

	expected.common.acknowledges_peer_info0 =
		left->common.acknowledges_peer_info0;
	return v90_info0d_encode(left, left_payload, sizeof(left_payload)) ==
		       V90_INFO0_OK &&
	       v90_info0d_encode(&expected, right_payload, sizeof(right_payload)) ==
		       V90_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool v92_analog_equal(
	const struct v92_info0a *left, const struct v92_info0a *right)
{
	struct v92_info0a expected = *right;
	uint8_t left_payload[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO0A_PAYLOAD_BYTES];

	expected.v90.acknowledges_peer_info0 =
		left->v90.acknowledges_peer_info0;
	return v92_info0a_encode(left, left_payload, sizeof(left_payload)) ==
		       V92_INFO0_OK &&
	       v92_info0a_encode(&expected, right_payload, sizeof(right_payload)) ==
		       V92_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static bool v92_digital_equal(
	const struct v92_info0d *left, const struct v92_info0d *right)
{
	struct v92_info0d expected = *right;
	uint8_t left_payload[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t right_payload[V90_INFO0D_PAYLOAD_BYTES];

	expected.v90.common.acknowledges_peer_info0 =
		left->v90.common.acknowledges_peer_info0;
	return v92_info0d_encode(left, left_payload, sizeof(left_payload)) ==
		       V92_INFO0_OK &&
	       v92_info0d_encode(&expected, right_payload, sizeof(right_payload)) ==
		       V92_INFO0_OK &&
	       memcmp(left_payload, right_payload, sizeof(left_payload)) == 0;
}

static enum pcm_protocol protocol_for_mode(enum test_info0_mode mode)
{
	return mode == TEST_INFO0_V90 ? PCM_PROTOCOL_V90 : PCM_PROTOCOL_V92;
}

static void observe(
	const struct pcm_phase2_info0_exchange_event *event,
	struct exchange_observation *observation)
{
	if (event->peer_info0_event == PCM_PEER_INFO0_RECEIVED) {
		observation->info_received++;
		observation->peer_info0 = event->peer_info0;
	} else if (event->peer_info0_event == PCM_PEER_INFO0_REJECTED) {
		observation->info_rejected++;
	}
	if (event->tone_event.kind == V34_PHASE2_TONE_STARTED)
		observation->tone_started++;
}

static bool check_exchange(
	enum test_codec codec, enum test_info0_mode analog_mode,
	enum test_info0_mode digital_mode, bool drop_initial_analog_info0)
{
	struct v92_info0a analog = {
		.v90 = analog_capabilities(),
		.v92_capable = analog_mode != TEST_INFO0_V90,
		.short_phase2_requested = analog_mode == TEST_INFO0_V92_SHORT,
	};
	struct v92_info0d digital = {
		.v90 = digital_capabilities(codec),
		.v92_capable = digital_mode != TEST_INFO0_V90,
		.short_phase2_requested = digital_mode == TEST_INFO0_V92_SHORT,
	};
	struct v92_info0a expected_analog = analog;
	struct v92_info0d expected_digital = digital;
	struct pcm_phase2_info0_exchange analog_end;
	struct pcm_phase2_info0_exchange digital_end;
	struct exchange_observation analog_observation = { 0 };
	struct exchange_observation digital_observation = { 0 };
	uint8_t analog_payload[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t digital_payload[V90_INFO0D_PAYLOAD_BYTES];
	int16_t analog_line = 0;
	int16_t digital_line = 0;
	size_t analog_tone_start = SIZE_MAX;
	size_t digital_tone_start = SIZE_MAX;
	size_t analog_ack_repeats = 0;
	size_t digital_ack_repeats = 0;
	size_t digital_unacknowledged_repeats = 0;
	bool forward_analog = !drop_initial_analog_info0;
	bool digital_entered_repeat = false;
	bool initialized;

	expected_analog.v90.acknowledges_peer_info0 = false;
	expected_digital.v90.common.acknowledges_peer_info0 = false;
	if (analog_mode == TEST_INFO0_V90)
		initialized = pcm_phase2_info0_exchange_init_v90_analog(
			&analog_end, TEST_AMPLITUDE, &analog.v90) ==
			PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v90_info0a_encode(
				&expected_analog.v90, analog_payload,
				sizeof(analog_payload)) == V90_INFO0_OK;
	else
		initialized = pcm_phase2_info0_exchange_init_v92_analog(
			&analog_end, TEST_AMPLITUDE, &analog) ==
			PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v92_info0a_encode(
				&expected_analog, analog_payload,
				sizeof(analog_payload)) == V92_INFO0_OK;
	if (digital_mode == TEST_INFO0_V90)
		initialized = initialized &&
			pcm_phase2_info0_exchange_init_v90_digital(
				&digital_end, TEST_AMPLITUDE, &digital.v90) ==
				PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v90_info0d_encode(
				&expected_digital.v90, digital_payload,
				sizeof(digital_payload)) == V90_INFO0_OK;
	else
		initialized = initialized &&
			pcm_phase2_info0_exchange_init_v92_digital(
				&digital_end, TEST_AMPLITUDE, &digital) ==
				PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v92_info0d_encode(
				&expected_digital, digital_payload,
				sizeof(digital_payload)) == V92_INFO0_OK;
	CHECK(initialized &&
	      analog_end.protocol == protocol_for_mode(analog_mode) &&
	      digital_end.protocol == protocol_for_mode(digital_mode) &&
	      analog_end.transport.signal_role == PHASE2_SIGNAL_A &&
	      digital_end.transport.signal_role == PHASE2_SIGNAL_B &&
	      analog_end.transport.info_transmitter.frame.payload_bit_count ==
		      V90_INFO0A_PAYLOAD_BITS &&
	      analog_end.transport.info_receiver.frame
		      .expected_payload_bit_count == V90_INFO0D_PAYLOAD_BITS &&
	      digital_end.transport.info_transmitter.frame.payload_bit_count ==
		      V90_INFO0D_PAYLOAD_BITS &&
	      digital_end.transport.info_receiver.frame
		      .expected_payload_bit_count == V90_INFO0A_PAYLOAD_BITS &&
	      memcmp(analog_end.transport.info_transmitter.frame.payload,
		     analog_payload, sizeof(analog_payload)) == 0 &&
	      memcmp(digital_end.transport.info_transmitter.frame.payload,
		     digital_payload, sizeof(digital_payload)) == 0,
	      "PCM INFO0 endpoints %d/%d did not initialize exactly",
	      analog_mode, digital_mode);

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct pcm_phase2_info0_exchange_event analog_event;
		struct pcm_phase2_info0_exchange_event digital_event;
		enum phase2_info0_transmit_phase analog_before =
			analog_end.transport.transmit_phase;
		enum phase2_info0_transmit_phase digital_before =
			digital_end.transport.transmit_phase;
		int16_t next_analog;
		int16_t next_digital;

		CHECK(pcm_phase2_info0_exchange_sample(
			      &analog_end,
			      codec_round_trip(digital_line, codec),
			      &next_analog, &analog_event) ==
			      PCM_PHASE2_INFO0_EXCHANGE_OK &&
		      pcm_phase2_info0_exchange_sample(
			      &digital_end,
			      forward_analog ?
				      codec_round_trip(analog_line, codec) : 0,
			      &next_digital, &digital_event) ==
			      PCM_PHASE2_INFO0_EXCHANGE_OK,
		      "PCM INFO0 exchange %d/%d failed at sample %zu",
		      analog_mode, digital_mode, index);
		CHECK(analog_event.protocol == protocol_for_mode(analog_mode) &&
		      digital_event.protocol == protocol_for_mode(digital_mode),
		      "PCM INFO0 event lost its protocol tag");
		observe(&analog_event, &analog_observation);
		observe(&digital_event, &digital_observation);
		if (index < PHASE2_INFO0_POST_V8_SILENCE_SAMPLES)
			CHECK(next_analog == 0 && next_digital == 0,
			      "post-V.8 silence ended at sample %zu", index);
		if (analog_tone_start == SIZE_MAX &&
		    analog_before == PHASE2_INFO0_CARRIER_TAIL &&
		    analog_end.transport.transmit_phase !=
			    PHASE2_INFO0_CARRIER_TAIL)
			analog_tone_start = index + 1u;
		if (digital_tone_start == SIZE_MAX &&
		    digital_before == PHASE2_INFO0_CARRIER_TAIL &&
		    digital_end.transport.transmit_phase !=
			    PHASE2_INFO0_CARRIER_TAIL)
			digital_tone_start = index + 1u;
		if (!forward_analog && analog_tone_start != SIZE_MAX)
			forward_analog = true;
		digital_entered_repeat |=
			digital_end.state == PCM_PHASE2_INFO0_REPEAT;
		if (analog_before != PHASE2_INFO0_SEQUENCE &&
		    analog_before != PHASE2_INFO0_POST_V8_SILENCE &&
		    analog_end.transport.transmit_phase ==
			    PHASE2_INFO0_SEQUENCE &&
		    analog_end.state == PCM_PHASE2_INFO0_ACKNOWLEDGING)
			analog_ack_repeats++;
		if (digital_before != PHASE2_INFO0_SEQUENCE &&
		    digital_before != PHASE2_INFO0_POST_V8_SILENCE &&
		    digital_end.transport.transmit_phase ==
			    PHASE2_INFO0_SEQUENCE) {
			digital_ack_repeats +=
				digital_end.state == PCM_PHASE2_INFO0_ACKNOWLEDGING;
			digital_unacknowledged_repeats +=
				digital_end.state == PCM_PHASE2_INFO0_REPEAT;
		}
		analog_line = next_analog;
		digital_line = next_digital;
		if (pcm_phase2_info0_exchange_complete(&analog_end) &&
		    pcm_phase2_info0_exchange_complete(&digital_end))
			break;
	}
	CHECK(analog_tone_start != SIZE_MAX &&
	      digital_tone_start != SIZE_MAX &&
	      analog_tone_start < digital_tone_start &&
	      analog_end.transport.transmit_phase == PHASE2_INFO0_TONE &&
	      digital_end.transport.transmit_phase == PHASE2_INFO0_TONE &&
	      pcm_phase2_info0_exchange_complete(&analog_end) &&
	      pcm_phase2_info0_exchange_complete(&digital_end) &&
	      analog_end.state == PCM_PHASE2_INFO0_COMPLETE &&
	      digital_end.state == PCM_PHASE2_INFO0_COMPLETE,
	      "unequal INFO0 frame timing or tone handoff changed");
	CHECK(analog_observation.info_received == 1 &&
	      analog_observation.info_rejected == 0 &&
	      digital_observation.info_received == 1 &&
	      digital_observation.info_rejected == 0,
	      "PCM INFO0 record counts changed for %d/%d over codec %d: info %zu/%zu reject %zu/%zu tone %zu/%zu",
	      analog_mode, digital_mode, codec,
	      analog_observation.info_received,
	      digital_observation.info_received,
	      analog_observation.info_rejected,
	      digital_observation.info_rejected,
	      analog_observation.tone_started,
	      digital_observation.tone_started);
	if (drop_initial_analog_info0)
		CHECK(digital_entered_repeat &&
		      digital_unacknowledged_repeats >= 1 &&
		      analog_ack_repeats >= 1 && digital_ack_repeats >= 1 &&
		      (digital_mode == TEST_INFO0_V90 ?
			       digital_end.peer_info0.analog
				       .acknowledges_peer_info0 :
			       digital_end.peer_info0.v92_analog.v90
				       .acknowledges_peer_info0),
		      "dropped INFO0 did not exercise repeat/ACK recovery");
	if (analog_mode == TEST_INFO0_V90)
		CHECK(digital_equal(
			      &analog_observation.peer_info0.digital,
			      &expected_digital.v90),
		      "V.90 analog endpoint changed the digital base record");
	else
		CHECK(v92_digital_equal(
			      &analog_observation.peer_info0.v92_digital,
			      &expected_digital),
		      "V.92 analog endpoint changed the digital extension");
	if (digital_mode == TEST_INFO0_V90)
		CHECK(analog_equal(
			      &digital_observation.peer_info0.analog,
			      &expected_analog.v90),
		      "V.90 digital endpoint changed the analog base record");
	else
		CHECK(v92_analog_equal(
			      &digital_observation.peer_info0.v92_analog,
			      &expected_analog),
		      "V.92 digital endpoint changed the analog extension");
	return true;
}

static bool test_pcm_exchange(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		for (enum test_info0_mode analog = TEST_INFO0_V90;
		     analog <= TEST_INFO0_V92_SHORT; ++analog) {
			for (enum test_info0_mode digital = TEST_INFO0_V90;
			     digital <= TEST_INFO0_V92_SHORT; ++digital)
				CHECK(check_exchange(
					      codec, analog, digital, false),
				      "PCM INFO0 matrix failed for %d/%d/%d",
				      codec, analog, digital);
		}
	}
	return true;
}

static bool test_dropped_initial_info0(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum test_info0_mode mode = TEST_INFO0_V90;
		     mode <= TEST_INFO0_V92_FULL; ++mode)
			CHECK(check_exchange(codec, mode, mode, true),
			      "dropped INFO0 failed for protocol/law %d/%d",
			      mode, codec);
	return true;
}

enum following_tone_case {
	TEST_TONE_DURING_ACK,
	TEST_TONE_AFTER_ACK,
	TEST_TONE_INVALIDATED_BY_REPEAT,
	TEST_TONE_REPEAT_AFTER_COMPLETION,
	TEST_TONE_REJECT_AFTER_COMPLETION,
};

static bool check_following_tone(
	enum test_codec codec, enum test_info0_mode mode,
	enum following_tone_case test_case, bool digital_local)
{
	struct v92_info0a analog = {
		.v90 = analog_capabilities(),
		.v92_capable = mode == TEST_INFO0_V92_FULL,
	};
	struct v92_info0d digital = {
		.v90 = digital_capabilities(codec),
		.v92_capable = mode == TEST_INFO0_V92_FULL,
	};
	struct pcm_phase2_info0_exchange exchange;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t repeat_payload[V90_INFO0D_PAYLOAD_BYTES];
	int16_t peer_line = 0;
	size_t acknowledge_starts = 0;
	size_t wait_starts = 0;
	size_t final_wait = SIZE_MAX;
	size_t completed = SIZE_MAX;
	size_t first_completion = SIZE_MAX;
	size_t received_records = 0;
	bool tone_during_ack = false;
	bool tone_retained = false;
	bool tone_invalidated = false;
	bool fresh_tone_after_repeat = false;
	bool repeated = false;
	bool overlapping_repeat = false;
	bool reopened_after_completion = false;
	bool rejected_after_completion = false;
	bool initialized;

	analog.v90.acknowledges_peer_info0 = false;
	digital.v90.common.acknowledges_peer_info0 = false;
	if (mode == TEST_INFO0_V90 && !digital_local)
		initialized = pcm_phase2_info0_exchange_init_v90_analog(
			&exchange, TEST_AMPLITUDE, &analog.v90) ==
			PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v90_info0d_encode(
				&digital.v90, payload, sizeof(payload)) == V90_INFO0_OK;
	else if (mode == TEST_INFO0_V90)
		initialized = pcm_phase2_info0_exchange_init_v90_digital(
			&exchange, TEST_AMPLITUDE, &digital.v90) ==
			PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v90_info0a_encode(
				&analog.v90, payload, sizeof(payload)) == V90_INFO0_OK;
	else if (!digital_local)
		initialized = pcm_phase2_info0_exchange_init_v92_analog(
			&exchange, TEST_AMPLITUDE, &analog) ==
			PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v92_info0d_encode(
				&digital, payload, sizeof(payload)) == V92_INFO0_OK;
	else
		initialized = pcm_phase2_info0_exchange_init_v92_digital(
			&exchange, TEST_AMPLITUDE, &digital) ==
			PCM_PHASE2_INFO0_EXCHANGE_OK &&
			v92_info0a_encode(
				&analog, payload, sizeof(payload)) == V92_INFO0_OK;
	CHECK(initialized &&
	      phase2_info0_exchange_init(
		      &peer, digital_local ? PHASE2_SIGNAL_A : PHASE2_SIGNAL_B,
		      TEST_AMPLITUDE, payload,
		      digital_local ? V90_INFO0A_PAYLOAD_BITS :
				      V90_INFO0D_PAYLOAD_BITS,
		      digital_local ? V90_INFO0D_PAYLOAD_BITS :
				      V90_INFO0A_PAYLOAD_BITS) ==
		      PHASE2_INFO0_EXCHANGE_OK,
	      "following-tone fixtures did not initialize for %d/%d/%d/%d",
	      mode, codec, test_case, digital_local);
	memcpy(repeat_payload, payload, sizeof(repeat_payload));
	if (test_case == TEST_TONE_REJECT_AFTER_COMPLETION) {
		if (digital_local) {
			struct v92_info0a changed = analog;

			changed.v90.symbol_rate_2743_supported =
				!changed.v90.symbol_rate_2743_supported;
			initialized = mode == TEST_INFO0_V90 ?
				v90_info0a_encode(
					&changed.v90, repeat_payload,
					sizeof(repeat_payload)) == V90_INFO0_OK :
				v92_info0a_encode(
					&changed, repeat_payload,
					sizeof(repeat_payload)) == V92_INFO0_OK;
		} else {
			struct v92_info0d changed = digital;

			changed.v90.nominal_transmit_power++;
			initialized = mode == TEST_INFO0_V90 ?
				v90_info0d_encode(
					&changed.v90, repeat_payload,
					sizeof(repeat_payload)) == V90_INFO0_OK :
				v92_info0d_encode(
					&changed, repeat_payload,
					sizeof(repeat_payload)) == V92_INFO0_OK;
		}
		CHECK(initialized,
		      "mismatched repeat did not encode for %d/%d/%d",
		      mode, codec, digital_local);
	}

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct pcm_phase2_info0_exchange_event event;
		enum phase2_info0_transmit_phase transmit_before =
			exchange.transport.transmit_phase;
		enum pcm_phase2_info0_exchange_state state_before =
			exchange.state;
		bool retain_tone_before = exchange.fresh_peer_tone_seen;
		int16_t received;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK,
		      "peer INFO0 exchange failed at %zu for %d/%d/%d",
		      index, mode, codec, test_case);
		received = test_case == TEST_TONE_AFTER_ACK &&
			   peer.transmit_phase == PHASE2_INFO0_TONE &&
			   exchange.state != PCM_PHASE2_INFO0_WAIT_COMPLETION ?
				      0 : codec_round_trip(peer_line, codec);
		CHECK(pcm_phase2_info0_exchange_sample(
			      &exchange, received,
			      &ignored, &event) == PCM_PHASE2_INFO0_EXCHANGE_OK,
		      "following-tone exchange failed at %zu for %d/%d/%d/%d",
		      index, mode, codec, test_case, digital_local);
		if (repeated && transmit_before == PHASE2_INFO0_CARRIER_TAIL &&
		    exchange.transport.transmit_phase == PHASE2_INFO0_TONE &&
		    (peer.transmit_phase == PHASE2_INFO0_SEQUENCE ||
		     peer.transmit_phase == PHASE2_INFO0_CARRIER_TAIL)) {
			overlapping_repeat = true;
			CHECK(!pcm_phase2_info0_exchange_complete(&exchange),
			      "peer INFO0 carrier completed exchange for %d/%d/%d",
			      mode, codec, digital_local);
		}
		if (transmit_before != PHASE2_INFO0_SEQUENCE &&
		    transmit_before != PHASE2_INFO0_POST_V8_SILENCE &&
		    exchange.transport.transmit_phase == PHASE2_INFO0_SEQUENCE)
			acknowledge_starts++;
		if (state_before != PCM_PHASE2_INFO0_WAIT_COMPLETION &&
		    exchange.state == PCM_PHASE2_INFO0_WAIT_COMPLETION) {
			wait_starts++;
			final_wait = index;
			CHECK(exchange.transport.tone_detector.carrier
				      .qualifying_samples == 0 &&
			      !exchange.transport.tone_detector.carrier
				       .tone_present,
			      "post-ACK tone detector retained prior evidence");
		}
		if (!retain_tone_before && exchange.fresh_peer_tone_seen) {
			tone_retained = true;
			tone_during_ack |=
				exchange.transport.transmit_phase != PHASE2_INFO0_TONE;
			if (tone_invalidated)
				fresh_tone_after_repeat = true;
		}
		if (repeated && retain_tone_before &&
		    !exchange.fresh_peer_tone_seen &&
		    !pcm_phase2_info0_exchange_complete(&exchange))
			tone_invalidated = true;
		if (state_before == PCM_PHASE2_INFO0_COMPLETE &&
		    !pcm_phase2_info0_exchange_complete(&exchange))
			reopened_after_completion = true;
		if (test_case == TEST_TONE_INVALIDATED_BY_REPEAT &&
		    !repeated && exchange.fresh_peer_tone_seen &&
		    acknowledge_starts == 1 &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(phase2_info0_exchange_restart_info(
				      &peer, payload,
				      digital_local ? V90_INFO0A_PAYLOAD_BITS :
					      V90_INFO0D_PAYLOAD_BITS) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "peer INFO0 repeat did not start for %d/%d",
			      mode, codec);
			repeated = true;
		}
		if (event.peer_info0_event == PCM_PEER_INFO0_REJECTED &&
		    test_case == TEST_TONE_REJECT_AFTER_COMPLETION) {
			rejected_after_completion = true;
			completed = index;
			break;
		}
		if (event.peer_info0_event == PCM_PEER_INFO0_RECEIVED) {
			received_records++;
			if (first_completion == SIZE_MAX)
				first_completion = index;
			else
				completed = index;
			fresh_tone_after_repeat |= repeated && tone_invalidated &&
				event.tone_event.kind == V34_PHASE2_TONE_STARTED;
			if (mode == TEST_INFO0_V90 && !digital_local)
				CHECK(!event.peer_info0.digital.common
					       .acknowledges_peer_info0 &&
				      digital_equal(
					      &event.peer_info0.digital,
					      &digital.v90),
				      "V.90 repeat changed retained capabilities");
			else if (mode != TEST_INFO0_V90 && !digital_local)
				CHECK(!event.peer_info0.v92_digital.v90.common
					       .acknowledges_peer_info0 &&
				      v92_digital_equal(
					      &event.peer_info0.v92_digital,
					      &digital),
				      "V.92 repeat changed retained capabilities");
			else if (mode == TEST_INFO0_V90)
				CHECK(!event.peer_info0.analog
					       .acknowledges_peer_info0 &&
				      analog_equal(
					      &event.peer_info0.analog,
					      &analog.v90),
				      "V.90 repeat changed retained capabilities");
			else
				CHECK(!event.peer_info0.v92_analog.v90
					       .acknowledges_peer_info0 &&
				      v92_analog_equal(
					      &event.peer_info0.v92_analog,
					      &analog),
				      "V.92 repeat changed retained capabilities");
			if (!repeated &&
			    (test_case == TEST_TONE_REPEAT_AFTER_COMPLETION ||
			     test_case == TEST_TONE_REJECT_AFTER_COMPLETION)) {
				CHECK(!repeated &&
				      phase2_info0_exchange_restart_info(
					      &peer, repeat_payload,
					      digital_local ?
						      V90_INFO0A_PAYLOAD_BITS :
						      V90_INFO0D_PAYLOAD_BITS) ==
					      PHASE2_INFO0_EXCHANGE_OK,
				      "post-completion repeat did not start for %d/%d/%d/%d",
				      mode, codec, test_case, digital_local);
				repeated = true;
				continue;
			}
			completed = index;
			break;
		}
	}
	CHECK(first_completion != SIZE_MAX,
	      "following tone did not initially complete INFO0 for %d/%d/%d",
	      mode, codec, test_case);
	if (test_case == TEST_TONE_DURING_ACK)
		CHECK(received_records == 1 && completed != SIZE_MAX &&
		      exchange.state == PCM_PHASE2_INFO0_COMPLETE &&
		      exchange.fresh_peer_tone_seen &&
		      acknowledge_starts == 1 && wait_starts == 0 &&
		      tone_retained && tone_during_ack,
		      "following tone was not retained during ACK for %d/%d",
		      mode, codec);
	else if (test_case == TEST_TONE_AFTER_ACK)
		CHECK(received_records == 1 && completed != SIZE_MAX &&
		      exchange.state == PCM_PHASE2_INFO0_COMPLETE &&
		      exchange.fresh_peer_tone_seen &&
		      acknowledge_starts == 1 && wait_starts == 1 &&
		      final_wait != SIZE_MAX &&
		      completed - final_wait ==
			      V34_PHASE2_TONE_WINDOW_SAMPLES +
			      V34_PHASE2_TONE_ACQUIRE_SAMPLES - 1u,
		      "post-ACK tone timing changed for %d/%d: %zu",
		      mode, codec, completed - final_wait);
	else if (test_case == TEST_TONE_INVALIDATED_BY_REPEAT)
		CHECK(received_records == 1 && completed != SIZE_MAX &&
		      exchange.state == PCM_PHASE2_INFO0_COMPLETE &&
		      exchange.fresh_peer_tone_seen && repeated &&
		      acknowledge_starts == 2 &&
		      tone_during_ack && tone_invalidated &&
		      fresh_tone_after_repeat &&
		      (digital_local || overlapping_repeat),
		      "peer repeat did not invalidate older tone for %d/%d/%d: ACK %zu invalid %d fresh %d overlap %d",
		      mode, codec, digital_local, acknowledge_starts,
		      tone_invalidated, fresh_tone_after_repeat,
		      overlapping_repeat);
	else if (test_case == TEST_TONE_REPEAT_AFTER_COMPLETION)
		CHECK(received_records == 2 && completed != SIZE_MAX &&
		      reopened_after_completion && acknowledge_starts == 2 &&
		      exchange.state == PCM_PHASE2_INFO0_COMPLETE &&
		      exchange.fresh_peer_tone_seen,
		      "valid completed-state repeat did not reopen/recover for %d/%d/%d",
		      mode, codec, digital_local);
	else
		CHECK(received_records == 1 && completed != SIZE_MAX &&
		      rejected_after_completion &&
		      !reopened_after_completion && acknowledge_starts == 1 &&
		      exchange.state == PCM_PHASE2_INFO0_COMPLETE &&
		      exchange.fresh_peer_tone_seen,
		      "rejected completed-state repeat mutated recovery for %d/%d/%d",
		      mode, codec, digital_local);
	return true;
}

static bool test_info0_following_tone_orderings(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum test_info0_mode mode = TEST_INFO0_V90;
		     mode <= TEST_INFO0_V92_FULL; ++mode)
			for (enum following_tone_case test_case =
				     TEST_TONE_DURING_ACK;
			     test_case <= TEST_TONE_REJECT_AFTER_COMPLETION;
			     ++test_case) {
				unsigned int roles = test_case >=
					TEST_TONE_INVALIDATED_BY_REPEAT ? 2u : 1u;

				for (unsigned int role = 0; role < roles; ++role)
					CHECK(check_following_tone(
						      codec, mode, test_case,
						      role != 0),
					      "INFO0 tone ordering failed for %d/%d/%d/%u",
					      mode, codec, test_case, role);
			}
	return true;
}

static bool test_retrain_record_is_immutable(void)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d retained =
		digital_capabilities(TEST_CODEC_PCMU);
	struct v90_info0d changed = retained;
	struct pcm_phase2_info0_exchange exchange;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];
	int16_t peer_line = 0;
	bool rejected = false;

	changed.nominal_transmit_power++;
	changed.common.acknowledges_peer_info0 = false;
	CHECK(pcm_phase2_info0_exchange_init_v90_analog(
		      &exchange, TEST_AMPLITUDE, &analog) ==
		      PCM_PHASE2_INFO0_EXCHANGE_OK &&
	      v90_info0d_encode(&changed, payload, sizeof(payload)) ==
		      V90_INFO0_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_B, TEST_AMPLITUDE,
		      payload, V90_INFO0D_PAYLOAD_BITS,
		      V90_INFO0A_PAYLOAD_BITS) == PHASE2_INFO0_EXCHANGE_OK,
	      "retrain record fixtures did not initialize");
	exchange.peer_info0.digital = retained;
	exchange.state = PCM_PHASE2_INFO0_RETRAIN;
	CHECK(phase2_info0_exchange_start_tone(&exchange.transport) ==
		      PHASE2_INFO0_EXCHANGE_OK &&
	      pcm_phase2_info0_exchange_complete(&exchange),
	      "retained INFO0 did not initialize explicit retrain state");
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct pcm_phase2_info0_exchange_event event;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      pcm_phase2_info0_exchange_sample(
			      &exchange,
			      codec_round_trip(peer_line, TEST_CODEC_PCMU),
			      &ignored, &event) == PCM_PHASE2_INFO0_EXCHANGE_OK,
		      "retrain mismatch injection failed at sample %zu", index);
		if (event.peer_info0_event == PCM_PEER_INFO0_REJECTED) {
			rejected = true;
			break;
		}
		CHECK(event.peer_info0_event != PCM_PEER_INFO0_RECEIVED,
		      "retrain INFO0 unexpectedly replaced its retained record");
	}
	CHECK(rejected && exchange.state == PCM_PHASE2_INFO0_RETRAIN &&
	      pcm_phase2_info0_exchange_complete(&exchange) &&
	      digital_equal(&exchange.peer_info0.digital, &retained),
	      "mismatched repeat changed terminal retrain state");
	return true;
}

static bool test_validation(void)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(TEST_CODEC_PCMU);
	struct v92_info0a v92_analog = {
		.v90 = analog,
	};
	struct pcm_phase2_info0_exchange exchange;
	struct pcm_phase2_info0_exchange before;
	struct pcm_phase2_info0_exchange_event event = {
		.peer_info0_event = PCM_PEER_INFO0_REJECTED,
	};
	int16_t sample = 1234;

	analog.maximum_symbol_rate_difference_steps = 6;
	CHECK(pcm_phase2_info0_exchange_init_v90_analog(
		      &exchange, TEST_AMPLITUDE, &analog) ==
		      PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
	      "V.90 INFO0 exchange accepted invalid analog capabilities");
	digital.pcm_law = (enum v90_pcm_law)2;
	CHECK(pcm_phase2_info0_exchange_init_v90_digital(
		      &exchange, TEST_AMPLITUDE, &digital) ==
		      PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
	      "V.90 INFO0 exchange accepted an invalid PCM law");
	analog = analog_capabilities();
	v92_analog.v90 = analog;
	CHECK(pcm_phase2_info0_exchange_init_v92_analog(
		      &exchange, TEST_AMPLITUDE, &v92_analog) ==
		      PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT,
	      "V.92 exchange accepted a local record without capability");
	v92_analog.v92_capable = true;
	CHECK(pcm_phase2_info0_exchange_init_v92_analog(
		      &exchange, TEST_AMPLITUDE, &v92_analog) ==
		      PCM_PHASE2_INFO0_EXCHANGE_OK,
	      "V.92 validation fixture did not initialize");
	exchange.protocol = (enum pcm_protocol)9;
	before = exchange;
	CHECK(pcm_phase2_info0_exchange_sample(
		      &exchange, 0, &sample, &event) ==
		      PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
	      sample == 1234 &&
	      event.peer_info0_event == PCM_PEER_INFO0_REJECTED,
	      "invalid PCM protocol tag mutated caller state");
	CHECK(pcm_phase2_info0_exchange_init_v90_analog(
		      &exchange, TEST_AMPLITUDE, &analog) ==
		      PCM_PHASE2_INFO0_EXCHANGE_OK,
	      "V.90 validation fixture did not initialize");
	exchange.transport.info_receiver.modulation.detector_output = NAN;
	before = exchange;
	CHECK(pcm_phase2_info0_exchange_sample(
		      &exchange, 0, &sample, &event) ==
		      PCM_PHASE2_INFO0_EXCHANGE_INVALID_ARGUMENT &&
	      memcmp(&exchange, &before, sizeof(exchange)) == 0 &&
	      sample == 1234 &&
	      event.peer_info0_event == PCM_PEER_INFO0_REJECTED &&
	      strcmp(pcm_phase2_info0_exchange_result_name(
		     PCM_PHASE2_INFO0_EXCHANGE_OK), "ok") == 0 &&
	      strcmp(pcm_phase2_info0_exchange_result_name(
		     (enum pcm_phase2_info0_exchange_result)9), "unknown") == 0,
	      "invalid V.90 INFO0 state mutated outputs or result names");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "protocol, codec, and capability matrix", test_pcm_exchange },
		{ "dropped first record recovery", test_dropped_initial_info0 },
		{ "INFO0 following-tone orderings",
		  test_info0_following_tone_orderings },
		{ "retrain record is immutable", test_retrain_record_is_immutable },
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
