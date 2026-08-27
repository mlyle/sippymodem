// SPDX-License-Identifier: BSD-3-Clause

#include <v34_info1.h>
#include <v34_phase2_info.h>

#include "test.h"

#include <string.h>

static struct v34_info1c info1c_fixture(void)
{
	return (struct v34_info1c){
		.common = {
			.minimum_peer_power_reduction_db = 5,
			.additional_peer_power_reduction_db = 3,
			.local_md_length_35_ms_units = 85,
			.frequency_offset_1_50_hz_units = -257,
		},
		.answer_transmit = {
			[V34_SYMBOL_RATE_2400] = { true, 10, 14 },
			[V34_SYMBOL_RATE_2743] = { false, 0, 0 },
			[V34_SYMBOL_RATE_2800] = { true, 3, 12 },
			[V34_SYMBOL_RATE_3000] = { false, 10, 7 },
			[V34_SYMBOL_RATE_3200] = { true, 1, 1 },
			[V34_SYMBOL_RATE_3429] = { false, 9, 13 },
		},
	};
}

static struct v34_info1a info1a_fixture(void)
{
	return (struct v34_info1a){
		.common = {
			.minimum_peer_power_reduction_db = 7,
			.additional_peer_power_reduction_db = 4,
			.local_md_length_35_ms_units = 127,
			.frequency_offset_1_50_hz_units = 511,
		},
		.call_transmit = { true, 10, 14 },
		.answer_to_call_symbol_rate = V34_SYMBOL_RATE_3429,
		.call_to_answer_symbol_rate = V34_SYMBOL_RATE_3000,
	};
}

static bool common_equal(
	const struct v34_info1_common *left,
	const struct v34_info1_common *right)
{
	return left->minimum_peer_power_reduction_db ==
		       right->minimum_peer_power_reduction_db &&
	       left->additional_peer_power_reduction_db ==
		       right->additional_peer_power_reduction_db &&
	       left->local_md_length_35_ms_units ==
		       right->local_md_length_35_ms_units &&
	       left->frequency_offset_1_50_hz_units ==
		       right->frequency_offset_1_50_hz_units;
}

static bool probe_result_equal(
	const struct v34_info1_probe_result *left,
	const struct v34_info1_probe_result *right)
{
	return left->high_carrier == right->high_carrier &&
	       left->preemphasis_filter_index ==
		       right->preemphasis_filter_index &&
	       left->projected_data_rate_2400_bps_units ==
		       right->projected_data_rate_2400_bps_units;
}

static bool info1c_equal(
	const struct v34_info1c *left,
	const struct v34_info1c *right)
{
	if (!common_equal(&left->common, &right->common))
		return false;
	for (size_t index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index)
		if (!probe_result_equal(
			    &left->answer_transmit[index],
			    &right->answer_transmit[index]))
			return false;
	return true;
}

static bool info1a_equal(
	const struct v34_info1a *left,
	const struct v34_info1a *right)
{
	return common_equal(&left->common, &right->common) &&
	       probe_result_equal(&left->call_transmit, &right->call_transmit) &&
	       left->answer_to_call_symbol_rate ==
		       right->answer_to_call_symbol_rate &&
	       left->call_to_answer_symbol_rate ==
		       right->call_to_answer_symbol_rate;
}

static bool test_info1c_codec(void)
{
	static const uint8_t expected[] = {
		0xba, 0xad, 0x5c, 0x01, 0xc3,
		0x2f, 0x62, 0x13, 0x7f, 0xe8,
	};
	struct v34_info1c info1c = info1c_fixture();
	struct v34_info1c decoded;
	uint8_t payload[V34_INFO1C_PAYLOAD_BYTES + 1u];

	memset(payload, 0xa5, sizeof(payload));
	CHECK(V34_INFO1C_PAYLOAD_BITS == 77 &&
	      V34_INFO1C_PAYLOAD_BYTES == sizeof(expected) &&
	      v34_phase2_info_frame_bit_count(
		      V34_INFO1C_PAYLOAD_BITS) == 109 &&
	      v34_info1c_is_valid(&info1c) &&
	      v34_info1c_encode(
		      &info1c, payload, sizeof(payload)) ==
		      V34_INFO1_OK &&
	      memcmp(payload, expected, sizeof(expected)) == 0 &&
	      payload[sizeof(expected)] == 0xa5,
	      "INFO1c fields changed transmission order");
	CHECK(v34_info1c_decode(
		      payload, sizeof(payload),
		      V34_INFO1C_PAYLOAD_BITS, &decoded) ==
		      V34_INFO1_OK &&
	      info1c_equal(&decoded, &info1c),
	      "INFO1c fields did not round trip");
	return true;
}

static bool test_info1a_codec(void)
{
	static const uint8_t expected[] = {
		0xe7, 0xfd, 0x5e, 0xef, 0xf8,
	};
	struct v34_info1a info1a = info1a_fixture();
	struct v34_info1a decoded;
	uint8_t payload[V34_INFO1A_PAYLOAD_BYTES + 1u];

	memset(payload, 0xa5, sizeof(payload));
	CHECK(V34_INFO1A_PAYLOAD_BITS == 38 &&
	      V34_INFO1A_PAYLOAD_BYTES == sizeof(expected) &&
	      v34_phase2_info_frame_bit_count(
		      V34_INFO1A_PAYLOAD_BITS) == 70 &&
	      v34_info1a_is_valid(&info1a) &&
	      v34_info1a_encode(
		      &info1a, payload, sizeof(payload)) ==
		      V34_INFO1_OK &&
	      memcmp(payload, expected, sizeof(expected)) == 0 &&
	      payload[sizeof(expected)] == 0xa5,
	      "INFO1a fields changed transmission order");
	CHECK(v34_info1a_decode(
		      payload, sizeof(payload),
		      V34_INFO1A_PAYLOAD_BITS, &decoded) ==
		      V34_INFO1_OK &&
	      info1a_equal(&decoded, &info1a),
	      "INFO1a fields did not round trip");
	return true;
}

static bool test_frequency_offsets(void)
{
	static const int16_t offsets[] = { -512, -511, -1, 0, 1, 511 };

	for (size_t index = 0; index < ARRAY_LENGTH(offsets); ++index) {
		struct v34_info1c info1c = info1c_fixture();
		struct v34_info1a info1a = info1a_fixture();
		struct v34_info1c decoded_c;
		struct v34_info1a decoded_a;
		uint8_t payload_c[V34_INFO1C_PAYLOAD_BYTES];
		uint8_t payload_a[V34_INFO1A_PAYLOAD_BYTES];

		info1c.common.frequency_offset_1_50_hz_units = offsets[index];
		info1a.common.frequency_offset_1_50_hz_units = offsets[index];
		CHECK(v34_info1c_encode(
			      &info1c, payload_c, sizeof(payload_c)) ==
			      V34_INFO1_OK &&
		      v34_info1c_decode(
			      payload_c, sizeof(payload_c),
			      V34_INFO1C_PAYLOAD_BITS, &decoded_c) ==
			      V34_INFO1_OK &&
		      v34_info1a_encode(
			      &info1a, payload_a, sizeof(payload_a)) ==
			      V34_INFO1_OK &&
		      v34_info1a_decode(
			      payload_a, sizeof(payload_a),
			      V34_INFO1A_PAYLOAD_BITS, &decoded_a) ==
			      V34_INFO1_OK &&
		      decoded_c.common.frequency_offset_1_50_hz_units ==
			      offsets[index] &&
		      decoded_a.common.frequency_offset_1_50_hz_units ==
			      offsets[index],
		      "INFO1 frequency offset %d did not round trip",
		      offsets[index]);
	}
	return true;
}

static void set_wire_field(
	uint8_t *payload, size_t first_bit, size_t bit_count, uint16_t value)
{
	for (size_t index = 0; index < bit_count; ++index) {
		uint8_t mask =
			(uint8_t)(1u << (7u - (first_bit + index) % 8u));

		if ((value & (uint16_t)(1u << index)) != 0)
			payload[(first_bit + index) / 8u] |= mask;
		else
			payload[(first_bit + index) / 8u] &= (uint8_t)~mask;
	}
}

static bool test_validation(void)
{
	struct v34_info1c valid_c = info1c_fixture();
	struct v34_info1a valid_a = info1a_fixture();
	struct v34_info1c invalid_c = valid_c;
	struct v34_info1a invalid_a = valid_a;
	struct v34_info1c decoded_c = valid_c;
	struct v34_info1a decoded_a = valid_a;
	uint8_t payload_c[V34_INFO1C_PAYLOAD_BYTES];
	uint8_t payload_a[V34_INFO1A_PAYLOAD_BYTES];
	uint8_t before_c[sizeof(payload_c)];
	uint8_t before_a[sizeof(payload_a)];

	memset(payload_c, 0xa5, sizeof(payload_c));
	memset(payload_a, 0x5a, sizeof(payload_a));
	memcpy(before_c, payload_c, sizeof(before_c));
	memcpy(before_a, payload_a, sizeof(before_a));
	invalid_c.common.frequency_offset_1_50_hz_units = -513;
	invalid_a.answer_to_call_symbol_rate =
		(enum v34_symbol_rate)V34_SYMBOL_RATE_COUNT;
	CHECK(!v34_info1c_is_valid(&invalid_c) &&
	      !v34_info1a_is_valid(&invalid_a) &&
	      v34_info1c_encode(
		      &invalid_c, payload_c, sizeof(payload_c)) ==
		      V34_INFO1_INVALID_PARAMETERS &&
	      v34_info1a_encode(
		      &invalid_a, payload_a, sizeof(payload_a)) ==
		      V34_INFO1_INVALID_PARAMETERS &&
	      memcmp(payload_c, before_c, sizeof(payload_c)) == 0 &&
	      memcmp(payload_a, before_a, sizeof(payload_a)) == 0,
	      "invalid INFO1 parameters mutated encoded output");

	invalid_c = valid_c;
	invalid_c.answer_transmit[0].preemphasis_filter_index = 11;
	invalid_a = valid_a;
	invalid_a.call_transmit.projected_data_rate_2400_bps_units = 15;
	CHECK(!v34_info1c_is_valid(&invalid_c) &&
	      !v34_info1a_is_valid(&invalid_a) &&
	      !v34_info1c_is_valid(NULL) &&
	      !v34_info1a_is_valid(NULL),
	      "INFO1 accepted a reserved field value");

	CHECK(v34_info1c_encode(
		      &valid_c, payload_c, sizeof(payload_c)) ==
		      V34_INFO1_OK &&
	      v34_info1a_encode(
		      &valid_a, payload_a, sizeof(payload_a)) ==
		      V34_INFO1_OK,
	      "invalid-payload fixtures did not encode");
	set_wire_field(payload_c, 14, 4, 11);
	set_wire_field(payload_a, 22, 3, 6);
	CHECK(v34_info1c_decode(
		      payload_c, sizeof(payload_c),
		      V34_INFO1C_PAYLOAD_BITS, &decoded_c) ==
		      V34_INFO1_INVALID_PAYLOAD &&
	      v34_info1a_decode(
		      payload_a, sizeof(payload_a),
		      V34_INFO1A_PAYLOAD_BITS, &decoded_a) ==
		      V34_INFO1_INVALID_PAYLOAD &&
	      info1c_equal(&decoded_c, &valid_c) &&
	      info1a_equal(&decoded_a, &valid_a),
	      "invalid INFO1 payload mutated decoded output");

	CHECK(v34_info1c_encode(
		      &valid_c, payload_c, sizeof(payload_c)) ==
		      V34_INFO1_OK &&
	      v34_info1a_encode(
		      &valid_a, payload_a, sizeof(payload_a)) ==
		      V34_INFO1_OK,
	      "padding fixtures did not encode");
	payload_c[sizeof(payload_c) - 1u] |= 1u;
	payload_a[sizeof(payload_a) - 1u] |= 1u;
	CHECK(v34_info1c_decode(
		      payload_c, sizeof(payload_c),
		      V34_INFO1C_PAYLOAD_BITS, &decoded_c) ==
		      V34_INFO1_INVALID_PAYLOAD &&
	      v34_info1a_decode(
		      payload_a, sizeof(payload_a),
		      V34_INFO1A_PAYLOAD_BITS, &decoded_a) ==
		      V34_INFO1_INVALID_PAYLOAD,
	      "INFO1 accepted noncanonical payload padding");

	CHECK(v34_info1c_encode(
		      NULL, payload_c, sizeof(payload_c)) ==
		      V34_INFO1_INVALID_ARGUMENT &&
	      v34_info1c_encode(
		      &valid_c, NULL, sizeof(payload_c)) ==
		      V34_INFO1_INVALID_ARGUMENT &&
	      v34_info1a_decode(
		      NULL, sizeof(payload_a),
		      V34_INFO1A_PAYLOAD_BITS, &decoded_a) ==
		      V34_INFO1_INVALID_ARGUMENT &&
	      v34_info1a_decode(
		      payload_a, V34_INFO1A_PAYLOAD_BYTES - 1u,
		      V34_INFO1A_PAYLOAD_BITS, &decoded_a) ==
		      V34_INFO1_INVALID_ARGUMENT &&
	      v34_info1c_decode(
		      payload_c, sizeof(payload_c),
		      V34_INFO1C_PAYLOAD_BITS - 1u, &decoded_c) ==
		      V34_INFO1_INVALID_PAYLOAD,
	      "INFO1 validation changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "INFO1c codec", test_info1c_codec },
		{ "INFO1a codec", test_info1a_codec },
		{ "frequency offsets", test_frequency_offsets },
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
