// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v34_constellation.h"
#include "v34_phase3_trn.h"
#include "v90_ja.h"

#include <stdint.h>
#include <string.h>

enum {
	HISTORY_BITS = 23,
	GPA_FIRST_TAP = 5,
	TRN_PREFIX_SYMBOLS = 617,
	JA_SYMBOLS = V90_DIL_ZERO_DESCRIPTOR_BITS / 2,
};

struct reference_stream {
	uint8_t history[HISTORY_BITS];
	uint8_t previous_rotation;
};

static bool prepare_trn(
	struct v34_phase3_trn *trn, enum v34_station_role role,
	unsigned int symbols)
{
	struct qam_point ignored_point;
	uint8_t ignored_rotation;

	if (v34_phase3_trn_init(trn, role) != V34_PHASE3_TRN_OK)
		return false;
	for (unsigned int symbol = 0; symbol < symbols; ++symbol) {
		if (v34_phase3_trn_next(
			    trn, &ignored_point, &ignored_rotation) !=
		    V34_PHASE3_TRN_OK)
			return false;
	}
	return true;
}

static void reference_init(
	struct reference_stream *stream,
	const struct v34_phase3_trn *trn, uint8_t phase_offset)
{
	memset(stream, 0, sizeof(*stream));
	for (size_t delay = 0; delay < HISTORY_BITS; ++delay)
		stream->history[delay] =
			(uint8_t)((trn->scrambler.history >> delay) & 1u);
	stream->previous_rotation =
		(uint8_t)((trn->last_clockwise_quarter_turns + phase_offset) & 3u);
}

static uint8_t reference_scramble_bit(
	struct reference_stream *stream, uint8_t input)
{
	uint8_t output = input ^ stream->history[GPA_FIRST_TAP - 1u] ^
			 stream->history[HISTORY_BITS - 1u];

	for (size_t delay = HISTORY_BITS - 1u; delay > 0; --delay)
		stream->history[delay] = stream->history[delay - 1u];
	stream->history[0] = output;
	return output;
}

static uint8_t descriptor_bit(
	const uint8_t *descriptor, size_t index)
{
	return (uint8_t)((descriptor[index / 8u] >>
			  (7u - index % 8u)) &
			 1u);
}

static uint8_t reference_next(
	struct reference_stream *stream, const uint8_t *descriptor,
	size_t symbol)
{
	size_t bit = symbol * 2u;
	uint8_t first = reference_scramble_bit(
		stream, descriptor_bit(descriptor, bit));
	uint8_t second = reference_scramble_bit(
		stream, descriptor_bit(descriptor, bit + 1u));
	uint8_t increment = (uint8_t)((second << 1) | first);

	stream->previous_rotation =
		(uint8_t)((stream->previous_rotation + increment) & 3u);
	return stream->previous_rotation;
}

static bool test_exact_repeating_stream(void)
{
	struct v34_phase3_trn trn;
	struct v90_ja_tx tx;
	struct reference_stream reference;
	uint8_t descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES];

	CHECK(prepare_trn(
		      &trn, V34_ANSWERING_STATION, TRN_PREFIX_SYMBOLS) &&
	      v90_dil_encode_zero(descriptor, sizeof(descriptor)) ==
		      V90_DIL_OK &&
	      v90_ja_tx_init_from_trn(&tx, &trn) == V90_JA_OK &&
	      v90_ja_tx_is_valid(&tx) &&
	      v90_ja_tx_at_descriptor_boundary(&tx),
	      "Ja stream fixture did not initialize");
	reference_init(&reference, &trn, 0);
	for (size_t symbol = 0; symbol < JA_SYMBOLS * 3u; ++symbol) {
		struct qam_point point = { 9.0, -7.0 };
		struct qam_point expected;
		uint8_t rotation = UINT8_MAX;
		uint8_t expected_rotation = reference_next(
			&reference, descriptor, symbol % JA_SYMBOLS);

		expected = v34_rotated_point_zero(-(int)expected_rotation);
		CHECK(v90_ja_tx_at_descriptor_boundary(&tx) ==
			      (symbol % JA_SYMBOLS == 0) &&
		      v90_ja_tx_next(&tx, &point, &rotation) == V90_JA_OK &&
		      rotation == expected_rotation &&
		      memcmp(&point, &expected, sizeof(point)) == 0 &&
		      v90_ja_tx_at_descriptor_boundary(&tx) ==
			      ((symbol + 1u) % JA_SYMBOLS == 0),
		      "Ja symbol %zu changed", symbol);
	}
	return true;
}

static bool test_round_trip_with_phase_offsets(void)
{
	struct v34_phase3_trn trn;

	CHECK(prepare_trn(
		      &trn, V34_ANSWERING_STATION, TRN_PREFIX_SYMBOLS + 29u),
	      "Ja round-trip TRN failed");
	for (uint8_t offset = 0; offset < 4u; ++offset) {
		struct v90_ja_tx tx;
		struct v90_ja_rx rx;

		CHECK(v90_ja_tx_init_from_trn(&tx, &trn) == V90_JA_OK &&
		      v90_ja_rx_init_from_trn(
			      &rx, &trn,
			      (uint8_t)((trn.last_clockwise_quarter_turns +
					 offset) &
					3u)) == V90_JA_OK,
		      "Ja phase-offset %u setup failed", offset);
		for (size_t symbol = 0; symbol < JA_SYMBOLS; ++symbol) {
			struct qam_point point;
			uint8_t rotation;
			enum v90_ja_result expected =
				symbol + 1u == JA_SYMBOLS ? V90_JA_DETECTED :
							     V90_JA_OK;

			CHECK(v90_ja_tx_next(&tx, &point, &rotation) ==
				      V90_JA_OK &&
			      v90_ja_rx_receive(
				      &rx,
				      (uint8_t)((rotation + offset) & 3u)) ==
				      expected,
			      "Ja phase-offset %u stopped at %zu",
			      offset, symbol);
		}
		CHECK(v90_ja_rx_is_complete(&rx) &&
		      v90_ja_rx_receive(&rx, 0) == V90_JA_COMPLETE,
		      "Ja phase-offset %u did not latch completion", offset);
	}
	return true;
}

static enum v90_ja_result feed_reference_descriptor(
	struct v90_ja_rx *rx, struct reference_stream *reference,
	const uint8_t *descriptor)
{
	enum v90_ja_result result = V90_JA_OK;

	for (size_t symbol = 0; symbol < JA_SYMBOLS; ++symbol) {
		result = v90_ja_rx_receive(
			rx, reference_next(reference, descriptor, symbol));
		if (symbol + 1u < JA_SYMBOLS && result != V90_JA_OK)
			return V90_JA_INVALID_ARGUMENT;
	}
	return result;
}

static bool test_rejected_repetitions_recover(void)
{
	struct v34_phase3_trn trn;
	struct v90_ja_rx rx;
	struct reference_stream reference;
	uint8_t canonical[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	uint8_t damaged[V90_DIL_ZERO_DESCRIPTOR_BYTES];
	uint8_t unsupported[V90_DIL_ZERO_DESCRIPTOR_BYTES];

	CHECK(prepare_trn(
		      &trn, V34_ANSWERING_STATION, TRN_PREFIX_SYMBOLS) &&
	      v90_dil_encode_zero(canonical, sizeof(canonical)) == V90_DIL_OK &&
	      v90_ja_rx_init_from_trn(
		      &rx, &trn, trn.last_clockwise_quarter_turns) == V90_JA_OK,
	      "Ja recovery fixture did not initialize");
	reference_init(&reference, &trn, 0);
	memcpy(damaged, canonical, sizeof(damaged));
	damaged[6] ^= 0x08u; /* Descriptor bit 52, covered by the CRC. */
	memcpy(unsupported, canonical, sizeof(unsupported));
	unsupported[2] |= 0x20u; /* N bit zero: request one DIL segment. */
	CHECK(feed_reference_descriptor(&rx, &reference, damaged) ==
		      V90_JA_REJECTED &&
	      rx.bit_index == 0 && !v90_ja_rx_is_complete(&rx) &&
	      feed_reference_descriptor(&rx, &reference, unsupported) ==
		      V90_JA_UNSUPPORTED &&
	      rx.bit_index == 0 && !v90_ja_rx_is_complete(&rx) &&
	      feed_reference_descriptor(&rx, &reference, canonical) ==
		      V90_JA_DETECTED &&
	      v90_ja_rx_is_complete(&rx) &&
	      memcmp(rx.descriptor, canonical, sizeof(canonical)) == 0,
	      "Ja receiver did not recover on a later descriptor repetition");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_trn empty;
	struct v34_phase3_trn wrong_polynomial;
	struct v34_phase3_trn trn;
	struct v90_ja_tx tx;
	struct v90_ja_tx saved_tx;
	struct v90_ja_rx rx;
	struct v90_ja_rx saved_rx;
	struct qam_point point = { 4.0, -3.0 };
	struct qam_point saved_point = point;
	uint8_t rotation = UINT8_MAX;

	memset(&tx, 0xa5, sizeof(tx));
	saved_tx = tx;
	CHECK(v34_phase3_trn_init(&empty, V34_ANSWERING_STATION) ==
		      V34_PHASE3_TRN_OK &&
	      prepare_trn(
		      &wrong_polynomial, V34_CALLING_STATION,
		      V34_PHASE3_TRN_MIN_SYMBOLS) &&
	      v90_ja_tx_init_from_trn(NULL, &empty) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_tx_init_from_trn(&tx, NULL) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_tx_init_from_trn(&tx, &empty) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_tx_init_from_trn(&tx, &wrong_polynomial) ==
		      V90_JA_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved_tx, sizeof(tx)) == 0,
	      "Ja accepted an invalid TRN boundary");
	CHECK(prepare_trn(
		      &trn, V34_ANSWERING_STATION,
		      V34_PHASE3_TRN_MIN_SYMBOLS) &&
	      v90_ja_tx_init_from_trn(&tx, &trn) == V90_JA_OK,
	      "Ja validation stream did not initialize");
	saved_tx = tx;
	CHECK(v90_ja_tx_next(NULL, &point, &rotation) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_tx_next(&tx, NULL, &rotation) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_tx_next(&tx, &point, NULL) ==
		      V90_JA_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved_tx, sizeof(tx)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0 &&
	      rotation == UINT8_MAX,
	      "Ja invalid transmit call mutated state or output");
	memset(&rx, 0xa5, sizeof(rx));
	saved_rx = rx;
	CHECK(v90_ja_rx_init_from_trn(NULL, &trn, 0) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_rx_init_from_trn(&rx, NULL, 0) ==
		      V90_JA_INVALID_ARGUMENT &&
	      v90_ja_rx_init_from_trn(&rx, &trn, 4) ==
		      V90_JA_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved_rx, sizeof(rx)) == 0 &&
	      !v90_ja_rx_is_complete(NULL),
	      "Ja accepted an invalid receive boundary");
	CHECK(v90_ja_rx_init_from_trn(
		      &rx, &trn, trn.last_clockwise_quarter_turns) == V90_JA_OK,
	      "Ja validation receiver did not initialize");
	saved_rx = rx;
	CHECK(v90_ja_rx_receive(NULL, 0) == V90_JA_INVALID_ARGUMENT &&
	      v90_ja_rx_receive(&rx, 4) == V90_JA_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved_rx, sizeof(rx)) == 0,
	      "Ja invalid receive call mutated state");
	tx.bit_index = 1;
	rx.descriptor[V90_DIL_ZERO_DESCRIPTOR_BYTES - 1u] = 1;
	CHECK(!v90_ja_tx_is_valid(&tx) &&
	      !v90_ja_tx_at_descriptor_boundary(&tx) &&
	      !v90_ja_rx_is_valid(&rx) &&
	      strcmp(v90_ja_result_name(V90_JA_DETECTED), "detected") == 0 &&
	      strcmp(v90_ja_result_name(V90_JA_UNSUPPORTED), "unsupported") ==
		      0 &&
	      strcmp(v90_ja_result_name((enum v90_ja_result)99), "unknown") ==
		      0,
	      "Ja validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact repeating stream", test_exact_repeating_stream },
		{ "phase-offset round trip", test_round_trip_with_phase_offsets },
		{ "rejected repetitions recover", test_rejected_repetitions_recover },
		{ "validation", test_validation },
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
