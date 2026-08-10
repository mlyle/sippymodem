// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v22bis_receiver.h>

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

static bool test_validation(void)
{
	struct sippymodem_v22bis_receiver receiver = { 0 };
	struct sippymodem_v22bis_receiver saved;
	struct sippymodem_qam_point reference = { 0.8, -0.2 };
	struct sippymodem_qam_point zero = { 0.0, 0.0 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };
	uint8_t bits[4] = { UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX };

	CHECK(sippymodem_v22bis_receiver_init(&receiver, &reference) ==
		      SIPPYMODEM_V22BIS_RECEIVER_OK,
	      "V.22bis receiver validation fixture did not initialize");
	saved = receiver;
	CHECK(sippymodem_v22bis_receiver_init(NULL, &reference) ==
		      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      sippymodem_v22bis_receiver_init(&receiver, &zero) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      sippymodem_v22bis_receiver_decode(
			      &receiver, &invalid, bits) ==
			      SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      bits[0] == UINT8_MAX && bits[1] == UINT8_MAX &&
		      bits[2] == UINT8_MAX && bits[3] == UINT8_MAX,
	      "V.22bis receiver invalid input was not transactional");
	receiver.channel_gain.in_phase = INFINITY;
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
