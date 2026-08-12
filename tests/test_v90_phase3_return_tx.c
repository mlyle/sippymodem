// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v90_phase3_return_tx.h>

#include "test.h"

#include <string.h>

static bool point_equal(
	const struct qam_point *left, const struct qam_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool run_timeline(uint32_t finish_after)
{
	struct v90_phase3_return_tx tx;
	struct qam_point point;

	CHECK(v90_phase3_return_tx_init(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "finish %u return transmitter did not initialize", finish_after);
	for (size_t index = 0; index < 37; ++index) {
		struct qam_point expected = { 0 };

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK &&
		      point_equal(&point, &expected),
		      "finish %u silence changed at %zu", finish_after, index);
	}
	CHECK(v90_phase3_return_tx_start_s(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "finish %u could not start S", finish_after);
	for (uint32_t index = 0; index < finish_after; ++index) {
		struct qam_point expected = v34_rotated_point_zero((int)(index & 1u));

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK &&
		      point_equal(&point, &expected),
		      "finish %u S changed at %u", finish_after, index);
	}
	CHECK(v90_phase3_return_tx_finish_s(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "finish %u request failed", finish_after);
	while (tx.stage == V90_PHASE3_RETURN_TX_S) {
		struct qam_point expected = v34_rotated_point_zero(
			(int)(tx.s_symbols & 1u));

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK &&
		      point_equal(&point, &expected),
		      "finish %u minimum S extension changed", finish_after);
	}
	CHECK(tx.s_symbols >= V34_PHASE3_S_SYMBOLS,
	      "finish %u entered S-bar before minimum S", finish_after);
	for (size_t index = 0; index < V34_PHASE3_S_BAR_SYMBOLS; ++index) {
		struct qam_point expected = v34_rotated_point_zero(
			(int)(2u + (index & 1u)));

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK &&
		      point_equal(&point, &expected),
		      "finish %u S-bar changed at %zu", finish_after, index);
	}
	CHECK(v90_phase3_return_tx_is_complete(&tx),
	      "finish %u return signal did not complete", finish_after);
	{
		struct v90_phase3_return_tx saved = tx;
		struct qam_point saved_point = point;

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_DONE &&
		      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
		      point_equal(&point, &saved_point),
		      "finish %u completed transmitter changed state", finish_after);
	}
	return true;
}

static bool test_timing_boundaries(void)
{
	CHECK(run_timeline(17) && run_timeline(V34_PHASE3_S_SYMBOLS) &&
	      run_timeline(V34_PHASE3_S_SYMBOLS + 53u),
	      "return timing boundaries failed");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v90_phase3_return_tx tx;
	struct v90_phase3_return_tx saved;
	struct qam_point point = { 3.0, -2.0 };
	struct qam_point saved_point = point;

	CHECK(v90_phase3_return_tx_init(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "validation fixture did not initialize");
	saved = tx;
	CHECK(v90_phase3_return_tx_start_s(NULL) ==
		      V90_PHASE3_RETURN_TX_INVALID_ARGUMENT &&
	      v90_phase3_return_tx_finish_s(&tx) ==
		      V90_PHASE3_RETURN_TX_INVALID_STATE &&
	      v90_phase3_return_tx_next(NULL, &point) ==
		      V90_PHASE3_RETURN_TX_INVALID_ARGUMENT &&
	      v90_phase3_return_tx_next(&tx, NULL) ==
		      V90_PHASE3_RETURN_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      point_equal(&point, &saved_point),
	      "rejected return operation changed state or output");
	tx.silence_symbols = UINT32_MAX;
	saved = tx;
	CHECK(v90_phase3_return_tx_next(&tx, &point) ==
		      V90_PHASE3_RETURN_TX_OUT_OF_RANGE &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      point_equal(&point, &saved_point),
	      "silence overflow was not transactional");
	tx.stage = (enum v90_phase3_return_tx_stage)99;
	CHECK(!v90_phase3_return_tx_is_valid(NULL) &&
	      !v90_phase3_return_tx_is_valid(&tx) &&
	      !v90_phase3_return_tx_is_complete(&tx) &&
	      strcmp(v90_phase3_return_tx_result_name(
		     V90_PHASE3_RETURN_TX_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v90_phase3_return_tx_result_name(
		     (enum v90_phase3_return_tx_result)99),
		     "unknown") == 0,
	      "return validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "timing boundaries", test_timing_boundaries },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
