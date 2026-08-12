// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_j_receiver.h>

#include "test.h"

#include <string.h>

static bool start_pair(
	struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_trn *source, enum v34_station_role role)
{
	return v34_phase3_j_receiver_init(receiver, role) ==
		       V34_PHASE3_J_RECEIVER_OK &&
	       v34_phase3_trn_init(source, role) == V34_PHASE3_TRN_OK;
}

static bool feed_trn(
	struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_trn *source, unsigned int symbols)
{
	for (unsigned int symbol = 0; symbol < symbols; ++symbol) {
		struct qam_point point;
		uint8_t rotation;

		if (v34_phase3_trn_next(source, &point, &rotation) !=
			    V34_PHASE3_TRN_OK ||
		    v34_phase3_j_receiver_receive(receiver, 0, rotation) !=
			    V34_PHASE3_J_RECEIVER_OK)
			return false;
	}
	return true;
}

static bool feed_j(
	struct v34_phase3_j_receiver *receiver,
	struct v34_phase3_j_sequence *source, unsigned int error_count,
	enum v34_phase3_j_receiver_result final_result)
{
	static const unsigned int ERROR_SYMBOLS[] = { 3, 11, 19 };

	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS; ++symbol) {
		struct qam_point point;
		uint8_t rotation;
		uint8_t quarter_point = 0;
		enum v34_phase3_j_receiver_result expected =
			symbol + 1u == V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS ?
				final_result :
				V34_PHASE3_J_RECEIVER_OK;

		if (v34_phase3_j_sequence_next(source, &point, &rotation) !=
		    V34_PHASE3_J_SEQUENCE_OK)
			return false;
		for (unsigned int error = 0; error < error_count; ++error) {
			if (symbol != ERROR_SYMBOLS[error])
				continue;
			if (error == 1)
				quarter_point = 1;
			else
				rotation = (uint8_t)((rotation + 1u) & 3u);
		}
		if (v34_phase3_j_receiver_receive(
			    receiver, quarter_point, rotation) != expected)
			return false;
	}
	return true;
}

static bool run_generated_transition(
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	unsigned int trn_symbols)
{
	struct v34_phase3_j_receiver receiver;
	struct v34_phase3_trn source_trn;
	struct v34_phase3_j_sequence source_j;
	struct v34_phase3_j_detection detection;

	CHECK(start_pair(&receiver, &source_trn, role) &&
	      feed_trn(&receiver, &source_trn, trn_symbols) &&
	      v34_phase3_j_sequence_init_from_trn(
		      &source_j, &source_trn, constellation) ==
		      V34_PHASE3_J_SEQUENCE_OK &&
	      feed_j(
		      &receiver, &source_j, 0,
		      V34_PHASE3_J_RECEIVER_DETECTED),
	      "role %d constellation %d transition at %u was not detected",
	      role, constellation, trn_symbols);
	CHECK(v34_phase3_j_receiver_is_valid(&receiver) &&
	      v34_phase3_j_receiver_is_complete(&receiver) &&
	      v34_phase3_j_receiver_get_detection(&receiver, &detection) &&
	      detection.trn_symbols == trn_symbols && detection.errors == 0 &&
	      detection.sequence.remote_constellation == constellation &&
	      detection.sequence.role == role &&
	      v34_phase3_j_sequence_at_pattern_boundary(
		      &detection.sequence) &&
	      memcmp(&detection.sequence, &source_j, sizeof(source_j)) == 0,
	      "role %d constellation %d detection lost boundary state",
	      role, constellation);
	{
		struct v34_phase3_j_receiver complete = receiver;

		CHECK(v34_phase3_j_receiver_receive(&receiver, 0, 0) ==
			      V34_PHASE3_J_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &complete, sizeof(receiver)) == 0,
		      "completed J receiver consumed another decision");
	}
	return true;
}

static bool test_generated_selections_and_boundaries(void)
{
	static const enum v34_station_role ROLES[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const enum v34_constellation_size CONSTELLATIONS[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};
	static const unsigned int TRN_LENGTHS[] = { 512, 613, 4097 };

	CHECK(V34_PHASE3_J_RECEIVER_PATTERN_REPETITIONS == 3 &&
	      V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS == 24,
	      "J receive window dimensions changed");
	for (size_t role = 0; role < ARRAY_LENGTH(ROLES); ++role)
		for (size_t constellation = 0;
		     constellation < ARRAY_LENGTH(CONSTELLATIONS);
		     ++constellation)
			for (size_t length = 0;
			     length < ARRAY_LENGTH(TRN_LENGTHS); ++length)
				CHECK(run_generated_transition(
					      ROLES[role],
					      CONSTELLATIONS[constellation],
					      TRN_LENGTHS[length]),
				      "generated J selection failed");
	return true;
}

static bool test_minimum_and_error_threshold(void)
{
	struct v34_phase3_j_receiver receiver;
	struct v34_phase3_trn source_trn;
	struct v34_phase3_j_sequence source_j;
	struct v34_phase3_j_detection detection;

	CHECK(start_pair(
		      &receiver, &source_trn, V34_CALLING_STATION) &&
	      feed_trn(
		      &receiver, &source_trn,
		      V34_PHASE3_TRN_MIN_SYMBOLS - 1u) &&
	      v34_phase3_j_sequence_init_from_trn(
		      &source_j, &source_trn,
		      V34_CONSTELLATION_4_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK &&
	      feed_j(
		      &receiver, &source_j, 0,
		      V34_PHASE3_J_RECEIVER_OK) &&
	      !v34_phase3_j_receiver_is_complete(&receiver),
	      "J was accepted before the minimum TRN interval");
	CHECK(start_pair(
		      &receiver, &source_trn, V34_ANSWERING_STATION) &&
	      feed_trn(&receiver, &source_trn, 701) &&
	      v34_phase3_j_sequence_init_from_trn(
		      &source_j, &source_trn,
		      V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK &&
	      feed_j(
		      &receiver, &source_j,
		      V34_PHASE3_J_RECEIVER_MAX_ERRORS,
		      V34_PHASE3_J_RECEIVER_DETECTED) &&
	      v34_phase3_j_receiver_get_detection(&receiver, &detection) &&
	      detection.trn_symbols == 701 &&
	      detection.errors == V34_PHASE3_J_RECEIVER_MAX_ERRORS &&
	      detection.sequence.remote_constellation ==
		      V34_CONSTELLATION_16_POINT,
	      "bounded J decision errors were not retained");
	CHECK(start_pair(
		      &receiver, &source_trn, V34_ANSWERING_STATION) &&
	      feed_trn(&receiver, &source_trn, 701) &&
	      v34_phase3_j_sequence_init_from_trn(
		      &source_j, &source_trn,
		      V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK &&
	      feed_j(
		      &receiver, &source_j,
		      V34_PHASE3_J_RECEIVER_MAX_ERRORS + 1u,
		      V34_PHASE3_J_RECEIVER_OK) &&
	      !v34_phase3_j_receiver_is_complete(&receiver),
	      "J beyond the decision-error threshold was accepted");
	return true;
}

static bool test_continued_trn_rejection(void)
{
	static const enum v34_station_role ROLES[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};

	for (size_t role = 0; role < ARRAY_LENGTH(ROLES); ++role) {
		struct v34_phase3_j_receiver receiver;
		struct v34_phase3_trn source;

		CHECK(start_pair(&receiver, &source, ROLES[role]) &&
		      feed_trn(&receiver, &source, 20000) &&
		      !v34_phase3_j_receiver_is_complete(&receiver) &&
		      receiver.received_symbols == 20000,
		      "continued role %d TRN falsely appeared to be J",
		      ROLES[role]);
	}
	return true;
}

static bool test_validation_and_range(void)
{
	struct v34_phase3_j_receiver receiver;
	struct v34_phase3_j_receiver saved;
	struct v34_phase3_j_receiver corrupt;
	struct v34_phase3_trn source;
	struct v34_phase3_j_detection detection;
	struct v34_phase3_j_detection saved_detection;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase3_j_receiver_init(NULL, V34_CALLING_STATION) ==
		      V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_receiver_init(
		      &receiver, (enum v34_station_role)99) ==
		      V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid J receiver initialization changed its destination");
	CHECK(start_pair(&receiver, &source, V34_CALLING_STATION),
	      "J validation fixture did not initialize");
	saved = receiver;
	memset(&detection, 0x5a, sizeof(detection));
	saved_detection = detection;
	CHECK(v34_phase3_j_receiver_receive(NULL, 0, 0) ==
		      V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_receiver_receive(&receiver, 4, 0) ==
		      V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_j_receiver_receive(&receiver, 0, 4) ==
		      V34_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      !v34_phase3_j_receiver_get_detection(&receiver, &detection) &&
	      memcmp(&detection, &saved_detection, sizeof(detection)) == 0,
	      "invalid J decision changed state or an output");
	CHECK(feed_trn(
		      &receiver, &source,
		      V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS),
	      "range fixture did not fill its history");
	receiver.received_symbols = UINT32_MAX;
	receiver.next_decision = (uint8_t)(
		UINT32_MAX % V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS);
	CHECK(v34_phase3_j_receiver_is_valid(&receiver),
	      "range fixture was not a valid saturated state");
	saved = receiver;
	CHECK(v34_phase3_j_receiver_receive(&receiver, 0, 0) ==
		      V34_PHASE3_J_RECEIVER_OUT_OF_RANGE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "saturated J receiver consumed another decision");
	corrupt = receiver;
	corrupt.next_decision = (uint8_t)(
		(corrupt.next_decision + 1u) %
		V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS);
	CHECK(!v34_phase3_j_receiver_is_valid(NULL) &&
	      !v34_phase3_j_receiver_is_valid(&corrupt) &&
	      !v34_phase3_j_receiver_is_complete(&corrupt) &&
	      !v34_phase3_j_receiver_get_detection(&saved, NULL) &&
	      strcmp(v34_phase3_j_receiver_result_name(
			     V34_PHASE3_J_RECEIVER_DETECTED),
		     "detected") == 0 &&
	      strcmp(v34_phase3_j_receiver_result_name(
			     V34_PHASE3_J_RECEIVER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v34_phase3_j_receiver_result_name(
			     (enum v34_phase3_j_receiver_result)99),
		     "unknown") == 0,
	      "J receiver validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated selections and boundaries",
		  test_generated_selections_and_boundaries },
		{ "minimum and error threshold",
		  test_minimum_and_error_threshold },
		{ "continued TRN rejection", test_continued_trn_rejection },
		{ "validation and range", test_validation_and_range },
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
