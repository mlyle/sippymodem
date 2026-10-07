// SPDX-License-Identifier: BSD-3-Clause

#include <v92_info1d.h>

#include "test.h"

#include <string.h>

static struct v92_info1d fixture(bool pcm_upstream_supported)
{
	return (struct v92_info1d){
		.v34 = {
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
		},
		.pcm_upstream_supported = pcm_upstream_supported,
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

static bool probes_equal(
	const struct v34_info1_probe_result *left,
	const struct v34_info1_probe_result *right)
{
	return left->high_carrier == right->high_carrier &&
	       left->preemphasis_filter_index ==
		       right->preemphasis_filter_index &&
	       left->projected_data_rate_2400_bps_units ==
		       right->projected_data_rate_2400_bps_units;
}

static bool records_equal(
	const struct v92_info1d *left, const struct v92_info1d *right)
{
	if (left->pcm_upstream_supported != right->pcm_upstream_supported ||
	    !common_equal(&left->v34.common, &right->v34.common))
		return false;
	for (size_t rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate)
		if (!probes_equal(
			    &left->v34.answer_transmit[rate],
			    &right->v34.answer_transmit[rate]))
			return false;
	return true;
}

static bool test_wire_image(void)
{
	static const uint8_t expected_base[] = {
		0xba, 0xad, 0x5c, 0x01, 0xc3,
		0x2f, 0x62, 0x13, 0x7f, 0xe8,
	};
	static const uint8_t expected_pcm[] = {
		0xba, 0xad, 0x5c, 0x01, 0xc3,
		0x2f, 0x62, 0x33, 0x7f, 0xe8,
	};
	struct v92_info1d unsupported = fixture(false);
	struct v92_info1d supported = fixture(true);
	struct v92_info1d decoded;
	struct v34_info1c aliased;
	uint8_t base[V92_INFO1D_PAYLOAD_BYTES];
	uint8_t pcm[V92_INFO1D_PAYLOAD_BYTES];

	CHECK(V92_INFO1D_PAYLOAD_BITS == 77 &&
	      V92_INFO1D_PAYLOAD_BYTES == sizeof(expected_pcm) &&
	      v92_info1d_encode(&unsupported, base, sizeof(base)) ==
		      V92_INFO1D_OK &&
	      v92_info1d_encode(&supported, pcm, sizeof(pcm)) ==
		      V92_INFO1D_OK &&
	      memcmp(base, expected_base, sizeof(base)) == 0 &&
	      memcmp(pcm, expected_pcm, sizeof(pcm)) == 0,
	      "V.92 INFO1d wire image changed");
	for (size_t byte = 0; byte < sizeof(pcm); ++byte)
		CHECK((uint8_t)(base[byte] ^ pcm[byte]) ==
			      (byte == 7 ? UINT8_C(0x20) : 0),
		      "PCM support changed INFO1d byte %zu unexpectedly", byte);
	CHECK(v92_info1d_decode(
		      pcm, sizeof(pcm), V92_INFO1D_PAYLOAD_BITS, &decoded) ==
		      V92_INFO1D_OK &&
	      records_equal(&decoded, &supported) &&
	      !decoded.v34.answer_transmit[V34_SYMBOL_RATE_3429]
		       .high_carrier,
	      "V.92 INFO1d did not restore typed PCM capability");
	CHECK(v34_info1c_decode(
		      pcm, sizeof(pcm), V34_INFO1C_PAYLOAD_BITS, &aliased) ==
		      V34_INFO1_OK &&
	      aliased.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier,
	      "V.92 capability did not occupy the V.34 alias bit");
	return true;
}

static bool test_capability_round_trip(void)
{
	for (unsigned int supported = 0; supported <= 1; ++supported) {
		struct v92_info1d original = fixture(supported != 0);
		struct v92_info1d decoded;
		uint8_t encoded[V92_INFO1D_PAYLOAD_BYTES];
		uint8_t repeated[V92_INFO1D_PAYLOAD_BYTES];

		CHECK(v92_info1d_is_valid(&original) &&
		      v92_info1d_encode(
			      &original, encoded, sizeof(encoded)) ==
			      V92_INFO1D_OK &&
		      v92_info1d_decode(
			      encoded, sizeof(encoded),
			      V92_INFO1D_PAYLOAD_BITS, &decoded) ==
			      V92_INFO1D_OK &&
		      records_equal(&decoded, &original) &&
		      v92_info1d_encode(
			      &decoded, repeated, sizeof(repeated)) ==
			      V92_INFO1D_OK &&
		      memcmp(encoded, repeated, sizeof(encoded)) == 0,
		      "V.92 INFO1d capability %u did not round trip",
		      supported);
	}
	return true;
}

static bool test_validation(void)
{
	struct v92_info1d valid = fixture(true);
	struct v92_info1d invalid = valid;
	struct v92_info1d decoded = fixture(false);
	struct v92_info1d saved_decoded = decoded;
	uint8_t payload[V92_INFO1D_PAYLOAD_BYTES + 1u];
	uint8_t saved_payload[sizeof(payload)];

	memset(payload, 0xa5, sizeof(payload));
	memcpy(saved_payload, payload, sizeof(payload));
	invalid.v34.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = true;
	CHECK(!v92_info1d_is_valid(&invalid) &&
	      v92_info1d_encode(&invalid, payload, sizeof(payload)) ==
		      V92_INFO1D_INVALID_PARAMETERS &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "duplicated PCM capability ownership was accepted");
	invalid = valid;
	invalid.v34.answer_transmit[V34_SYMBOL_RATE_3000]
		.preemphasis_filter_index = 11;
	CHECK(!v92_info1d_is_valid(&invalid) &&
	      v92_info1d_encode(&invalid, payload, sizeof(payload)) ==
		      V92_INFO1D_INVALID_PARAMETERS &&
	      v92_info1d_encode(NULL, payload, sizeof(payload)) ==
		      V92_INFO1D_INVALID_ARGUMENT &&
	      v92_info1d_encode(&valid, NULL, sizeof(payload)) ==
		      V92_INFO1D_INVALID_ARGUMENT &&
	      v92_info1d_encode(
		      &valid, payload, V92_INFO1D_PAYLOAD_BYTES - 1u) ==
		      V92_INFO1D_INVALID_ARGUMENT &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "invalid V.92 INFO1d input changed output");

	CHECK(v92_info1d_encode(&valid, payload, sizeof(payload)) ==
		      V92_INFO1D_OK,
	      "malformed INFO1d fixture did not encode");
	payload[V92_INFO1D_PAYLOAD_BYTES - 1u] |= 1u;
	CHECK(v92_info1d_decode(
		      payload, sizeof(payload), V92_INFO1D_PAYLOAD_BITS,
		      &decoded) == V92_INFO1D_INVALID_PAYLOAD &&
	      v92_info1d_decode(
		      payload, sizeof(payload), V92_INFO1D_PAYLOAD_BITS - 1u,
		      &decoded) == V92_INFO1D_INVALID_PAYLOAD &&
	      v92_info1d_decode(
		      NULL, sizeof(payload), V92_INFO1D_PAYLOAD_BITS,
		      &decoded) == V92_INFO1D_INVALID_ARGUMENT &&
	      v92_info1d_decode(
		      payload, sizeof(payload), V92_INFO1D_PAYLOAD_BITS,
		      NULL) == V92_INFO1D_INVALID_ARGUMENT &&
	      v92_info1d_decode(
		      payload, V92_INFO1D_PAYLOAD_BYTES - 1u,
		      V92_INFO1D_PAYLOAD_BITS, &decoded) ==
		      V92_INFO1D_INVALID_ARGUMENT &&
	      records_equal(&decoded, &saved_decoded) &&
	      !v92_info1d_is_valid(NULL),
	      "malformed V.92 INFO1d changed output or was accepted");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire image", test_wire_image },
		{ "capability round trip", test_capability_round_trip },
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
