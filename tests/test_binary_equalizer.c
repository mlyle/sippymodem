// SPDX-License-Identifier: BSD-3-Clause

#include <binary_equalizer.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_MAGNITUDE = 4096,
};

static bool trained_equalizer(struct qam_equalizer *equalizer)
{
	const struct qam_point reference = {
		.in_phase = TEST_MAGNITUDE,
	};

	if (qam_equalizer_init(
		    equalizer, &reference, &reference) != QAM_EQUALIZER_OK)
		return false;
	for (size_t index = 0; index <= equalizer->decision_delay; ++index) {
		struct qam_point output;
		bool ready;

		if (qam_equalizer_push(
			    equalizer, &reference, &output, &ready) !=
		    QAM_EQUALIZER_OK)
			return false;
		if (ready && qam_equalizer_adapt(
				     equalizer, &reference, 0.0) !=
				     QAM_EQUALIZER_OK)
			return false;
	}
	return qam_equalizer_is_valid(equalizer) &&
	       equalizer->fill == equalizer->decision_delay + 1u &&
	       !equalizer->can_adapt;
}

static bool check_polarity(int8_t polarity)
{
	struct qam_equalizer trained;
	struct binary_equalizer receiver;
	uint8_t sign;
	int16_t positive = polarity > 0 ? TEST_MAGNITUDE : -TEST_MAGNITUDE;
	int16_t negative = (int16_t)-positive;

	CHECK(trained_equalizer(&trained) &&
	      binary_equalizer_init(
		      &receiver, &trained, TEST_MAGNITUDE, 0.125,
		      polarity) == BINARY_EQUALIZER_OK &&
	      binary_equalizer_is_valid(&receiver),
	      "polarity %d receiver did not initialize", polarity);
	for (size_t index = 0; index < 8u; ++index)
		CHECK(binary_equalizer_receive(
			      &receiver, positive, &sign) ==
			      BINARY_EQUALIZER_OK &&
		      sign == 0u,
		      "positive decision %zu changed for polarity %d",
		      index, polarity);
	for (size_t index = 0;
	     index < receiver.equalizer.decision_delay; ++index)
		CHECK(binary_equalizer_receive(
			      &receiver, negative, &sign) ==
			      BINARY_EQUALIZER_OK &&
		      sign == 0u,
		      "decision delay changed at %zu for polarity %d",
		      index, polarity);
	CHECK(binary_equalizer_receive(&receiver, negative, &sign) ==
		      BINARY_EQUALIZER_OK &&
	      sign == 1u,
	      "negative decision changed for polarity %d", polarity);
	return true;
}

static bool test_decisions(void)
{
	CHECK(check_polarity(1), "normal polarity failed");
	CHECK(check_polarity(-1), "inverted polarity failed");
	return true;
}

static bool test_contracts(void)
{
	struct qam_equalizer trained;
	struct qam_equalizer invalid;
	struct binary_equalizer receiver;
	struct binary_equalizer before;
	uint8_t sign = 0xa5u;
	static const char *const names[] = {
		"ok", "invalid argument", "invalid state", "internal error",
	};

	CHECK(trained_equalizer(&trained),
	      "contract equalizer did not initialize");
	invalid = trained;
	invalid.fill--;
	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(binary_equalizer_init(
		      NULL, &trained, TEST_MAGNITUDE, 0.0, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, NULL, TEST_MAGNITUDE, 0.0, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, &invalid, TEST_MAGNITUDE, 0.0, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, &trained, 0.0, 0.0, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, &trained, NAN, 0.0, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, &trained, TEST_MAGNITUDE, -0.1, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, &trained, TEST_MAGNITUDE, 1.1, 1) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_init(
		      &receiver, &trained, TEST_MAGNITUDE, 0.0, 0) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid initialization changed its destination");
	CHECK(binary_equalizer_init(
		      &receiver, &trained, TEST_MAGNITUDE, 0.0, 1) ==
		      BINARY_EQUALIZER_OK,
	      "contract receiver did not initialize");
	before = receiver;
	CHECK(binary_equalizer_receive(NULL, 0, &sign) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      binary_equalizer_receive(&receiver, 0, NULL) ==
		      BINARY_EQUALIZER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      sign == 0xa5u,
	      "invalid receive changed state or output");
	receiver.polarity = 0;
	before = receiver;
	CHECK(!binary_equalizer_is_valid(NULL) &&
	      !binary_equalizer_is_valid(&receiver) &&
	      binary_equalizer_receive(&receiver, 0, &sign) ==
		      BINARY_EQUALIZER_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      sign == 0xa5u,
	      "corrupt receiver was not rejected transactionally");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      binary_equalizer_result_name(
				      (enum binary_equalizer_result)index),
			      names[index]) == 0,
		      "result name %zu changed", index);
	CHECK(strcmp(
		      binary_equalizer_result_name(
			      (enum binary_equalizer_result)-1),
		      "unknown") == 0,
	      "unknown result name changed");
	return true;
}

/* Immediate decisions must agree with an observation retained for one step. */
static bool check_immediate_adaptation(
	unsigned tap_count, bool tracking, int8_t polarity, double adaptation)
{
	static const int16_t samples[] = {
		INT16_MIN, -4096, -1, 0, 1, 4096, INT16_MAX,
	};
	const struct qam_point reference = { .in_phase = TEST_MAGNITUDE };
	struct qam_equalizer trained;
	struct binary_equalizer receiver;
	unsigned delay = tap_count - 1u;

	CHECK(qam_equalizer_init_geometry(&trained, &reference, &reference,
	      tap_count, delay) == QAM_EQUALIZER_OK, "immediate test geometry failed");
	for (unsigned i = 0; i <= delay; ++i) {
		struct qam_point output;
		bool ready;
		CHECK(qam_equalizer_push(&trained, &reference, &output, &ready) ==
		      QAM_EQUALIZER_OK && (!ready || qam_equalizer_adapt(
		      &trained, &reference, 0.0) == QAM_EQUALIZER_OK),
		      "immediate test history failed");
	}
	if (tracking) {
		CHECK(qam_equalizer_enable_carrier_tracking(&trained, .01, .001) ==
		      QAM_EQUALIZER_OK && qam_equalizer_set_carrier_error_limit(
		      &trained, .25) == QAM_EQUALIZER_OK, "carrier setup failed");
		trained.carrier_phase = .031;
		trained.carrier_frequency = .002;
	}
	CHECK(binary_equalizer_init(&receiver, &trained, TEST_MAGNITUDE,
	      adaptation, polarity) == BINARY_EQUALIZER_OK, "receiver setup failed");
	for (size_t i = 0; i < 4u * ARRAY_LENGTH(samples); ++i) {
		int16_t sample = samples[i % ARRAY_LENGTH(samples)];
		struct qam_point input = { .in_phase = polarity * (double)sample };
		struct qam_point target;
		struct qam_equalizer_observation observation = { 0 };
		bool ready;
		uint8_t sign = 0xa5;

		CHECK(qam_equalizer_observe(&trained, &input, &observation, &ready) ==
		      QAM_EQUALIZER_OK && ready, "retained observation failed");
		target = (struct qam_point){ .in_phase =
			observation.output.in_phase < 0.0 ? -TEST_MAGNITUDE : TEST_MAGNITUDE };
		CHECK(qam_equalizer_adapt_observation(&trained, &observation, &target,
		      adaptation) == QAM_EQUALIZER_OK &&
		      binary_equalizer_receive(&receiver, sample, &sign) == BINARY_EQUALIZER_OK &&
		      sign == (uint8_t)(target.in_phase < 0.0) &&
		      memcmp(&receiver.equalizer, &trained, sizeof(trained)) == 0,
		      "immediate/retained mismatch at step %zu", i);
	}
	return true;
}

static bool test_immediate_adaptation(void)
{
	static const unsigned spans[] = { 1, 7, QAM_EQUALIZER_MAX_TAP_COUNT };
	static const double adaptations[] = { 0.0, 0.125, 1.0 };

	for (size_t span = 0; span < ARRAY_LENGTH(spans); ++span)
		for (unsigned tracking = 0; tracking < 2; ++tracking)
			for (int polarity = -1; polarity <= 1; polarity += 2)
				for (size_t a = 0; a < ARRAY_LENGTH(adaptations); ++a)
					CHECK(check_immediate_adaptation(spans[span], tracking,
					      (int8_t)polarity, adaptations[a]),
					      "span %u tracking %u polarity %d adaptation %g",
					      spans[span], tracking, polarity, adaptations[a]);
	return true;
}

static bool test_failed_adaptation_preserves_owner(void)
{
	struct qam_equalizer trained;
	struct binary_equalizer receiver, before;
	uint8_t sign = 0xa5;

	CHECK(trained_equalizer(&trained), "failure test training failed");
	/* Finite history is accepted, but its squared adaptation energy overflows. */
	trained.history[1].in_phase = DBL_MAX / 4.0;
	CHECK(binary_equalizer_init(&receiver, &trained, TEST_MAGNITUDE, .125, 1) ==
	      BINARY_EQUALIZER_OK, "failure test owner rejected prematurely");
	before = receiver;
	CHECK(binary_equalizer_receive(&receiver, TEST_MAGNITUDE, &sign) ==
	      BINARY_EQUALIZER_INTERNAL_ERROR && sign == 0xa5 &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "failed immediate adaptation changed owner or output");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] binary equalizer decisions\n");
	if (!test_decisions()) {
		fprintf(stderr, "[  FAILED  ] binary equalizer decisions\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] binary equalizer decisions\n");
	fprintf(stderr, "[ RUN      ] binary equalizer contracts\n");
	if (!test_contracts() || !test_immediate_adaptation() ||
	    !test_failed_adaptation_preserves_owner()) {
		fprintf(stderr, "[  FAILED  ] binary equalizer contracts\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] binary equalizer contracts\n");
	return 0;
}
