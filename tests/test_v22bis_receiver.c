// SPDX-License-Identifier: BSD-3-Clause

#include <v22bis_receiver.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum {
	TEST_SYMBOLS_PER_ROTATION = 512,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static struct qam_point complex_multiply(
	const struct qam_point *left,
	const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static double fixture_noise(uint32_t *random)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.004;
}

static bool check_rotation(uint8_t initial_quadrant)
{
	static const struct qam_point drift = {
		.in_phase = 1.000002,
		.quadrature = 0.00004,
	};
	struct v22_symbol_encoder encoder;
	struct v22bis_receiver receiver;
	struct qam_point channel = { 0.72, -0.31 };
	struct qam_point reference;
	struct qam_point received_reference;
	uint32_t random = UINT32_C(0x2400a11f) + initial_quadrant;

	CHECK(v22bis_1200_constellation_point(
		      initial_quadrant, &reference) == V22_FRAMING_OK &&
		      v22_symbol_encoder_init(
			      &encoder, initial_quadrant) == V22_FRAMING_OK,
	      "V.22bis receiver rotation %u reference was not generated",
	      initial_quadrant);
	received_reference = complex_multiply(&channel, &reference);
	CHECK(v22bis_receiver_init(
			      &receiver, &received_reference) ==
			      V22BIS_RECEIVER_OK,
	      "V.22bis receiver rotation %u did not initialize",
	      initial_quadrant);
	for (size_t index = 0; index < TEST_SYMBOLS_PER_ROTATION; ++index) {
		struct qam_point transmitted;
		struct qam_point received;
		uint8_t bits[V22BIS_BITS_PER_SYMBOL];
		uint8_t decoded[V22BIS_BITS_PER_SYMBOL];
		uint8_t symbol;

		for (size_t bit = 0; bit < ARRAY_LENGTH(bits); ++bit)
			bits[bit] = (uint8_t)(fixture_random(&random) >> 31);
		CHECK(v22bis_symbol_encode(
			      &encoder, bits, &symbol) ==
			      V22_FRAMING_OK &&
			      v22bis_constellation_point(
				      symbol, &transmitted) ==
				      V22_FRAMING_OK,
		      "V.22bis transmit fixture failed at %u/%zu",
		      initial_quadrant, index);
		channel = complex_multiply(&channel, &drift);
		received = complex_multiply(&channel, &transmitted);
		received.in_phase += fixture_noise(&random);
		received.quadrature += fixture_noise(&random);
		CHECK(v22bis_receiver_decode(
			      &receiver, &received, decoded) ==
			      V22BIS_RECEIVER_OK &&
			      memcmp(bits, decoded, sizeof(bits)) == 0,
		      "V.22bis receiver changed rotation %u symbol %zu",
		      initial_quadrant, index);
	}
	CHECK((uint8_t)((encoder.phase_quadrant - initial_quadrant) & 3u) ==
		      receiver.decoder.phase_quadrant,
	      "V.22bis relative phase diverged for rotation %u",
	      initial_quadrant);
	return true;
}

static bool test_channel_tracking(void)
{
	for (uint8_t quadrant = 0; quadrant < 4; ++quadrant)
		if (!check_rotation(quadrant))
			return false;
	return true;
}

static struct qam_point add_relative_error(
	const struct qam_point *point, double error_amplitude)
{
	return (struct qam_point){
		.in_phase =
			point->in_phase - point->quadrature * error_amplitude,
		.quadrature =
			point->quadrature + point->in_phase * error_amplitude,
	};
}

static bool test_v22_match_threshold(void)
{
	struct v22bis_receiver receiver;
	struct qam_point channel = { 0.61, 0.27 };
	struct qam_point ideal;
	struct qam_point received;
	struct qam_point near;
	struct qam_point far;
	bool matches = false;

	CHECK(v22bis_1200_constellation_point(0, &ideal) ==
		      V22_FRAMING_OK,
	      "V.22bis match reference was not generated");
	received = complex_multiply(&channel, &ideal);
	near = add_relative_error(&received, 0.28);
	far = add_relative_error(&received, 0.29);
	CHECK(v22bis_receiver_init(&receiver, &received) ==
		      V22BIS_RECEIVER_OK &&
		      fabs(receiver.gain.channel_gain.in_phase -
			   channel.in_phase) <= 1e-12 &&
		      fabs(receiver.gain.channel_gain.quadrature -
			   channel.quadrature) <= 1e-12 &&
		      v22bis_receiver_matches_v22(
			      &receiver, &near, &matches) ==
			      V22BIS_RECEIVER_OK &&
		      matches,
	      "V.22bis receiver did not normalize the 01 reference ray");
	CHECK(v22bis_receiver_matches_v22(
		      &receiver, &far, &matches) == V22BIS_RECEIVER_OK &&
		      !matches,
	      "V.22bis receiver accepted excessive V.22 reference error");
	return true;
}

static bool test_v22_transition_tracking(void)
{
	static const uint8_t turns[] = { 1, 3, 0, 2, 3, 1, 1, 0 };
	struct v22_symbol_encoder encoder;
	struct v22bis_receiver receiver;
	struct qam_point channel = { 0.61, 0.27 };
	uint8_t quadrant = 2;
	struct qam_point ideal;
	struct qam_point received;
	bool matches = false;

	CHECK(v22bis_1200_constellation_point(quadrant, &ideal) ==
		      V22_FRAMING_OK,
	      "V.22bis transition reference was not generated");
	received = complex_multiply(&channel, &ideal);
	CHECK(v22bis_receiver_init(&receiver, &received) ==
		      V22BIS_RECEIVER_OK,
	      "V.22bis transition receiver did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(turns); ++index) {
		quadrant = (uint8_t)((quadrant + turns[index]) & 3u);
		CHECK(v22bis_1200_constellation_point(
			      quadrant, &ideal) == V22_FRAMING_OK,
		      "V.22bis transition point was not generated at %zu",
		      index);
		received = complex_multiply(&channel, &ideal);
		CHECK(v22bis_receiver_matches_v22(
			      &receiver, &received, &matches) ==
			      V22BIS_RECEIVER_OK &&
			      matches &&
			      v22bis_receiver_track_v22(
				      &receiver, turns[index], &received) ==
				      V22BIS_RECEIVER_OK,
		      "V.22 reference tracking failed at symbol %zu", index);
	}
	CHECK(v22_symbol_encoder_init(&encoder, quadrant) ==
		      V22_FRAMING_OK,
	      "V.22bis transition encoder did not initialize");
	for (size_t index = 0; index < 64; ++index) {
		uint8_t bits[4] = { 1, 1, 1, 1 };
		uint8_t decoded[4];
		uint8_t symbol;

		CHECK(v22bis_symbol_encode(
				      &encoder, bits, &symbol) ==
				      V22_FRAMING_OK &&
			      v22bis_constellation_point(
				      symbol, &ideal) ==
				      V22_FRAMING_OK,
		      "V.22bis transition symbol was not generated");
		received = complex_multiply(&channel, &ideal);
		if (index == 0)
			CHECK(v22bis_receiver_matches_v22(
				      &receiver, &received, &matches) ==
				      V22BIS_RECEIVER_OK &&
				      !matches,
			      "V.22bis transition was not detected");
		CHECK(v22bis_receiver_decode(
				      &receiver, &received, decoded) ==
				      V22BIS_RECEIVER_OK &&
			      memcmp(bits, decoded, sizeof(bits)) == 0,
		      "V.22bis confirmation failed at symbol %zu", index);
	}
	CHECK(v22bis_receiver_track_v22(
		      &receiver, 0, &received) ==
		      V22BIS_RECEIVER_INVALID_ARGUMENT,
	      "V.22bis receiver resumed V.22 tracking after transition");
	return true;
}

static bool test_validation(void)
{
	struct v22bis_receiver receiver = { 0 };
	struct v22bis_receiver saved;
	struct qam_point reference = { 0.8, -0.2 };
	struct qam_point zero = { 0.0, 0.0 };
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point overflow = { DBL_MAX, 0.0 };
	uint8_t bits[4] = { UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX };
	bool matches = true;

	CHECK(v22bis_receiver_init(&receiver, &reference) ==
		      V22BIS_RECEIVER_OK,
	      "V.22bis receiver validation fixture did not initialize");
	saved = receiver;
	CHECK(v22bis_receiver_is_valid(&receiver) &&
		      !v22bis_receiver_is_valid(NULL) &&
		      v22bis_receiver_init(NULL, &reference) ==
		      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      v22bis_receiver_init(&receiver, &zero) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      v22bis_receiver_matches_v22(
			      NULL, &reference, &matches) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      matches &&
		      v22bis_receiver_matches_v22(
			      &receiver, &reference, NULL) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      v22bis_receiver_track_v22(
			      &receiver, QPSK_QUADRANT_COUNT,
			      &reference) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      v22bis_receiver_decode(
			      &receiver, &invalid, bits) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      v22bis_receiver_decode(&receiver, &overflow, bits) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      v22bis_receiver_matches_v22(&receiver, &overflow, &matches) ==
			      V22BIS_RECEIVER_INVALID_ARGUMENT && matches &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      bits[0] == UINT8_MAX && bits[1] == UINT8_MAX &&
		      bits[2] == UINT8_MAX && bits[3] == UINT8_MAX,
	      "V.22bis receiver invalid input was not transactional");
	receiver.gain.channel_gain.in_phase = INFINITY;
	CHECK(v22bis_receiver_decode(
		      &receiver, &reference, bits) ==
		      V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      strcmp(v22bis_receiver_result_name(
			     V22BIS_RECEIVER_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "V.22bis receiver accepted corrupt state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "adaptive channel tracking", test_channel_tracking },
		{ "V.22 reference threshold", test_v22_match_threshold },
		{ "V.22 transition tracking", test_v22_transition_tracking },
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
