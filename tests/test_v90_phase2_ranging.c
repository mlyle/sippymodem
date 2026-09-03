// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase2_ranging.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_LINE_DELAY_SAMPLES = 13,
	TEST_SAMPLE_LIMIT = 6000,
	TEST_EDGE_TOLERANCE_SAMPLES = 4,
	RETRAIN_PRIOR_RTD_SAMPLES = 2967,
	RETRAIN_EARLY_EDGE_ADVANCE_SAMPLES = 50,
};

enum test_codec {
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_delay_line {
	int16_t samples[TEST_LINE_DELAY_SAMPLES];
	size_t cursor;
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

static size_t absolute_difference(size_t left, size_t right)
{
	return left > right ? left - right : right - left;
}

static bool check_ranging(enum test_codec codec)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(codec);
	struct v90_phase2_ranging analog_end;
	struct v90_phase2_ranging digital_end;
	struct test_delay_line analog_to_digital = { 0 };
	struct test_delay_line digital_to_analog = { 0 };
	uint16_t analog_delay = 0;
	uint16_t digital_delay = 0;
	bool analog_ready = false;
	bool digital_ready = false;
	bool analog_received_info = false;
	bool digital_received_info = false;
	size_t analog_tone_handoffs = 0;
	size_t analog_ack_tone_start = SIZE_MAX;
	size_t analog_first_reversal = SIZE_MAX;

	CHECK(v90_phase2_ranging_init_analog(
		      &analog_end, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_RANGING_OK &&
	      v90_phase2_ranging_init_digital(
		      &digital_end, TEST_AMPLITUDE, &digital) ==
		      V90_PHASE2_RANGING_OK &&
	      analog_end.info0_exchange.transport.signal_role ==
		      PHASE2_SIGNAL_A &&
	      digital_end.info0_exchange.transport.signal_role ==
		      PHASE2_SIGNAL_B,
	      "V.90 ranging endpoints did not initialize");

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct v90_phase2_ranging_event analog_event;
		struct v90_phase2_ranging_event digital_event;
		enum v90_phase2_ranging_result analog_result;
		enum v90_phase2_ranging_result digital_result;
		enum phase2_info0_transmit_phase analog_transmit_before =
			analog_end.info0_exchange.transport.transmit_phase;
		enum phase2_ranging_phase analog_phase_before =
			analog_end.control.phase;
		int16_t analog_transmit;
		int16_t digital_transmit;

		analog_result = v90_phase2_ranging_sample(
			&analog_end, delay_line_receive(&digital_to_analog),
			&analog_transmit, &analog_event);
		digital_result = v90_phase2_ranging_sample(
			&digital_end, delay_line_receive(&analog_to_digital),
			&digital_transmit, &digital_event);
		CHECK(analog_result == V90_PHASE2_RANGING_OK &&
		      digital_result == V90_PHASE2_RANGING_OK,
		      "V.90 ranging failed for codec %d at sample %zu (analog %d digital %d)",
		      codec, index, analog_result, digital_result);
		delay_line_transmit(
			&analog_to_digital,
			codec_round_trip(analog_transmit, codec));
		delay_line_transmit(
			&digital_to_analog,
			codec_round_trip(digital_transmit, codec));
		if (analog_transmit_before == PHASE2_INFO0_CARRIER_TAIL &&
		    analog_end.info0_exchange.transport.transmit_phase ==
			    PHASE2_INFO0_TONE &&
		    ++analog_tone_handoffs == 2u)
			analog_ack_tone_start = index + 1u;
		if (analog_phase_before == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		    analog_end.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
			analog_first_reversal = index;

		if (analog_event.exchange_event.peer_info0_event ==
		    PCM_PEER_INFO0_RECEIVED) {
			analog_received_info =
				analog_event.exchange_event.peer_info0.digital
					.nominal_transmit_power ==
				digital.nominal_transmit_power;
		}
		if (digital_event.exchange_event.peer_info0_event ==
		    PCM_PEER_INFO0_RECEIVED) {
			digital_received_info =
				digital_event.exchange_event.peer_info0.analog
					.transmit_power_reduction_supported ==
				analog.transmit_power_reduction_supported;
		}
		if (analog_event.kind == PHASE2_PROBE_TRANSMIT_READY) {
			analog_ready = true;
			analog_delay = analog_event.round_trip_delay_samples;
		}
		if (digital_event.kind == PHASE2_PROBE_RECEIVE_READY) {
			digital_ready = true;
			digital_delay = digital_event.round_trip_delay_samples;
		}
		if (analog_end.control.phase == PHASE2_RANGING_PROBE_READY &&
		    digital_end.control.phase == PHASE2_RANGING_PROBE_READY)
			break;
	}

	CHECK(analog_end.control.phase == PHASE2_RANGING_PROBE_READY &&
	      digital_end.control.phase == PHASE2_RANGING_PROBE_READY &&
	      analog_received_info && digital_received_info &&
	      analog_ready && digital_ready,
	      "V.90 ranging did not reach both typed probe boundaries");
	CHECK(absolute_difference(
		      analog_delay, 2u * TEST_LINE_DELAY_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES &&
	      absolute_difference(
		      digital_delay, 2u * TEST_LINE_DELAY_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "V.90 round-trip estimates changed for codec %d: analog %u digital %u",
	      codec, analog_delay, digital_delay);
	CHECK(analog_ack_tone_start != SIZE_MAX &&
	      analog_first_reversal != SIZE_MAX &&
	      analog_first_reversal - analog_ack_tone_start ==
		      PHASE2_RANGING_INITIAL_TONE_SAMPLES,
	      "repeated INFO0 did not restart the 50 ms Tone A epoch for codec %d: %zu/%zu",
	      codec, analog_ack_tone_start, analog_first_reversal);
	return true;
}

static bool test_pcm_ranging(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_ranging(codec),
		      "V.90 ranging failed for codec %d", codec);
	return true;
}

static enum pcm_phase2_info0_exchange_result initialize_info0_exchange(
	struct pcm_phase2_info0_exchange *exchange,
	enum v90_modem_role role, enum pcm_protocol protocol,
	enum test_codec codec)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(codec);

	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			pcm_phase2_info0_exchange_init_v90_analog(
				exchange, TEST_AMPLITUDE, &analog) :
			pcm_phase2_info0_exchange_init_v90_digital(
				exchange, TEST_AMPLITUDE, &digital);
	if (role == V90_ANALOG_MODEM) {
		struct v92_info0a v92 = { .v90 = analog, .v92_capable = true };

		return pcm_phase2_info0_exchange_init_v92_analog(
			exchange, TEST_AMPLITUDE, &v92);
	}
	{
		struct v92_info0d v92 = { .v90 = digital, .v92_capable = true };

		return pcm_phase2_info0_exchange_init_v92_digital(
			exchange, TEST_AMPLITUDE, &v92);
	}
}

static bool encode_peer_info0(
	uint8_t *payload,
	enum v90_modem_role role, enum pcm_protocol protocol,
	enum test_codec codec, bool acknowledge)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(codec);

	if (role == V90_ANALOG_MODEM) {
		struct v92_info0a v92 = {
			.v90 = analog,
			.v92_capable = true,
		};

		v92.v90.acknowledges_peer_info0 = acknowledge;
		return protocol == PCM_PROTOCOL_V90 ?
			v90_info0a_encode(
				&v92.v90, payload,
				V90_INFO0D_PAYLOAD_BYTES) == V90_INFO0_OK :
			v92_info0a_encode(
				&v92, payload,
				V90_INFO0D_PAYLOAD_BYTES) == V92_INFO0_OK;
	}
	{
		struct v92_info0d v92 = {
			.v90 = digital,
			.v92_capable = true,
		};

		v92.v90.common.acknowledges_peer_info0 = acknowledge;
		return protocol == PCM_PROTOCOL_V90 ?
			v90_info0d_encode(
				&v92.v90, payload,
				V90_INFO0D_PAYLOAD_BYTES) == V90_INFO0_OK :
			v92_info0d_encode(
				&v92, payload,
				V90_INFO0D_PAYLOAD_BYTES) == V92_INFO0_OK;
	}
}

static bool initialize_interrupted_ranging(
	struct v90_phase2_ranging *ranging,
	struct phase2_info0_exchange *peer,
	enum v90_modem_role local_role, enum pcm_protocol protocol,
	enum test_codec codec, uint8_t *peer_payload,
	size_t *peer_payload_bits)
{
	enum v90_modem_role peer_role = local_role == V90_ANALOG_MODEM ?
		V90_DIGITAL_MODEM : V90_ANALOG_MODEM;
	size_t expected_bits = local_role == V90_ANALOG_MODEM ?
		V90_INFO0A_PAYLOAD_BITS : V90_INFO0D_PAYLOAD_BITS;

	*peer_payload_bits = peer_role == V90_ANALOG_MODEM ?
		V90_INFO0A_PAYLOAD_BITS : V90_INFO0D_PAYLOAD_BITS;
	return initialize_info0_exchange(
		       &ranging->info0_exchange, local_role, protocol, codec) ==
		       PCM_PHASE2_INFO0_EXCHANGE_OK &&
	       encode_peer_info0(
		       peer_payload, peer_role, protocol, codec, false) &&
	       phase2_ranging_control_init(
		       &ranging->control,
		       &ranging->info0_exchange.transport) == PHASE2_RANGING_OK &&
	       phase2_info0_exchange_init(
		       peer,
		       peer_role == V90_ANALOG_MODEM ?
			       PHASE2_SIGNAL_A : PHASE2_SIGNAL_B,
		       TEST_AMPLITUDE, peer_payload, *peer_payload_bits,
		       expected_bits) == PHASE2_INFO0_EXCHANGE_OK &&
	       v90_phase2_ranging_is_valid(ranging);
}

static bool check_info_interrupted_reversal(
	enum test_codec codec, enum pcm_protocol protocol,
	enum v90_modem_role local_role)
{
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	size_t payload_bits = 0;
	size_t edge_sample = SIZE_MAX;
	size_t local_reversal = SIZE_MAX;
	bool repeat_started = false;
	bool repeat_synchronized = false;
	bool ranging_wait_preserved = true;
	bool peer_reversed = false;

	CHECK(initialize_interrupted_ranging(
		      &ranging, &peer, local_role, protocol, codec,
		      payload, &payload_bits),
	      "interrupted ranging fixture did not initialize");
	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		enum v34_phase2_info_receive_stage receive_stage_before =
			ranging.info0_exchange.transport.info_receiver.frame.stage;
		int16_t peer_line;
		int16_t ignored;

		if (repeat_started && !peer_reversed &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) == V34_PHASE2_TONE_OK,
			      "peer following tone did not reverse");
			peer_reversed = true;
			edge_sample = index;
		}
		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V90_PHASE2_RANGING_OK,
		      "interrupted ranging failed at %zu", index);
		if (local_role == V90_DIGITAL_MODEM && repeat_started &&
		    !peer_reversed) {
			repeat_synchronized |=
				receive_stage_before == V34_PHASE2_INFO_SEARCH_SYNC &&
				ranging.info0_exchange.transport.info_receiver.frame.stage !=
					V34_PHASE2_INFO_SEARCH_SYNC;
			ranging_wait_preserved &= ranging.control.phase ==
				PHASE2_WAIT_FIRST_REMOTE_REVERSAL;
		}
		if (phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL) {
			local_reversal = index;
			break;
		}
		if (!repeat_started &&
		    pcm_phase2_info0_exchange_complete(
			    &ranging.info0_exchange) &&
		    ranging.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			enum v90_modem_role peer_role = local_role == V90_ANALOG_MODEM ?
				V90_DIGITAL_MODEM : V90_ANALOG_MODEM;

			CHECK(encode_peer_info0(
				      payload, peer_role, protocol, codec, true) &&
			      phase2_info0_exchange_restart_info(
				      &peer, payload, payload_bits) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "peer repeated INFO did not start");
			repeat_started = true;
		}
	}
	CHECK(repeat_started && peer_reversed && edge_sample != SIZE_MAX &&
	      local_reversal != SIZE_MAX &&
	      (local_role != V90_DIGITAL_MODEM ||
	       (repeat_synchronized && ranging_wait_preserved)) &&
	      absolute_difference(
		      local_reversal,
		      edge_sample + PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "INFO-interrupted reversal timing failed for %d/%d/%d at %zu/%zu",
	      codec, protocol, local_role, edge_sample, local_reversal);
	return true;
}

static bool test_info_interrupted_reversal(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			for (enum v90_modem_role role = V90_ANALOG_MODEM;
			     role <= V90_DIGITAL_MODEM; ++role)
				CHECK(check_info_interrupted_reversal(
					      codec, protocol, role),
				      "INFO interruption failed for %d/%d/%d",
				      codec, protocol, role);
	return true;
}

static bool check_response_during_repeat(
	enum test_codec codec, enum pcm_protocol protocol)
{
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	size_t payload_bits = 0;
	int16_t local_line = 0;
	int16_t peer_line = 0;
	bool edge_started = false;
	bool repeat_started = false;
	bool response_scheduled_during_info = false;
	bool measurement_advanced_during_info = false;

	CHECK(initialize_interrupted_ranging(
		      &ranging, &peer, V90_DIGITAL_MODEM, protocol, codec,
		      payload, &payload_bits),
	      "response-during-repeat fixture did not initialize");
	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum phase2_info0_transmit_phase transmit_before =
			ranging.info0_exchange.transport.transmit_phase;
		enum phase2_ranging_phase phase_before = ranging.control.phase;

		CHECK(phase2_info0_exchange_sample(
			      &peer, codec_round_trip(local_line, codec),
			      &peer_line, &peer_event) == PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &local_line, &event) == V90_PHASE2_RANGING_OK,
		      "response-during-repeat failed at %zu for %d/%d",
		      index, codec, protocol);
		if (!edge_started &&
		    pcm_phase2_info0_exchange_complete(
			    &ranging.info0_exchange) &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "peer first reversal did not start");
			edge_started = true;
		}
		if (edge_started && !repeat_started &&
		    phase_before == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    ranging.control.phase == PHASE2_RESPONSE_DELAY) {
			CHECK(encode_peer_info0(
				      payload, V90_ANALOG_MODEM, protocol,
				      codec, true) &&
			      phase2_info0_exchange_restart_info(
				      &peer, payload, payload_bits) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "peer INFO repeat did not interrupt response");
			repeat_started = true;
		}
		if (repeat_started && phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL)
			response_scheduled_during_info =
				ranging.info0_exchange.transport.transmit_phase !=
				PHASE2_INFO0_TONE;
		if (ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL &&
		    ranging.info0_exchange.transport.transmit_phase !=
			    PHASE2_INFO0_TONE) {
			CHECK(ranging.control.post_reversal_samples == 0,
			      "INFO record consumed the physical Tone-B tail");
			measurement_advanced_during_info |=
				ranging.control.measurement_elapsed_samples != 0;
		}
		if (ranging.control.phase ==
			    PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL) {
			CHECK(ranging.control.post_reversal_samples ==
				      PHASE2_RANGING_TONE_TAIL_SAMPLES &&
			      ranging.control.measurement_elapsed_samples >
				      ranging.control.post_reversal_samples &&
			      transmit_before == PHASE2_INFO0_TONE,
			      "recovered Tone-B tail did not finish exactly");
			break;
		}
	}
	CHECK(edge_started && repeat_started &&
	      response_scheduled_during_info &&
	      measurement_advanced_during_info &&
	      ranging.control.phase ==
		      PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL,
	      "INFO repeat did not preserve scheduled response for %d/%d: edge=%d repeat=%d scheduled=%d measured=%d phase=%d",
	      codec, protocol, edge_started, repeat_started,
	      response_scheduled_during_info,
	      measurement_advanced_during_info,
	      ranging.control.phase);
	return true;
}

static bool test_response_during_repeat(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			CHECK(check_response_during_repeat(codec, protocol),
			      "response-during-repeat failed for %d/%d",
			      codec, protocol);
	return true;
}

static bool check_deferred_first_reversal(
	enum test_codec codec, enum pcm_protocol protocol)
{
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	size_t local_frame_bits = v34_phase2_info_frame_bit_count(
		V90_INFO0D_PAYLOAD_BITS);
	size_t edge_sample = SIZE_MAX;
	size_t response_sample = SIZE_MAX;
	bool acknowledging_record_started = false;
	bool edge_deferred = false;
	enum phase2_info0_transmit_phase last_transmit_phase =
		PHASE2_INFO0_POST_V8_SILENCE;

	CHECK(initialize_info0_exchange(
		      &ranging.info0_exchange, V90_DIGITAL_MODEM,
		      protocol, codec) == PCM_PHASE2_INFO0_EXCHANGE_OK &&
	      encode_peer_info0(
		      payload, V90_ANALOG_MODEM, protocol, codec, true) &&
	      phase2_ranging_control_init(
		      &ranging.control,
		      &ranging.info0_exchange.transport) == PHASE2_RANGING_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_A, TEST_AMPLITUDE,
		      payload, V90_INFO0A_PAYLOAD_BITS,
		      V90_INFO0D_PAYLOAD_BITS) == PHASE2_INFO0_EXCHANGE_OK &&
	      v90_phase2_ranging_is_valid(&ranging),
	      "deferred-reversal fixture did not initialize for %d/%d",
	      codec, protocol);

	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		const struct v34_phase2_info_generator *local_frame =
			&ranging.info0_exchange.transport.info_transmitter.frame;
		int16_t peer_line;
		int16_t ignored;

		if (acknowledging_record_started && edge_sample == SIZE_MAX &&
		    ranging.info0_exchange.state ==
			    PCM_PHASE2_INFO0_ACKNOWLEDGING &&
		    ranging.info0_exchange.transport.transmit_phase ==
			    PHASE2_INFO0_SEQUENCE &&
		    local_frame->bit_index + 2u >= local_frame_bits &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "deferred peer reversal failed for %d/%d",
			      codec, protocol);
			edge_sample = index;
		}
		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V90_PHASE2_RANGING_OK,
		      "deferred reversal failed at %zu for %d/%d",
		      index, codec, protocol);
		if (last_transmit_phase == PHASE2_INFO0_CARRIER_TAIL &&
		    ranging.info0_exchange.transport.transmit_phase ==
			    PHASE2_INFO0_SEQUENCE)
			acknowledging_record_started = true;
		last_transmit_phase =
			ranging.info0_exchange.transport.transmit_phase;
		if (event.exchange_event.tone_event.kind ==
			    V34_PHASE2_TONE_PHASE_REVERSED &&
		    ranging.control.deferred_reversal_samples != 0)
			edge_deferred = true;
		if (phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL) {
			response_sample = index;
			break;
		}
	}
	CHECK(acknowledging_record_started && edge_sample != SIZE_MAX &&
	      edge_deferred,
	      "acknowledging tail did not retain reversal for %d/%d",
	      codec, protocol);
	CHECK(response_sample != SIZE_MAX,
	      "deferred reversal did not schedule a response for %d/%d: phase=%d tx=%d",
	      codec, protocol, ranging.control.phase,
	      ranging.info0_exchange.transport.transmit_phase);
	CHECK(absolute_difference(
		      response_sample,
		      edge_sample + PHASE2_RANGING_TURNAROUND_SAMPLES) <=
		      TEST_EDGE_TOLERANCE_SAMPLES,
	      "deferred reversal timing failed for %d/%d at %zu/%zu",
	      codec, protocol, edge_sample, response_sample);
	return true;
}

static bool test_deferred_first_reversal(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			CHECK(check_deferred_first_reversal(codec, protocol),
			      "deferred reversal failed for %d/%d",
			      codec, protocol);
	return true;
}

static bool check_retrain_early_second_reversal(
	uint16_t prior_round_trip_delay_samples,
	uint16_t edge_advance_samples, bool expect_acceptance)
{
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	size_t payload_bits = 0;
	int16_t local_line = 0;
	uint16_t observed_edge_samples = UINT16_MAX;
	bool first_reversed = false;
	bool local_response_started = false;
	bool second_reversed = false;
	bool second_observed = false;

	CHECK(initialize_interrupted_ranging(
		      &ranging, &peer, V90_DIGITAL_MODEM,
		      PCM_PROTOCOL_V90, TEST_CODEC_PCMU,
		      payload, &payload_bits),
	      "retrain early-edge fixture did not initialize");
	ranging.control.prior_round_trip_delay_samples =
		prior_round_trip_delay_samples;
	CHECK(v90_phase2_ranging_is_valid(&ranging),
	      "retrain prior RTD made ranging invalid");

	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		uint16_t measurement_before =
			ranging.control.measurement_elapsed_samples;
		int16_t peer_line;

		if (!first_reversed &&
		    pcm_phase2_info0_exchange_complete(
			    &ranging.info0_exchange) &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "retrain peer first reversal did not start");
			first_reversed = true;
		}
		if (local_response_started && !second_reversed &&
		    ranging.control.measurement_elapsed_samples >=
			    PHASE2_RANGING_TURNAROUND_SAMPLES -
				    edge_advance_samples) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "retrain peer early second reversal did not start");
			second_reversed = true;
		}
		CHECK(phase2_info0_exchange_sample(
			      &peer, codec_round_trip(local_line, TEST_CODEC_PCMU),
			      &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging,
			      codec_round_trip(peer_line, TEST_CODEC_PCMU),
			      &local_line, &event) == V90_PHASE2_RANGING_OK,
		      "retrain early-edge ranging failed at %zu", index);
		if (phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL)
			local_response_started = true;
		if (second_reversed &&
		    event.exchange_event.tone_event.kind ==
			    V34_PHASE2_TONE_PHASE_REVERSED) {
			if (measurement_before >= event.exchange_event.tone_event
						 .reversal_delay_samples)
				observed_edge_samples = measurement_before -
					event.exchange_event.tone_event
						.reversal_delay_samples;
			second_observed = true;
			break;
		}
	}

	CHECK(first_reversed && local_response_started && second_reversed &&
	      second_observed && observed_edge_samples != UINT16_MAX &&
	      absolute_difference(
		      observed_edge_samples,
		      PHASE2_RANGING_TURNAROUND_SAMPLES -
			      edge_advance_samples) <= TEST_EDGE_TOLERANCE_SAMPLES,
	      "retrain early-edge fixture did not reach its discriminator: edge=%u",
	      observed_edge_samples);
	if (expect_acceptance)
		CHECK(ranging.control.round_trip_delay_valid &&
		      ranging.control.round_trip_delay_samples ==
			      prior_round_trip_delay_samples &&
		      (ranging.control.phase ==
			       PHASE2_SIGNAL_B_WAIT_PROBE_START ||
		       ranging.control.phase == PHASE2_RANGING_PROBE_READY) &&
		      !ranging.info0_exchange.transport.tone_detector.carrier
			       .rejected_reversal_active,
		      "retrain early edge did not retain prior RTD: phase=%d delay=%u",
		      ranging.control.phase,
		      ranging.control.round_trip_delay_samples);
	else
		CHECK(!ranging.control.round_trip_delay_valid &&
		      ranging.control.phase ==
			      PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL &&
		      ranging.info0_exchange.transport.tone_detector.carrier
			      .rejected_reversal_active,
		      "unqualified early edge escaped rejection: phase=%d delay=%u",
		      ranging.control.phase,
		      ranging.control.round_trip_delay_samples);
	return true;
}

static bool test_retrain_early_second_reversal(void)
{
	CHECK(check_retrain_early_second_reversal(
		      0u, RETRAIN_EARLY_EDGE_ADVANCE_SAMPLES, false),
	      "initial ranging accepted a captured early edge");
	CHECK(check_retrain_early_second_reversal(
		      RETRAIN_PRIOR_RTD_SAMPLES,
		      RETRAIN_EARLY_EDGE_ADVANCE_SAMPLES, true),
	      "retrain did not recover a captured early edge");
	CHECK(check_retrain_early_second_reversal(
		      RETRAIN_PRIOR_RTD_SAMPLES,
		      PHASE2_RANGING_TONE_TAIL_SAMPLES + 1u, false),
	      "retrain accepted an edge before its bounded recovery window");
	return true;
}

static bool test_second_reversal_timeout_recovery(void)
{
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	size_t payload_bits = 0;
	int16_t local_line = 0;
	size_t recovery_tone_samples = 0;
	size_t timeout_count = 0;
	bool first_reversed = false;
	bool first_response_started = false;
	bool recovery_started = false;
	bool recovery_boundary_was_silent = false;
	bool recovery_tone_transmitted = false;
	bool retry_first_reversed = false;
	bool retry_first_accepted = false;
	bool retry_response_started = false;
	bool retry_second_reversed = false;
	bool probe_ready = false;

	CHECK(initialize_interrupted_ranging(
		      &ranging, &peer, V90_DIGITAL_MODEM,
		      PCM_PROTOCOL_V90, TEST_CODEC_PCMU,
		      payload, &payload_bits),
	      "second-reversal recovery fixture did not initialize");
	ranging.control.prior_round_trip_delay_samples =
		RETRAIN_PRIOR_RTD_SAMPLES;
	CHECK(v90_phase2_ranging_is_valid(&ranging),
	      "second-reversal recovery prior RTD was invalid");

	for (size_t index = 0;
	     index < PHASE2_RANGING_MAXIMUM_WAIT_SAMPLES +
			     2u * TEST_SAMPLE_LIMIT;
	     ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		int16_t peer_line;

		if (!first_reversed &&
		    pcm_phase2_info0_exchange_complete(
			    &ranging.info0_exchange) &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    peer.transmit_phase == PHASE2_INFO0_TONE) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "initial peer reversal did not start");
			first_reversed = true;
		}
		if (recovery_started && !retry_first_reversed &&
		    recovery_tone_samples >=
			    2u * V34_PHASE2_TONE_WINDOW_SAMPLES) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "recovery peer first reversal did not start");
			retry_first_reversed = true;
		}
		if (retry_response_started && !retry_second_reversed &&
		    ranging.control.measurement_elapsed_samples >=
			    PHASE2_RANGING_TURNAROUND_SAMPLES) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "recovery peer second reversal did not start");
			retry_second_reversed = true;
		}
		CHECK(phase2_info0_exchange_sample(
			      &peer,
			      codec_round_trip(local_line, TEST_CODEC_PCMU),
			      &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging,
			      codec_round_trip(peer_line, TEST_CODEC_PCMU),
			      &local_line, &event) ==
			      V90_PHASE2_RANGING_OK,
		      "second-reversal recovery failed at %zu", index);
		if (event.kind == PHASE2_RANGING_TIMEOUT)
			timeout_count++;
		if (!first_response_started &&
		    phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL)
			first_response_started = true;
		if (first_response_started && !recovery_started &&
		    phase_before ==
			    PHASE2_SIGNAL_B_WAIT_SECOND_REMOTE_REVERSAL &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			recovery_started = true;
			recovery_boundary_was_silent = local_line == 0;
			CHECK(ranging.control.prior_round_trip_delay_samples ==
				      RETRAIN_PRIOR_RTD_SAMPLES &&
			      !ranging.control.round_trip_delay_valid,
			      "recovery discarded prior RTD context");
		}
		if (recovery_started && !retry_first_reversed &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			recovery_tone_samples++;
			recovery_tone_transmitted |= local_line != 0;
		}
		if (recovery_started && retry_first_reversed &&
		    phase_before == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    ranging.control.phase == PHASE2_RESPONSE_DELAY)
			retry_first_accepted = true;
		if (recovery_started &&
		    phase_before == PHASE2_RESPONSE_DELAY &&
		    ranging.control.phase == PHASE2_RESPONSE_TONE_TAIL)
			retry_response_started = true;
		if (event.kind == PHASE2_PROBE_RECEIVE_READY) {
			probe_ready = true;
			break;
		}
	}

	CHECK(first_reversed && first_response_started && recovery_started &&
	      recovery_boundary_was_silent && recovery_tone_transmitted &&
	      retry_first_reversed && retry_first_accepted &&
	      retry_response_started && retry_second_reversed && probe_ready &&
	      timeout_count == 0 &&
	      ranging.control.phase == PHASE2_RANGING_PROBE_READY &&
	      ranging.control.round_trip_delay_valid &&
	      ranging.control.round_trip_delay_samples ==
		      RETRAIN_PRIOR_RTD_SAMPLES,
	      "second-reversal retry did not complete: first=%d response=%d "
	      "recovery=%d silent=%d tone=%d retry=%d/%d/%d/%d probe=%d "
	      "timeouts=%zu phase=%d delay=%u",
	      first_reversed, first_response_started, recovery_started,
	      recovery_boundary_was_silent, recovery_tone_transmitted,
	      retry_first_reversed, retry_first_accepted,
	      retry_response_started, retry_second_reversed, probe_ready,
	      timeout_count,
	      ranging.control.phase, ranging.control.round_trip_delay_samples);
	return true;
}

static bool check_rejected_edge_before_info(
	enum test_codec codec, enum pcm_protocol protocol)
{
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES] = { 0 };
	size_t payload_bits = 0;
	size_t stable_rejected_samples = 0;
	size_t reversal_events = 0;
	bool early_edge_started = false;
	bool repeat_started = false;
	bool info_synchronized = false;
	bool causal_edge_accepted = false;

	CHECK(initialize_interrupted_ranging(
		      &ranging, &peer, V90_ANALOG_MODEM, protocol, codec,
		      payload, &payload_bits),
	      "rejected-edge fixture did not initialize");
	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		int16_t peer_line;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V90_PHASE2_RANGING_OK,
		      "rejected-edge ranging failed at %zu for %d/%d",
		      index, codec, protocol);
		if (event.exchange_event.tone_event.kind ==
		    V34_PHASE2_TONE_PHASE_REVERSED) {
			reversal_events++;
			if (reversal_events == 1u)
				CHECK(ranging.control.phase ==
					      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
				      ranging.info0_exchange.transport.tone_detector
					      .carrier
					      .rejected_reversal_active,
				      "early edge was not rejected transactionally");
		}
		if (repeat_started &&
		    ranging.info0_exchange.transport.info_receiver.frame.stage !=
			    V34_PHASE2_INFO_SEARCH_SYNC) {
			info_synchronized = true;
			CHECK(!ranging.info0_exchange.transport.tone_detector
				       .carrier
				       .rejected_reversal_active,
			      "INFO synchronization retained the rejected-edge gate");
		}
		if (phase_before == PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
		    ranging.control.phase == PHASE2_RESPONSE_DELAY) {
			causal_edge_accepted = true;
			break;
		}
		if (reversal_events == 1u && !repeat_started) {
			stable_rejected_samples++;
			CHECK(ranging.control.phase ==
				      PHASE2_WAIT_FIRST_REMOTE_REVERSAL &&
			      reversal_events == 1u,
			      "stable rejected phase changed ranging state");
			if (stable_rejected_samples ==
			    2u * V34_PHASE2_TONE_WINDOW_SAMPLES) {
				CHECK(encode_peer_info0(
					      payload, V90_DIGITAL_MODEM,
					      protocol, codec, true) &&
				      phase2_info0_exchange_restart_info(
					      &peer, payload, payload_bits) ==
					      PHASE2_INFO0_EXCHANGE_OK,
				      "peer INFO repeat did not start");
				repeat_started = true;
			}
		}
		if (!early_edge_started &&
		    pcm_phase2_info0_exchange_complete(
			    &ranging.info0_exchange) &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			CHECK(v34_phase2_tone_transmitter_reverse(
				      &peer.tone_transmitter) ==
				      V34_PHASE2_TONE_OK,
			      "early peer edge did not start");
			early_edge_started = true;
		}
	}
	CHECK(early_edge_started && repeat_started && info_synchronized &&
	      stable_rejected_samples ==
		      2u * V34_PHASE2_TONE_WINDOW_SAMPLES &&
		      reversal_events == 2u && causal_edge_accepted &&
		      !ranging.info0_exchange.transport.tone_detector
		       .carrier
		       .rejected_reversal_active,
	      "rejected-edge recovery failed for %d/%d: edges=%zu stable=%zu sync=%d accepted=%d phase=%d",
	      codec, protocol, reversal_events, stable_rejected_samples,
	      info_synchronized, causal_edge_accepted, ranging.control.phase);
	return true;
}

static bool test_rejected_edge_before_info(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum pcm_protocol protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol)
			CHECK(check_rejected_edge_before_info(codec, protocol),
			      "rejected edge failed for %d/%d", codec, protocol);
	return true;
}

static bool check_completed_exchange_repeat(
	enum test_codec codec, bool reject_repeat)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_info0d digital = digital_capabilities(codec);
	struct v90_info0d repeated = digital;
	struct v90_phase2_ranging ranging;
	struct phase2_info0_exchange peer;
	uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];
	int16_t peer_line = 0;
	size_t first_completion = SIZE_MAX;
	size_t rejection = SIZE_MAX;
	size_t reversal = SIZE_MAX;
	size_t completed_records = 0;
	bool repeat_started = false;
	bool receiver_synchronized = false;
	bool acknowledgment_started_on_sync = false;
	bool reopened = false;
	bool reversed_during_record = false;

	digital.common.acknowledges_peer_info0 = false;
	if (reject_repeat)
		repeated.nominal_transmit_power++;
	CHECK(v90_phase2_ranging_init_analog(
		      &ranging, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_RANGING_OK &&
	      v90_info0d_encode(
		      &digital, payload, sizeof(payload)) == V90_INFO0_OK &&
	      phase2_info0_exchange_init(
		      &peer, PHASE2_SIGNAL_B, TEST_AMPLITUDE,
		      payload, V90_INFO0D_PAYLOAD_BITS,
		      V90_INFO0A_PAYLOAD_BITS) == PHASE2_INFO0_EXCHANGE_OK,
	      "completed-repeat ranging fixture did not initialize");

	for (size_t index = 0; index < 2u * TEST_SAMPLE_LIMIT; ++index) {
		struct phase2_info0_exchange_event peer_event;
		struct v90_phase2_ranging_event event;
		enum v34_phase2_info_receive_stage receive_stage_before =
			ranging.info0_exchange.transport.info_receiver.frame.stage;
		enum phase2_ranging_phase phase_before = ranging.control.phase;
		int16_t ignored;

		CHECK(phase2_info0_exchange_sample(
			      &peer, 0, &peer_line, &peer_event) ==
			      PHASE2_INFO0_EXCHANGE_OK &&
		      v90_phase2_ranging_sample(
			      &ranging, codec_round_trip(peer_line, codec),
			      &ignored, &event) == V90_PHASE2_RANGING_OK,
		      "completed-repeat ranging failed at %zu for %d/%d",
		      index, codec, reject_repeat);
		if (repeat_started &&
		    receive_stage_before == V34_PHASE2_INFO_SEARCH_SYNC &&
		    ranging.info0_exchange.transport.info_receiver.frame.stage !=
			    V34_PHASE2_INFO_SEARCH_SYNC) {
			receiver_synchronized = true;
			acknowledgment_started_on_sync =
				ranging.info0_exchange.state ==
					PCM_PHASE2_INFO0_VERIFY_REPEAT &&
				ranging.info0_exchange.transport.transmit_phase ==
					PHASE2_INFO0_SEQUENCE &&
				ranging.control.phase == phase_before;
		}
		if (receive_stage_before != V34_PHASE2_INFO_SEARCH_SYNC &&
		    phase_before == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		    ranging.control.phase ==
			    PHASE2_WAIT_FIRST_REMOTE_REVERSAL)
			reversed_during_record = true;
		if (event.exchange_event.peer_info0_event ==
		    PCM_PEER_INFO0_RECEIVED) {
			completed_records++;
			if (first_completion == SIZE_MAX)
				first_completion = index;
		}
		if (event.exchange_event.peer_info0_event ==
		    PCM_PEER_INFO0_REJECTED)
			rejection = index;
		reopened |= repeat_started &&
			!pcm_phase2_info0_exchange_complete(
				&ranging.info0_exchange);
		if (phase_before == PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		    ranging.control.phase == PHASE2_WAIT_FIRST_REMOTE_REVERSAL) {
			reversal = index;
			if ((!reject_repeat && completed_records == 2u) ||
			    (reject_repeat && rejection != SIZE_MAX))
				break;
		}
		if (!repeat_started && first_completion != SIZE_MAX &&
		    ranging.control.phase ==
			    PHASE2_SIGNAL_A_WAIT_INITIAL_TONE &&
		    ranging.control.local_tone_samples == 100u) {
			CHECK(v90_info0d_encode(
				      &repeated, payload, sizeof(payload)) ==
				      V90_INFO0_OK &&
			      phase2_info0_exchange_restart_info(
				      &peer, payload,
				      V90_INFO0D_PAYLOAD_BITS) ==
				      PHASE2_INFO0_EXCHANGE_OK,
			      "completed peer repeat did not start for %d/%d",
			      codec, reject_repeat);
			repeat_started = true;
		}
	}
	CHECK(repeat_started && receiver_synchronized &&
	      acknowledgment_started_on_sync &&
	      !reversed_during_record && reversal != SIZE_MAX,
	      "synchronized INFO0 did not defer reversal for %d/%d",
	      codec, reject_repeat);
	if (reject_repeat)
		CHECK(rejection != SIZE_MAX && reversal > rejection && reopened &&
		      completed_records == 2u &&
		      ranging.info0_exchange.state == PCM_PHASE2_INFO0_COMPLETE,
		      "rejected INFO0 changed completed ranging for %d: rejected=%zu reversal=%zu reopened=%d records=%zu state=%d phase=%d local=%u",
		      codec, rejection, reversal, reopened, completed_records,
		      ranging.info0_exchange.state, ranging.control.phase,
		      ranging.control.local_tone_samples);
	else
		CHECK(reopened && completed_records == 2u &&
		      ranging.info0_exchange.state == PCM_PHASE2_INFO0_COMPLETE &&
		      reversal > first_completion,
		      "valid INFO0 repeat did not restart completed ranging for %d",
		      codec);
	return true;
}

static bool test_completed_exchange_repeat(void)
{
	for (enum test_codec codec = TEST_CODEC_PCMU;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (unsigned int reject = 0; reject < 2; ++reject)
			CHECK(check_completed_exchange_repeat(codec, reject != 0),
			      "completed INFO0 ordering failed for %d/%u",
			      codec, reject);
	return true;
}

static bool test_validation(void)
{
	struct v90_info0 analog = analog_capabilities();
	struct v90_phase2_ranging ranging;
	struct v90_phase2_ranging before;
	struct v90_phase2_ranging_event event = {
		.kind = PHASE2_RANGING_TIMEOUT,
	};
	int16_t sample = 1234;

	CHECK(v90_phase2_ranging_init_analog(
		      &ranging, TEST_AMPLITUDE, &analog) ==
		      V90_PHASE2_RANGING_OK,
	      "validation fixture did not initialize");
	ranging.info0_exchange.transport.info_receiver.modulation.detector_output =
		NAN;
	before = ranging;
	CHECK(v90_phase2_ranging_sample(
		      &ranging, 0, &sample, &event) ==
		      V90_PHASE2_RANGING_INVALID_ARGUMENT &&
	      memcmp(&ranging, &before, sizeof(ranging)) == 0 &&
	      sample == 1234 && event.kind == PHASE2_RANGING_TIMEOUT,
	      "invalid V.90 ranging state mutated state or outputs");
	CHECK(strcmp(
		      v90_phase2_ranging_result_name(V90_PHASE2_RANGING_OK),
		      "ok") == 0 &&
	      strcmp(v90_phase2_ranging_result_name(
			     (enum v90_phase2_ranging_result)99),
		     "unknown") == 0,
	      "V.90 ranging result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "PCMU and PCMA ranging", test_pcm_ranging },
		{ "INFO-interrupted reversal", test_info_interrupted_reversal },
		{ "response during INFO repeat", test_response_during_repeat },
		{ "deferred first reversal", test_deferred_first_reversal },
		{ "retrain early second reversal",
		  test_retrain_early_second_reversal },
		{ "second reversal timeout recovery",
		  test_second_reversal_timeout_recovery },
		{ "rejected edge before INFO", test_rejected_edge_before_info },
		{ "completed INFO0 repeat ordering",
		  test_completed_exchange_repeat },
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
