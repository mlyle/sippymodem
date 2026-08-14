// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_pp_receiver.h>
#include <v34_phase3_trn.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum { CHANNEL_TAPS = 3 };

struct test_channel {
	struct qam_point history[CHANNEL_TAPS];
};

static const struct qam_point CHANNEL[CHANNEL_TAPS] = {
	{ 0.92, 0.17 },
	{ 0.12, -0.06 },
	{ -0.04, 0.03 },
};

static struct qam_point multiply(
	const struct qam_point *left, const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static struct qam_point channel_point(
	struct test_channel *channel, const struct qam_point *transmitted)
{
	struct qam_point received = { 0 };

	for (size_t index = CHANNEL_TAPS - 1; index != 0; --index)
		channel->history[index] = channel->history[index - 1];
	channel->history[0] = *transmitted;
	for (size_t index = 0; index < CHANNEL_TAPS; ++index) {
		struct qam_point term = multiply(
			&CHANNEL[index], &channel->history[index]);

		received.in_phase += term.in_phase;
		received.quadrature += term.quadrature;
	}
	return received;
}

static double error_power(
	const struct qam_point *actual, const struct qam_point *expected)
{
	double in_phase = actual->in_phase - expected->in_phase;
	double quadrature = actual->quadrature - expected->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

static bool test_training_and_delayed_handoff(void)
{
	struct v34_phase3_pp_receiver receiver;
	struct v34_phase3_pp_sequence pp;
	struct v34_phase3_trn trn;
	struct v34_equalizer_handoff handoff;
	struct test_channel channel = { 0 };
	struct qam_point lookahead[V34_EQUALIZER_DECISION_DELAY];
	struct qam_point first_trn;
	struct qam_point transmitted;
	struct qam_point received;
	uint8_t rotation;

	CHECK(v34_phase3_pp_receiver_init(&receiver, 0.2) ==
		      V34_PHASE3_PP_RECEIVER_OK &&
	      v34_phase3_pp_sequence_init(&pp) ==
		      V34_PHASE3_PP_SEQUENCE_OK &&
	      v34_phase3_trn_init(&trn, V34_CALLING_STATION) ==
		      V34_PHASE3_TRN_OK,
	      "PP receive fixture did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_PP_SEQUENCE_SYMBOLS; ++symbol) {
		unsigned int accepted = symbol + 1u;
		unsigned int adapted = accepted > V34_EQUALIZER_DECISION_DELAY ?
				       accepted - V34_EQUALIZER_DECISION_DELAY :
				       0;

		CHECK(v34_phase3_pp_sequence_next(&pp, &transmitted) ==
			      V34_PHASE3_PP_SEQUENCE_OK,
		      "PP generator stopped at symbol %u", symbol);
		received = channel_point(&channel, &transmitted);
		CHECK(v34_phase3_pp_receive_point(&receiver, &received) ==
			      V34_PHASE3_PP_RECEIVER_OK &&
		      receiver.received_targets.emitted_symbols == accepted &&
		      receiver.adaptation_targets.emitted_symbols == adapted &&
		      v34_phase3_pp_receiver_is_valid(&receiver) &&
		      !v34_phase3_pp_receiver_is_complete(&receiver),
		      "PP receive state diverged at symbol %u", symbol);
	}
	{
		struct v34_equalizer_handoff untouched;
		struct v34_equalizer_handoff saved;

		memset(&untouched, 0xa5, sizeof(untouched));
		saved = untouched;
		CHECK(v34_phase3_pp_receiver_handoff(
			      &receiver, &untouched) ==
			      V34_PHASE3_PP_RECEIVER_INVALID_STATE &&
		      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
		      "PP receiver exported before delayed decisions completed");
	}
	for (size_t index = 0; index < ARRAY_LENGTH(lookahead); ++index) {
		CHECK(v34_phase3_trn_next(
			      &trn, &transmitted, &rotation) ==
			      V34_PHASE3_TRN_OK,
		      "TRN lookahead stopped at %zu", index);
		if (index == 0)
			first_trn = transmitted;
		lookahead[index] = channel_point(&channel, &transmitted);
		CHECK(v34_phase3_pp_receive_point(
			      &receiver, &lookahead[index]) ==
			      V34_PHASE3_PP_RECEIVER_OK &&
		      receiver.adaptation_targets.emitted_symbols ==
			      V34_PHASE3_PP_SEQUENCE_SYMBOLS -
				      V34_EQUALIZER_DECISION_DELAY + index + 1u &&
		      v34_phase3_pp_receiver_is_complete(&receiver) ==
			      (index + 1u == ARRAY_LENGTH(lookahead)),
		      "TRN lookahead %zu did not retire one PP target", index);
	}
	CHECK(v34_phase3_pp_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_PP_RECEIVER_OK &&
	      v34_equalizer_handoff_is_valid(&handoff),
	      "completed PP receiver did not export its equalizer");
	for (size_t index = 0; index < ARRAY_LENGTH(lookahead); ++index) {
		size_t history = ARRAY_LENGTH(lookahead) - index - 1u;

		CHECK(handoff.equalizer.history[index].in_phase ==
			      lookahead[history].in_phase &&
		      handoff.equalizer.history[index].quadrature ==
			      lookahead[history].quadrature,
		      "handoff reordered TRN lookahead at tap %zu", index);
	}
	{
		struct v34_phase3_pp_receiver complete = receiver;
		struct qam_point output;
		bool ready;

		CHECK(v34_phase3_pp_receive_point(&receiver, &received) ==
			      V34_PHASE3_PP_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &complete, sizeof(receiver)) == 0,
		      "completed PP receiver consumed another point");
		/* The next input produces the first TRN output because the retained
		 * points above belong to TRN, not PP. */
		CHECK(v34_phase3_trn_next(&trn, &transmitted, &rotation) ==
			      V34_PHASE3_TRN_OK,
		      "TRN continuation did not generate");
		received = channel_point(&channel, &transmitted);
		CHECK(qam_equalizer_push(
			      &handoff.equalizer, &received, &output, &ready) ==
			      QAM_EQUALIZER_OK &&
		      ready && error_power(&output, &first_trn) < 0.002,
		      "trained equalizer did not produce the first TRN point: %.6g",
		      error_power(&output, &first_trn));
	}
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_pp_receiver receiver;
	struct v34_phase3_pp_receiver saved;
	struct v34_phase3_pp_receiver corrupt;
	struct qam_point zero = { 0 };
	struct qam_point invalid = { NAN, 0.0 };
	struct v34_equalizer_handoff handoff;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase3_pp_receiver_init(NULL, 0.2) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_pp_receiver_init(&receiver, NAN) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_pp_receiver_init(&receiver, -0.01) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_pp_receiver_init(&receiver, 1.01) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid PP initialization changed destination state");
	CHECK(v34_phase3_pp_receiver_init(&receiver, 0.2) ==
		      V34_PHASE3_PP_RECEIVER_OK,
	      "valid PP receiver did not initialize");
	saved = receiver;
	CHECK(v34_phase3_pp_receive_point(NULL, &zero) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_pp_receive_point(&receiver, NULL) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_pp_receive_point(&receiver, &invalid) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid PP point changed receiver state");
	CHECK(v34_phase3_pp_receive_point(&receiver, &zero) ==
		      V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "failed PP equalizer seed changed receiver state");
	memset(&handoff, 0x5a, sizeof(handoff));
	CHECK(v34_phase3_pp_receiver_handoff(NULL, &handoff) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_pp_receiver_handoff(&receiver, NULL) ==
		      V34_PHASE3_PP_RECEIVER_INVALID_ARGUMENT,
	      "PP handoff accepted invalid arguments");
	corrupt = receiver;
	corrupt.adaptation_targets.emitted_symbols = 1;
	CHECK(!v34_phase3_pp_receiver_is_valid(NULL) &&
	      !v34_phase3_pp_receiver_is_valid(&corrupt) &&
	      !v34_phase3_pp_receiver_is_complete(&corrupt) &&
	      strcmp(v34_phase3_pp_receiver_result_name(
			     V34_PHASE3_PP_RECEIVER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_phase3_pp_receiver_result_name(
			     (enum v34_phase3_pp_receiver_result)99),
		     "unknown") == 0,
	      "PP receiver validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "training and delayed handoff",
		  test_training_and_delayed_handoff },
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
