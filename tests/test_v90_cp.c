// SPDX-License-Identifier: BSD-3-Clause

#include <v90_cp.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	REFERENCE_CRC_POLYNOMIAL = 0x1021,
	REFERENCE_CRC_INITIAL = 0xffff,
	CP_INFORMATION_FIRST_BIT = 18,
	CP_RATE_FIRST_BIT = 20,
	CP_RATE_BITS = 5,
	CP_MAP_FIRST_BIT = 103,
	CP_MASKS_FIRST_BIT = V90_CP_HEADER_BITS,
	CP_MINIMUM_PAYLOAD_BITS =
		V90_CP_HEADER_BITS + V90_CP_CONSTELLATION_BITS,
	CP_CRC_BITS = 16,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters baseline(void)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = G711_LAW_PCMU,
		.upstream_rate_mask = V90_CP_UPSTREAM_RATE_MASK,
		.trn1d_ratio_q3_13 = 0x2000,
		.selected_rate_index = 1,
		.constellation_count = 1,
	};

	add_ucode(&parameters.transmit_constellations[0], 0);
	add_ucode(&parameters.transmit_constellations[0], 127);
	return parameters;
}

static struct v90_cp_parameters wire_parameters(void)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_DATA,
		.law = G711_LAW_PCMA,
		.upstream_rate_mask = 0x1555,
		.trn1d_ratio_q3_13 = 0x9abc,
		.shaping_a1_q1_6 = -64,
		.shaping_a2_q1_6 = 63,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = -127,
		.selected_rate_index = V90_CP_RATE_INDEX_MAX,
		.sign_redundancy = 2,
		.spectral_lookahead = 3,
		.constellation_count = 1,
		.request_silence = true,
		.acknowledges_peer = true,
	};
	static const uint8_t ucodes[] = {
		0, 1, 15, 16, 31, 32, 63, 64, 95, 96, 126, 127,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(ucodes); ++index)
		add_ucode(&parameters.transmit_constellations[0], ucodes[index]);
	return parameters;
}

static struct v90_cp_parameters variable_parameters(
	uint8_t count, bool codec_constellations_differ)
{
	struct v90_cp_parameters parameters = wire_parameters();

	memset(parameters.transmit_constellations, 0,
	       sizeof(parameters.transmit_constellations));
	parameters.constellation_count = count;
	parameters.codec_constellations_differ =
		codec_constellations_differ;
	for (size_t index = 0; index < count; ++index) {
		add_ucode(
			&parameters.transmit_constellations[index],
			(uint8_t)(index * 19u));
		add_ucode(
			&parameters.transmit_constellations[index],
			(uint8_t)(127u - index));
		if (codec_constellations_differ) {
			add_ucode(
				&parameters.codec_constellations[index],
				(uint8_t)(index * 7u));
			add_ucode(
				&parameters.codec_constellations[index],
				(uint8_t)(126u - index));
		}
	}
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		parameters.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return parameters;
}

static uint8_t reference_frame_bit(
	const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void replace_frame_bit(
	uint8_t *frame, size_t bit_index, uint8_t bit)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit_index % 8u));

	if (bit != 0)
		frame[bit_index / 8u] |= mask;
	else
		frame[bit_index / 8u] &= (uint8_t)~mask;
}

static void replace_lsb_first(
	uint8_t *frame, size_t first_bit, size_t bit_count,
	uint32_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		replace_frame_bit(
			frame, first_bit + index,
			(uint8_t)(value >> index) & 1u);
}

static uint16_t reference_crc(
	const uint8_t *frame, size_t payload_end)
{
	uint16_t remainder = REFERENCE_CRC_INITIAL;

	for (size_t index = CP_INFORMATION_FIRST_BIT;
	     index < payload_end; ++index) {
		uint16_t next;

		if (index % 17u == 0)
			continue;
		next = (uint16_t)(remainder << 1);
		if ((((remainder >> 15) ^ reference_frame_bit(frame, index)) &
		     1u) != 0)
			next ^= REFERENCE_CRC_POLYNOMIAL;
		remainder = next;
	}
	return remainder;
}

static void replace_crc(uint8_t *frame, size_t payload_end)
{
	uint16_t crc = reference_crc(frame, payload_end);

	for (size_t index = 0; index < CP_CRC_BITS; ++index)
		replace_frame_bit(
			frame, payload_end + 1u + index,
			(uint8_t)(crc >> (CP_CRC_BITS - index - 1u)) & 1u);
}

static bool test_dimensions_and_boundaries(void)
{
	struct v90_cp_parameters parameters = baseline();

	CHECK(V90_CP_FRAME_INTERVALS == 6 &&
	      V90_CP_MAX_CONSTELLATIONS == 6 &&
	      V90_CP_CONSTELLATION_UCODES == 128 &&
	      V90_CP_CONSTELLATION_MASK_BYTES == 16 &&
	      V90_CP_HEADER_BITS == 136 &&
	      V90_CP_CONSTELLATION_BITS == 136 &&
	      V90_CP_TRAILER_BITS == 20 &&
	      V90_CP_MIN_FRAME_BITS == 292 &&
	      V90_CP_MAX_FRAME_BITS == 1788 &&
	      V90_CP_MAX_FRAME_BYTES == 224 &&
	      V90_CP_RATE_INDEX_MAX == 22 &&
	      V90_CP_UPSTREAM_RATE_MASK == 0x1fff,
	      "CP dimensions changed");
	CHECK(v90_cp_parameters_are_valid(&parameters) &&
	      v90_cp_frame_bits(&parameters) == V90_CP_MIN_FRAME_BITS &&
	      v90_cp_frame_bytes(&parameters) == 37,
	      "minimum CP frame dimensions are wrong");

	parameters.kind = V90_CP_DATA;
	parameters.law = G711_LAW_PCMA;
	parameters.selected_rate_index = V90_CP_RATE_INDEX_MAX;
	parameters.sign_redundancy = 3;
	parameters.spectral_lookahead = 3;
	parameters.request_silence = true;
	parameters.acknowledges_peer = true;
	parameters.codec_constellations_differ = true;
	parameters.constellation_count = V90_CP_MAX_CONSTELLATIONS;
	for (size_t index = 0;
	     index < V90_CP_MAX_CONSTELLATIONS; ++index) {
		parameters.frame_constellation[index] = (uint8_t)index;
		memset(&parameters.transmit_constellations[index], 0,
		       sizeof(parameters.transmit_constellations[index]));
		add_ucode(
			&parameters.transmit_constellations[index],
			(uint8_t)(index * 17u));
		add_ucode(
			&parameters.codec_constellations[index],
			(uint8_t)(127u - index * 17u));
	}
	CHECK(v90_cp_parameters_are_valid(&parameters) &&
	      v90_cp_frame_bits(&parameters) == V90_CP_MAX_FRAME_BITS &&
	      v90_cp_frame_bytes(&parameters) == V90_CP_MAX_FRAME_BYTES,
	      "maximum CP frame dimensions are wrong");
	return true;
}

static bool test_constellation_membership(void)
{
	struct pcm_constellation constellation = { 0 };

	add_ucode(&constellation, 0);
	add_ucode(&constellation, 63);
	add_ucode(&constellation, 127);
	CHECK(pcm_constellation_contains(&constellation, 0) &&
	      pcm_constellation_contains(&constellation, 63) &&
	      pcm_constellation_contains(&constellation, 127) &&
	      !pcm_constellation_contains(&constellation, 1) &&
	      !pcm_constellation_contains(&constellation, 128) &&
	      !pcm_constellation_contains(NULL, 0),
	      "CP constellation membership changed");
	return true;
}

static bool test_constellation_cardinality(void)
{
	struct pcm_constellation constellation;

	CHECK(pcm_constellation_count(NULL) == 0 &&
	      !pcm_constellation_is_empty(NULL),
	      "missing constellation was accepted as an empty set");
	/* Every byte value in every position, both isolated and surrounded by
	 * set bits, covers the packed sum and the full 128-code boundary. */
	for (unsigned int value = 0; value <= UINT8_MAX; ++value) {
		unsigned int expected = 0;

		for (unsigned int bit = 0; bit < 8u; ++bit)
			expected += (value >> bit) & 1u;
		for (size_t byte = 0;
		     byte < PCM_CONSTELLATION_MASK_BYTES; ++byte) {
			memset(&constellation, 0, sizeof(constellation));
			constellation.ucode_mask[byte] = (uint8_t)value;
			CHECK(pcm_constellation_count(&constellation) == expected &&
			      pcm_constellation_is_empty(&constellation) ==
				      (expected == 0),
			      "isolated cardinality differs for byte %zu value %u",
			      byte, value);
			memset(&constellation, 0xff, sizeof(constellation));
			constellation.ucode_mask[byte] = (uint8_t)value;
			CHECK(pcm_constellation_count(&constellation) ==
				      PCM_CONSTELLATION_UCODES - 8u + expected &&
			      !pcm_constellation_is_empty(&constellation),
			      "dense cardinality differs for byte %zu value %u",
			      byte, value);
		}
	}
	return true;
}

static bool test_scalar_validation(void)
{
	struct v90_cp_parameters valid = baseline();
	struct v90_cp_parameters changed;

	changed = valid;
	changed.kind = (enum v90_cp_kind)99;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an unknown kind");
	changed = valid;
	changed.law = (enum g711_law)99;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an unknown law");
	changed = valid;
	changed.selected_rate_index = V90_CP_RATE_INDEX_MAX + 1u;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an out-of-range selected rate");
	changed = valid;
	changed.sign_redundancy = 4;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted out-of-range sign redundancy");
	changed = valid;
	changed.spectral_lookahead = 4;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted out-of-range lookahead");
	changed = valid;
	changed.upstream_rate_mask |= 0x2000;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an undefined upstream-rate bit");
	changed = valid;
	changed.constellation_count = 0;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted no constellations");
	changed = valid;
	changed.constellation_count = V90_CP_MAX_CONSTELLATIONS + 1u;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted too many constellations");
	CHECK(!v90_cp_parameters_are_valid(NULL) &&
	      v90_cp_frame_bits(NULL) == 0 &&
	      v90_cp_frame_bytes(NULL) == 0 &&
	      v90_cp_frame_bits(&changed) == 0 &&
	      v90_cp_frame_bytes(&changed) == 0,
	      "invalid CP parameters produced a frame size");
	return true;
}

static bool test_canonical_masks(void)
{
	struct v90_cp_parameters valid = baseline();
	struct v90_cp_parameters changed;

	CHECK(!pcm_constellations_are_canonical(
		      NULL, valid.codec_constellations,
		      valid.frame_constellation, valid.constellation_count, false) &&
	      !pcm_constellations_are_canonical(
		      valid.transmit_constellations, NULL,
		      valid.frame_constellation, valid.constellation_count, false) &&
	      !pcm_constellations_are_canonical(
		      valid.transmit_constellations, valid.codec_constellations,
		      NULL, valid.constellation_count, false),
	      "canonical layout accepted missing storage");
	changed = valid;
	memset(&changed.transmit_constellations[0], 0,
	       sizeof(changed.transmit_constellations[0]));
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an empty active constellation");
	changed = valid;
	add_ucode(&changed.transmit_constellations[1], 1);
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an unused transmit constellation");
	changed = valid;
	add_ucode(&changed.codec_constellations[0], 0);
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted codec masks without the difference flag");
	changed = valid;
	changed.codec_constellations_differ = true;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an empty active codec constellation");
	add_ucode(&changed.codec_constellations[0], 0);
	CHECK(v90_cp_parameters_are_valid(&changed),
	      "CP rejected a complete codec constellation");
	add_ucode(&changed.codec_constellations[1], 1);
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an unused codec constellation");

	changed = valid;
	changed.constellation_count = 2;
	add_ucode(&changed.transmit_constellations[1], 64);
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted a transmitted but unreferenced constellation");
	changed.frame_constellation[5] = 1;
	CHECK(v90_cp_parameters_are_valid(&changed),
	      "CP rejected two referenced constellations");
	changed.frame_constellation[4] = 2;
	CHECK(!v90_cp_parameters_are_valid(&changed),
	      "CP accepted an out-of-range constellation index");
	return true;
}

static bool test_known_wire_image(void)
{
	static const uint8_t expected[] = {
		0xff, 0xff, 0x96, 0x82, 0xda, 0xaa, 0xe3, 0xd5,
		0x90, 0x1f, 0xe3, 0xfe, 0x04, 0x00, 0x00, 0x00,
		0x00, 0x60, 0x00, 0xa0, 0x00, 0x50, 0x00, 0x00,
		0x00, 0x14, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
		0x00, 0x03, 0x66, 0xb2, 0x00,
	};
	struct v90_cp_parameters parameters = wire_parameters();
	struct v90_cp_parameters decoded = { 0 };
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits = 0;

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v90_cp_encode(
		      &parameters, frame, sizeof(frame), &frame_bits) ==
		      V90_CP_OK &&
	      frame_bits == V90_CP_MIN_FRAME_BITS &&
	      sizeof(expected) == v90_cp_frame_bytes(&parameters) &&
	      memcmp(frame, expected, sizeof(expected)) == 0 &&
	      frame[sizeof(expected)] == 0xa5 &&
	      reference_crc(frame, CP_MINIMUM_PAYLOAD_BITS) == 0xcd64,
	      "CP minimum wire image changed");
	CHECK(v90_cp_decode(
		      frame, sizeof(expected), frame_bits, &decoded) ==
		      V90_CP_OK &&
	      v90_cp_parameters_are_equal(&decoded, &parameters),
	      "CP minimum wire image did not round-trip");
	return true;
}

static bool test_variable_round_trip(void)
{
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];

	for (uint8_t count = 1;
	     count <= V90_CP_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int codec_copy = 0; codec_copy < 2;
		     ++codec_copy) {
			bool differs = codec_copy != 0;
			struct v90_cp_parameters parameters =
				variable_parameters(count, differs);
			struct v90_cp_parameters decoded = { 0 };
			size_t expected_bits =
				V90_CP_HEADER_BITS +
				(size_t)count *
					(differs ? 2u : 1u) *
					V90_CP_CONSTELLATION_BITS +
				V90_CP_TRAILER_BITS;
			size_t frame_bits = 0;

			CHECK(v90_cp_encode(
				      &parameters, frame, sizeof(frame),
				      &frame_bits) == V90_CP_OK &&
			      frame_bits == expected_bits &&
			      v90_cp_decode(
				      frame, (frame_bits + 7u) / 8u,
				      frame_bits, &decoded) == V90_CP_OK &&
			      v90_cp_parameters_are_equal(&decoded, &parameters),
			      "CP count %u codec-copy %u did not round-trip",
			      count, codec_copy);
		}
	}
	return true;
}

static bool rejected_decode_is_transactional(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bits, enum v90_cp_result expected)
{
	struct v90_cp_parameters output;
	struct v90_cp_parameters saved;

	memset(&output, 0xa5, sizeof(output));
	saved = output;
	CHECK(v90_cp_decode(
		      frame, frame_capacity, frame_bits, &output) == expected &&
	      memcmp(&output, &saved, sizeof(output)) == 0,
	      "rejected CP decode was not transactional");
	return true;
}

static bool test_cleardown(void)
{
	struct v90_cp_parameters parameters = wire_parameters();
	struct v90_cp_parameters decoded;
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	size_t bits;

	parameters.selected_rate_index = 0u;
	parameters.constellation_count = V90_CP_MAX_CONSTELLATIONS;
	parameters.frame_constellation[0] = V90_CP_MAX_CONSTELLATIONS - 1u;
	parameters.codec_constellations_differ = true;
	memset(parameters.transmit_constellations, 0,
	       sizeof(parameters.transmit_constellations));
	/* Empty, unreferenced, and unequal codec rows are unused in cleardown. */
	add_ucode(&parameters.codec_constellations[1], 12u);
	CHECK(v90_cp_parameters_are_valid(&parameters) &&
	      v90_cp_encode(&parameters, frame, sizeof(frame), &bits) == V90_CP_OK &&
	      v90_cp_decode(frame, sizeof(frame), bits, &decoded) == V90_CP_OK &&
	      v90_cp_parameters_are_equal(&parameters, &decoded),
	      "zero-rate CP interpreted its unused constellations");
	/* Neither zero rate nor ignored constellation contents bypass integrity. */
	replace_frame_bit(frame, CP_MASKS_FIRST_BIT + 1u, 1u);
	CHECK(rejected_decode_is_transactional(frame, sizeof(frame), bits,
					      V90_CP_CRC_MISMATCH),
	      "zero-rate CP bypassed its CRC");
	parameters.selected_rate_index = 1u;
	CHECK(!v90_cp_parameters_are_valid(&parameters),
	      "nonzero CP accepted unused cleardown constellations");
	parameters.selected_rate_index = 0u;
	parameters.frame_constellation[0] = V90_CP_MAX_CONSTELLATIONS;
	CHECK(!v90_cp_parameters_are_valid(&parameters),
	      "zero-rate CP bypassed wire-layout bounds");
	parameters.frame_constellation[0] = 0u;
	CHECK(!v90_cp_parameters_are_valid(&parameters),
	      "zero-rate CP accepted a length not declared on the wire");
	return true;
}

static bool test_call_contracts(void)
{
	struct v90_cp_parameters parameters = wire_parameters();
	struct v90_cp_parameters invalid = parameters;
	struct v90_cp_parameters output;
	struct v90_cp_parameters saved_output;
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	uint8_t saved_frame[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits = 123;
	size_t frame_bytes = v90_cp_frame_bytes(&parameters);

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved_frame, frame, sizeof(frame));
	CHECK(v90_cp_encode(
		      &parameters, frame, frame_bytes - 1u,
		      &frame_bits) == V90_CP_INVALID_ARGUMENT &&
	      frame_bits == 123 &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "short CP encode mutated its output");
	invalid.selected_rate_index = V90_CP_RATE_INDEX_MAX + 1u;
	CHECK(v90_cp_encode(
		      &invalid, frame, sizeof(frame), &frame_bits) ==
		      V90_CP_INVALID_PARAMETERS &&
	      frame_bits == 123 &&
	      memcmp(frame, saved_frame, sizeof(frame)) == 0,
	      "invalid CP parameters mutated encoded output");
	CHECK(v90_cp_encode(NULL, frame, sizeof(frame), &frame_bits) ==
		      V90_CP_INVALID_ARGUMENT &&
	      v90_cp_encode(&parameters, NULL, 0, &frame_bits) ==
		      V90_CP_INVALID_ARGUMENT &&
	      v90_cp_encode(&parameters, frame, sizeof(frame), NULL) ==
		      V90_CP_INVALID_ARGUMENT,
	      "CP encode accepted a missing argument");

	CHECK(v90_cp_encode(
		      &parameters, frame, sizeof(frame), &frame_bits) ==
		      V90_CP_OK,
	      "CP decode contract fixture did not encode");
	memset(&output, 0x5a, sizeof(output));
	saved_output = output;
	CHECK(v90_cp_decode(NULL, frame_bytes, frame_bits, &output) ==
		      V90_CP_INVALID_ARGUMENT &&
	      v90_cp_decode(frame, frame_bytes, frame_bits, NULL) ==
		      V90_CP_INVALID_ARGUMENT &&
	      v90_cp_decode(
		      frame, frame_bytes, V90_CP_MIN_FRAME_BITS - 1u,
		      &output) == V90_CP_INVALID_ARGUMENT &&
	      v90_cp_decode(
		      frame, frame_bytes, V90_CP_MAX_FRAME_BITS + 1u,
		      &output) == V90_CP_INVALID_ARGUMENT &&
	      v90_cp_decode(
		      frame, frame_bytes - 1u, frame_bits, &output) ==
		      V90_CP_INVALID_ARGUMENT &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "invalid CP decode arguments mutated output");
	CHECK(strcmp(v90_cp_result_name(V90_CP_OK), "ok") == 0 &&
	      strcmp(v90_cp_result_name(V90_CP_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v90_cp_result_name(V90_CP_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v90_cp_result_name(V90_CP_INVALID_FRAME),
		     "invalid frame") == 0 &&
	      strcmp(v90_cp_result_name(V90_CP_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(v90_cp_result_name((enum v90_cp_result)99),
		     "unknown") == 0,
	      "CP result names changed");
	return true;
}

static bool test_corrupt_frames(void)
{
	struct v90_cp_parameters parameters = wire_parameters();
	uint8_t valid[V90_CP_MAX_FRAME_BYTES];
	uint8_t changed[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits = 0;
	size_t frame_bytes;

	CHECK(v90_cp_encode(
		      &parameters, valid, sizeof(valid), &frame_bits) ==
		      V90_CP_OK,
	      "CP corruption fixture did not encode");
	frame_bytes = (frame_bits + 7u) / 8u;

	for (size_t bit = 0; bit < 17u; ++bit) {
		memcpy(changed, valid, frame_bytes);
		replace_frame_bit(changed, bit, 0);
		CHECK(rejected_decode_is_transactional(
			      changed, frame_bytes, frame_bits,
			      V90_CP_INVALID_FRAME),
		      "CP accepted a broken sync bit %zu", bit);
	}

	memcpy(changed, valid, frame_bytes);
	replace_frame_bit(
		changed, CP_RATE_FIRST_BIT,
		reference_frame_bit(changed, CP_RATE_FIRST_BIT) ^ 1u);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_CRC_MISMATCH),
	      "CP accepted corrupted information");

	memcpy(changed, valid, frame_bytes);
	replace_frame_bit(changed, 17, 1);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_INVALID_FRAME),
	      "CP accepted a nonzero start bit");
	memcpy(changed, valid, frame_bytes);
	replace_frame_bit(changed, 25, 1);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_CRC_MISMATCH),
	      "CP ignored a reserved bit's CRC contribution");
	memcpy(changed, valid, frame_bytes);
	replace_frame_bit(changed, CP_MINIMUM_PAYLOAD_BITS + 17u, 1);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_INVALID_FRAME),
	      "CP accepted a nonzero fill bit");
	memcpy(changed, valid, frame_bytes);
	replace_frame_bit(changed, frame_bits, 1);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_INVALID_FRAME),
	      "CP accepted nonzero storage padding");

	memcpy(changed, valid, frame_bytes);
	replace_lsb_first(
		changed, CP_RATE_FIRST_BIT, CP_RATE_BITS,
		V90_CP_RATE_INDEX_MAX + 1u);
	replace_crc(changed, CP_MINIMUM_PAYLOAD_BITS);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_INVALID_FRAME),
	      "CP accepted a CRC-valid invalid rate");
	memcpy(changed, valid, frame_bytes);
	for (uint8_t ucode = 0;
	     ucode < V90_CP_CONSTELLATION_UCODES; ++ucode) {
		size_t bit = CP_MASKS_FIRST_BIT +
			     (size_t)(ucode / 16u) * 17u +
			     1u + ucode % 16u;

		replace_frame_bit(changed, bit, 0);
	}
	replace_crc(changed, CP_MINIMUM_PAYLOAD_BITS);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_INVALID_FRAME),
	      "CP accepted an empty CRC-valid constellation");
	memcpy(changed, valid, frame_bytes);
	replace_lsb_first(changed, CP_MAP_FIRST_BIT, 4, 1);
	CHECK(rejected_decode_is_transactional(
		      changed, frame_bytes, frame_bits,
		      V90_CP_INVALID_FRAME),
	      "CP accepted an incomplete constellation record set");
	return true;
}

static bool wire_records_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	uint8_t left_frame[V90_CP_MAX_FRAME_BYTES];
	uint8_t right_frame[V90_CP_MAX_FRAME_BYTES];
	size_t left_bits, right_bits;

	return v90_cp_encode(left, left_frame, sizeof(left_frame), &left_bits) == V90_CP_OK &&
	       v90_cp_encode(right, right_frame, sizeof(right_frame), &right_bits) == V90_CP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, (left_bits + 7u) / 8u) == 0;
}

static bool test_record_comparison(void)
{
	struct v90_cp_parameters value = baseline(), changed = value;

	CHECK(!v90_cp_parameters_are_equal(NULL, NULL) &&
	      !v90_cp_parameters_are_equal(NULL, &value) &&
	      !v90_cp_parameters_are_equal(&value, NULL),
	      "null CP records compared equal");
	changed.selected_rate_index = V90_CP_RATE_INDEX_MAX + 1u;
	CHECK(!v90_cp_parameters_are_equal(&changed, &changed),
	      "invalid CP record compared equal to itself");
	value.selected_rate_index = changed.selected_rate_index = 0u;
	memset(changed.codec_constellations, 0xa5, sizeof(changed.codec_constellations));
	memset(changed.transmit_constellations + 1, 0x5a,
	       sizeof(changed.transmit_constellations) - sizeof(changed.transmit_constellations[0]));
	CHECK(v90_cp_parameters_are_equal(&value, &changed) && wire_records_equal(&value, &changed),
	      "comparison interpreted undeclared cleardown rows or an absent codec set");

	/* Independently re-CRC each single-bit mutation, including reserved bits.
	 * Compare accepted, normalized records against their actual encoded bytes. */
	for (uint8_t count = 1; count <= V90_CP_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int codec = 0; codec < 2; ++codec) {
			for (unsigned int cleardown = 0; cleardown < 2; ++cleardown) {
				uint8_t frame[V90_CP_MAX_FRAME_BYTES];
				size_t bits;

				value = variable_parameters(count, codec != 0);
				if (cleardown != 0)
					value.selected_rate_index = 0u;
				CHECK(v90_cp_encode(&value, frame, sizeof(frame), &bits) == V90_CP_OK,
				      "comparison fixture did not encode");
				for (size_t bit = CP_INFORMATION_FIRST_BIT;
				     bit < bits - V90_CP_TRAILER_BITS; ++bit) {
					uint8_t mutated[V90_CP_MAX_FRAME_BYTES];

					memcpy(mutated, frame, (bits + 7u) / 8u);
					replace_frame_bit(mutated, bit, reference_frame_bit(frame, bit) ^ 1u);
					replace_crc(mutated, bits - V90_CP_TRAILER_BITS);
					if (v90_cp_decode(mutated, sizeof(mutated), bits, &changed) != V90_CP_OK)
						continue;
					CHECK(v90_cp_parameters_are_equal(&value, &changed) ==
					      wire_records_equal(&value, &changed),
					      "comparison differs from wire: count %u codec %u cleardown %u bit %zu",
					      count, codec, cleardown, bit);
				}
			}
		}
	}
	return true;
}

static bool test_reserved_bits(void)
{
	/* Table 14: the sender clears these bits; the receiver does not
	 * interpret them. They remain part of the CRC-protected payload. */
	static const size_t reserved[] = {
		18, 25, 26, 27, 28, 29, 129, 130, 131, 132, 133, 134, 135,
	};

	for (uint8_t count = 1; count <= V90_CP_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int codec_copy = 0; codec_copy < 2; ++codec_copy) {
			struct v90_cp_parameters parameters =
				variable_parameters(count, codec_copy != 0);
			uint8_t valid[V90_CP_MAX_FRAME_BYTES];
			size_t bits;

			parameters.kind = count % 2u ? V90_CP_TRAINING : V90_CP_DATA;
			CHECK(v90_cp_encode(&parameters, valid, sizeof(valid), &bits) ==
			      V90_CP_OK, "reserved-bit fixture did not encode");
			for (size_t pattern = 0; pattern <= ARRAY_LENGTH(reserved); ++pattern) {
				struct v90_cp_parameters decoded;
				uint8_t changed[V90_CP_MAX_FRAME_BYTES];
				uint8_t encoded[V90_CP_MAX_FRAME_BYTES];
				size_t encoded_bits;
				size_t bytes = (bits + 7u) / 8u;

				memcpy(changed, valid, bytes);
				for (size_t bit = 0; bit < ARRAY_LENGTH(reserved); ++bit) {
					CHECK(reference_frame_bit(valid, reserved[bit]) == 0,
					      "CP encoder set reserved bit %zu", reserved[bit]);
					if (pattern == bit || pattern == ARRAY_LENGTH(reserved))
						replace_frame_bit(changed, reserved[bit], 1);
				}
				replace_crc(changed, bits - V90_CP_TRAILER_BITS);
				CHECK(v90_cp_decode(changed, bytes, bits, &decoded) == V90_CP_OK &&
				      v90_cp_parameters_are_equal(&decoded, &parameters),
				      "CP interpreted reserved pattern %zu, count %u, codec %u",
				      pattern, count, codec_copy);
				CHECK(v90_cp_encode(&decoded, encoded, sizeof(encoded), &encoded_bits) ==
				      V90_CP_OK && encoded_bits == bits &&
				      memcmp(encoded, valid, bytes) == 0,
				      "CP re-encoding preserved received reserved bits");
			}
		}
	}
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

static bool test_sparse_wire_maps(void)
{
	for (unsigned int differs = 0; differs < 2u; ++differs) {
		struct v90_cp_parameters value = variable_parameters(6u, differs != 0u);
		size_t payload_end = 136u + 6u * 136u * (differs + 1u);

		for (unsigned int subset = 32u; subset < 64u; ++subset) {
			struct v90_cp_parameters decoded;
			uint8_t frame[V90_CP_MAX_FRAME_BYTES];
			uint8_t rows[6];
			uint8_t count = 0;
			size_t bits = 0;

			CHECK(v90_cp_encode(&value, frame, sizeof(frame), &bits) == V90_CP_OK,
			      "could not encode sparse map fixture");
			for (uint8_t row = 0; row < 6u; ++row) {
				if ((subset & (1u << row)) != 0)
					rows[count++] = row;
			}
			for (size_t interval = 0; interval < 6u; ++interval) {
				size_t first = 103u + 4u * interval + (interval >= 4u);
				uint8_t row = rows[interval % count];

				for (size_t bit = 0; bit < 4u; ++bit)
					replace_frame_bit(frame, first + bit,
						(uint8_t)((row >> bit) & 1u));
			}
			/* Unreferenced wire rows may also be empty. Their framing and
			 * CRC are still checked before the decoded rows are compacted. */
			if (subset & 1u)
				for (size_t row = 0; row < 6u; ++row) {
					if ((subset & (1u << row)) != 0)
						continue;
					for (size_t plane = 0; plane <= differs; ++plane)
						for (size_t bit = 0; bit < 136u; ++bit)
							replace_frame_bit(frame, 136u + (6u * plane + row) * 136u + bit, 0);
				}
			uint16_t crc = reference_crc(frame, payload_end);
			for (size_t bit = 0; bit < 16u; ++bit)
				replace_frame_bit(frame, payload_end + 1u + bit,
					(uint8_t)((crc >> (15u - bit)) & 1u));
			CHECK(v90_cp_decode(frame, sizeof(frame), bits, &decoded) == V90_CP_OK &&
			      decoded.constellation_count == count,
			      "rejected sparse wire map %u", subset);
			for (size_t interval = 0; interval < 6u; ++interval) {
				CHECK(decoded.frame_constellation[interval] == interval % count,
				      "did not compact interval map");
			}
			for (size_t row = 0; row < count; ++row) {
				CHECK(memcmp(&decoded.transmit_constellations[row],
					     &value.transmit_constellations[rows[row]],
					     sizeof(struct pcm_constellation)) == 0 &&
				      memcmp(&decoded.codec_constellations[row],
					     &value.codec_constellations[rows[row]],
					     sizeof(struct pcm_constellation)) == 0,
				      "compaction changed selected constellation");
			}
			CHECK(v90_cp_parameters_are_valid(&decoded),
			      "decoded sparse map is not canonical");
		}
	}
	return true;
}

int main(void)
{
	static const struct test_case tests[] = {
		{ "wire record comparison", test_record_comparison },
		{ "sparse wire maps", test_sparse_wire_maps },
		{ "dimensions and boundaries", test_dimensions_and_boundaries },
		{ "constellation membership", test_constellation_membership },
		{ "constellation cardinality", test_constellation_cardinality },
		{ "scalar validation", test_scalar_validation },
		{ "canonical masks", test_canonical_masks },
		{ "known wire image", test_known_wire_image },
		{ "variable round trip", test_variable_round_trip },
		{ "cleardown ignores unused constellations", test_cleardown },
		{ "call contracts", test_call_contracts },
		{ "corrupt frames", test_corrupt_frames },
		{ "uninterpreted reserved bits", test_reserved_bits },
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
