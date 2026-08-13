// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v90_phase3_signs.h"
#include "v92_phase3_j_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct v92_jd jd_parameters(void)
{
	return (struct v92_jd){
		.data_rate_capability_mask = UINT32_C(0x002a1555),
		.maximum_spectral_shaping_lookahead = 3,
	};
}

static struct v92_jp jp_parameters(void)
{
	return (struct v92_jp){
		.su_phase_extension_q16 = UINT16_C(0x9234),
		.training_constellation = V92_CONSTELLATION_8_POINT,
		.rate_renegotiation_constellation =
			V92_CONSTELLATION_4_POINT,
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

static uint8_t frame_bit(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >> (7u - index % 8u)) & 1u);
}

static bool feed_trn1d(
	struct v90_phase3_signs *transmitter,
	struct v92_phase3_j_receiver *receiver, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		struct v90_pcm_symbol symbol;
		enum v92_phase3_j_receiver_result expected =
			receiver->trn1d_symbols + 1u ==
					V90_PHASE3_TRN1D_MIN_SYMBOLS ?
				V92_PHASE3_J_RECEIVER_TRN1D_READY :
				V92_PHASE3_J_RECEIVER_OK;

		CHECK(v90_phase3_signs_next_trn1d(
			      transmitter, &symbol) == V90_PHASE3_SIGNS_OK &&
		      v92_phase3_j_receiver_receive(
			      receiver, symbol.octet) == expected,
		      "TRN1d receiver mismatch at symbol %zu", index);
	}
	return true;
}

static bool feed_frame(
	struct v90_phase3_signs *transmitter,
	struct v92_phase3_j_receiver *receiver, const uint8_t *frame,
	enum v92_phase3_j_receiver_result final_result)
{
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		enum v92_phase3_j_receiver_result result;

		CHECK(v90_phase3_signs_next_jd_bit(
			      transmitter, frame_bit(frame, index), &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "could not generate J bit %zu", index);
		result = v92_phase3_j_receiver_receive(receiver, symbol.octet);
		CHECK(result == (index == V92_J_FRAME_BITS - 1u ?
				       final_result :
				       V92_PHASE3_J_RECEIVER_OK),
		      "J receiver returned %s at bit %zu",
		      v92_phase3_j_receiver_result_name(result), index);
	}
	return true;
}

static bool run_round_trip(enum g711_law law)
{
	struct v92_jd transmitted_jd = jd_parameters();
	struct v92_jp transmitted_jp = jp_parameters();
	struct v92_jd received_jd;
	struct v92_jp received_jp;
	struct v90_phase3_signs transmitter;
	struct v92_phase3_j_receiver receiver;
	uint8_t jd_frame[V92_J_FRAME_BYTES];
	uint8_t jp_frame[V92_J_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(&transmitter, law, 78) ==
		      V90_PHASE3_SIGNS_OK &&
	      v92_phase3_j_receiver_init(&receiver, law, 78) ==
		      V92_PHASE3_J_RECEIVER_OK &&
	      v92_jd_encode(
		      &transmitted_jd, jd_frame, sizeof(jd_frame)) ==
		      V92_J_OK &&
	      v92_jp_encode(
		      &transmitted_jp, jp_frame, sizeof(jp_frame)) ==
		      V92_J_OK &&
	      feed_trn1d(
		      &transmitter, &receiver,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS),
	      "could not initialize J receiver for law %u",
	      (unsigned int)law);
	CHECK(v92_phase3_j_receiver_begin_jd_search(&receiver) ==
		      V92_PHASE3_J_RECEIVER_OK &&
	      v90_phase3_signs_begin_jd(&transmitter) ==
		      V90_PHASE3_SIGNS_OK &&
	      feed_frame(
		      &transmitter, &receiver, jd_frame,
		      V92_PHASE3_J_RECEIVER_JD_DETECTED) &&
	      v92_phase3_j_receiver_jd(&receiver, &received_jd) &&
	      jd_equal(&received_jd, &transmitted_jd),
	      "receiver did not recover Jd for law %u", (unsigned int)law);
	CHECK(feed_frame(
		      &transmitter, &receiver, jd_frame,
		      V92_PHASE3_J_RECEIVER_JD_REPEATED),
	      "receiver did not recover repeated Jd for law %u",
	      (unsigned int)law);
	CHECK(feed_frame(
		      &transmitter, &receiver, jp_frame,
		      V92_PHASE3_J_RECEIVER_JP_DETECTED) &&
	      v92_phase3_j_receiver_jp(&receiver, &received_jp) &&
	      jp_equal(&received_jp, &transmitted_jp),
	      "receiver did not recover Jp for law %u", (unsigned int)law);
	CHECK(feed_frame(
		      &transmitter, &receiver, jp_frame,
		      V92_PHASE3_J_RECEIVER_JP_REPEATED),
	      "receiver did not recover repeated Jp for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < V92_PHASE3_JP_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		enum v92_phase3_j_receiver_result result;

		CHECK(v90_phase3_signs_next_jd_bit(
			      &transmitter, 0, &symbol) == V90_PHASE3_SIGNS_OK,
		      "could not generate Jp-prime bit %zu", index);
		result = v92_phase3_j_receiver_receive(
			&receiver, symbol.octet);
		CHECK(result == (index == V92_PHASE3_JP_PRIME_BITS - 1u ?
				       V92_PHASE3_J_RECEIVER_COMPLETE :
				       V92_PHASE3_J_RECEIVER_OK),
		      "Jp-prime receiver returned %s at bit %zu",
		      v92_phase3_j_receiver_result_name(result), index);
	}
	CHECK(v92_phase3_j_receiver_is_complete(&receiver),
	      "receiver did not retain completion for law %u",
	      (unsigned int)law);
	{
		struct v92_phase3_j_receiver saved = receiver;

		CHECK(v92_phase3_j_receiver_receive(&receiver, 0xa5) ==
			      V92_PHASE3_J_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0,
		      "completed receiver consumed another character");
	}
	return true;
}

static bool test_round_trips(void)
{
	return run_round_trip(G711_LAW_PCMU) &&
	       run_round_trip(G711_LAW_PCMA);
}

static bool test_early_search(void)
{
	struct v92_jd jd = jd_parameters();
	struct v90_phase3_signs transmitter;
	struct v92_phase3_j_receiver receiver;
	uint8_t frame[V92_J_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(
		      &transmitter, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SIGNS_OK &&
	      v92_phase3_j_receiver_init(
		      &receiver, G711_LAW_PCMU, 91) ==
		      V92_PHASE3_J_RECEIVER_OK &&
	      feed_trn1d(
		      &transmitter, &receiver,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
	      v92_phase3_j_receiver_begin_jd_search(&receiver) ==
		      V92_PHASE3_J_RECEIVER_OK &&
	      feed_trn1d(&transmitter, &receiver, 60),
	      "could not start early Jd search");
	CHECK(v90_phase3_signs_begin_jd(&transmitter) ==
		      V90_PHASE3_SIGNS_OK &&
	      v92_jd_encode(&jd, frame, sizeof(frame)) == V92_J_OK &&
	      feed_frame(
		      &transmitter, &receiver, frame,
		      V92_PHASE3_J_RECEIVER_OK) &&
	      feed_frame(
		      &transmitter, &receiver, frame,
		      V92_PHASE3_J_RECEIVER_JD_DETECTED),
	      "receiver did not reacquire after an early search");
	return true;
}

static bool start_records(
	struct v90_phase3_signs *transmitter,
	struct v92_phase3_j_receiver *receiver)
{
	return v90_phase3_signs_init(
		       transmitter, G711_LAW_PCMU, 78) ==
		       V90_PHASE3_SIGNS_OK &&
	       v92_phase3_j_receiver_init(
		       receiver, G711_LAW_PCMU, 78) ==
		       V92_PHASE3_J_RECEIVER_OK &&
	       feed_trn1d(
		       transmitter, receiver,
		       V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
	       v92_phase3_j_receiver_begin_jd_search(receiver) ==
		       V92_PHASE3_J_RECEIVER_OK &&
	       v90_phase3_signs_begin_jd(transmitter) ==
		       V90_PHASE3_SIGNS_OK;
}

static bool test_parameter_consistency(void)
{
	struct v92_jd jd = jd_parameters();
	struct v92_jd different_jd = jd;
	struct v92_jp jp = jp_parameters();
	struct v92_jp different_jp = jp;
	struct v92_jd received_jd;
	struct v92_jp received_jp;
	struct v90_phase3_signs transmitter;
	struct v92_phase3_j_receiver receiver;
	uint8_t jd_frame[V92_J_FRAME_BYTES];
	uint8_t different_jd_frame[V92_J_FRAME_BYTES];
	uint8_t jp_frame[V92_J_FRAME_BYTES];
	uint8_t different_jp_frame[V92_J_FRAME_BYTES];

	different_jd.maximum_spectral_shaping_lookahead = 2;
	different_jp.su_phase_extension_q16++;
	CHECK(v92_jd_encode(&jd, jd_frame, sizeof(jd_frame)) == V92_J_OK &&
	      v92_jd_encode(
		      &different_jd, different_jd_frame,
		      sizeof(different_jd_frame)) == V92_J_OK &&
	      v92_jp_encode(&jp, jp_frame, sizeof(jp_frame)) == V92_J_OK &&
	      v92_jp_encode(
		      &different_jp, different_jp_frame,
		      sizeof(different_jp_frame)) == V92_J_OK,
	      "could not encode consistency fixtures");
	CHECK(start_records(&transmitter, &receiver) &&
	      feed_frame(
		      &transmitter, &receiver, jd_frame,
		      V92_PHASE3_J_RECEIVER_JD_DETECTED) &&
	      feed_frame(
		      &transmitter, &receiver, different_jd_frame,
		      V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE) &&
	      v92_phase3_j_receiver_jd(&receiver, &received_jd) &&
	      jd_equal(&received_jd, &jd),
	      "receiver accepted changed Jd parameters");
	CHECK(start_records(&transmitter, &receiver) &&
	      feed_frame(
		      &transmitter, &receiver, jd_frame,
		      V92_PHASE3_J_RECEIVER_JD_DETECTED) &&
	      feed_frame(
		      &transmitter, &receiver, jp_frame,
		      V92_PHASE3_J_RECEIVER_JP_DETECTED) &&
	      feed_frame(
		      &transmitter, &receiver, different_jp_frame,
		      V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE) &&
	      v92_phase3_j_receiver_jp(&receiver, &received_jp) &&
	      jp_equal(&received_jp, &jp),
	      "receiver accepted changed Jp parameters");
	return true;
}

static bool test_boundaries_and_validation(void)
{
	struct v90_phase3_signs transmitter;
	struct v92_phase3_j_receiver receiver;
	struct v92_phase3_j_receiver saved;
	struct v92_jd jd = { .data_rate_capability_mask = 0xa5 };
	struct v92_jp jp = { .su_phase_extension_q16 = 0xa5a5 };
	struct v90_pcm_symbol symbol;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v92_phase3_j_receiver_init(NULL, G711_LAW_PCMU, 78) ==
		      V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      v92_phase3_j_receiver_init(
		      &receiver, (enum g711_law)2, 78) ==
		      V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      v92_phase3_j_receiver_init(
		      &receiver, G711_LAW_PCMU, 66) ==
		      V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "invalid initialization changed state");
	CHECK(v90_phase3_signs_init(
		      &transmitter, G711_LAW_PCMA, 78) ==
		      V90_PHASE3_SIGNS_OK &&
	      v92_phase3_j_receiver_init(
		      &receiver, G711_LAW_PCMA, 78) ==
		      V92_PHASE3_J_RECEIVER_OK &&
	      feed_trn1d(
		      &transmitter, &receiver,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS - 1u),
	      "could not reach early TRN1d boundary");
	saved = receiver;
	CHECK(v92_phase3_j_receiver_begin_jd_search(&receiver) ==
		      V92_PHASE3_J_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      feed_trn1d(&transmitter, &receiver, 2),
	      "early search was not transactional");
	saved = receiver;
	CHECK(v92_phase3_j_receiver_begin_jd_search(&receiver) ==
		      V92_PHASE3_J_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      feed_trn1d(&transmitter, &receiver, 5) &&
	      v92_phase3_j_receiver_begin_jd_search(&receiver) ==
		      V92_PHASE3_J_RECEIVER_OK,
	      "unaligned Jd-search boundary was not enforced");
	CHECK(v90_pcm_symbol_make(
		      &symbol, G711_LAW_PCMA, 79, true),
	      "could not build wrong-code character");
	saved = receiver;
	CHECK(v92_phase3_j_receiver_receive(&receiver, symbol.octet) ==
		      V92_PHASE3_J_RECEIVER_INVALID_SEQUENCE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      v92_phase3_j_receiver_receive(NULL, 0) ==
		      V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      v92_phase3_j_receiver_begin_jd_search(NULL) ==
		      V92_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      !v92_phase3_j_receiver_jd(&receiver, &jd) &&
	      !v92_phase3_j_receiver_jd(NULL, &jd) &&
	      !v92_phase3_j_receiver_jd(&receiver, NULL) &&
	      !v92_phase3_j_receiver_jp(&receiver, &jp) &&
	      !v92_phase3_j_receiver_jp(NULL, &jp) &&
	      !v92_phase3_j_receiver_jp(&receiver, NULL) &&
	      !v92_phase3_j_receiver_is_valid(NULL) &&
	      !v92_phase3_j_receiver_is_complete(NULL),
	      "invalid operation changed state or output");
	CHECK(strcmp(v92_phase3_j_receiver_result_name(
		     V92_PHASE3_J_RECEIVER_JP_DETECTED),
		     "Jp detected") == 0 &&
	      strcmp(v92_phase3_j_receiver_result_name(
		     (enum v92_phase3_j_receiver_result)10),
		     "unknown") == 0,
	      "result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "continuous PCMU and PCMA sequences", test_round_trips },
		{ "early Jd search", test_early_search },
		{ "stable J parameters", test_parameter_consistency },
		{ "boundaries and validation", test_boundaries_and_validation },
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
