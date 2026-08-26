// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapping_frame_transmitter.h>
#include <v34_data_receiver.h>

#include "test.h"

#include <string.h>

static const struct v34_q14_coefficient ZERO_COEFFICIENTS[] = {
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
};

struct configuration {
	enum v34_symbol_rate symbol_rate;
	uint8_t primary_rate;
	bool auxiliary;
	bool expanded;
	enum v34_station_role transmitting_role;
};

static const struct configuration CONFIGURATIONS[] = {
	{ V34_SYMBOL_RATE_3000, 8, true, false,
	  V34_CALLING_STATION },
	{ V34_SYMBOL_RATE_3429, 14, false, true,
	  V34_ANSWERING_STATION },
};

struct fixture {
	struct v34_frame_geometry geometry;
	struct v34_frame_schedule transmit_schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_data_receiver receiver;
};

struct packet {
	struct v34_mapping_frame frame;
	struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	uint8_t primary[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary;
};

static bool initialize_fixture(
	struct fixture *fixture, const struct configuration *configuration)
{
	struct v34_mapping_parameters parameters;

	memset(fixture, 0, sizeof(*fixture));
	return v34_frame_geometry_init(
		       &fixture->geometry, configuration->symbol_rate,
		       configuration->primary_rate, configuration->auxiliary) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_mapping_parameters_init(
		       &parameters, &fixture->geometry, configuration->expanded) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_b1_frame_schedule_init(
		       &fixture->transmit_schedule, &fixture->geometry) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_transmit_scrambler_init(
		       &fixture->scrambler, configuration->transmitting_role) ==
		       V34_FRAMING_OK &&
	       v34_4d_encoder_init(
		       &fixture->encoder, &parameters, ZERO_COEFFICIENTS,
		       V34_TRELLIS_16_STATE) ==
		       V34_4D_ENCODER_OK &&
	       v34_data_receiver_init(
		       &fixture->receiver, &fixture->geometry,
		       configuration->expanded,
		       configuration->transmitting_role) ==
		       V34_DATA_RECEIVE_OK;
}

static void decisions_from_encoded(
	const struct v34_encoded_mapping_frame *encoded,
	struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME])
{
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		decided[interval].trellis_output =
			encoded->interval[interval].trellis_output;
		for (uint8_t symbol = 0;
		     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL;
		     ++symbol)
			decided[interval].mapper_output[symbol] =
				encoded->interval[interval]
					.mapped[symbol]
					.mapper_output;
	}
}

static void generate_primary_bits(
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS], uint8_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (uint8_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static bool prepare_packet(
	struct fixture *fixture, struct packet *packet)
{
	struct v34_frame_schedule preview;

	if (v34_frame_schedule_is_complete(
		    &fixture->transmit_schedule) &&
	    v34_frame_schedule_advance_data_frame(
		    &fixture->transmit_schedule) !=
		    V34_FRAME_SCHEDULE_OK)
		return false;
	preview = fixture->transmit_schedule;
	return v34_frame_schedule_next(&preview, &packet->frame) ==
	       V34_FRAME_SCHEDULE_OK;
}

static bool transmit_packet(
	struct fixture *fixture, struct packet *packet)
{
	struct v34_transmitted_mapping_frame transmitted;
	const uint8_t *auxiliary =
		packet->frame.carries_auxiliary_bit ?
			&packet->auxiliary :
			NULL;

	if (v34_transmit_mapping_frame(
		    &fixture->transmit_schedule, &fixture->scrambler,
		    &fixture->encoder, packet->primary,
		    packet->frame.primary_bits, auxiliary,
		    V34_NONLINEAR_THETA_ZERO, 512.0,
		    &transmitted) != V34_MAPPING_FRAME_TRANSMITTER_OK)
		return false;
	decisions_from_encoded(&transmitted.encoded, packet->decided);
	return true;
}

static bool transmit_next(
	struct fixture *fixture, bool b1, bool corrupt_primary,
	bool corrupt_auxiliary, uint32_t seed, struct packet *packet)
{
	if (!prepare_packet(fixture, packet))
		return false;
	memset(packet->primary, 1, packet->frame.primary_bits);
	packet->auxiliary = b1 ? 1u : (uint8_t)(seed & 1u);
	if (!b1)
		generate_primary_bits(
			packet->primary, packet->frame.primary_bits, seed);
	if (corrupt_primary)
		packet->primary[packet->frame.primary_bits - 1u] = 0;
	if (corrupt_auxiliary)
		packet->auxiliary = 0;
	if (corrupt_auxiliary &&
	    !packet->frame.carries_auxiliary_bit)
		return false;
	return transmit_packet(fixture, packet);
}

static bool transmit_uniform(
	struct fixture *fixture, uint8_t bit, struct packet *packet)
{
	if (bit > 1u || !prepare_packet(fixture, packet))
		return false;
	memset(packet->primary, bit, packet->frame.primary_bits);
	packet->auxiliary = bit;
	return transmit_packet(fixture, packet);
}

static bool received_matches(
	const struct v34_received_mapping_frame *received,
	const struct packet *packet)
{
	const struct v34_mapping_frame *left =
		&received->mapping_frame;
	const struct v34_mapping_frame *right = &packet->frame;

	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame &&
	       received->primary_bit_count == right->primary_bits &&
	       memcmp(received->primary_bits, packet->primary,
		      right->primary_bits) == 0 &&
	       received->auxiliary_bit_count ==
		       (uint8_t)right->carries_auxiliary_bit &&
	       (!right->carries_auxiliary_bit ||
		received->auxiliary_bit == packet->auxiliary);
}

static bool test_b1_and_superframe_handoff(void)
{
	for (size_t index = 0; index < ARRAY_LENGTH(CONFIGURATIONS); ++index) {
		struct fixture fixture;
		struct packet packet;
		struct v34_received_mapping_frame received;
		size_t mapping_frames = 0;
		size_t data_frames = 0;

		CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[index]) &&
		      v34_data_receiver_is_receiving_b1(
			      &fixture.receiver),
		      "B1 fixture %zu did not initialize", index);
		while (v34_data_receiver_is_receiving_b1(
			&fixture.receiver)) {
			enum v34_data_receive_result expected;

			CHECK(transmit_next(
				      &fixture, true, false, false,
				      (uint32_t)mapping_frames, &packet),
			      "B1 frame %zu/%zu did not transmit", index,
			      mapping_frames);
			expected = packet.frame.completes_data_frame ?
				V34_DATA_RECEIVE_B1_COMPLETE :
				V34_DATA_RECEIVE_B1_IN_PROGRESS;
			CHECK(v34_data_receiver_receive_mapping_frame(
				      &fixture.receiver, packet.decided, &received) ==
				      expected &&
			      received_matches(&received, &packet),
			      "B1 frame %zu/%zu did not receive", index,
			      mapping_frames);
			mapping_frames++;
		}
		while (data_frames <=
		       fixture.geometry.data_frames_per_superframe) {
			CHECK(transmit_next(
				      &fixture, false, false, false,
				      UINT32_C(0x6d2b79f5) ^
					      (uint32_t)mapping_frames,
				      &packet) &&
			      v34_data_receiver_receive_mapping_frame(
				      &fixture.receiver, packet.decided, &received) ==
				      V34_DATA_RECEIVE_MAPPING_FRAME_READY &&
			      received_matches(&received, &packet) &&
			      fixture.receiver.schedule.data_frame_in_superframe ==
				      fixture.transmit_schedule
					      .data_frame_in_superframe &&
			      fixture.receiver.schedule.next_mapping_frame ==
				      fixture.transmit_schedule.next_mapping_frame &&
			      fixture.receiver.descrambler.history ==
				      fixture.scrambler.history &&
			      fixture.receiver.mapping_decoder
					      .previous_differential_output ==
				      fixture.encoder.previous_differential_output,
			      "data frame %zu/%zu did not receive", index,
			      mapping_frames);
			mapping_frames++;
			if (packet.frame.completes_data_frame)
				data_frames++;
		}
		CHECK(!v34_data_receiver_is_receiving_b1(
			      &fixture.receiver) &&
		      fixture.receiver.b1_received_bits ==
			      fixture.geometry.data_bits_per_frame &&
		      fixture.receiver.b1_error_bits == 0u &&
		      data_frames >
			      fixture.geometry.data_frames_per_superframe,
		      "receiver %zu did not cross a superframe", index);
	}
	return true;
}

static bool test_b1_errors_and_rejection(void)
{
	struct fixture fixture;
	struct packet packet;
	struct v34_data_receiver saved;
	struct v34_decided_4d_interval
		malformed[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct v34_received_mapping_frame received;
	struct v34_received_mapping_frame saved_received;

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]),
	      "B1 error fixture did not initialize");
	CHECK(transmit_next(&fixture, true, true, false, 0, &packet),
	      "primary error did not transmit");
	memset(&received, 0xa5, sizeof(received));
	saved_received = received;
	CHECK(v34_data_receiver_receive_mapping_frame(
		      &fixture.receiver, packet.decided, &received) ==
		      V34_DATA_RECEIVE_B1_IN_PROGRESS &&
	      received_matches(&received, &packet) &&
	      fixture.receiver.b1_received_bits == packet.frame.data_bits &&
	      fixture.receiver.b1_error_bits == 1u,
	      "primary B1 error was not recorded");
	CHECK(transmit_next(&fixture, true, false, false, 1, &packet),
	      "structural rejection frame did not transmit");
	memcpy(malformed, packet.decided, sizeof(malformed));
	malformed[3].trellis_output = 2;
	saved = fixture.receiver;
	received = saved_received;
	CHECK(v34_data_receiver_receive_mapping_frame(
		      &fixture.receiver, malformed, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&received, &saved_received, sizeof(received)) == 0 &&
	      v34_data_receiver_receive_mapping_frame(
		      &fixture.receiver, packet.decided, &received) ==
		      V34_DATA_RECEIVE_B1_IN_PROGRESS &&
	      received_matches(&received, &packet),
	      "structural rejection changed state or prevented retry");
	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]),
	      "auxiliary error fixture did not initialize");
	for (;;) {
		struct v34_frame_schedule preview =
			fixture.transmit_schedule;
		struct v34_mapping_frame frame;

		CHECK(v34_frame_schedule_next(&preview, &frame) ==
			      V34_FRAME_SCHEDULE_OK,
		      "auxiliary B1 schedule ended early");
		if (frame.carries_auxiliary_bit)
			break;
		CHECK(transmit_next(&fixture, true, false, false, 0, &packet) &&
		      v34_data_receiver_receive_mapping_frame(
			      &fixture.receiver, packet.decided, &received) ==
			      V34_DATA_RECEIVE_B1_IN_PROGRESS,
		      "B1 did not advance to an auxiliary frame");
	}
	saved = fixture.receiver;
	CHECK(transmit_next(&fixture, true, false, true, 0, &packet) &&
	      v34_data_receiver_receive_mapping_frame(
		      &fixture.receiver, packet.decided, &received) ==
		      (packet.frame.completes_data_frame ?
			       V34_DATA_RECEIVE_B1_COMPLETE :
			       V34_DATA_RECEIVE_B1_IN_PROGRESS) &&
	      received_matches(&received, &packet) &&
	      fixture.receiver.b1_received_bits ==
		      saved.b1_received_bits + packet.frame.data_bits &&
	      fixture.receiver.b1_error_bits == saved.b1_error_bits + 1u,
	      "auxiliary B1 error was not recorded");
	while (v34_data_receiver_is_receiving_b1(
		&fixture.receiver)) {
		CHECK(transmit_next(&fixture, true, false, false, 0, &packet) &&
		      v34_data_receiver_receive_mapping_frame(
			      &fixture.receiver, packet.decided, &received) ==
			      (packet.frame.completes_data_frame ?
				       V34_DATA_RECEIVE_B1_COMPLETE :
				       V34_DATA_RECEIVE_B1_IN_PROGRESS),
		      "errored B1 did not reach its data handoff");
	}
	CHECK(fixture.receiver.b1_received_bits ==
		      fixture.geometry.data_bits_per_frame &&
	      fixture.receiver.b1_error_bits == 1u,
	      "B1 diagnostics changed at the data handoff");
	return true;
}

static bool test_b1_schedule_continuity(void)
{
	for (size_t index = 0; index < ARRAY_LENGTH(CONFIGURATIONS); ++index) {
		struct fixture fixture;
		struct packet packet;
		struct v34_received_mapping_frame received;
		uint16_t initial_errors;

		CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[index]) &&
		      transmit_uniform(&fixture, 0, &packet),
		      "B1 continuity fixture %zu did not initialize", index);
		initial_errors = packet.frame.data_bits;
		CHECK(v34_data_receiver_receive_mapping_frame(
			      &fixture.receiver, packet.decided, &received) ==
			      V34_DATA_RECEIVE_B1_IN_PROGRESS &&
		      received_matches(&received, &packet) &&
		      fixture.receiver.b1_received_bits ==
			      packet.frame.data_bits &&
		      fixture.receiver.b1_error_bits == initial_errors &&
		      fixture.receiver.schedule.next_mapping_frame ==
			      fixture.transmit_schedule.next_mapping_frame,
		      "noisy B1 prefix %zu changed frame alignment", index);
		while (v34_data_receiver_is_receiving_b1(&fixture.receiver)) {
			enum v34_data_receive_result expected;

			CHECK(transmit_next(
				      &fixture, true, false, false, 0, &packet),
			      "B1 continuation %zu did not transmit", index);
			expected = packet.frame.completes_data_frame ?
				V34_DATA_RECEIVE_B1_COMPLETE :
				V34_DATA_RECEIVE_B1_IN_PROGRESS;
			CHECK(v34_data_receiver_receive_mapping_frame(
				      &fixture.receiver, packet.decided,
				      &received) == expected &&
			      received_matches(&received, &packet) &&
			      fixture.receiver.schedule.next_mapping_frame ==
				      fixture.transmit_schedule.next_mapping_frame,
			      "B1 continuation %zu lost frame alignment", index);
		}
		CHECK(fixture.receiver.b1_received_bits ==
			      fixture.geometry.data_bits_per_frame &&
		      fixture.receiver.b1_error_bits == initial_errors,
		      "B1 continuity diagnostics %zu changed", index);
	}
	return true;
}

static bool test_validation(void)
{
	struct fixture fixture;
	struct v34_data_receiver saved;
	struct v34_frame_geometry invalid_geometry;
	struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME] = { 0 };
	struct v34_received_mapping_frame received;

	CHECK(initialize_fixture(&fixture, &CONFIGURATIONS[0]),
	      "validation fixture did not initialize");
	saved = fixture.receiver;
	invalid_geometry = fixture.geometry;
	invalid_geometry.mapping_frames_per_data_frame = 0;
	CHECK(v34_data_receiver_init(
		      NULL, &fixture.geometry, false,
		      V34_CALLING_STATION) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_receiver_init(
		      &fixture.receiver, &invalid_geometry, false,
		      V34_CALLING_STATION) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_receiver_init(
		      &fixture.receiver, &fixture.geometry, false,
		      (enum v34_station_role)2) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved, sizeof(saved)) == 0,
	      "invalid initialization changed the receiver");
	memset(&received, 0xa5, sizeof(received));
	CHECK(v34_data_receiver_receive_mapping_frame(
		      &fixture.receiver, NULL, &received) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      v34_data_receiver_receive_mapping_frame(
		      &fixture.receiver, decided, NULL) ==
		      V34_DATA_RECEIVE_INVALID_ARGUMENT &&
	      memcmp(&fixture.receiver, &saved, sizeof(saved)) == 0,
	      "invalid receive arguments changed the receiver");
	CHECK(v34_data_receiver_is_valid(&fixture.receiver) &&
	      !v34_data_receiver_is_valid(NULL),
	      "data receiver validation changed");
	fixture.receiver.schedule.next_mapping_frame =
		fixture.receiver.schedule.geometry.mapping_frames_per_data_frame;
	CHECK(!v34_data_receiver_is_valid(&fixture.receiver) &&
	      !v34_data_receiver_is_receiving_b1(&fixture.receiver),
	      "completed B1 state was accepted");
	fixture.receiver = saved;
	fixture.receiver.b1_received_bits = 1;
	CHECK(!v34_data_receiver_is_valid(&fixture.receiver),
	      "B1 bit count was accepted at the wrong schedule position");
	fixture.receiver = saved;
	fixture.receiver.b1_error_bits = 1;
	CHECK(!v34_data_receiver_is_valid(&fixture.receiver),
	      "B1 error count exceeded its received-bit count");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "B1 and superframe handoff", test_b1_and_superframe_handoff },
		{ "B1 errors and rejection", test_b1_errors_and_rejection },
		{ "B1 schedule continuity", test_b1_schedule_continuity },
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
