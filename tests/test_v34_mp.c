// SPDX-License-Identifier: BSD-3-Clause

#include <v34_mp.h>

#include "test.h"

#include <string.h>

enum {
	REFERENCE_CRC_POLYNOMIAL = 0x1021,
	REFERENCE_CRC_INITIAL = 0xffff,
	MP_START0_BIT = 17,
	MP_TYPE_BIT = 18,
	MP_FIELD_STRIDE_BITS = 17,
	MP_TYPE1_FIRST_COEFFICIENT_BIT = 52,
	MP_COEFFICIENT_COMPONENT_BITS = 16,
	MP_TYPE0_CRC_FIRST_BIT = 69,
	MP_TYPE1_RESERVED_FIRST_BIT = 154,
	MP_TYPE1_RESERVED_BITS = 16,
	MP_TYPE1_CRC_FIRST_BIT = 171,
	MP_TYPE1_FILL_BIT = 187,
};

_Static_assert(V34_MP_TYPE0_FRAME_SYNC_BITS == 17,
	       "Type 0 sync length changed");
_Static_assert(V34_MP_TYPE0_INFORMATION_BITS == 48,
	       "Type 0 information length changed");
_Static_assert(V34_MP_TYPE0_FRAME_BITS == 88,
	       "Type 0 frame length changed");
_Static_assert(V34_MP_TYPE0_FRAME_BYTES == 11,
	       "Type 0 packed length changed");
_Static_assert(V34_MP_TYPE1_INFORMATION_BITS == 144,
	       "Type 1 information length changed");
_Static_assert(V34_MP_TYPE1_FRAME_BITS == 188,
	       "Type 1 frame length changed");
_Static_assert(V34_MP_TYPE1_FRAME_BYTES == 24,
	       "Type 1 packed length changed");

static struct v34_mp_parameters base_parameters(bool acknowledged)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 10,
		.remote_transmitter_trellis =
			V34_TRELLIS_32_STATE,
		.acknowledges_peer_mp = acknowledged,
		.data_rate_capability_mask = 0x0fff,
	};
}

static struct v34_mp_type1_parameters type1_parameters(void)
{
	return (struct v34_mp_type1_parameters){
		.common = {
			.maximum_call_to_answer_rate_2400_bps_units = 1,
			.maximum_answer_to_call_rate_2400_bps_units = 14,
			.auxiliary_channel_enabled = true,
			.remote_transmitter_trellis =
				V34_TRELLIS_64_STATE,
			.remote_transmitter_nonlinear_encoding = true,
			.remote_transmitter_expanded_shaping = true,
			.acknowledges_peer_mp = true,
			.data_rate_capability_mask = 0x2555,
			.asymmetric_rates_enabled = true,
		},
		.precoder_coefficients = {
			{ .real = 0x1234, .imaginary = -1 },
			{ .real = -32767, .imaginary = INT16_MAX },
			{ .real = 0, .imaginary = 16384 },
		},
	};
}

static bool parameters_are_equal(
	const struct v34_mp_parameters *left,
	const struct v34_mp_parameters *right)
{
	return left->maximum_call_to_answer_rate_2400_bps_units ==
		       right->maximum_call_to_answer_rate_2400_bps_units &&
	       left->maximum_answer_to_call_rate_2400_bps_units ==
		       right->maximum_answer_to_call_rate_2400_bps_units &&
	       left->auxiliary_channel_enabled ==
		       right->auxiliary_channel_enabled &&
	       left->remote_transmitter_trellis ==
		       right->remote_transmitter_trellis &&
	       left->remote_transmitter_nonlinear_encoding ==
		       right->remote_transmitter_nonlinear_encoding &&
	       left->remote_transmitter_expanded_shaping ==
		       right->remote_transmitter_expanded_shaping &&
	       left->acknowledges_peer_mp == right->acknowledges_peer_mp &&
	       left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->asymmetric_rates_enabled ==
		       right->asymmetric_rates_enabled;
}

static bool type1_parameters_are_equal(
	const struct v34_mp_type1_parameters *left,
	const struct v34_mp_type1_parameters *right)
{
	if (!parameters_are_equal(&left->common, &right->common))
		return false;
	for (size_t index = 0; index < V34_PRECODER_TAP_COUNT;
	     ++index) {
		if (left->precoder_coefficients[index].real !=
			    right->precoder_coefficients[index].real ||
		    left->precoder_coefficients[index].imaginary !=
			    right->precoder_coefficients[index].imaginary)
			return false;
	}
	return true;
}

static uint8_t reference_frame_bit(
	const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void replace_frame_bit(
	uint8_t *frame, size_t bit_index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (bit != 0)
		frame[bit_index / 8u] |= mask;
	else
		frame[bit_index / 8u] &= (uint8_t)~mask;
}

static void replace_lsb_first_field(
	uint8_t *frame, size_t first_bit, size_t bit_count, uint16_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		replace_frame_bit(
			frame, first_bit + index,
			(uint8_t)(value >> index) & 1u);
}

static uint16_t reference_crc(
	const uint8_t *frame, size_t crc_first_bit)
{
	uint16_t remainder = REFERENCE_CRC_INITIAL;

	for (size_t index = MP_TYPE_BIT; index < crc_first_bit; ++index) {
		uint16_t next;

		if ((index - MP_START0_BIT) % MP_FIELD_STRIDE_BITS == 0)
			continue;
		next = (uint16_t)(remainder << 1);
		if ((((remainder >> 15) ^ reference_frame_bit(frame, index)) &
		     1u) != 0)
			next ^= REFERENCE_CRC_POLYNOMIAL;
		remainder = next;
	}
	return remainder;
}

static void replace_crc(uint8_t *frame, size_t crc_first_bit)
{
	uint16_t remainder = reference_crc(frame, crc_first_bit);

	for (size_t index = 0; index < V34_MP_CRC_BITS; ++index)
		replace_frame_bit(
			frame, crc_first_bit + index,
			(uint8_t)(remainder >>
				  (V34_MP_CRC_BITS - index - 1u)) &
				1u);
}

static uint16_t chronological_crc_word(
	const uint8_t *frame, size_t crc_first_bit)
{
	uint16_t word = 0;

	for (size_t index = 0; index < V34_MP_CRC_BITS; ++index)
		word |= (uint16_t)reference_frame_bit(
				frame, crc_first_bit + index) <<
			index;
	return word;
}

static bool receive_packed_frame(
	struct v34_mp_receiver *receiver, const uint8_t *frame,
	size_t frame_bit_count, struct v34_mp_receive_event *event)
{
	struct v34_mp_receive_event emitted = { 0 };

	for (size_t index = 0; index < frame_bit_count; ++index) {
		CHECK(v34_mp_receive_bit(
			      receiver, reference_frame_bit(frame, index),
			      &emitted) == V34_MP_OK,
		      "MP stream receiver stopped at bit %zu/%zu", index,
		      frame_bit_count);
		if (index + 1u < frame_bit_count)
			CHECK(emitted.type == V34_MP_RECEIVE_NO_EVENT,
			      "MP stream receiver emitted early at bit %zu/%zu",
			      index, frame_bit_count);
	}
	CHECK(emitted.type != V34_MP_RECEIVE_NO_EVENT,
	      "MP stream receiver did not emit a completed frame");
	*event = emitted;
	return true;
}

static bool test_stream_receiver(void)
{
	struct v34_mp_parameters type0 = base_parameters(false);
	struct v34_mp_type1_parameters type1 = type1_parameters();
	struct v34_mp_receiver receiver;
	struct v34_mp_receive_event event;
	uint8_t type0_frame[V34_MP_TYPE0_FRAME_BYTES];
	uint8_t type1_frame[V34_MP_TYPE1_FRAME_BYTES];

	CHECK(v34_mp_type0_encode(
		      &type0, type0_frame, sizeof(type0_frame)) ==
		      V34_MP_OK &&
	      v34_mp_type1_encode(
		      &type1, type1_frame, sizeof(type1_frame)) ==
		      V34_MP_OK &&
	      v34_mp_receiver_init(&receiver) ==
		      V34_MP_OK,
	      "MP stream fixture did not initialize");
	/* Model trailing TRN ones and prove a shorter run ending in zero does
	 * not disturb the saturated 17-one MP prefix search. */
	for (size_t index = 0; index < 10; ++index) {
		uint8_t bit = index == 2u ? 0u : 1u;

		CHECK(v34_mp_receive_bit(
			      &receiver, bit, &event) == V34_MP_OK &&
		      event.type == V34_MP_RECEIVE_NO_EVENT,
		      "MP stream preamble produced an event at bit %zu", index);
	}
	CHECK(receive_packed_frame(
		      &receiver, type0_frame,
		      V34_MP_TYPE0_FRAME_BITS, &event) &&
	      event.type == V34_MP_RECEIVE_TYPE0 &&
	      event.frame_result == V34_MP_OK &&
	      parameters_are_equal(&event.parameters.type0, &type0) &&
	      v34_mp_receiver_is_valid(&receiver) &&
	      receiver.stage == V34_MP_SEARCH_SYNC,
	      "MP stream receiver changed a Type 0 record");
	CHECK(receive_packed_frame(
		      &receiver, type1_frame,
		      V34_MP_TYPE1_FRAME_BITS, &event) &&
	      event.type == V34_MP_RECEIVE_TYPE1 &&
	      event.frame_result == V34_MP_OK &&
	      type1_parameters_are_equal(&event.parameters.type1, &type1) &&
	      v34_mp_receiver_is_valid(&receiver) &&
	      receiver.stage == V34_MP_SEARCH_SYNC,
	      "MP stream receiver changed a Type 1 record");
	return true;
}

static bool test_stream_rejection_and_reacquisition(void)
{
	struct v34_mp_parameters parameters =
		base_parameters(false);
	struct v34_mp_receiver receiver;
	struct v34_mp_receive_event event;
	uint8_t valid[V34_MP_TYPE0_FRAME_BYTES];
	uint8_t corrupt[V34_MP_TYPE0_FRAME_BYTES];

	CHECK(v34_mp_type0_encode(
		      &parameters, valid, sizeof(valid)) ==
		      V34_MP_OK &&
	      v34_mp_receiver_init(&receiver) ==
		      V34_MP_OK,
	      "MP rejection stream did not initialize");
	memcpy(corrupt, valid, sizeof(corrupt));
	replace_frame_bit(
		corrupt, 20u, reference_frame_bit(corrupt, 20u) ^ 1u);
	CHECK(receive_packed_frame(
		      &receiver, corrupt, V34_MP_TYPE0_FRAME_BITS,
		      &event) &&
	      event.type == V34_MP_RECEIVE_REJECTED &&
	      event.frame_result == V34_MP_CRC_MISMATCH &&
	      receive_packed_frame(
		      &receiver, valid, V34_MP_TYPE0_FRAME_BITS,
		      &event) &&
	      event.type == V34_MP_RECEIVE_TYPE0 &&
	      parameters_are_equal(&event.parameters.type0, &parameters),
	      "MP stream receiver did not reject and reacquire");
	return true;
}

static bool test_stream_validation(void)
{
	struct v34_mp_receiver receiver;
	struct v34_mp_receiver saved;
	struct v34_mp_receive_event event;
	struct v34_mp_receive_event saved_event;

	CHECK(v34_mp_receiver_init(NULL) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_receiver_init(&receiver) ==
		      V34_MP_OK &&
	      v34_mp_receiver_is_valid(&receiver) &&
	      !v34_mp_receiver_is_valid(NULL),
	      "MP stream validation fixture did not initialize");
	saved = receiver;
	memset(&event, 0xa5, sizeof(event));
	saved_event = event;
	CHECK(v34_mp_receive_bit(NULL, 0, &event) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_receive_bit(&receiver, 2, &event) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_receive_bit(&receiver, 0, NULL) ==
		      V34_MP_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "invalid MP stream input mutated state or event");
	receiver.sync_one_count =
		V34_MP_TYPE0_FRAME_SYNC_BITS + 1u;
	CHECK(!v34_mp_receiver_is_valid(&receiver),
	      "MP stream receiver accepted an excessive sync run");
	receiver = saved;
	receiver.stage = (enum v34_mp_receive_stage)3;
	CHECK(!v34_mp_receiver_is_valid(&receiver),
	      "MP stream receiver accepted an unknown stage");
	receiver = saved;
	receiver.bit_index = 1;
	CHECK(!v34_mp_receiver_is_valid(&receiver),
	      "MP stream receiver accepted a search cursor");
	receiver = saved;
	receiver.frame[0] = 1;
	CHECK(!v34_mp_receiver_is_valid(&receiver),
	      "MP stream receiver accepted stale frame storage");
	return true;
}

static bool test_known_wire_images(void)
{
	static const uint8_t expected_mp[] = {
		0xff, 0xff, 0x85, 0x54, 0x1f, 0xfe,
		0x00, 0x00, 0x07, 0x52, 0xb0,
	};
	static const uint8_t expected_mp_prime[] = {
		0xff, 0xff, 0x85, 0x54, 0x5f, 0xfe,
		0x00, 0x00, 0x02, 0x00, 0x38,
	};
	struct v34_mp_parameters parameters =
		base_parameters(false);
	struct v34_mp_parameters decoded = { 0 };
	uint8_t frame[V34_MP_TYPE0_FRAME_BYTES + 2u];

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      V34_MP_OK &&
	      memcmp(frame, expected_mp, sizeof(expected_mp)) == 0 &&
	      frame[11] == 0xa5 && frame[12] == 0xa5 &&
	      chronological_crc_word(frame, MP_TYPE0_CRC_FIRST_BIT) ==
		      UINT16_C(0x6a57) &&
	      v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "known Type 0 MP wire image changed");

	parameters.acknowledges_peer_mp = true;
	CHECK(v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      V34_MP_OK &&
	      memcmp(frame, expected_mp_prime, sizeof(expected_mp_prime)) == 0 &&
	      chronological_crc_word(frame, MP_TYPE0_CRC_FIRST_BIT) ==
		      UINT16_C(0xe002) &&
	      v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "known Type 0 MP-prime wire image changed");
	return true;
}

static bool test_type1_known_wire_image(void)
{
	static const uint8_t expected[] = {
		0xff, 0xff, 0xa8, 0x7b, 0xd5, 0x54, 0xa2, 0xc4,
		0x87, 0xff, 0xfa, 0x00, 0x05, 0xff, 0xfc, 0x00,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x16, 0x6e, 0x20,
	};
	struct v34_mp_type1_parameters parameters =
		type1_parameters();
	struct v34_mp_type1_parameters decoded = { 0 };
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES + 2u];

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v34_mp_type1_parameters_are_valid(&parameters) &&
	      v34_mp_type1_encode(
		      &parameters, frame, sizeof(frame)) ==
		      V34_MP_OK &&
	      memcmp(frame, expected, sizeof(expected)) == 0 &&
	      frame[24] == 0xa5 && frame[25] == 0xa5 &&
	      (frame[23] & 0x1fu) == 0 &&
	      reference_crc(frame, MP_TYPE1_CRC_FIRST_BIT) ==
		      UINT16_C(0xb371) &&
	      chronological_crc_word(frame, MP_TYPE1_CRC_FIRST_BIT) ==
		      UINT16_C(0x8ecd) &&
	      v34_mp_type1_decode(
		      frame, sizeof(frame), &decoded) ==
		      V34_MP_OK &&
	      type1_parameters_are_equal(&decoded, &parameters),
	      "known Type 1 MP wire image changed");
	return true;
}

static bool test_selected_fields_round_trip(void)
{
	struct v34_mp_parameters parameters = {
		.maximum_call_to_answer_rate_2400_bps_units = 1,
		.maximum_answer_to_call_rate_2400_bps_units = 14,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis =
			V34_TRELLIS_64_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.remote_transmitter_expanded_shaping = true,
		.acknowledges_peer_mp = true,
		.data_rate_capability_mask = 0x2555,
		.asymmetric_rates_enabled = true,
	};
	struct v34_mp_parameters decoded = { 0 };
	uint8_t frame[V34_MP_TYPE0_FRAME_BYTES];

	CHECK(v34_mp_parameters_are_valid(&parameters) &&
	      v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      V34_MP_OK &&
	      v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "selected Type 0 MP fields did not round trip");
	return true;
}

static bool test_reserved_bits_are_ignored(void)
{
	static const size_t reserved_bits[] = { 19, 49, 52, 67 };
	struct v34_mp_parameters parameters =
		base_parameters(false);
	struct v34_mp_parameters decoded = { 0 };
	uint8_t frame[V34_MP_TYPE0_FRAME_BYTES];

	CHECK(v34_mp_type0_encode(
		      &parameters, frame, sizeof(frame)) ==
		      V34_MP_OK,
	      "reserved-bit fixture did not encode");
	for (size_t index = 0; index < ARRAY_LENGTH(reserved_bits); ++index)
		replace_frame_bit(frame, reserved_bits[index], 1);
	replace_crc(frame, MP_TYPE0_CRC_FIRST_BIT);
	CHECK(v34_mp_type0_decode(
		      frame, sizeof(frame), &decoded) ==
		      V34_MP_OK &&
	      parameters_are_equal(&decoded, &parameters),
	      "Type 0 MP decoder interpreted reserved information bits");
	return true;
}

static bool test_frame_rejection(void)
{
	static const size_t framing_bits[] = { 0, 17, 18, 34, 51, 68, 85, 87 };
	struct v34_mp_parameters parameters =
		base_parameters(false);
	struct v34_mp_parameters sentinel = {
		.maximum_call_to_answer_rate_2400_bps_units = 3,
		.maximum_answer_to_call_rate_2400_bps_units = 4,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis =
			V34_TRELLIS_16_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.data_rate_capability_mask = 0x0005,
		.asymmetric_rates_enabled = true,
	};
	uint8_t valid[V34_MP_TYPE0_FRAME_BYTES];

	CHECK(v34_mp_type0_encode(
		      &parameters, valid, sizeof(valid)) ==
		      V34_MP_OK,
	      "frame-rejection fixture did not encode");
	for (size_t index = 0; index < ARRAY_LENGTH(framing_bits); ++index) {
		struct v34_mp_parameters decoded = sentinel;
		uint8_t malformed[sizeof(valid)];

		memcpy(malformed, valid, sizeof(malformed));
		replace_frame_bit(
			malformed, framing_bits[index],
			reference_frame_bit(malformed, framing_bits[index]) ^ 1u);
		CHECK(v34_mp_type0_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      V34_MP_INVALID_FRAME &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted malformed framing bit %zu",
		      framing_bits[index]);
	}
	{
		struct v34_mp_parameters decoded = sentinel;
		uint8_t corrupt[sizeof(valid)];

		memcpy(corrupt, valid, sizeof(corrupt));
		replace_frame_bit(corrupt, 20, reference_frame_bit(corrupt, 20) ^ 1u);
		CHECK(v34_mp_type0_decode(
			      corrupt, sizeof(corrupt), &decoded) ==
			      V34_MP_CRC_MISMATCH &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted a corrupt information bit");
		memcpy(corrupt, valid, sizeof(corrupt));
		replace_frame_bit(
			corrupt, MP_TYPE0_CRC_FIRST_BIT,
			reference_frame_bit(corrupt, MP_TYPE0_CRC_FIRST_BIT) ^ 1u);
		CHECK(v34_mp_type0_decode(
			      corrupt, sizeof(corrupt), &decoded) ==
			      V34_MP_CRC_MISMATCH &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted a corrupt CRC bit");
	}
	{
		static const size_t invalid_field_bits[] = { 29, 30 };
		struct v34_mp_parameters decoded = sentinel;
		uint8_t malformed[sizeof(valid)];

		memcpy(malformed, valid, sizeof(malformed));
		for (size_t index = 0; index < ARRAY_LENGTH(invalid_field_bits);
		     ++index)
			replace_frame_bit(malformed, invalid_field_bits[index], 1);
		replace_crc(malformed, MP_TYPE0_CRC_FIRST_BIT);
		CHECK(v34_mp_type0_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      V34_MP_INVALID_FRAME &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted reserved trellis selection");

		memcpy(malformed, valid, sizeof(malformed));
		for (size_t index = 20; index < 24; ++index)
			replace_frame_bit(malformed, index, 0);
		replace_crc(malformed, MP_TYPE0_CRC_FIRST_BIT);
		decoded = sentinel;
		CHECK(v34_mp_type0_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      V34_MP_INVALID_FRAME &&
		      parameters_are_equal(&decoded, &sentinel),
		      "Type 0 MP accepted a zero maximum rate");
	}
	return true;
}

static bool test_type1_reserved_bits_and_frame_rejection(void)
{
	struct v34_mp_type1_parameters parameters =
		type1_parameters();
	struct v34_mp_type1_parameters sentinel = {
		.precoder_coefficients = {
			{ .real = 1, .imaginary = 2 },
			{ .real = 3, .imaginary = 4 },
			{ .real = 5, .imaginary = 6 },
		},
	};
	uint8_t valid[V34_MP_TYPE1_FRAME_BYTES];
	size_t framing_bits[13];
	size_t framing_bit_count = 0;

	sentinel.common = base_parameters(false);
	CHECK(v34_mp_type1_encode(
		      &parameters, valid, sizeof(valid)) ==
		      V34_MP_OK,
	      "Type 1 frame-rejection fixture did not encode");
	{
		struct v34_mp_type1_parameters decoded = sentinel;
		uint8_t with_reserved_bits[sizeof(valid)];

		memcpy(with_reserved_bits, valid, sizeof(with_reserved_bits));
		for (size_t index = MP_TYPE1_RESERVED_FIRST_BIT;
		     index < MP_TYPE1_RESERVED_FIRST_BIT +
				     MP_TYPE1_RESERVED_BITS;
		     ++index)
			replace_frame_bit(with_reserved_bits, index, 1);
		replace_crc(with_reserved_bits, MP_TYPE1_CRC_FIRST_BIT);
		CHECK(v34_mp_type1_decode(
			      with_reserved_bits, sizeof(with_reserved_bits),
			      &decoded) == V34_MP_OK &&
		      type1_parameters_are_equal(&decoded, &parameters),
		      "Type 1 MP decoder interpreted reserved bits");
	}

	framing_bits[framing_bit_count++] = 0;
	framing_bits[framing_bit_count++] = MP_TYPE_BIT;
	for (size_t index = MP_START0_BIT;
	     index < MP_TYPE1_CRC_FIRST_BIT; index += MP_FIELD_STRIDE_BITS)
		framing_bits[framing_bit_count++] = index;
	framing_bits[framing_bit_count++] = MP_TYPE1_FILL_BIT;
	CHECK(framing_bit_count == ARRAY_LENGTH(framing_bits),
	      "Type 1 framing-bit generator changed");
	for (size_t index = 0; index < framing_bit_count; ++index) {
		struct v34_mp_type1_parameters decoded = sentinel;
		uint8_t malformed[sizeof(valid)];
		size_t bit = framing_bits[index];

		memcpy(malformed, valid, sizeof(malformed));
		replace_frame_bit(
			malformed, bit,
			reference_frame_bit(malformed, bit) ^ 1u);
		CHECK(v34_mp_type1_decode(
			      malformed, sizeof(malformed), &decoded) ==
			      V34_MP_INVALID_FRAME &&
		      type1_parameters_are_equal(&decoded, &sentinel),
		      "Type 1 MP accepted malformed framing bit %zu", bit);
	}
	{
		static const size_t corrupt_bits[] = {
			MP_TYPE1_FIRST_COEFFICIENT_BIT,
			MP_TYPE1_CRC_FIRST_BIT,
		};

		for (size_t index = 0; index < ARRAY_LENGTH(corrupt_bits);
		     ++index) {
			struct v34_mp_type1_parameters decoded =
				sentinel;
			uint8_t corrupt[sizeof(valid)];
			size_t bit = corrupt_bits[index];

			memcpy(corrupt, valid, sizeof(corrupt));
			replace_frame_bit(
				corrupt, bit,
				reference_frame_bit(corrupt, bit) ^ 1u);
			CHECK(v34_mp_type1_decode(
				      corrupt, sizeof(corrupt), &decoded) ==
				      V34_MP_CRC_MISMATCH &&
			      type1_parameters_are_equal(&decoded, &sentinel),
			      "Type 1 MP accepted corrupt bit %zu", bit);
		}
	}
	return true;
}

static bool test_type1_validation(void)
{
	struct v34_mp_type1_parameters valid =
		type1_parameters();
	struct v34_mp_type1_parameters sentinel = {
		.precoder_coefficients = {
			{ .real = 7, .imaginary = 8 },
			{ .real = 9, .imaginary = 10 },
			{ .real = 11, .imaginary = 12 },
		},
	};
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES + 1u];
	uint8_t saved_frame[sizeof(frame)];

	sentinel.common = base_parameters(false);
	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		for (size_t component = 0; component < 2; ++component) {
			struct v34_mp_type1_parameters invalid = valid;

			if (component == 0)
				invalid.precoder_coefficients[tap].real = INT16_MIN;
			else
				invalid.precoder_coefficients[tap].imaginary =
					INT16_MIN;
			CHECK(!v34_mp_type1_parameters_are_valid(
				      &invalid) &&
			      v34_mp_type1_encode(
				      &invalid, frame, sizeof(frame)) ==
				      V34_MP_INVALID_PARAMETERS &&
			      memcmp(frame, saved_frame, sizeof(frame)) == 0,
			      "Type 1 MP accepted Q14 endpoint for tap %zu "
			      "component %zu",
			      tap, component);
		}
	}
	CHECK(!v34_mp_type1_parameters_are_valid(NULL) &&
	      v34_mp_type1_encode(
		      NULL, frame, sizeof(frame)) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_type1_encode(
		      &valid, NULL, sizeof(frame)) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_type1_encode(
		      &valid, frame,
		      V34_MP_TYPE1_FRAME_BYTES - 1u) ==
		      V34_MP_INVALID_ARGUMENT &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid Type 1 MP encoding mutated output");

	CHECK(v34_mp_type1_encode(
		      &valid, frame, sizeof(frame)) == V34_MP_OK,
	      "invalid Type 1 MP decoding fixture did not encode");
	{
		struct v34_mp_type1_parameters decoded = sentinel;

		CHECK(v34_mp_type1_decode(
			      NULL, sizeof(frame), &decoded) ==
			      V34_MP_INVALID_ARGUMENT &&
		      v34_mp_type1_decode(
			      frame, sizeof(frame), NULL) ==
			      V34_MP_INVALID_ARGUMENT &&
		      v34_mp_type1_decode(
			      frame,
			      V34_MP_TYPE1_FRAME_BYTES - 1u,
			      &decoded) == V34_MP_INVALID_ARGUMENT &&
		      type1_parameters_are_equal(&decoded, &sentinel),
		      "invalid Type 1 MP decoding mutated output");
	}
	{
		struct v34_mp_type1_parameters decoded = sentinel;

		replace_lsb_first_field(
			frame, MP_TYPE1_FIRST_COEFFICIENT_BIT,
			MP_COEFFICIENT_COMPONENT_BITS, UINT16_C(0x8000));
		replace_crc(frame, MP_TYPE1_CRC_FIRST_BIT);
		CHECK(v34_mp_type1_decode(
			      frame, sizeof(frame), &decoded) ==
			      V34_MP_INVALID_FRAME &&
		      type1_parameters_are_equal(&decoded, &sentinel),
		      "Type 1 MP decoded the excluded Q14 endpoint");
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_mp_parameters valid = base_parameters(false);
	struct v34_mp_parameters invalid = valid;
	struct v34_mp_parameters decoded = base_parameters(true);
	struct v34_mp_parameters saved_decoded = decoded;
	uint8_t frame[V34_MP_TYPE0_FRAME_BYTES + 1u];
	uint8_t saved_frame[sizeof(frame)];

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	invalid.maximum_call_to_answer_rate_2400_bps_units = 0;
	CHECK(!v34_mp_parameters_are_valid(&invalid) &&
	      v34_mp_type0_encode(
		      &invalid, frame, sizeof(frame)) ==
		      V34_MP_INVALID_PARAMETERS &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "Type 0 MP accepted a zero maximum rate");
	invalid = valid;
	invalid.maximum_answer_to_call_rate_2400_bps_units = 15;
	CHECK(!v34_mp_parameters_are_valid(&invalid),
	      "Type 0 MP accepted an out-of-range maximum rate");
	invalid = valid;
	invalid.remote_transmitter_trellis =
		(enum v34_trellis_selection)3;
	CHECK(!v34_mp_parameters_are_valid(&invalid),
	      "Type 0 MP accepted a reserved trellis selection");
	invalid = valid;
	invalid.data_rate_capability_mask = 0x4000;
	CHECK(!v34_mp_parameters_are_valid(&invalid) &&
	      !v34_mp_parameters_are_valid(NULL),
	      "Type 0 MP accepted capability bits outside its mask");

	CHECK(v34_mp_type0_encode(
		      NULL, frame, sizeof(frame)) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_type0_encode(
		      &valid, NULL, sizeof(frame)) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_type0_encode(
		      &valid, frame,
		      V34_MP_TYPE0_FRAME_BYTES - 1u) ==
		      V34_MP_INVALID_ARGUMENT &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid Type 0 MP encoding mutated output");
	CHECK(v34_mp_type0_decode(
		      NULL, sizeof(frame), &decoded) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_type0_decode(
		      frame, sizeof(frame), NULL) ==
		      V34_MP_INVALID_ARGUMENT &&
	      v34_mp_type0_decode(
		      frame, V34_MP_TYPE0_FRAME_BYTES - 1u,
		      &decoded) == V34_MP_INVALID_ARGUMENT &&
	      parameters_are_equal(&decoded, &saved_decoded),
	      "invalid Type 0 MP decoding mutated output");
	CHECK(strcmp(v34_mp_result_name(
		     V34_MP_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v34_mp_result_name(
		     V34_MP_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(v34_mp_result_name(
		     (enum v34_mp_result)5),
		     "unknown") == 0,
	      "Type 0 MP result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "stream receiver", test_stream_receiver },
		{ "stream rejection and reacquisition",
		  test_stream_rejection_and_reacquisition },
		{ "stream validation", test_stream_validation },
		{ "known wire images", test_known_wire_images },
		{ "Type 1 known wire image", test_type1_known_wire_image },
		{ "selected fields round trip", test_selected_fields_round_trip },
		{ "reserved bits ignored", test_reserved_bits_are_ignored },
		{ "frame rejection", test_frame_rejection },
		{ "Type 1 reserved bits and frame rejection",
		  test_type1_reserved_bits_and_frame_rejection },
		{ "Type 1 validation", test_type1_validation },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
