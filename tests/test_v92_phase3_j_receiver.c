// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v90_phase3_signs.h"
#include "pcm_phase3_j_receiver.h"

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
	struct pcm_phase3_j_receiver *receiver, size_t count)
{
	for (size_t index = 0; index < count; ++index) {
		struct v90_pcm_symbol symbol;
		enum pcm_phase3_j_receiver_result expected =
			receiver->trn1d_symbols + 1u ==
					V90_PHASE3_TRN1D_MIN_SYMBOLS ?
				PCM_PHASE3_J_RECEIVER_TRN1D_READY :
				PCM_PHASE3_J_RECEIVER_OK;

		CHECK(v90_phase3_signs_next_trn1d(
			      transmitter, &symbol) == V90_PHASE3_SIGNS_OK &&
		      pcm_phase3_j_receiver_receive(
			      receiver, symbol.octet) == expected,
		      "TRN1d receiver mismatch at symbol %zu", index);
	}
	return true;
}

static bool feed_frame(
	struct v90_phase3_signs *transmitter,
	struct pcm_phase3_j_receiver *receiver, const uint8_t *frame,
	enum pcm_phase3_j_receiver_result final_result)
{
	for (size_t index = 0; index < V92_J_FRAME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		struct pcm_phase3_j_receiver saved = *receiver;
		enum pcm_phase3_j_receiver_result result;

		CHECK(v90_phase3_signs_next_jd_bit(
			      transmitter, frame_bit(frame, index), &symbol) ==
			      V90_PHASE3_SIGNS_OK,
		      "could not generate J bit %zu", index);
		result = pcm_phase3_j_receiver_receive(receiver, symbol.octet);
		CHECK(result == (index == V92_J_FRAME_BITS - 1u ?
				       final_result :
				       PCM_PHASE3_J_RECEIVER_OK),
		      "J receiver returned %s at bit %zu",
		      pcm_phase3_j_receiver_result_name(result), index);
		CHECK(result != PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE ||
		      memcmp(receiver, &saved, sizeof(saved)) == 0,
		      "rejected J record changed receiver state");
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
	struct pcm_phase3_j_receiver receiver;
	uint8_t jd_frame[V92_J_FRAME_BYTES];
	uint8_t jp_frame[V92_J_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(&transmitter, law, 78) ==
		      V90_PHASE3_SIGNS_OK &&
	      pcm_phase3_j_receiver_init(&receiver, PCM_PROTOCOL_V92, law, 78) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
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
	CHECK(pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
	      v90_phase3_signs_begin_jd(&transmitter) ==
		      V90_PHASE3_SIGNS_OK &&
	      feed_frame(
		      &transmitter, &receiver, jd_frame,
		      PCM_PHASE3_J_RECEIVER_JD_DETECTED) &&
	      pcm_phase3_j_receiver_v92_jd(&receiver, &received_jd) &&
	      jd_equal(&received_jd, &transmitted_jd),
	      "receiver did not recover Jd for law %u", (unsigned int)law);
	CHECK(feed_frame(
		      &transmitter, &receiver, jd_frame,
		      PCM_PHASE3_J_RECEIVER_JD_REPEATED),
	      "receiver did not recover repeated Jd for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < V92_PHASE3_JP_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;

		CHECK(v90_phase3_signs_next_jd_bit(
			      &transmitter, 0, &symbol) == V90_PHASE3_SIGNS_OK &&
		      pcm_phase3_j_receiver_receive(&receiver, symbol.octet) ==
			      PCM_PHASE3_J_RECEIVER_OK &&
		      receiver.stage == PCM_PHASE3_J_RECEIVER_JD,
		      "V.92 completed on Jd-prime without Jp");
	}
	CHECK(feed_frame(
		      &transmitter, &receiver, jp_frame,
		      PCM_PHASE3_J_RECEIVER_JP_DETECTED) &&
	      pcm_phase3_j_receiver_v92_jp(&receiver, &received_jp) &&
	      jp_equal(&received_jp, &transmitted_jp),
	      "receiver did not recover Jp for law %u", (unsigned int)law);
	CHECK(feed_frame(
		      &transmitter, &receiver, jp_frame,
		      PCM_PHASE3_J_RECEIVER_JP_REPEATED),
	      "receiver did not recover repeated Jp for law %u",
	      (unsigned int)law);
	for (size_t index = 0; index < V92_PHASE3_JP_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;
		enum pcm_phase3_j_receiver_result result;

		CHECK(v90_phase3_signs_next_jd_bit(
			      &transmitter, 0, &symbol) == V90_PHASE3_SIGNS_OK,
		      "could not generate Jp-prime bit %zu", index);
		result = pcm_phase3_j_receiver_receive(
			&receiver, symbol.octet);
		CHECK(result == (index == V92_PHASE3_JP_PRIME_BITS - 1u ?
				       PCM_PHASE3_J_RECEIVER_COMPLETE :
				       PCM_PHASE3_J_RECEIVER_OK),
		      "Jp-prime receiver returned %s at bit %zu",
		      pcm_phase3_j_receiver_result_name(result), index);
	}
	CHECK(pcm_phase3_j_receiver_is_complete(&receiver),
	      "receiver did not retain completion for law %u",
	      (unsigned int)law);
	{
		struct v90_jd_parameters jd = { .data_rate_capability_mask = 123 };

		CHECK(!pcm_phase3_j_receiver_v90_jd(&receiver, &jd) &&
		      jd.data_rate_capability_mask == 123,
		      "V.92 receiver exposed V.90 parameters");
	}
	{
		struct pcm_phase3_j_receiver saved = receiver;

		CHECK(pcm_phase3_j_receiver_receive(&receiver, 0xa5) ==
			      PCM_PHASE3_J_RECEIVER_COMPLETE &&
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
	struct pcm_phase3_j_receiver receiver;
	uint8_t frame[V92_J_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(
		      &transmitter, G711_LAW_PCMU, 91) ==
		      V90_PHASE3_SIGNS_OK &&
	      pcm_phase3_j_receiver_init(
		&receiver, PCM_PROTOCOL_V92, G711_LAW_PCMU, 91) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
	      feed_trn1d(
		      &transmitter, &receiver,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
	      pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
	      feed_trn1d(&transmitter, &receiver, 60),
	      "could not start early Jd search");
	CHECK(v90_phase3_signs_begin_jd(&transmitter) ==
		      V90_PHASE3_SIGNS_OK &&
	      v92_jd_encode(&jd, frame, sizeof(frame)) == V92_J_OK &&
	      feed_frame(
		      &transmitter, &receiver, frame,
		      PCM_PHASE3_J_RECEIVER_OK) &&
	      feed_frame(
		      &transmitter, &receiver, frame,
		      PCM_PHASE3_J_RECEIVER_JD_DETECTED),
	      "receiver did not reacquire after an early search");
	return true;
}

struct sampled_channel {
	int16_t previous[2];
	size_t changed_codes;
};

static uint8_t sampled_octet(
	struct sampled_channel *channel, enum g711_law law,
	const struct v90_pcm_symbol *symbol)
{
	/* Attenuation and two postcursors, followed by another codec boundary.
	 * Keep the channel history across training, both records, and Jp-prime. */
	int32_t weighted = 4 * (90 * (int32_t)symbol->sample +
		8 * (int32_t)channel->previous[0] +
		2 * (int32_t)channel->previous[1]);
	int16_t sample = (int16_t)((weighted + (weighted >= 0 ? 250 : -250)) / 500);
	uint8_t octet = law == G711_LAW_PCMU ?
		g711_encode_pcmu(sample) : g711_encode_pcma(sample);

	channel->previous[1] = channel->previous[0];
	channel->previous[0] = symbol->sample;
	channel->changed_codes += octet != symbol->octet;
	return octet;
}

static bool run_sampled_signs(enum g711_law law)
{
	static const enum pcm_phase3_j_receiver_result detected[] = {
		PCM_PHASE3_J_RECEIVER_JD_DETECTED, PCM_PHASE3_J_RECEIVER_JP_DETECTED,
	};
	static const enum pcm_phase3_j_receiver_result repeated[] = {
		PCM_PHASE3_J_RECEIVER_JD_REPEATED, PCM_PHASE3_J_RECEIVER_JP_REPEATED,
	};
	struct v90_phase3_signs transmitter;
	struct pcm_phase3_j_receiver receiver;
	struct v92_jd jd = jd_parameters(), received_jd;
	struct v92_jp jp = jp_parameters(), received_jp;
	struct sampled_channel channel = { 0 };
	uint8_t frames[2][V92_J_FRAME_BYTES];

	CHECK(v90_phase3_signs_init(&transmitter, law, 78) ==
		      V90_PHASE3_SIGNS_OK &&
	      pcm_phase3_j_receiver_init(&receiver, PCM_PROTOCOL_V92, law, 78) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
	      v92_jd_encode(&jd, frames[0], sizeof(frames[0])) == V92_J_OK &&
	      v92_jp_encode(&jp, frames[1], sizeof(frames[1])) == V92_J_OK,
	      "could not initialize sampled signs for law %u", (unsigned int)law);
	for (size_t index = 0; index < V90_PHASE3_TRN1D_MIN_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		uint8_t octet;
		enum pcm_phase3_j_receiver_result expected =
			index + 1u == V90_PHASE3_TRN1D_MIN_SYMBOLS ?
				PCM_PHASE3_J_RECEIVER_TRN1D_READY :
				PCM_PHASE3_J_RECEIVER_OK;

		CHECK(v90_phase3_signs_next_trn1d(&transmitter, &symbol) ==
			      V90_PHASE3_SIGNS_OK, "could not generate TRN1d");
		octet = sampled_octet(&channel, law, &symbol);
		/* Include the last training sign: its error propagates into Jd. */
		if (index == 0 || index == 200 ||
		    index + 1u == V90_PHASE3_TRN1D_MIN_SYMBOLS)
			octet ^= 0x80u;
		CHECK(pcm_phase3_j_receiver_receive(&receiver, octet) == expected,
		      "sampled TRN1d stalled for law %u at %zu",
		      (unsigned int)law, index);
	}
	CHECK(pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
	      v90_phase3_signs_begin_jd(&transmitter) == V90_PHASE3_SIGNS_OK,
	      "could not start sampled Jd search");
	for (size_t kind = 0; kind < ARRAY_LENGTH(frames); ++kind) {
		for (size_t repeat = 0; repeat < 3; ++repeat) {
			for (size_t index = 0; index < V92_J_FRAME_BITS; ++index) {
				struct v90_pcm_symbol symbol;
				uint8_t octet;
				enum pcm_phase3_j_receiver_result expected =
					PCM_PHASE3_J_RECEIVER_OK;

				CHECK(v90_phase3_signs_next_jd_bit(
					      &transmitter, frame_bit(frames[kind], index),
					      &symbol) == V90_PHASE3_SIGNS_OK,
				      "could not generate sampled record");
				octet = sampled_octet(&channel, law, &symbol);
				if (repeat == 0 && index == 20)
					octet ^= 0x80u;
				if (repeat != 0 && index + 1u == V92_J_FRAME_BITS)
					expected = repeat == 1 ? detected[kind] : repeated[kind];
				CHECK(pcm_phase3_j_receiver_receive(&receiver, octet) == expected,
				      "CRC recovery failed for law %u record %zu repeat %zu bit %zu",
				      (unsigned int)law, kind, repeat, index);
			}
		}
	}
	CHECK(pcm_phase3_j_receiver_v92_jd(&receiver, &received_jd) &&
	      jd_equal(&received_jd, &jd) &&
	      pcm_phase3_j_receiver_v92_jp(&receiver, &received_jp) &&
	      jp_equal(&received_jp, &jp), "sampled J parameters changed");
	for (size_t index = 0; index < V92_PHASE3_JP_PRIME_BITS; ++index) {
		struct v90_pcm_symbol symbol;

		CHECK(v90_phase3_signs_next_jd_bit(&transmitter, 0, &symbol) ==
			      V90_PHASE3_SIGNS_OK &&
		      pcm_phase3_j_receiver_receive(
			      &receiver, sampled_octet(&channel, law, &symbol)) ==
			      (index + 1u == V92_PHASE3_JP_PRIME_BITS ?
			       PCM_PHASE3_J_RECEIVER_COMPLETE : PCM_PHASE3_J_RECEIVER_OK),
		      "sampled Jp-prime boundary changed at %zu", index);
	}
	CHECK(pcm_phase3_j_receiver_is_complete(&receiver) &&
	      channel.changed_codes == V90_PHASE3_TRN1D_MIN_SYMBOLS +
		      6u * V92_J_FRAME_BITS + V92_PHASE3_JP_PRIME_BITS,
	      "sampled sequence did not complete with changed amplitude codes");
	return true;
}

static bool test_sampled_signs(void)
{
	return run_sampled_signs(G711_LAW_PCMU) &&
	       run_sampled_signs(G711_LAW_PCMA);
}

static bool test_unqualified_signs(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_phase3_j_receiver receiver;
		uint32_t noise = UINT32_C(0x1c87a459);

		CHECK(pcm_phase3_j_receiver_init(
			&receiver, PCM_PROTOCOL_V92, law, 78) ==
			      PCM_PHASE3_J_RECEIVER_OK, "could not initialize noise test");
		for (size_t index = 0; index < 16000; ++index) {
			enum pcm_phase3_j_receiver_result expected =
				index + 1u == V90_PHASE3_TRN1D_MIN_SYMBOLS ?
					PCM_PHASE3_J_RECEIVER_TRN1D_READY :
					PCM_PHASE3_J_RECEIVER_OK;

			noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
			CHECK(pcm_phase3_j_receiver_receive(
				      &receiver, (uint8_t)(noise >> 24)) == expected,
			      "unqualified signs advanced the protocol at %zu", index);
			if (expected == PCM_PHASE3_J_RECEIVER_TRN1D_READY)
				CHECK(pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
					      PCM_PHASE3_J_RECEIVER_OK,
				      "could not start noise Jd search");
		}
		CHECK(receiver.stage == PCM_PHASE3_J_RECEIVER_JD_SEARCH,
		      "unqualified signs escaped Jd search");
	}
	return true;
}

static bool start_records(
	struct v90_phase3_signs *transmitter,
	struct pcm_phase3_j_receiver *receiver)
{
	return v90_phase3_signs_init(
		       transmitter, G711_LAW_PCMU, 78) ==
		       V90_PHASE3_SIGNS_OK &&
	       pcm_phase3_j_receiver_init(
		receiver, PCM_PROTOCOL_V92, G711_LAW_PCMU, 78) ==
		       PCM_PHASE3_J_RECEIVER_OK &&
	       feed_trn1d(
		       transmitter, receiver,
		       V90_PHASE3_TRN1D_MIN_SYMBOLS) &&
	       pcm_phase3_j_receiver_begin_jd_search(receiver) ==
		       PCM_PHASE3_J_RECEIVER_OK &&
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
	struct pcm_phase3_j_receiver receiver;
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
		      PCM_PHASE3_J_RECEIVER_JD_DETECTED) &&
	      feed_frame(
		      &transmitter, &receiver, different_jd_frame,
		      PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE) &&
	      pcm_phase3_j_receiver_v92_jd(&receiver, &received_jd) &&
	      jd_equal(&received_jd, &jd),
	      "receiver accepted changed Jd parameters");
	CHECK(start_records(&transmitter, &receiver) &&
	      feed_frame(
		      &transmitter, &receiver, jd_frame,
		      PCM_PHASE3_J_RECEIVER_JD_DETECTED) &&
	      feed_frame(
		      &transmitter, &receiver, jp_frame,
		      PCM_PHASE3_J_RECEIVER_JP_DETECTED) &&
	      feed_frame(
		      &transmitter, &receiver, different_jp_frame,
		      PCM_PHASE3_J_RECEIVER_INVALID_SEQUENCE) &&
	      pcm_phase3_j_receiver_v92_jp(&receiver, &received_jp) &&
	      jp_equal(&received_jp, &jp),
	      "receiver accepted changed Jp parameters");
	return true;
}

static bool test_boundaries_and_validation(void)
{
	struct v90_phase3_signs transmitter;
	struct pcm_phase3_j_receiver receiver;
	struct pcm_phase3_j_receiver saved;
	struct v92_jd jd = { .data_rate_capability_mask = 0xa5 };
	struct v92_jp jp = { .su_phase_extension_q16 = 0xa5a5 };

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(pcm_phase3_j_receiver_init(
		      &receiver, (enum pcm_protocol)99, G711_LAW_PCMU, 78) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "invalid protocol changed receiver storage");
	CHECK(pcm_phase3_j_receiver_init(NULL, PCM_PROTOCOL_V92, G711_LAW_PCMU, 78) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      pcm_phase3_j_receiver_init(
		&receiver, PCM_PROTOCOL_V92, (enum g711_law)2, 78) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      pcm_phase3_j_receiver_init(
		&receiver, PCM_PROTOCOL_V92, G711_LAW_PCMU, 66) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "invalid initialization changed state");
	CHECK(v90_phase3_signs_init(
		      &transmitter, G711_LAW_PCMA, 78) ==
		      V90_PHASE3_SIGNS_OK &&
	      pcm_phase3_j_receiver_init(
		&receiver, PCM_PROTOCOL_V92, G711_LAW_PCMA, 78) ==
		      PCM_PHASE3_J_RECEIVER_OK &&
	      feed_trn1d(
		      &transmitter, &receiver,
		      V90_PHASE3_TRN1D_MIN_SYMBOLS - 1u),
	      "could not reach early TRN1d boundary");
	saved = receiver;
	CHECK(pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      feed_trn1d(&transmitter, &receiver, 2),
	      "early search was not transactional");
	saved = receiver;
	CHECK(pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      feed_trn1d(&transmitter, &receiver, 5) &&
	      pcm_phase3_j_receiver_begin_jd_search(&receiver) ==
		      PCM_PHASE3_J_RECEIVER_OK,
	      "unaligned Jd-search boundary was not enforced");
	CHECK(pcm_phase3_j_receiver_receive(NULL, 0) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      pcm_phase3_j_receiver_begin_jd_search(NULL) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_ARGUMENT &&
	      !pcm_phase3_j_receiver_v92_jd(&receiver, &jd) &&
	      !pcm_phase3_j_receiver_v92_jd(NULL, &jd) &&
	      !pcm_phase3_j_receiver_v92_jd(&receiver, NULL) &&
	      !pcm_phase3_j_receiver_v92_jp(&receiver, &jp) &&
	      !pcm_phase3_j_receiver_v92_jp(NULL, &jp) &&
	      !pcm_phase3_j_receiver_v92_jp(&receiver, NULL) &&
	      !pcm_phase3_j_receiver_is_valid(NULL) &&
	      !pcm_phase3_j_receiver_is_complete(NULL),
	      "invalid operation changed state or output");
	receiver.protocol = (enum pcm_protocol)99;
	saved = receiver;
	CHECK(pcm_phase3_j_receiver_receive(&receiver, 0) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "invalid protocol changed receiver state");
	receiver.protocol = PCM_PROTOCOL_V92;
	receiver.stage = (enum pcm_phase3_j_receiver_stage)99;
	saved = receiver;
	CHECK(pcm_phase3_j_receiver_receive(&receiver, 0) ==
		      PCM_PHASE3_J_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "invalid receiver changed state");
	CHECK(strcmp(pcm_phase3_j_receiver_result_name(
		     PCM_PHASE3_J_RECEIVER_JP_DETECTED),
		     "Jp detected") == 0 &&
	      strcmp(pcm_phase3_j_receiver_result_name(
		     (enum pcm_phase3_j_receiver_result)10),
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
		{ "sampled signs and CRC recovery", test_sampled_signs },
		{ "unqualified signs require Jd", test_unqualified_signs },
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
