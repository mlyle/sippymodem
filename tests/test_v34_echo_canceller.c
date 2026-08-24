// SPDX-License-Identifier: BSD-3-Clause

#include <v34_echo_canceller.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static int16_t reference_sample(uint64_t epoch)
{
	uint32_t mixed = (uint32_t)epoch * UINT32_C(0x9e3779b9) +
			 UINT32_C(0x34ec4017);

	mixed ^= mixed >> 16;
	mixed *= UINT32_C(0x7feb352d);
	mixed ^= mixed >> 15;
	return (int16_t)((int32_t)(mixed % 12001u) - 6000);
}

static int16_t echo_sample(uint64_t epoch, uint16_t delay)
{
	int32_t first = reference_sample(epoch - (delay - 5u));
	int32_t second = reference_sample(epoch - (delay + 21u));
	int32_t noise = (int32_t)(epoch % 7u) - 3;

	return (int16_t)((-7 * first + 3 * second) / 200 + noise);
}

static bool step(
	struct v34_echo_canceller *canceller, int16_t received,
	int16_t transmit, int16_t *cleaned)
{
	struct v34_echo_prepared_sample prepared;

	return v34_echo_canceller_prepare(
		       canceller, received, cleaned, &prepared) == V34_ECHO_OK &&
	       v34_echo_canceller_commit(
		       canceller, transmit, &prepared) == V34_ECHO_OK;
}

static bool fill_reference(struct v34_echo_canceller *canceller)
{
	for (size_t index = 0; index < V34_ECHO_REFERENCE_SAMPLES; ++index) {
		int16_t cleaned;

		if (!step(
			    canceller, 0,
			    reference_sample(canceller->epoch), &cleaned) ||
		    cleaned != 0)
			return false;
	}
	return true;
}

static bool test_training_and_freeze(void)
{
	enum { DELAY = 1935 };
	struct v34_echo_canceller canceller;
	double input_energy = 0.0;
	double output_energy = 0.0;

	CHECK(v34_echo_canceller_init(&canceller) == V34_ECHO_OK &&
	      fill_reference(&canceller) &&
	      v34_echo_canceller_start_training(&canceller, DELAY) ==
		      V34_ECHO_OK,
	      "echo training fixture did not initialize");
	for (size_t index = 0; index < 5000u; ++index) {
		int16_t received = echo_sample(canceller.epoch, DELAY);
		int16_t cleaned;

		CHECK(step(
			      &canceller, received,
			      reference_sample(canceller.epoch), &cleaned) &&
		      cleaned == received,
		      "training changed receive sample %zu", index);
	}
	CHECK(canceller.train_samples == V34_ECHO_TRAIN_SAMPLES &&
	      canceller.validation_samples == V34_ECHO_VALIDATE_SAMPLES &&
	      v34_echo_canceller_finish_training(&canceller) == V34_ECHO_OK &&
	      canceller.mode == V34_ECHO_ACTIVE,
	      "independent validation did not activate the learned path");
	for (size_t index = 0; index < 4000u; ++index) {
		int16_t received = echo_sample(canceller.epoch, DELAY);
		int16_t cleaned;

		CHECK(step(
			      &canceller, received,
			      reference_sample(canceller.epoch), &cleaned),
		      "active filter stopped at sample %zu", index);
		input_energy += (double)received * received;
		output_energy += (double)cleaned * cleaned;
	}
	CHECK(output_energy * 10.0 < input_energy,
	      "frozen echo suppression was too weak: %.6f",
	      output_energy / input_energy);
	return true;
}

static bool test_unproven_paths_stay_disabled(void)
{
	struct v34_echo_canceller silent;
	struct v34_echo_canceller interrupted;

	CHECK(v34_echo_canceller_init(&silent) == V34_ECHO_OK &&
	      fill_reference(&silent) &&
	      v34_echo_canceller_start_training(&silent, 13) == V34_ECHO_OK,
	      "silent fixture did not initialize");
	for (size_t index = 0; index < 5000u; ++index) {
		int16_t cleaned;

		CHECK(step(
			      &silent, 0, reference_sample(silent.epoch),
			      &cleaned),
		      "silent training stopped at sample %zu", index);
	}
	CHECK(v34_echo_canceller_finish_training(&silent) == V34_ECHO_OK &&
	      silent.mode == V34_ECHO_DISABLED,
	      "a zero-energy path activated cancellation");

	CHECK(v34_echo_canceller_init(&interrupted) == V34_ECHO_OK &&
	      fill_reference(&interrupted) &&
	      v34_echo_canceller_start_training(&interrupted, 13) ==
		      V34_ECHO_OK,
	      "interrupted fixture did not initialize");
	for (size_t index = 0; index < 5000u; ++index) {
		int16_t received = index < 2500u ? 0 :
			(int16_t)((index & 1u) != 0 ? 7000 : -7000);
		int16_t cleaned;

		CHECK(step(
			      &interrupted, received,
			      reference_sample(interrupted.epoch), &cleaned),
		      "quiet interruption stopped at sample %zu", index);
	}
	CHECK(interrupted.train_samples < V34_ECHO_TRAIN_SAMPLES &&
	      v34_echo_canceller_finish_training(&interrupted) ==
		      V34_ECHO_OK &&
	      interrupted.mode == V34_ECHO_DISABLED,
	      "a remote response tone completed echo training");
	return true;
}

static bool test_transactionality(void)
{
	struct v34_echo_canceller canceller;
	struct v34_echo_canceller saved;
	struct v34_echo_prepared_sample prepared;
	struct v34_echo_prepared_sample prepared_saved;
	int16_t cleaned = 1234;

	memset(&canceller, 0xa5, sizeof(canceller));
	saved = canceller;
	CHECK(v34_echo_canceller_init(NULL) == V34_ECHO_INVALID_ARGUMENT &&
	      v34_echo_canceller_start_training(&canceller, 100) ==
		      V34_ECHO_INVALID_STATE &&
	      memcmp(&canceller, &saved, sizeof(saved)) == 0,
	      "invalid lifecycle call changed its destination");
	CHECK(v34_echo_canceller_init(&canceller) == V34_ECHO_OK,
	      "transaction fixture did not initialize");
	saved = canceller;
	memset(&prepared, 0x5a, sizeof(prepared));
	prepared_saved = prepared;
	CHECK(v34_echo_canceller_prepare(
		      &canceller, 0, NULL, &prepared) ==
		      V34_ECHO_INVALID_ARGUMENT &&
	      v34_echo_canceller_prepare(
		      &canceller, 0, &cleaned, NULL) ==
		      V34_ECHO_INVALID_ARGUMENT &&
	      v34_echo_canceller_prepare(
		      &canceller, 0, (int16_t *)&canceller,
		      &prepared) == V34_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(saved)) == 0 &&
	      memcmp(&prepared, &prepared_saved, sizeof(prepared)) == 0 &&
	      cleaned == 1234,
	      "rejected preparation changed state or outputs");
	CHECK(v34_echo_canceller_prepare(
		      &canceller, 0, &cleaned, &prepared) == V34_ECHO_OK &&
	      memcmp(&canceller, &saved, sizeof(saved)) == 0 &&
	      v34_echo_canceller_commit(
		      &canceller, 0, &prepared) == V34_ECHO_OK,
	      "ordinary preparation/commit did not advance");
	saved = canceller;
	CHECK(v34_echo_canceller_commit(
		      &canceller, 0, &prepared) == V34_ECHO_INVALID_STATE &&
	      v34_echo_canceller_commit(
		      &canceller, 0,
		      (const struct v34_echo_prepared_sample *)&canceller) ==
		      V34_ECHO_INVALID_ARGUMENT &&
	      memcmp(&canceller, &saved, sizeof(saved)) == 0 &&
	      !v34_echo_canceller_is_valid(NULL),
	      "stale or aliased commit changed the owner");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "training and frozen cancellation", test_training_and_freeze },
		{ "unproven paths remain disabled",
		  test_unproven_paths_stay_disabled },
		{ "transactionality", test_transactionality },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
