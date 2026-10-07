// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "pcm_j_frame.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct v92_jd jd_parameters(void)
{
	return (struct v92_jd){
		.data_rate_capability_mask = UINT32_C(0x2a5a5a),
		.maximum_spectral_shaping_lookahead = 3,
	};
}

static struct v92_jp jp_parameters(void)
{
	return (struct v92_jp){
		.su_phase_extension_q16 = UINT16_C(0x80b0),
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation = V92_CONSTELLATION_4_POINT,
	};
}

static bool jd_equal(const struct v92_jd *left, const struct v92_jd *right)
{
	return left->data_rate_capability_mask ==
		       right->data_rate_capability_mask &&
	       left->maximum_spectral_shaping_lookahead ==
		       right->maximum_spectral_shaping_lookahead;
}

static bool jp_equal(const struct v92_jp *left, const struct v92_jp *right)
{
	return left->su_phase_extension_q16 == right->su_phase_extension_q16 &&
	       left->training_constellation == right->training_constellation &&
	       left->rate_renegotiation_constellation ==
		       right->rate_renegotiation_constellation;
}

static void flip_bit(uint8_t *frame, size_t index)
{
	frame[index / 8u] ^= (uint8_t)(1u << (7u - index % 8u));
}

static bool test_jd_semantics(void)
{
	struct v92_jd selected = jd_parameters();
	uint8_t frame[V92_J_FRAME_BYTES];
	uint32_t payload;

	CHECK(V92_J_FRAME_BITS == 72 && V92_J_FRAME_BYTES == 9 &&
	      V92_JD_RATE_CAPABILITY_BITS == 22 &&
	      v92_jd_is_valid(&selected),
	      "V.92 Jd constants or baseline parameters changed");
	for (uint8_t lookahead = V92_JD_MINIMUM_LOOKAHEAD;
	     lookahead <= V92_JD_MAXIMUM_LOOKAHEAD; ++lookahead) {
		struct v92_jd decoded;

		selected.maximum_spectral_shaping_lookahead = lookahead;
		CHECK(v92_jd_encode(&selected, frame, sizeof(frame)) ==
			      V92_J_OK &&
		      pcm_j_frame_decode(frame, sizeof(frame), &payload) ==
			      PCM_J_FRAME_OK &&
		      payload == (selected.data_rate_capability_mask |
				  (uint32_t)lookahead << 30) &&
		      v92_jd_decode(frame, sizeof(frame), &decoded) ==
			      V92_J_OK &&
		      jd_equal(&selected, &decoded),
		      "V.92 Jd lookahead %u did not preserve its wire fields",
		      lookahead);
	}
	selected.data_rate_capability_mask = 0;
	selected.maximum_spectral_shaping_lookahead = 1;
	CHECK(v92_jd_encode(&selected, frame, sizeof(frame)) == V92_J_OK,
	      "V.92 Jd rejected an empty capability intersection");
	selected.data_rate_capability_mask = V92_JD_RATE_CAPABILITY_MASK;
	CHECK(v92_jd_encode(&selected, frame, sizeof(frame)) == V92_J_OK,
	      "V.92 Jd rejected the complete rate mask");
	return true;
}

static uint32_t expected_jp_payload(const struct v92_jp *jp)
{
	uint32_t payload = jp->su_phase_extension_q16 |
			   UINT32_C(1) << 28;

	if (jp->training_constellation == V92_CONSTELLATION_8_POINT)
		payload |= UINT32_C(1) << 29;
	if (jp->rate_renegotiation_constellation ==
	    V92_CONSTELLATION_8_POINT)
		payload |= UINT32_C(1) << 30;
	return payload;
}

static bool test_jp_wire_and_fields(void)
{
	static const uint8_t default_frame[V92_J_FRAME_BYTES] = {
		0xff, 0xff, 0x80, 0x00, 0x00, 0x01, 0x00, 0x5c, 0x80,
	};
	static const uint16_t phases[] = {
		0, 1, UINT16_C(0x7f9c), UINT16_C(0x80b0), UINT16_MAX,
	};
	struct v92_jp selected = {
		.su_phase_extension_q16 = 0,
		.training_constellation = V92_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation = V92_CONSTELLATION_4_POINT,
	};
	uint8_t frame[V92_J_FRAME_BYTES];
	uint32_t payload;

	CHECK(v92_jp_encode(&selected, frame, sizeof(frame)) == V92_J_OK &&
	      memcmp(frame, default_frame, sizeof(frame)) == 0,
	      "V.92 default Jp wire image or 0x05c8 CRC changed");
	for (size_t index = 0; index < ARRAY_LENGTH(phases); ++index) {
		struct v92_jp decoded;

		selected.su_phase_extension_q16 = phases[index];
		selected.training_constellation =
			(index & 1u) != 0 ? V92_CONSTELLATION_8_POINT :
					  V92_CONSTELLATION_4_POINT;
		selected.rate_renegotiation_constellation =
			(index & 2u) != 0 ? V92_CONSTELLATION_8_POINT :
					  V92_CONSTELLATION_4_POINT;
		CHECK(v92_jp_encode(&selected, frame, sizeof(frame)) ==
			      V92_J_OK &&
		      pcm_j_frame_decode(frame, sizeof(frame), &payload) ==
			      PCM_J_FRAME_OK &&
		      payload == expected_jp_payload(&selected) &&
		      v92_jp_decode(frame, sizeof(frame), &decoded) ==
			      V92_J_OK &&
		      jp_equal(&selected, &decoded),
		      "V.92 Jp case %zu did not preserve its typed fields", index);
	}
	return true;
}

static bool test_reserved_bits_and_record_kind(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jd decoded_jd;
	struct v92_jp jp = jp_parameters();
	struct v92_jp decoded_jp;
	uint8_t frame[V92_J_FRAME_BYTES];
	uint32_t payload;
	uint32_t canonical;

	canonical = jd.data_rate_capability_mask |
		    (uint32_t)jd.maximum_spectral_shaping_lookahead << 30;
	payload = canonical | (UINT32_C(0x3f) << 22) |
		  (UINT32_C(1) << 29);
	CHECK(pcm_j_frame_encode(payload, frame, sizeof(frame)) ==
		      PCM_J_FRAME_OK &&
	      v92_jd_decode(frame, sizeof(frame), &decoded_jd) == V92_J_OK &&
	      jd_equal(&jd, &decoded_jd) &&
	      v92_jd_encode(&decoded_jd, frame, sizeof(frame)) == V92_J_OK &&
	      pcm_j_frame_decode(frame, sizeof(frame), &payload) ==
		      PCM_J_FRAME_OK &&
	      payload == canonical,
	      "V.92 Jd interpreted or retained reserved bits");

	canonical = expected_jp_payload(&jp);
	payload = canonical | (UINT32_C(0xfff) << 16) |
		  (UINT32_C(1) << 31);
	CHECK(pcm_j_frame_encode(payload, frame, sizeof(frame)) ==
		      PCM_J_FRAME_OK &&
	      v92_jp_decode(frame, sizeof(frame), &decoded_jp) == V92_J_OK &&
	      jp_equal(&jp, &decoded_jp) &&
	      v92_jp_encode(&decoded_jp, frame, sizeof(frame)) == V92_J_OK &&
	      pcm_j_frame_decode(frame, sizeof(frame), &payload) ==
		      PCM_J_FRAME_OK &&
	      payload == canonical,
	      "V.92 Jp interpreted or retained reserved bits");

	CHECK(v92_jd_encode(&jd, frame, sizeof(frame)) == V92_J_OK &&
	      v92_jp_decode(frame, sizeof(frame), &decoded_jp) ==
		      V92_J_WRONG_RECORD &&
	      jp_equal(&decoded_jp, &jp) &&
	      v92_jp_encode(&jp, frame, sizeof(frame)) == V92_J_OK &&
	      v92_jd_decode(frame, sizeof(frame), &decoded_jd) ==
		      V92_J_WRONG_RECORD &&
	      jd_equal(&decoded_jd, &jd),
	      "V.92 Jd and Jp identifiers were not distinguished");
	return true;
}

static bool test_rejection_and_contracts(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jd decoded_jd = jd;
	struct v92_jd saved_jd = decoded_jd;
	struct v92_jp jp = jp_parameters();
	struct v92_jp decoded_jp = jp;
	struct v92_jp saved_jp = decoded_jp;
	uint8_t frame[V92_J_FRAME_BYTES + 1u];
	uint8_t saved_frame[sizeof(frame)];

	CHECK(v92_jd_encode(&jd, frame, sizeof(frame)) == V92_J_OK,
	      "V.92 J rejection fixture did not encode");
	flip_bit(frame, 18);
	CHECK(v92_jd_decode(frame, V92_J_FRAME_BYTES, &decoded_jd) ==
		      V92_J_CRC_MISMATCH &&
	      jd_equal(&decoded_jd, &saved_jd),
	      "V.92 Jd accepted or published a corrupt payload");
	CHECK(v92_jp_encode(&jp, frame, sizeof(frame)) == V92_J_OK,
	      "V.92 Jp rejection fixture did not encode");
	flip_bit(frame, 0);
	CHECK(v92_jp_decode(frame, V92_J_FRAME_BYTES, &decoded_jp) ==
		      V92_J_INVALID_FRAME &&
	      jp_equal(&decoded_jp, &saved_jp),
	      "V.92 Jp accepted or published malformed framing");

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	jd.data_rate_capability_mask =
		UINT32_C(1) << V92_JD_RATE_CAPABILITY_BITS;
	CHECK(!v92_jd_is_valid(&jd) &&
	      v92_jd_encode(&jd, frame, sizeof(frame)) ==
		      V92_J_INVALID_PARAMETERS,
	      "V.92 Jd accepted an out-of-range rate mask");
	jd = jd_parameters();
	jd.maximum_spectral_shaping_lookahead = 0;
	CHECK(!v92_jd_is_valid(&jd) &&
	      v92_jd_encode(&jd, frame, sizeof(frame)) ==
		      V92_J_INVALID_PARAMETERS,
	      "V.92 Jd accepted zero lookahead");
	jd.maximum_spectral_shaping_lookahead = 4;
	CHECK(!v92_jd_is_valid(&jd) &&
	      v92_jd_encode(&jd, frame, sizeof(frame)) ==
		      V92_J_INVALID_PARAMETERS,
	      "V.92 Jd accepted excessive lookahead");
	jp.training_constellation = (enum v92_constellation_size)16;
	CHECK(v92_constellation_size_is_valid(V92_CONSTELLATION_4_POINT) &&
	      v92_constellation_size_is_valid(V92_CONSTELLATION_8_POINT) &&
	      !v92_constellation_size_is_valid(
		      (enum v92_constellation_size)0) &&
	      !v92_constellation_size_is_valid(jp.training_constellation) &&
	      !v92_jp_is_valid(&jp) &&
	      v92_jp_encode(&jp, frame, sizeof(frame)) ==
		      V92_J_INVALID_PARAMETERS &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid V.92 J parameters changed output");

	jd = jd_parameters();
	jp = jp_parameters();
	CHECK(v92_jd_encode(NULL, frame, sizeof(frame)) ==
		      V92_J_INVALID_ARGUMENT &&
	      v92_jd_encode(&jd, NULL, sizeof(frame)) ==
		      V92_J_INVALID_ARGUMENT &&
	      v92_jp_encode(&jp, frame, V92_J_FRAME_BYTES - 1u) ==
		      V92_J_INVALID_ARGUMENT &&
	      v92_jd_decode(NULL, V92_J_FRAME_BYTES, &decoded_jd) ==
		      V92_J_INVALID_ARGUMENT &&
	      v92_jd_decode(frame, V92_J_FRAME_BYTES, NULL) ==
		      V92_J_INVALID_ARGUMENT &&
	      v92_jp_decode(frame, V92_J_FRAME_BYTES - 1u, &decoded_jp) ==
		      V92_J_INVALID_ARGUMENT &&
	      jd_equal(&decoded_jd, &saved_jd) &&
	      jp_equal(&decoded_jp, &saved_jp) &&
	      !v92_jd_is_valid(NULL) && !v92_jp_is_valid(NULL),
	      "V.92 J argument validation changed state");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "Jd semantics", test_jd_semantics },
		{ "Jp wire and fields", test_jp_wire_and_fields },
		{ "reserved bits and record kind",
		  test_reserved_bits_and_record_kind },
		{ "rejection and contracts", test_rejection_and_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
