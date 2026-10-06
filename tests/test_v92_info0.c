// SPDX-License-Identifier: BSD-3-Clause

#include <v92_info0.h>

#include "test.h"

#include <string.h>

static struct v34_info0 common(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 3,
		.cme_modem = true,
		.constellation_1664_supported = true,
	};
}

static struct v92_info0d digital(bool capable, bool short_requested)
{
	return (struct v92_info0d){
		.v90 = {
			.common = common(),
			.nominal_transmit_power = 7,
			.maximum_transmit_power = 19,
			.measure_power_at_codec_output = true,
			.pcm_law = V90_PCM_A_LAW,
			.upstream_symbol_rate_3429_supported = true,
		},
		.v92_capable = capable,
		.short_phase2_requested = short_requested,
	};
}

static bool test_role_bits(void)
{
	struct v92_info0a analog = {
		.v90 = common(),
		.v92_capable = true,
	};
	struct v92_info0d headend = digital(true, false);
	struct v92_info0a decoded_a;
	struct v92_info0d decoded_d;
	uint8_t v90_a[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t v90_d[V90_INFO0D_PAYLOAD_BYTES];
	uint8_t v92_a[V90_INFO0A_PAYLOAD_BYTES];
	uint8_t v92_d[V90_INFO0D_PAYLOAD_BYTES];

	CHECK(v90_info0a_encode(&analog.v90, v90_a, sizeof(v90_a)) ==
		      V90_INFO0_OK &&
	      v90_info0d_encode(&headend.v90, v90_d, sizeof(v90_d)) ==
		      V90_INFO0_OK &&
	      v92_info0a_encode(&analog, v92_a, sizeof(v92_a)) ==
		      V92_INFO0_OK &&
	      v92_info0d_encode(&headend, v92_d, sizeof(v92_d)) ==
		      V92_INFO0_OK,
	      "V.92 INFO0 role fixtures did not encode");
	CHECK((v90_a[1] & UINT8_C(0x03)) == 0 &&
	      (v90_d[1] & UINT8_C(0x03)) == 0 &&
	      (v92_a[1] & UINT8_C(0x03)) == UINT8_C(0x02) &&
	      (v92_d[1] & UINT8_C(0x03)) == UINT8_C(0x01),
	      "V.92 capability used the wrong role-specific bit");
	for (size_t byte = 0; byte < sizeof(v92_a); ++byte)
		CHECK((v92_a[byte] & (byte == 1 ? UINT8_C(0xfc) : UINT8_MAX)) ==
			      v90_a[byte],
		      "INFO0a extension changed base byte %zu", byte);
	for (size_t byte = 0; byte < sizeof(v92_d); ++byte)
		CHECK((v92_d[byte] & (byte == 1 ? UINT8_C(0xfc) : UINT8_MAX)) ==
			      v90_d[byte],
		      "INFO0d extension changed base byte %zu", byte);
	analog.short_phase2_requested = true;
	headend.short_phase2_requested = true;
	CHECK(v92_info0a_encode(&analog, v92_a, sizeof(v92_a)) ==
		      V92_INFO0_OK &&
	      v92_info0d_encode(&headend, v92_d, sizeof(v92_d)) ==
		      V92_INFO0_OK &&
	      (v92_a[1] & UINT8_C(0x03)) == UINT8_C(0x03) &&
	      (v92_d[1] & UINT8_C(0x03)) == UINT8_C(0x03) &&
	      v92_info0a_decode(
		      v92_a, sizeof(v92_a), V90_INFO0A_PAYLOAD_BITS,
		      &decoded_a) == V92_INFO0_OK &&
	      v92_info0d_decode(
		      v92_d, sizeof(v92_d), V90_INFO0D_PAYLOAD_BITS,
		      &decoded_d) == V92_INFO0_OK &&
	      decoded_a.v92_capable && decoded_a.short_phase2_requested &&
	      decoded_d.v92_capable && decoded_d.short_phase2_requested,
	      "V.92 INFO0 short-Phase-2 bits did not round trip");
	return true;
}

static bool test_capability_matrix(void)
{
	for (unsigned int capable = 0; capable <= 1; ++capable) {
		for (unsigned int short_requested = 0;
		     short_requested <= capable; ++short_requested) {
			struct v92_info0a analog = {
				.v90 = common(),
				.v92_capable = capable != 0,
				.short_phase2_requested = short_requested != 0,
			};
			struct v92_info0d headend = digital(
				capable != 0, short_requested != 0);
			struct v92_info0a decoded_a;
			struct v92_info0d decoded_d;
			uint8_t a[V90_INFO0A_PAYLOAD_BYTES];
			uint8_t d[V90_INFO0D_PAYLOAD_BYTES];
			uint8_t repeated_a[V90_INFO0A_PAYLOAD_BYTES];
			uint8_t repeated_d[V90_INFO0D_PAYLOAD_BYTES];

			CHECK(v92_info0a_is_valid(&analog) &&
			      v92_info0d_is_valid(&headend) &&
			      v92_info0a_encode(&analog, a, sizeof(a)) ==
				      V92_INFO0_OK &&
			      v92_info0d_encode(&headend, d, sizeof(d)) ==
				      V92_INFO0_OK &&
			      v92_info0a_decode(
				      a, sizeof(a), V90_INFO0A_PAYLOAD_BITS,
				      &decoded_a) == V92_INFO0_OK &&
			      v92_info0d_decode(
				      d, sizeof(d), V90_INFO0D_PAYLOAD_BITS,
				      &decoded_d) == V92_INFO0_OK &&
			      v92_info0a_equal(&analog, &decoded_a) &&
			      v92_info0d_equal(&headend, &decoded_d) &&
			      v92_info0a_encode(
				      &decoded_a, repeated_a,
				      sizeof(repeated_a)) == V92_INFO0_OK &&
			      v92_info0d_encode(
				      &decoded_d, repeated_d,
				      sizeof(repeated_d)) == V92_INFO0_OK &&
			      memcmp(a, repeated_a, sizeof(a)) == 0 &&
			      memcmp(d, repeated_d, sizeof(d)) == 0,
			      "capability matrix %u/%u did not round trip",
			      capable, short_requested);
			decoded_a.v92_capable = decoded_d.v92_capable = !analog.v92_capable;
			CHECK(!v92_info0a_equal(&analog, &decoded_a) && !v92_info0d_equal(&headend, &decoded_d),
			      "INFO0 comparison ignored V.92 capability");
			decoded_a.v92_capable = decoded_d.v92_capable = analog.v92_capable;
			decoded_a.short_phase2_requested = !analog.short_phase2_requested;
			decoded_d.short_phase2_requested = !headend.short_phase2_requested;
			CHECK(!v92_info0a_equal(&analog, &decoded_a) && !v92_info0d_equal(&headend, &decoded_d),
			      "INFO0 comparison ignored short Phase 2");
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v92_info0a invalid = {
		.v90 = common(),
		.short_phase2_requested = true,
	};
	struct v92_info0a decoded = {
		.v90 = common(),
		.v92_capable = true,
	};
	struct v92_info0a saved = decoded;
	uint8_t payload[V90_INFO0A_PAYLOAD_BYTES] = {
		UINT8_C(0xa5), UINT8_C(0xa5), UINT8_C(0xa5),
	};
	uint8_t unchanged[sizeof(payload)];

	memcpy(unchanged, payload, sizeof(payload));
	CHECK(!v92_info0a_is_valid(&invalid) &&
	      v92_info0a_encode(&invalid, payload, sizeof(payload)) ==
		      V92_INFO0_INVALID_CAPABILITIES &&
	      memcmp(payload, unchanged, sizeof(payload)) == 0,
	      "short Phase 2 was accepted without V.92 capability");
	CHECK(v90_info0a_encode(&invalid.v90, payload, sizeof(payload)) ==
		      V90_INFO0_OK,
	      "malformed extension fixture did not encode");
	payload[1] |= UINT8_C(0x01);
	CHECK(v92_info0a_decode(
		      payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded) == V92_INFO0_INVALID_PAYLOAD &&
	      v92_info0a_equal(&decoded, &saved),
	      "malformed extension changed the decoded record");
	CHECK(v92_info0a_encode(NULL, payload, sizeof(payload)) ==
		      V92_INFO0_INVALID_ARGUMENT &&
	      v92_info0a_encode(&saved, NULL, sizeof(payload)) ==
		      V92_INFO0_INVALID_ARGUMENT &&
	      v92_info0a_decode(
		      NULL, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      &decoded) == V92_INFO0_INVALID_ARGUMENT &&
	      v92_info0a_decode(
		      payload, sizeof(payload), V90_INFO0A_PAYLOAD_BITS,
		      NULL) == V92_INFO0_INVALID_ARGUMENT &&
	      !v92_info0a_is_valid(NULL) && !v92_info0d_is_valid(NULL) &&
	      strcmp(v92_info0_result_name(V92_INFO0_INVALID_PAYLOAD),
		     "invalid payload") == 0 &&
	      strcmp(v92_info0_result_name((enum v92_info0_result)4),
		     "unknown") == 0,
	      "V.92 INFO0 validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "role-specific extension bits", test_role_bits },
		{ "capability matrix", test_capability_matrix },
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
