// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mapping_frame_transmitter.h>
#include <v34_mapping_frame_receiver.h>

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
	uint8_t data_frame;
};

static const struct configuration CONFIGURATIONS[] = {
	{ V34_SYMBOL_RATE_2400, 1, false, false,
	  V34_CALLING_STATION, 0 },
	{ V34_SYMBOL_RATE_2743, 2, true, true,
	  V34_ANSWERING_STATION, 1 },
	{ V34_SYMBOL_RATE_3000, 8, true, false,
	  V34_CALLING_STATION, 2 },
	{ V34_SYMBOL_RATE_3429, 14, false, true,
	  V34_ANSWERING_STATION, 3 },
};

struct modem_pair {
	struct v34_frame_schedule transmit_schedule;
	struct v34_frame_schedule receive_schedule;
	struct v34_scrambler scrambler;
	struct v34_scrambler descrambler;
	struct v34_4d_encoder encoder;
	struct v34_4d_decoder decoder;
};

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

static bool generate_decision(
	const struct v34_mapping_parameters *parameters,
	uint8_t previous_differential_output,
	const struct v34_4d_mapping_input *mapping,
	const uint8_t ring_index[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t trellis_output, struct v34_decided_4d_interval *decided)
{
	uint8_t differential_output = (uint8_t)(
		(previous_differential_output + mapping->differential_input) & 3u);
	uint8_t rotations[V34_2D_SYMBOLS_PER_4D_INTERVAL] = {
		differential_output,
		(uint8_t)((differential_output + 2u * mapping->rotation_bit +
			   trellis_output) &
			  3u),
	};

	decided->trellis_output = trellis_output;
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		const struct v34_2d_mapping_input input = {
			.ring_index = ring_index[symbol],
			.point_index = mapping->point_index[symbol],
			.clockwise_quarter_turns = rotations[symbol],
		};
		struct v34_2d_mapping_output output;

		if (v34_map_2d_symbol(parameters, &input, &output) !=
		    V34_MAPPER_OK)
			return false;
		decided->mapper_output[symbol] = output.mapper_output;
	}
	return true;
}

static bool mapping_frames_are_equal(
	const struct v34_mapping_frame *left,
	const struct v34_mapping_frame *right)
{
	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame;
}

static bool received_matches(
	const struct v34_received_mapping_frame *received,
	const struct v34_mapping_frame *frame,
	const uint8_t primary[V34_MAX_MAPPING_FRAME_BITS],
	uint8_t auxiliary)
{
	if (!mapping_frames_are_equal(&received->mapping_frame, frame) ||
	    received->primary_bit_count != frame->primary_bits ||
	    received->auxiliary_bit_count !=
		    (uint8_t)frame->carries_auxiliary_bit ||
	    memcmp(received->primary_bits, primary, frame->primary_bits) != 0 ||
	    (frame->carries_auxiliary_bit &&
	     received->auxiliary_bit != auxiliary))
		return false;
	for (uint8_t index = frame->primary_bits;
	     index < V34_MAX_MAPPING_FRAME_BITS; ++index) {
		if (received->primary_bits[index] != 0)
			return false;
	}
	return true;
}

static bool initialize_pair(
	struct modem_pair *pair, const struct configuration *configuration,
	struct v34_frame_geometry *geometry)
{
	struct v34_mapping_parameters parameters;

	if (v34_frame_geometry_init(
		    geometry, configuration->symbol_rate,
		    configuration->primary_rate, configuration->auxiliary) !=
		    V34_FRAME_SCHEDULE_OK)
		return false;
	return v34_mapping_parameters_init(
		       &parameters, geometry, configuration->expanded) ==
		       V34_MAPPING_PARAMETERS_OK &&
	       v34_frame_schedule_init(
		       &pair->transmit_schedule, geometry,
		       (uint8_t)(configuration->data_frame %
				 geometry->data_frames_per_superframe)) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_frame_schedule_init(
		       &pair->receive_schedule, geometry,
		       pair->transmit_schedule.data_frame_in_superframe) ==
		       V34_FRAME_SCHEDULE_OK &&
	       v34_transmit_scrambler_init(
		       &pair->scrambler, configuration->transmitting_role) ==
		       V34_FRAMING_OK &&
	       v34_transmit_scrambler_init(
		       &pair->descrambler, configuration->transmitting_role) ==
		       V34_FRAMING_OK &&
	       v34_4d_encoder_init(
		       &pair->encoder, &parameters, ZERO_COEFFICIENTS,
		       V34_TRELLIS_16_STATE) ==
		       V34_4D_ENCODER_OK &&
	       v34_4d_decoder_init(&pair->decoder, &parameters) ==
		       V34_4D_DECODER_OK;
}

static bool test_generated_round_trip(void)
{
	bool polynomial_seen[2] = { false };
	bool auxiliary_seen[2] = { false };
	bool high_frame_seen[2] = { false };
	size_t mapping_frame_count = 0;

	for (size_t configuration = 0;
	     configuration < ARRAY_LENGTH(CONFIGURATIONS); ++configuration) {
		struct v34_frame_geometry geometry;
		struct modem_pair pair;

		CHECK(initialize_pair(
			      &pair, &CONFIGURATIONS[configuration], &geometry),
		      "configuration %zu did not initialize", configuration);
		polynomial_seen[pair.scrambler.polynomial] = true;
		while (!v34_frame_schedule_is_complete(
			&pair.transmit_schedule)) {
			struct v34_frame_schedule preview =
				pair.transmit_schedule;
			struct v34_mapping_frame frame;
			struct v34_transmitted_mapping_frame transmitted;
			struct v34_received_mapping_frame received;
			struct v34_decided_4d_interval
				decided[V34_4D_INTERVALS_PER_MAPPING_FRAME];
			uint8_t primary[V34_MAX_MAPPING_FRAME_BITS] = { 0 };
			uint8_t auxiliary = (uint8_t)(mapping_frame_count & 1u);
			const uint8_t *auxiliary_pointer;

			CHECK(v34_frame_schedule_next(&preview, &frame) ==
				      V34_FRAME_SCHEDULE_OK,
			      "configuration %zu ended early", configuration);
			generate_primary_bits(
				primary, frame.primary_bits,
				UINT32_C(0x9e3779b9) ^ (uint32_t)mapping_frame_count);
			auxiliary_pointer = frame.carries_auxiliary_bit ?
				&auxiliary :
				NULL;
			CHECK(v34_transmit_mapping_frame(
				      &pair.transmit_schedule, &pair.scrambler,
				      &pair.encoder, primary, frame.primary_bits,
				      auxiliary_pointer,
				      V34_NONLINEAR_THETA_ZERO, 512.0,
				      &transmitted) ==
				      V34_MAPPING_FRAME_TRANSMITTER_OK,
			      "mapping frame %zu did not transmit", mapping_frame_count);
			decisions_from_encoded(&transmitted.encoded, decided);
			CHECK(v34_receive_mapping_frame(
				      &pair.receive_schedule, &pair.descrambler,
				      &pair.decoder, decided, &received) ==
				      V34_MAPPING_FRAME_RECEIVER_OK &&
			      received_matches(&received, &frame, primary, auxiliary) &&
			      pair.receive_schedule.data_frame_in_superframe ==
				      pair.transmit_schedule.data_frame_in_superframe &&
			      pair.receive_schedule.next_mapping_frame ==
				      pair.transmit_schedule.next_mapping_frame &&
			      pair.descrambler.history == pair.scrambler.history &&
			      pair.decoder.previous_differential_output ==
				      pair.encoder.previous_differential_output,
			      "mapping frame %zu did not receive", mapping_frame_count);
			high_frame_seen[frame.high_frame] = true;
			auxiliary_seen[frame.carries_auxiliary_bit] = true;
			mapping_frame_count++;
		}
	}
	CHECK(mapping_frame_count > 40u && polynomial_seen[0] &&
	      polynomial_seen[1] && auxiliary_seen[0] && auxiliary_seen[1] &&
	      high_frame_seen[0] && high_frame_seen[1],
	      "generated coverage changed at %zu frames", mapping_frame_count);
	return true;
}

static bool test_noncodebook_decision_is_erased(void)
{
	struct v34_frame_geometry geometry;
	struct modem_pair pair;
	struct v34_frame_schedule expected_schedule;
	struct v34_scrambler expected_descrambler;
	struct v34_4d_decoder expected_decoder;
	struct v34_mapping_frame frame;
	struct v34_recovered_mapping_frame recovered;
	struct v34_received_mapping_frame received;
	struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	const struct v34_4d_mapping_input mapping = { 0 };
	uint8_t ring_index[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	uint8_t ignored;

	CHECK(initialize_pair(&pair, &CONFIGURATIONS[3], &geometry),
	      "non-codebook receiver fixture did not initialize");
	expected_schedule = pair.receive_schedule;
	CHECK(v34_frame_schedule_next(&expected_schedule, &frame) ==
		      V34_FRAME_SCHEDULE_OK,
	      "non-codebook receiver schedule did not advance");
	for (uint8_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		ring_index[symbol] = (uint8_t)(pair.decoder.parameters.ring_count - 1u);
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME; ++interval) {
		CHECK(generate_decision(
			      &pair.decoder.parameters,
			      pair.decoder.previous_differential_output, &mapping,
			      ring_index, 0, &decided[interval]),
		      "non-codebook interval %u was not generated", interval);
	}

	expected_decoder = pair.decoder;
	CHECK(v34_decode_mapping_frame(
		      &expected_decoder, &frame, decided, &recovered) ==
		      V34_MAPPING_FRAME_DECODER_OK &&
	      !recovered.shell_decision_exact,
	      "outside shell sequence was not recognized as a channel error");
	expected_descrambler = pair.descrambler;
	for (uint8_t index = 0; index < recovered.primary_bit_count; ++index) {
		CHECK(v34_descramble_group(
			      &expected_descrambler, recovered.primary_bits[index], 1,
			      &ignored) == V34_FRAMING_OK,
		      "expected descrambler did not advance at bit %u", index);
	}
	CHECK(v34_receive_mapping_frame(
		      &pair.receive_schedule, &pair.descrambler, &pair.decoder,
		      decided, &received) == V34_MAPPING_FRAME_RECEIVER_OK &&
	      memcmp(&pair.receive_schedule, &expected_schedule,
		     sizeof(expected_schedule)) == 0 &&
	      memcmp(&pair.descrambler, &expected_descrambler,
		     sizeof(expected_descrambler)) == 0 &&
	      memcmp(&pair.decoder, &expected_decoder,
		     sizeof(expected_decoder)) == 0 &&
	      received.primary_bit_count == frame.primary_bits &&
	      received.auxiliary_bit_count ==
		      (uint8_t)frame.carries_auxiliary_bit,
	      "non-codebook decision did not advance receiver state");
	for (uint8_t index = 0; index < received.primary_bit_count; ++index)
		CHECK(received.primary_bits[index] == 1,
		      "non-codebook decision exposed primary bit %u", index);
	if (received.auxiliary_bit_count != 0)
		CHECK(received.auxiliary_bit == 1,
		      "non-codebook decision exposed its auxiliary bit");
	return true;
}

struct rejection_fixture {
	struct modem_pair pair;
	struct v34_mapping_frame frame;
	struct v34_decided_4d_interval
		decided[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	uint8_t primary[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary;
};

static bool initialize_rejection_fixture(struct rejection_fixture *fixture)
{
	struct v34_frame_geometry geometry;
	struct v34_frame_schedule preview;
	struct v34_transmitted_mapping_frame transmitted;
	const uint8_t *auxiliary_pointer;

	memset(fixture, 0, sizeof(*fixture));
	if (!initialize_pair(&fixture->pair, &CONFIGURATIONS[2], &geometry))
		return false;
	preview = fixture->pair.transmit_schedule;
	if (v34_frame_schedule_next(&preview, &fixture->frame) !=
	    V34_FRAME_SCHEDULE_OK)
		return false;
	generate_primary_bits(fixture->primary, fixture->frame.primary_bits, 23u);
	fixture->auxiliary = 1;
	auxiliary_pointer = fixture->frame.carries_auxiliary_bit ?
		&fixture->auxiliary :
		NULL;
	if (v34_transmit_mapping_frame(
		    &fixture->pair.transmit_schedule, &fixture->pair.scrambler,
		    &fixture->pair.encoder, fixture->primary,
		    fixture->frame.primary_bits, auxiliary_pointer,
		    V34_NONLINEAR_THETA_ZERO, 512.0,
		    &transmitted) != V34_MAPPING_FRAME_TRANSMITTER_OK)
		return false;
	decisions_from_encoded(&transmitted.encoded, fixture->decided);
	return true;
}

static bool receiver_state_is_equal(
	const struct modem_pair *pair,
	const struct v34_frame_schedule *schedule,
	const struct v34_scrambler *descrambler,
	const struct v34_4d_decoder *decoder)
{
	return memcmp(&pair->receive_schedule, schedule, sizeof(*schedule)) == 0 &&
	       memcmp(&pair->descrambler, descrambler, sizeof(*descrambler)) == 0 &&
	       memcmp(&pair->decoder, decoder, sizeof(*decoder)) == 0;
}

static bool test_transactional_boundaries(void)
{
	struct rejection_fixture fixture;
	struct v34_frame_schedule saved_schedule;
	struct v34_scrambler saved_descrambler;
	struct v34_4d_decoder saved_decoder;
	struct v34_decided_4d_interval
		malformed[V34_4D_INTERVALS_PER_MAPPING_FRAME];
	struct v34_received_mapping_frame received;
	struct v34_received_mapping_frame saved_received;

	CHECK(initialize_rejection_fixture(&fixture),
	      "receiver rejection fixture did not initialize");
	saved_schedule = fixture.pair.receive_schedule;
	saved_descrambler = fixture.pair.descrambler;
	saved_decoder = fixture.pair.decoder;
	memset(&received, 0xa5, sizeof(received));
	saved_received = received;
	memcpy(malformed, fixture.decided, sizeof(malformed));
	malformed[3].trellis_output = 2;
	CHECK(v34_receive_mapping_frame(
		      &fixture.pair.receive_schedule, &fixture.pair.descrambler,
		      &fixture.pair.decoder, malformed, &received) ==
		      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      receiver_state_is_equal(
		      &fixture.pair, &saved_schedule, &saved_descrambler,
		      &saved_decoder) &&
	      memcmp(&received, &saved_received, sizeof(received)) == 0,
	      "late decision rejection changed state or output");
	{
		struct modem_pair other;
		struct v34_frame_geometry other_geometry;
		struct v34_4d_decoder saved_other;

		CHECK(initialize_pair(
			      &other, &CONFIGURATIONS[0], &other_geometry) &&
		      other.decoder.parameters.mapping_frame_bits !=
			      saved_schedule.geometry.high_mapping_frame_bits,
		      "mismatched decoder fixture did not initialize");
		saved_other = other.decoder;
		CHECK(v34_receive_mapping_frame(
			      &fixture.pair.receive_schedule,
			      &fixture.pair.descrambler, &other.decoder,
			      fixture.decided, &received) ==
			      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&other.decoder, &saved_other, sizeof(saved_other)) == 0 &&
		      receiver_state_is_equal(
			      &fixture.pair, &saved_schedule, &saved_descrambler,
			      &saved_decoder) &&
		      memcmp(&received, &saved_received, sizeof(received)) == 0,
		      "mismatched decoder changed receiver state or output");
	}
	CHECK(v34_receive_mapping_frame(
		      NULL, &fixture.pair.descrambler, &fixture.pair.decoder,
		      fixture.decided, &received) ==
		      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      v34_receive_mapping_frame(
		      &fixture.pair.receive_schedule, NULL, &fixture.pair.decoder,
		      fixture.decided, &received) ==
		      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      v34_receive_mapping_frame(
		      &fixture.pair.receive_schedule, &fixture.pair.descrambler,
		      NULL, fixture.decided, &received) ==
		      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      v34_receive_mapping_frame(
		      &fixture.pair.receive_schedule, &fixture.pair.descrambler,
		      &fixture.pair.decoder, NULL, &received) ==
		      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      v34_receive_mapping_frame(
		      &fixture.pair.receive_schedule, &fixture.pair.descrambler,
		      &fixture.pair.decoder, fixture.decided, NULL) ==
		      V34_MAPPING_FRAME_RECEIVER_INVALID_ARGUMENT &&
	      receiver_state_is_equal(
		      &fixture.pair, &saved_schedule, &saved_descrambler,
		      &saved_decoder) &&
	      memcmp(&received, &saved_received, sizeof(received)) == 0,
	      "invalid arguments changed receiver state or output");
	CHECK(v34_receive_mapping_frame(
		      &fixture.pair.receive_schedule, &fixture.pair.descrambler,
		      &fixture.pair.decoder, fixture.decided, &received) ==
		      V34_MAPPING_FRAME_RECEIVER_OK &&
	      received_matches(
		      &received, &fixture.frame, fixture.primary,
		      fixture.auxiliary),
	      "valid frame did not recover after rejection");
	{
		struct v34_frame_schedule completed = saved_schedule;
		struct v34_frame_schedule expected_completed;
		struct v34_scrambler expected_descrambler;
		struct v34_4d_decoder expected_decoder;
		struct v34_mapping_frame ignored;

		while (v34_frame_schedule_next(&completed, &ignored) ==
		       V34_FRAME_SCHEDULE_OK)
			;
		expected_completed = completed;
		expected_descrambler = saved_descrambler;
		expected_decoder = saved_decoder;
		received = saved_received;
		CHECK(v34_receive_mapping_frame(
			      &completed, &saved_descrambler, &saved_decoder,
			      fixture.decided, &received) ==
			      V34_MAPPING_FRAME_RECEIVER_FRAME_COMPLETE &&
		      memcmp(&completed, &expected_completed,
			     sizeof(completed)) == 0 &&
		      memcmp(&saved_descrambler, &expected_descrambler,
			     sizeof(saved_descrambler)) == 0 &&
		      memcmp(&saved_decoder, &expected_decoder,
			     sizeof(saved_decoder)) == 0 &&
		      memcmp(&received, &saved_received, sizeof(received)) == 0,
		      "completed schedule changed receiver state or output");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated round trip", test_generated_round_trip },
		{ "non-codebook decision erasure",
		  test_noncodebook_decision_is_erased },
		{ "transactional boundaries", test_transactional_boundaries },
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
