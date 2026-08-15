// SPDX-License-Identifier: BSD-3-Clause

#include <binary_equalizer.h>

#include "test.h"

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

int main(void)
{
	fprintf(stderr, "[ RUN      ] binary equalizer decisions\n");
	if (!test_decisions()) {
		fprintf(stderr, "[  FAILED  ] binary equalizer decisions\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] binary equalizer decisions\n");
	fprintf(stderr, "[ RUN      ] binary equalizer contracts\n");
	if (!test_contracts()) {
		fprintf(stderr, "[  FAILED  ] binary equalizer contracts\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] binary equalizer contracts\n");
	return 0;
}
