// SPDX-License-Identifier: BSD-3-Clause

#include <v22bis_receiver.h>

#include "test.h"

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

static struct sippymodem_qam_point complex_multiply(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return (struct sippymodem_qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static struct sippymodem_qam_point quadrant_point(uint8_t quadrant)
{
	switch (quadrant) {
	case 0:
		return (struct sippymodem_qam_point){ 1.0, 0.0 };
	case 1:
		return (struct sippymodem_qam_point){ 0.0, 1.0 };
	case 2:
		return (struct sippymodem_qam_point){ -1.0, 0.0 };
	default:
		return (struct sippymodem_qam_point){ 0.0, -1.0 };
	}
}

static double fixture_noise(uint32_t *random)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.004;
}

static bool check_rotation(uint8_t initial_quadrant)
{
	static const struct sippymodem_qam_point drift = {
		.in_phase = 1.000002,
		.quadrature = 0.00004,
	};
	struct sippymodem_v22bis_symbol_encoder encoder;
	struct sippymodem_v22bis_receiver receiver;
	struct sippymodem_qam_point channel = { 0.72, -0.31 };
	struct sippymodem_qam_point reference =
		quadrant_point(initial_quadrant);
	struct sippymodem_qam_point received_reference =
		complex_multiply(&channel, &reference);
	uint32_t random = UINT32_C(0x2400a11f) + initial_quadrant;

	CHECK(sippymodem_v22bis_symbol_encoder_init(
		      &encoder, initial_quadrant) ==
		      SIPPYMODEM_V22_FRAMING_OK &&
		      sippymodem_v22bis_receiver_init(
			      &receiver, &received_reference) ==
			      SIPPYMODEM_V22BIS_RECEIVER_OK,
	      "V.22bis receiver rotation %u did not initialize",
	      initial_quadrant);
	for (size_t index = 0; index < TEST_SYMBOLS_PER_ROTATION; ++index) {
		struct sippymodem_qam_point transmitted;
		struct sippymodem_qam_point received;
		uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL];
		uint8_t decoded[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL];
		uint8_t symbol;

		for (size_t bit = 0; bit < ARRAY_LENGTH(bits); ++bit)
			bits[bit] = (uint8_t)(fixture_random(&random) >> 31);
		CHECK(sippymodem_v22bis_symbol_encode(
			      &encoder, bits, &symbol) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      sippymodem_v22bis_constellation_point(
				      symbol, &transmitted) ==
				      SIPPYMODEM_V22_FRAMING_OK,
		      "V.22bis transmit fixture failed at %u/%zu",
		      initial_quadrant, index);
		channel = complex_multiply(&channel, &drift);
		received = complex_multiply(&channel, &transmitted);
		received.in_phase += fixture_noise(&random);
		received.quadrature += fixture_noise(&random);
		CHECK(sippymodem_v22bis_receiver_decode(
			      &receiver, &received, decoded) ==
			      SIPPYMODEM_V22BIS_RECEIVER_OK &&
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

static bool test_v22_transition_tracking(void)
{
	static const uint8_t turns[] = { 1, 3, 0, 2, 3, 1, 1, 0 };
	struct sippymodem_v22bis_symbol_encoder encoder;
	struct sippymodem_v22bis_receiver receiver;
	struct sippymodem_qam_point channel = { 0.61, 0.27 };
	uint8_t quadrant = 2;
	struct sippymodem_qam_point ideal = quadrant_point(quadrant);
	struct sippymodem_qam_point received =
		complex_multiply(&channel, &ideal);
	bool matches = false;

	CHECK(sippymodem_v22bis_receiver_init(&receiver, &received) ==
		      SIPPYMODEM_V22BIS_RECEIVER_OK,
	      "V.22bis transition receiver did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(turns); ++index) {
		quadrant = (uint8_t)((quadrant + turns[index]) & 3u);
		ideal = quadrant_point(quadrant);
		received = complex_multiply(&channel, &ideal);
		CHECK(sippymodem_v22bis_receiver_matches_v22(
			      &receiver, &received, &matches) ==
			      SIPPYMODEM_V22BIS_RECEIVER_OK &&
			      matches &&
			      sippymodem_v22bis_receiver_track_v22(
				      &receiver, turns[index], &received) ==
				      SIPPYMODEM_V22BIS_RECEIVER_OK,
		      "V.22 reference tracking failed at symbol %zu", index);
	}
	{
		uint8_t bits[4] = { 1, 1, 1, 1 };
		uint8_t decoded[4];
		uint8_t symbol;

		CHECK(sippymodem_v22bis_symbol_encoder_init(
			      &encoder, quadrant) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      sippymodem_v22bis_symbol_encode(
				      &encoder, bits, &symbol) ==
				      SIPPYMODEM_V22_FRAMING_OK &&
			      sippymodem_v22bis_constellation_point(
				      symbol, &ideal) ==
				      SIPPYMODEM_V22_FRAMING_OK,
		      "V.22bis transition symbol was not generated");
		received = complex_multiply(&channel, &ideal);
		CHECK(sippymodem_v22bis_receiver_matches_v22(
			      &receiver, &received, &matches) ==
			      SIPPYMODEM_V22BIS_RECEIVER_OK &&
			      !matches &&
			      sippymodem_v22bis_receiver_decode(
				      &receiver, &received, decoded) ==
				      SIPPYMODEM_V22BIS_RECEIVER_OK &&
			      memcmp(bits, decoded, sizeof(bits)) == 0 &&
			      sippymodem_v22bis_receiver_track_v22(
				      &receiver, 0, &received) ==
				      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT,
		      "V.22bis transition was not detected cleanly");
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v22bis_receiver receiver = { 0 };
	struct sippymodem_v22bis_receiver saved;
	struct sippymodem_qam_point reference = { 0.8, -0.2 };
	struct sippymodem_qam_point zero = { 0.0, 0.0 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };
	uint8_t bits[4] = { UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX };
	bool matches = true;

	CHECK(sippymodem_v22bis_receiver_init(&receiver, &reference) ==
		      SIPPYMODEM_V22BIS_RECEIVER_OK,
	      "V.22bis receiver validation fixture did not initialize");
	saved = receiver;
	CHECK(sippymodem_v22bis_receiver_is_valid(&receiver) &&
		      !sippymodem_v22bis_receiver_is_valid(NULL) &&
		      sippymodem_v22bis_receiver_init(NULL, &reference) ==
		      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v22bis_receiver_init(&receiver, &zero) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v22bis_receiver_matches_v22(
			      NULL, &reference, &matches) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      matches &&
		      sippymodem_v22bis_receiver_matches_v22(
			      &receiver, &reference, NULL) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v22bis_receiver_track_v22(
			      &receiver, SIPPYMODEM_QPSK_QUADRANT_COUNT,
			      &reference) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      sippymodem_v22bis_receiver_decode(
			      &receiver, &invalid, bits) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      bits[0] == UINT8_MAX && bits[1] == UINT8_MAX &&
		      bits[2] == UINT8_MAX && bits[3] == UINT8_MAX,
	      "V.22bis receiver invalid input was not transactional");
	receiver.gain.channel_gain.in_phase = INFINITY;
	CHECK(sippymodem_v22bis_receiver_decode(
		      &receiver, &reference, bits) ==
		      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      strcmp(sippymodem_v22bis_receiver_result_name(
			     SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT),
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
