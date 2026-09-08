// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_cpt_rx.h>
#include <v92_pcm_phase3_cpt_tx.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	MID_RECORD_BITS = 37,
	CORRUPT_RECORD_BIT = 80,
};

struct cpt_pair {
	struct v92_pcm_phase3_cpt_tx transmitter;
	struct v92_pcm_phase3_cpt_rx receiver;
	struct v92_cpt_parameters parameters;
	size_t frame_bits;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters make_parameters(
	enum g711_law law, uint8_t count, bool codec_differs)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4a35),
		.shaping_a1_q1_6 = 12,
		.shaping_a2_q1_6 = -7,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.selected_rate_index = 9,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = count,
		.acknowledges_peer = true,
		.codec_constellations_differ = codec_differs,
	};

	for (uint8_t index = 0; index < count; ++index) {
		add_ucode(&parameters.transmit_constellations[index],
			  (uint8_t)(20u + index));
		add_ucode(&parameters.transmit_constellations[index],
			  (uint8_t)(80u + index));
		if (codec_differs) {
			add_ucode(&parameters.codec_constellations[index],
				  (uint8_t)(24u + index));
			add_ucode(&parameters.codec_constellations[index],
				  (uint8_t)(88u + index));
		}
	}
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval)
		parameters.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return parameters;
}

static bool parameters_match(
	const struct v92_cpt_parameters *first,
	const struct v92_cpt_parameters *second)
{
	uint8_t first_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	uint8_t second_frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t first_bits = 0u;
	size_t second_bits = 0u;

	return v92_cpt_encode(
		       first, first_frame, sizeof(first_frame), &first_bits) ==
		       V92_CPT_OK &&
	       v92_cpt_encode(
		       second, second_frame, sizeof(second_frame),
		       &second_bits) == V92_CPT_OK &&
	       first_bits == second_bits &&
	       memcmp(first_frame, second_frame, sizeof(first_frame)) == 0;
}

static uint8_t symbol_sign(const struct v90_pcm_symbol *symbol)
{
	return symbol->positive ? 0u : 1u;
}

static uint8_t packed_bit(const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static bool feed_encoded_bit(
	struct v92_pcm_phase3_cpt_rx *receiver,
	struct pcm_binary_sign *coding, uint8_t bit,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	uint8_t sign;

	return pcm_binary_sign_encode(coding, bit, &sign) ==
		       PCM_BINARY_SIGN_OK &&
	       v92_pcm_phase3_cpt_rx_receive(receiver, sign, event) ==
		       V92_PCM_PHASE3_CPT_RX_OK;
}

static bool initialize_pair(
	struct cpt_pair *pair, enum g711_law law,
	uint8_t constellation_count, bool codec_differs)
{
	struct cpt_pair initialized = {
		.parameters = make_parameters(
			law, constellation_count, codec_differs),
	};

	if (v92_pcm_phase3_cpt_tx_init(
		    &initialized.transmitter, law, 24u,
		    &initialized.parameters) != V92_PCM_PHASE3_CPT_TX_OK ||
	    v92_pcm_phase3_cpt_rx_init(&initialized.receiver) !=
		    V92_PCM_PHASE3_CPT_RX_OK)
		return false;
	initialized.frame_bits = v92_cpt_frame_bits(&initialized.parameters);
	for (size_t index = 0;
	     index < V92_PCM_PHASE3_TRN1U_MIN_SYMBOLS; ++index) {
		struct v90_pcm_symbol symbol;
		struct v92_pcm_phase3_cpt_rx_event event = { 0 };
		enum v92_pcm_phase3_cpt_rx_result result;

		if (v92_pcm_phase3_cpt_tx_next(
			    &initialized.transmitter, &symbol) !=
		    V92_PCM_PHASE3_CPT_TX_OK)
			return false;
		if (index < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS) {
			result = v92_pcm_phase3_cpt_rx_train(
				&initialized.receiver, symbol_sign(&symbol));
		} else {
			if (index == V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS &&
			    v92_pcm_phase3_cpt_rx_begin(
				    &initialized.receiver) !=
				    V92_PCM_PHASE3_CPT_RX_OK)
				return false;
			result = v92_pcm_phase3_cpt_rx_receive(
				&initialized.receiver, symbol_sign(&symbol), &event);
			if (event.type != V92_PCM_PHASE3_CPT_RX_NO_EVENT)
				return false;
		}
		if (result != V92_PCM_PHASE3_CPT_RX_OK)
			return false;
	}
	if (v92_pcm_phase3_cpt_tx_start(&initialized.transmitter) !=
	    V92_PCM_PHASE3_CPT_TX_OK)
		return false;
	*pair = initialized;
	return true;
}

static bool feed_symbol(
	struct cpt_pair *pair, bool corrupt,
	struct v92_pcm_phase3_cpt_rx_event *event)
{
	struct v90_pcm_symbol symbol;
	uint8_t sign;

	if (v92_pcm_phase3_cpt_tx_next(&pair->transmitter, &symbol) !=
	    V92_PCM_PHASE3_CPT_TX_OK)
		return false;
	sign = symbol_sign(&symbol) ^ (corrupt ? 1u : 0u);
	return v92_pcm_phase3_cpt_rx_receive(
		       &pair->receiver, sign, event) ==
	       V92_PCM_PHASE3_CPT_RX_OK;
}

static bool receive_first_record(struct cpt_pair *pair)
{
	struct v92_pcm_phase3_cpt_rx_event event;

	for (size_t index = 0;
	     index < V92_PHASE3_MARKER_BITS + pair->frame_bits; ++index) {
		if (!feed_symbol(pair, false, &event))
			return false;
		if (event.type == V92_PCM_PHASE3_CPT_RX_PARAMETERS)
			return event.frame_result == V92_CPT_OK &&
			       event.observed_bits == pair->frame_bits &&
			       parameters_match(
				       &event.parameters, &pair->parameters);
		if (event.type != V92_PCM_PHASE3_CPT_RX_NO_EVENT)
			return false;
	}
	return false;
}

static bool check_complete_sequence(
	enum g711_law law, uint8_t count, bool codec_differs)
{
	struct cpt_pair pair;
	struct v92_cpt_parameters received;
	struct v92_pcm_phase3_cpt_rx_event event;
	uint8_t previous_sign = 2u;
	bool repeated = false;
	bool e1u = false;

	CHECK(initialize_pair(&pair, law, count, codec_differs) &&
	      receive_first_record(&pair) &&
	      pair.receiver.matching_records == 1u &&
	      v92_pcm_phase3_cpt_rx_parameters(&pair.receiver, &received) &&
	      parameters_match(&received, &pair.parameters),
	      "first CPt record failed for law %u count %u",
	      (unsigned int)law, (unsigned int)count);
	for (size_t index = 0; index < MID_RECORD_BITS; ++index) {
		CHECK(feed_symbol(&pair, false, &event) &&
		      event.type == V92_PCM_PHASE3_CPT_RX_NO_EVENT,
		      "CPt repeat emitted early at bit %zu", index);
	}
	CHECK(v92_pcm_phase3_cpt_tx_request_finish(&pair.transmitter) ==
		      V92_PCM_PHASE3_CPT_TX_OK,
	      "CPt transmitter did not latch mid-record finish");
	for (size_t index = MID_RECORD_BITS;
	     index < pair.frame_bits + V92_CPT_SYMBOLS_PER_FRAME; ++index) {
		CHECK(feed_symbol(&pair, false, &event),
		      "CPt receiver failed while completing the sequence");
		if (event.type == V92_PCM_PHASE3_CPT_RX_REPEAT) {
			CHECK(!repeated && event.observed_bits == pair.frame_bits &&
			      parameters_match(
				      &event.parameters, &pair.parameters),
			      "CPt repeat event differed");
			repeated = true;
		} else if (event.type == V92_PCM_PHASE3_CPT_RX_E1U) {
			CHECK(repeated && !e1u &&
			      event.observed_bits == V92_CPT_SYMBOLS_PER_FRAME,
			      "E1u event had the wrong boundary");
			e1u = true;
		} else {
			CHECK(event.type == V92_PCM_PHASE3_CPT_RX_NO_EVENT,
			      "unexpected event %u during completion",
			      (unsigned int)event.type);
		}
	}
	CHECK(repeated && e1u &&
	      pair.receiver.matching_records == 2u &&
	      v92_pcm_phase3_cpt_rx_is_complete(&pair.receiver) &&
	      v92_pcm_phase3_cpt_tx_is_complete(&pair.transmitter) &&
	      v92_pcm_phase3_cpt_rx_previous_sign(
		      &pair.receiver, &previous_sign) &&
	      previous_sign == pair.transmitter.coding.previous_sign,
	      "CPt/E1u did not complete with continuous differential memory");
	{
		struct v92_pcm_phase3_cpt_rx before = pair.receiver;
		struct v92_pcm_phase3_cpt_rx_event before_event;

		memset(&event, 0xa5, sizeof(event));
		before_event = event;
		CHECK(v92_pcm_phase3_cpt_rx_receive(
			      &pair.receiver, 0u, &event) ==
			      V92_PCM_PHASE3_CPT_RX_DONE &&
		      memcmp(&pair.receiver, &before, sizeof(before)) == 0 &&
		      memcmp(&event, &before_event, sizeof(event)) == 0,
		      "complete CPt receiver consumed another sign");
	}
	return true;
}

static bool test_generated_sequences(void)
{
	CHECK(check_complete_sequence(G711_LAW_PCMU, 1u, false),
	      "minimal PCMU CPt sequence failed");
	CHECK(check_complete_sequence(G711_LAW_PCMA, 6u, true),
	      "maximal PCMA CPt sequence failed");
	return true;
}

static bool test_corrupt_repeat_recovery(void)
{
	struct cpt_pair pair;
	struct v92_pcm_phase3_cpt_rx_event event;
	bool corrupted = false;
	bool rejected = false;
	bool repeated = false;

	CHECK(initialize_pair(&pair, G711_LAW_PCMU, 3u, true) &&
	      receive_first_record(&pair),
	      "CPt corruption fixture did not initialize");
	for (size_t index = 0; index < 3u * pair.frame_bits; ++index) {
		bool flip = !corrupted &&
			pair.transmitter.completed_records == 1u &&
			pair.transmitter.frame_bit_index == CORRUPT_RECORD_BIT;

		CHECK(feed_symbol(&pair, flip, &event),
		      "CPt recovery stream failed at symbol %zu", index);
		corrupted |= flip;
		rejected |= event.type == V92_PCM_PHASE3_CPT_RX_REJECTED;
		if (event.type == V92_PCM_PHASE3_CPT_RX_REPEAT) {
			repeated = true;
			break;
		}
		CHECK(event.type == V92_PCM_PHASE3_CPT_RX_NO_EVENT ||
		      event.type == V92_PCM_PHASE3_CPT_RX_REJECTED,
		      "corrupt CPt emitted unexpected event %u",
		      (unsigned int)event.type);
	}
	CHECK(corrupted && rejected && repeated &&
	      pair.receiver.matching_records == 2u,
	      "CPt receiver did not reject and reacquire an identical repeat");
	return true;
}

static bool test_mismatched_repeat_and_e1u_tolerance(void)
{
	struct cpt_pair pair;
	struct v92_cpt_parameters different;
	struct pcm_binary_sign source;
	struct v92_pcm_phase3_cpt_rx_event event;
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	uint8_t previous_sign = 2u;

	CHECK(initialize_pair(&pair, G711_LAW_PCMU, 2u, false) &&
	      receive_first_record(&pair),
	      "CPt mismatch fixture did not initialize");
	different = pair.parameters;
	different.selected_rate_index++;
	source = pair.transmitter.coding;
	CHECK(v92_cpt_encode(
		      &different, frame, sizeof(frame), &frame_bits) ==
		      V92_CPT_OK &&
	      frame_bits == pair.frame_bits,
	      "alternate CPt record did not encode");
	for (size_t index = 0; index < frame_bits; ++index) {
		CHECK(feed_encoded_bit(
			      &pair.receiver, &source,
			      packed_bit(frame, index), &event),
		      "alternate CPt record failed at bit %zu", index);
		if (index + 1u < frame_bits)
			CHECK(event.type == V92_PCM_PHASE3_CPT_RX_NO_EVENT,
			      "alternate CPt record emitted early");
	}
	CHECK(event.type == V92_PCM_PHASE3_CPT_RX_REJECTED &&
	      event.frame_result == V92_CPT_INVALID_FRAME &&
	      pair.receiver.matching_records == 1u,
	      "non-identical valid CPt repeat was not rejected");
	for (size_t index = 0; index < V92_CPT_SYMBOLS_PER_FRAME; ++index) {
		CHECK(feed_encoded_bit(
			      &pair.receiver, &source, index == 5u, &event),
		      "one-error E1u failed at bit %zu", index);
		if (index + 1u < V92_CPT_SYMBOLS_PER_FRAME)
			CHECK(event.type == V92_PCM_PHASE3_CPT_RX_NO_EVENT,
			      "one-error E1u emitted early");
	}
	CHECK(event.type == V92_PCM_PHASE3_CPT_RX_E1U &&
	      v92_pcm_phase3_cpt_rx_previous_sign(
		      &pair.receiver, &previous_sign) &&
	      previous_sign == source.previous_sign,
	      "one-error E1u was not accepted at its exact boundary");
	return true;
}

static bool test_repeat_fields(void)
{
	struct cpt_pair pair;
	struct v92_cpt_parameters variants[16];

	CHECK(initialize_pair(&pair, G711_LAW_PCMU, 2u, true) &&
	      receive_first_record(&pair), "CPt field fixture failed");
	for (size_t index = 0; index < ARRAY_LENGTH(variants); ++index)
		variants[index] = pair.parameters;
	variants[0].law = G711_LAW_PCMA;
	variants[1].trn1d_ratio_q3_13++;
	variants[2].shaping_a1_q1_6++;
	variants[3].shaping_a2_q1_6++;
	variants[4].shaping_b1_q1_6++;
	variants[5].shaping_b2_q1_6++;
	variants[6].selected_rate_index++;
	variants[7].sign_redundancy++;
	variants[8].spectral_lookahead++;
	variants[9] = make_parameters(G711_LAW_PCMU, 3u, true);
	variants[10].acknowledges_peer = false;
	variants[11].codec_constellations_differ = false;
	memset(variants[11].codec_constellations, 0,
	       sizeof(variants[11].codec_constellations));
	variants[12].frame_constellation[0] = 1u;
	add_ucode(&variants[13].transmit_constellations[0], 127u);
	add_ucode(&variants[14].transmit_constellations[1], 127u);
	add_ucode(&variants[15].codec_constellations[1], 127u);
	for (size_t index = 0; index < ARRAY_LENGTH(variants); ++index) {
		struct v92_pcm_phase3_cpt_rx receiver = pair.receiver;
		struct pcm_binary_sign source = pair.transmitter.coding;
		struct v92_pcm_phase3_cpt_rx_event event;
		struct v92_cpt_parameters accepted;
		uint8_t frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
		size_t bits = 0u;

		CHECK(!parameters_match(&variants[index], &pair.parameters) &&
		      v92_cpt_encode(&variants[index], frame, sizeof(frame),
				     &bits) == V92_CPT_OK,
		      "CPt variant %zu did not change the wire fields", index);
		for (size_t bit = 0; bit < bits; ++bit)
			CHECK(feed_encoded_bit(&receiver, &source,
					       packed_bit(frame, bit), &event),
			      "CPt variant %zu failed at bit %zu", index, bit);
		CHECK(event.type == V92_PCM_PHASE3_CPT_RX_REJECTED &&
		      event.frame_result == V92_CPT_INVALID_FRAME &&
		      receiver.matching_records == 1u &&
		      v92_pcm_phase3_cpt_rx_parameters(&receiver, &accepted) &&
		      parameters_match(&accepted, &pair.parameters),
		      "CPt variant %zu replaced the first accepted record", index);
		/* A rejected, CRC-valid repeat must not poison the next repeat. */
		CHECK(v92_cpt_encode(&pair.parameters, frame, sizeof(frame),
				    &bits) == V92_CPT_OK,
		      "CPt recovery record failed to encode");
		for (size_t bit = 0; bit < bits; ++bit)
			CHECK(feed_encoded_bit(&receiver, &source,
					       packed_bit(frame, bit), &event),
			      "CPt recovery %zu failed at bit %zu", index, bit);
		CHECK(event.type == V92_PCM_PHASE3_CPT_RX_REPEAT &&
		      receiver.matching_records == 2u,
		      "CPt variant %zu prevented a later valid repeat", index);
	}
	/* Validation still rejects an invalid negotiated configuration. */
	pair.receiver.parameters.constellation_count = 0u;
	CHECK(!v92_pcm_phase3_cpt_rx_is_valid(&pair.receiver),
	      "CPt receiver accepted invalid stored parameters");
	return true;
}

static bool test_contracts(void)
{
	struct v92_pcm_phase3_cpt_rx receiver;
	struct v92_pcm_phase3_cpt_rx before;
	struct v92_phase3_sync training;
	struct v92_pcm_phase3_cpt_rx_event event;
	struct v92_pcm_phase3_cpt_rx_event before_event;
	struct v92_cpt_parameters parameters;
	struct v92_cpt_parameters before_parameters;
	uint8_t previous_sign = 0xa5u;
	static const char *const names[] = {
		"ok", "done", "invalid argument", "invalid state",
		"internal error",
	};

	CHECK(v92_pcm_phase3_cpt_rx_init(NULL) ==
			      V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_rx_init(&receiver) ==
		      V92_PCM_PHASE3_CPT_RX_OK &&
	      v92_pcm_phase3_cpt_rx_is_valid(&receiver) &&
	      !v92_pcm_phase3_cpt_rx_is_complete(&receiver),
		      "CPt receiver did not initialize");
	CHECK(v92_phase3_sync_init(&training) == V92_PHASE3_SYNC_OK,
	      "CPt training snapshot did not initialize");
	before = receiver;
	CHECK(v92_pcm_phase3_cpt_rx_init_from_training(NULL, &training) ==
			      V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_rx_init_from_training(&receiver, NULL) ==
			      V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_rx_init_from_training(
		      &receiver, &training) ==
			      V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "short CPt training snapshot changed receiver state");
	for (size_t index = 0;
	     index < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS; ++index)
		CHECK(v92_phase3_sync_train(&training, index & 1u) ==
			      V92_PHASE3_SYNC_OK,
		      "CPt training snapshot failed at %zu", index);
	CHECK(v92_pcm_phase3_cpt_rx_init_from_training(
		      &receiver, &training) == V92_PCM_PHASE3_CPT_RX_OK &&
	      v92_pcm_phase3_cpt_rx_is_valid(&receiver) &&
	      receiver.stage == V92_PCM_PHASE3_CPT_RX_SEARCHING &&
	      receiver.sync.stage == V92_PHASE3_SYNC_SEARCHING &&
	      receiver.sync.trained_symbols == training.trained_symbols &&
	      receiver.sync.training.scrambler.history ==
		      training.training.scrambler.history &&
	      receiver.sync.training.previous_sign ==
		      training.training.previous_sign,
	      "trained CPt initialization lost synchronizer state");
	CHECK(v92_pcm_phase3_cpt_rx_init(&receiver) ==
		      V92_PCM_PHASE3_CPT_RX_OK,
	      "CPt receiver did not reset after trained initialization");
	memset(&event, 0xa5, sizeof(event));
	before_event = event;
	before = receiver;
	CHECK(v92_pcm_phase3_cpt_rx_train(&receiver, 2u) ==
		      V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase3_cpt_rx_receive(&receiver, 0u, &event) ==
		      V92_PCM_PHASE3_CPT_RX_INVALID_STATE &&
	      v92_pcm_phase3_cpt_rx_begin(&receiver) ==
		      V92_PCM_PHASE3_CPT_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &before_event, sizeof(event)) == 0,
	      "invalid training calls changed CPt receiver state");
	memset(&parameters, 0xa5, sizeof(parameters));
	before_parameters = parameters;
	CHECK(!v92_pcm_phase3_cpt_rx_parameters(&receiver, &parameters) &&
	      memcmp(&parameters, &before_parameters, sizeof(parameters)) == 0 &&
	      !v92_pcm_phase3_cpt_rx_previous_sign(
		      &receiver, &previous_sign) &&
	      previous_sign == 0xa5u,
	      "unavailable CPt getters changed outputs");
	for (size_t index = 0;
	     index < V92_PHASE3_SYNC_MIN_TRAINING_SYMBOLS; ++index)
		CHECK(v92_pcm_phase3_cpt_rx_train(&receiver, index & 1u) ==
			      V92_PCM_PHASE3_CPT_RX_OK,
		      "CPt receiver training failed at %zu", index);
	CHECK(v92_pcm_phase3_cpt_rx_begin(&receiver) ==
		      V92_PCM_PHASE3_CPT_RX_OK,
	      "CPt receiver did not begin search");
	before = receiver;
	CHECK(v92_pcm_phase3_cpt_rx_begin(&receiver) ==
		      V92_PCM_PHASE3_CPT_RX_INVALID_STATE &&
	      v92_pcm_phase3_cpt_rx_train(&receiver, 0u) ==
		      V92_PCM_PHASE3_CPT_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "CPt search accepted an invalid transition");
	receiver.stage = (enum v92_pcm_phase3_cpt_rx_stage)-1;
	before = receiver;
	CHECK(!v92_pcm_phase3_cpt_rx_is_valid(&receiver) &&
	      v92_pcm_phase3_cpt_rx_receive(&receiver, 0u, &event) ==
		      V92_PCM_PHASE3_CPT_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "corrupt CPt receiver state was not rejected transactionally");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_phase3_cpt_rx_result_name(
				      (enum v92_pcm_phase3_cpt_rx_result)index),
			      names[index]) == 0,
		      "CPt receiver result name %zu differed", index);
	CHECK(strcmp(
		      v92_pcm_phase3_cpt_rx_result_name(
			      (enum v92_pcm_phase3_cpt_rx_result)-1),
		      "unknown") == 0,
	      "unknown CPt receiver result name differed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated CPt and E1u sequences", test_generated_sequences },
		{ "corrupt repeat recovery", test_corrupt_repeat_recovery },
		{ "repeat identity and E1u tolerance",
		  test_mismatched_repeat_and_e1u_tolerance },
		{ "repeat field identity and recovery", test_repeat_fields },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
