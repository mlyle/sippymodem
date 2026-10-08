// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpt.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	CPT_INFORMATION_FIRST_BIT = 18,
	CPT_RATE_FIRST_BIT = 21,
	CPT_SIGN_REDUNDANCY_FIRST_BIT = 31,
	CPT_ACKNOWLEDGMENT_BIT = 33,
	CPT_LAW_BIT = 35,
	CPT_LOOKAHEAD_FIRST_BIT = 49,
	CPT_RATIO_FIRST_BIT = 52,
	CPT_A1_FIRST_BIT = 69,
	CPT_A2_FIRST_BIT = 77,
	CPT_B1_FIRST_BIT = 86,
	CPT_B2_FIRST_BIT = 94,
	CPT_MAP_FIRST_BIT = 103,
	CPT_CODEC_DIFFERENCE_BIT = 128,
	CPT_MASKS_FIRST_BIT = 136,
	CPT_CRC_BITS = 16,
};

static uint8_t bit_at(const uint8_t *frame, size_t index)
{
	return (uint8_t)((frame[index / 8u] >>
			  (7u - index % 8u)) &
			 1u);
}

static void set_bit(uint8_t *frame, size_t index, uint8_t value)
{
	uint8_t mask = (uint8_t)(1u << (7u - index % 8u));

	if (value != 0)
		frame[index / 8u] |= mask;
	else
		frame[index / 8u] &= (uint8_t)~mask;
}

static uint32_t lsb_field(
	const uint8_t *frame, size_t first, size_t count)
{
	uint32_t value = 0;

	for (size_t index = 0; index < count; ++index)
		value |= (uint32_t)bit_at(frame, first + index) << index;
	return value;
}

static uint16_t msb_field(
	const uint8_t *frame, size_t first, size_t count)
{
	uint16_t value = 0;

	for (size_t index = 0; index < count; ++index)
		value = (uint16_t)((value << 1) |
			bit_at(frame, first + index));
	return value;
}

static uint16_t reference_crc(
	const uint8_t *frame, size_t payload_end)
{
	uint16_t remainder = UINT16_C(0xffff);

	for (size_t index = CPT_INFORMATION_FIRST_BIT;
	     index < payload_end; ++index) {
		uint16_t feedback = (uint16_t)(
			((remainder >> 15) ^ bit_at(frame, index)) & 1u);

		if (index % 17u == 0)
			continue;
		remainder = (uint16_t)(remainder << 1);
		if (feedback != 0)
			remainder ^= UINT16_C(0x1021);
	}
	return remainder;
}

static void rewrite_crc(uint8_t *frame, size_t payload_end)
{
	uint16_t crc = reference_crc(frame, payload_end);

	for (size_t index = 0; index < CPT_CRC_BITS; ++index)
		set_bit(
			frame, payload_end + 1u + index,
			(uint8_t)((crc >> (CPT_CRC_BITS - index - 1u)) & 1u));
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpt_parameters parameters(
	uint8_t count, bool codec_differs)
{
	struct v92_cpt_parameters value = {
		.law = G711_LAW_PCMA,
		.trn1d_ratio_q3_13 = 0x9abcu,
		.shaping_a1_q1_6 = -64,
		.shaping_a2_q1_6 = 63,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = -127,
		.selected_rate_index = V92_CPT_RATE_INDEX_MAX,
		.sign_redundancy = 3,
		.spectral_lookahead = 2,
		.constellation_count = count,
		.acknowledges_peer = true,
		.codec_constellations_differ = codec_differs,
	};

	for (uint8_t index = 0; index < count; ++index) {
		add_ucode(&value.transmit_constellations[index], index);
		add_ucode(
			&value.transmit_constellations[index],
			(uint8_t)(31u + index * 7u));
		add_ucode(
			&value.transmit_constellations[index],
			(uint8_t)(127u - index));
		if (codec_differs) {
			add_ucode(
				&value.codec_constellations[index],
				(uint8_t)(16u + index));
			add_ucode(
				&value.codec_constellations[index],
				(uint8_t)(96u - index * 3u));
		}
	}
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval)
		value.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return value;
}

static bool parameters_match(
	const struct v92_cpt_parameters *left,
	const struct v92_cpt_parameters *right)
{
	return left->law == right->law &&
	       left->trn1d_ratio_q3_13 == right->trn1d_ratio_q3_13 &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->selected_rate_index == right->selected_rate_index &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead == right->spectral_lookahead &&
	       left->constellation_count == right->constellation_count &&
	       left->acknowledges_peer == right->acknowledges_peer &&
	       left->codec_constellations_differ ==
		       right->codec_constellations_differ &&
	       memcmp(
		       left->frame_constellation,
		       right->frame_constellation,
		       sizeof(left->frame_constellation)) == 0 &&
	       memcmp(
		       left->transmit_constellations,
		       right->transmit_constellations,
		       sizeof(left->transmit_constellations)) == 0 &&
	       memcmp(
		       left->codec_constellations,
		       right->codec_constellations,
		       sizeof(left->codec_constellations)) == 0;
}

static size_t map_first_bit(size_t interval)
{
	return CPT_MAP_FIRST_BIT + interval * 4u +
	       (interval >= 4u ? 1u : 0u);
}

static size_t mask_bit(size_t set, uint8_t ucode)
{
	return CPT_MASKS_FIRST_BIT + set * V92_CPT_CONSTELLATION_BITS +
	       (size_t)(ucode / 16u) * 17u + 1u + ucode % 16u;
}

static bool bits_are(
	const uint8_t *frame, size_t first, size_t count, uint8_t wanted)
{
	for (size_t index = 0; index < count; ++index) {
		if (bit_at(frame, first + index) != wanted)
			return false;
	}
	return true;
}

static bool test_dimensions(void)
{
	static const size_t single_set[] = {
		300, 432, 564, 708, 840, 972,
	};
	static const size_t double_set[] = {
		432, 708, 972, 1248, 1524, 1788,
	};

	CHECK(V92_CPT_MIN_FRAME_BITS == 300 &&
	      V92_CPT_MAX_FRAME_BITS == 1788 &&
	      V92_CPT_MAX_FRAME_BYTES == 224,
	      "CPt public bounds changed");
	for (uint8_t count = 1;
	     count <= V92_CPT_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int differs = 0; differs < 2; ++differs) {
			struct v92_cpt_parameters value =
				parameters(count, differs != 0);
			size_t expected = differs != 0 ?
				double_set[count - 1u] : single_set[count - 1u];

			CHECK(v92_cpt_parameters_are_valid(&value) &&
			      v92_cpt_frame_bits(&value) == expected &&
			      v92_cpt_frame_bytes(&value) == (expected + 7u) / 8u &&
			      expected % V92_CPT_SYMBOLS_PER_FRAME == 0,
			      "CPt count %u set count %u has the wrong extent",
			      count, differs + 1u);
		}
	}
	return true;
}

static bool test_wire_fields(void)
{
	struct v92_cpt_parameters value = parameters(6, true);
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES + 1];
	size_t frame_bits = 0;
	size_t set_count = 2u * value.constellation_count;
	size_t payload_end = V92_CPT_HEADER_BITS +
		set_count * V92_CPT_CONSTELLATION_BITS;
	size_t frame_bytes;

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v92_cpt_encode(
		      &value, frame, V92_CPT_MAX_FRAME_BYTES, &frame_bits) ==
		      V92_CPT_OK &&
	      frame_bits == V92_CPT_MAX_FRAME_BITS,
	      "maximum CPt frame did not encode");
	frame_bytes = (frame_bits + 7u) / 8u;
	CHECK(frame[frame_bytes] == 0xa5 &&
	      bits_are(frame, 0, 17, 1) &&
	      bit_at(frame, 17) == 0 && bit_at(frame, 18) == 0 &&
	      bit_at(frame, 19) == 0 && bit_at(frame, 20) == 0 &&
	      bits_are(frame, 26, 5, 0) && bit_at(frame, 34) == 0 &&
	      bits_are(frame, 36, 13, 0) && bit_at(frame, 51) == 0 &&
	      bit_at(frame, 68) == 0 && bit_at(frame, 85) == 0 &&
	      bit_at(frame, 102) == 0 && bit_at(frame, 119) == 0 &&
	      bits_are(frame, 129, 7, 0),
	      "CPt fixed or reserved header bits changed");
	CHECK(lsb_field(frame, CPT_RATE_FIRST_BIT, 5) ==
		      value.selected_rate_index &&
	      lsb_field(frame, CPT_SIGN_REDUNDANCY_FIRST_BIT, 2) ==
		      value.sign_redundancy &&
	      bit_at(frame, CPT_ACKNOWLEDGMENT_BIT) == 1 &&
	      bit_at(frame, CPT_LAW_BIT) == 1 &&
	      lsb_field(frame, CPT_LOOKAHEAD_FIRST_BIT, 2) ==
		      value.spectral_lookahead &&
	      lsb_field(frame, CPT_RATIO_FIRST_BIT, 16) ==
		      value.trn1d_ratio_q3_13 &&
	      lsb_field(frame, CPT_A1_FIRST_BIT, 8) ==
		      (uint8_t)value.shaping_a1_q1_6 &&
	      lsb_field(frame, CPT_A2_FIRST_BIT, 8) ==
		      (uint8_t)value.shaping_a2_q1_6 &&
	      lsb_field(frame, CPT_B1_FIRST_BIT, 8) ==
		      (uint8_t)value.shaping_b1_q1_6 &&
	      lsb_field(frame, CPT_B2_FIRST_BIT, 8) ==
		      (uint8_t)value.shaping_b2_q1_6 &&
	      bit_at(frame, CPT_CODEC_DIFFERENCE_BIT) == 1,
	      "CPt scalar fields changed");
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval) {
		CHECK(lsb_field(frame, map_first_bit(interval), 4) ==
			      value.frame_constellation[interval],
		      "CPt interval %zu map changed", interval);
	}
	for (size_t set = 0; set < set_count; ++set) {
		const struct pcm_constellation *wanted =
			set < value.constellation_count ?
				&value.transmit_constellations[set] :
				&value.codec_constellations[
					set - value.constellation_count];

		for (size_t row = 0; row < 8; ++row)
			CHECK(bit_at(
				      frame,
				      CPT_MASKS_FIRST_BIT +
					      set * V92_CPT_CONSTELLATION_BITS +
					      row * 17u) == 0,
			      "CPt set %zu row %zu start bit changed", set, row);
		for (uint8_t ucode = 0;
		     ucode < PCM_CONSTELLATION_UCODES; ++ucode) {
			CHECK(bit_at(frame, mask_bit(set, ucode)) ==
				      pcm_constellation_contains(wanted, ucode),
			      "CPt set %zu Ucode %u changed", set, ucode);
		}
	}
	CHECK(bit_at(frame, payload_end) == 0 &&
	      msb_field(frame, payload_end + 1u, CPT_CRC_BITS) ==
		      reference_crc(frame, payload_end) &&
	      bits_are(
		      frame, payload_end + 1u + CPT_CRC_BITS,
		      frame_bits - payload_end - 1u - CPT_CRC_BITS, 0) &&
	      bits_are(
		      frame, frame_bits, frame_bytes * 8u - frame_bits, 0),
	      "CPt CRC, fill, or storage padding changed");
	return true;
}

static bool test_decode_layouts(void)
{
	for (uint8_t count = 1;
	     count <= V92_CPT_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int differs = 0; differs < 2; ++differs) {
			struct v92_cpt_parameters wanted =
				parameters(count, differs != 0);
			struct v92_cpt_parameters decoded;
			uint8_t frame[V92_CPT_MAX_FRAME_BYTES];
			size_t frame_bits = 0;

			memset(&decoded, 0xa5, sizeof(decoded));
			CHECK(v92_cpt_encode(
				      &wanted, frame, sizeof(frame), &frame_bits) ==
				      V92_CPT_OK &&
			      v92_cpt_decode(
				      frame, (frame_bits + 7u) / 8u, frame_bits,
				      &decoded) == V92_CPT_OK &&
			      parameters_match(&wanted, &decoded),
			      "CPt count %u set count %u did not round-trip",
			      count, differs + 1u);
		}
	}
	return true;
}

static bool decode_rejects_unchanged(
	const uint8_t *frame, size_t capacity, size_t frame_bits,
	enum v92_cpt_result wanted)
{
	struct v92_cpt_parameters decoded;
	struct v92_cpt_parameters before;

	memset(&decoded, 0xa5, sizeof(decoded));
	before = decoded;
	return v92_cpt_decode(
		       frame, capacity, frame_bits, &decoded) == wanted &&
	       memcmp(&decoded, &before, sizeof(decoded)) == 0;
}

static bool test_decode_corruption(void)
{
	struct v92_cpt_parameters wanted = parameters(1, false);
	struct v92_cpt_parameters decoded;
	uint8_t original[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	uint8_t changed[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bits = 0;
	size_t frame_bytes;
	size_t payload_end = V92_CPT_HEADER_BITS +
		V92_CPT_CONSTELLATION_BITS;

	CHECK(v92_cpt_encode(
		      &wanted, original, sizeof(original), &frame_bits) ==
		      V92_CPT_OK && frame_bits == V92_CPT_MIN_FRAME_BITS,
	      "CPt corruption fixture did not encode");
	frame_bytes = (frame_bits + 7u) / 8u;

	for (size_t bit = 0; bit < 17u; ++bit) {
		memcpy(changed, original, sizeof(changed));
		set_bit(changed, bit, 0);
		CHECK(decode_rejects_unchanged(
			      changed, frame_bytes, frame_bits,
			      V92_CPT_INVALID_FRAME),
		      "CPt accepted a broken sync bit %zu", bit);
	}
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, 19, 1);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted a non-CPt record type");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, 34, 1);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted a nonzero start bit");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, CPT_MASKS_FIRST_BIT, 1);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted a nonzero mask-row start bit");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, CPT_RATE_FIRST_BIT,
		(uint8_t)!bit_at(changed, CPT_RATE_FIRST_BIT));
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_CRC_MISMATCH),
	      "CPt accepted corrupted information");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, payload_end + 1u,
		(uint8_t)!bit_at(changed, payload_end + 1u));
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_CRC_MISMATCH),
	      "CPt accepted a corrupted CRC");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, payload_end + 1u + CPT_CRC_BITS, 1);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted nonzero frame fill");
	memcpy(changed, original, sizeof(changed));
	set_bit(changed, frame_bits, 1);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted nonzero storage padding");

	memcpy(changed, original, sizeof(changed));
	for (size_t index = 26; index <= 30; ++index)
		set_bit(changed, index, 1);
	for (size_t index = 36; index <= 48; ++index)
		set_bit(changed, index, 1);
	for (size_t index = 129; index <= 135; ++index)
		set_bit(changed, index, 1);
	rewrite_crc(changed, payload_end);
	CHECK(v92_cpt_decode(
		      changed, frame_bytes, frame_bits, &decoded) ==
		      V92_CPT_OK && parameters_match(&wanted, &decoded),
	      "CPt decoder interpreted reserved fields");

	memcpy(changed, original, sizeof(changed));
	for (uint8_t ucode = 0; ucode < PCM_CONSTELLATION_UCODES; ++ucode)
		set_bit(changed, mask_bit(0, ucode), 0);
	rewrite_crc(changed, payload_end);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted an empty active constellation");
	return true;
}

static bool test_decode_extent(void)
{
	struct v92_cpt_parameters wanted = parameters(2, false);
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES] = { 0 };
	uint8_t changed[V92_CPT_MAX_FRAME_BYTES];
	size_t frame_bits = 0;
	size_t frame_bytes;

	CHECK(v92_cpt_encode(
		      &wanted, frame, sizeof(frame), &frame_bits) == V92_CPT_OK,
	      "CPt extent fixture did not encode");
	frame_bytes = (frame_bits + 7u) / 8u;
	CHECK(decode_rejects_unchanged(
		      frame, frame_bytes - 1u, frame_bits,
		      V92_CPT_INVALID_ARGUMENT) &&
	      decode_rejects_unchanged(
		      frame, frame_bytes, frame_bits - 1u,
		      V92_CPT_INVALID_FRAME) &&
	      decode_rejects_unchanged(
		      frame, frame_bytes, V92_CPT_MIN_FRAME_BITS - 12u,
		      V92_CPT_INVALID_FRAME) &&
	      decode_rejects_unchanged(
		      frame, frame_bytes, V92_CPT_MAX_FRAME_BITS + 12u,
		      V92_CPT_INVALID_FRAME),
	      "CPt decoder accepted an invalid extent");
	memcpy(changed, frame, sizeof(changed));
	for (size_t bit = 0; bit < 4u; ++bit)
		set_bit(changed, map_first_bit(0) + bit, 1);
	CHECK(decode_rejects_unchanged(
		      changed, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted an out-of-range constellation map");

	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval) {
		for (size_t bit = 0; bit < 4u; ++bit)
			set_bit(frame, map_first_bit(interval) + bit, 0);
	}
	CHECK(decode_rejects_unchanged(
		      frame, frame_bytes, frame_bits,
		      V92_CPT_INVALID_FRAME),
	      "CPt accepted unused constellation records");
	return true;
}

static bool test_validation(void)
{
	struct v92_cpt_parameters valid = parameters(1, false);
	struct v92_cpt_parameters changed;

	changed = valid;
	changed.law = (enum g711_law)99;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an invalid law");
	changed = valid;
	changed.selected_rate_index = V92_CPT_RATE_INDEX_MAX + 1u;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an invalid rate");
	changed = valid;
	changed.sign_redundancy = 4;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted invalid sign redundancy");
	changed = valid;
	changed.spectral_lookahead = 4;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an invalid lookahead");
	changed = valid;
	changed.constellation_count = 0;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted no constellations");
	changed = valid;
	changed.constellation_count = V92_CPT_MAX_CONSTELLATIONS + 1u;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted too many constellations");
	changed = valid;
	memset(&changed.transmit_constellations[0], 0,
	       sizeof(changed.transmit_constellations[0]));
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an empty active constellation");
	changed = valid;
	add_ucode(&changed.transmit_constellations[1], 2);
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an inactive constellation");
	changed = valid;
	add_ucode(&changed.codec_constellations[0], 2);
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an undeclared codec constellation");
	changed = valid;
	changed.codec_constellations_differ = true;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted a missing codec constellation");
	add_ucode(&changed.codec_constellations[0], 2);
	CHECK(v92_cpt_parameters_are_valid(&changed),
	      "CPt rejected a complete codec constellation");
	changed = parameters(2, false);
	for (size_t interval = 0;
	     interval < V92_CPT_FRAME_INTERVALS; ++interval)
		changed.frame_constellation[interval] = 0;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an unreferenced constellation");
	changed.frame_constellation[5] = 2;
	CHECK(!v92_cpt_parameters_are_valid(&changed),
	      "CPt accepted an out-of-range map");
	CHECK(!v92_cpt_parameters_are_valid(NULL) &&
	      v92_cpt_frame_bits(NULL) == 0 &&
	      v92_cpt_frame_bytes(NULL) == 0,
	      "invalid CPt input produced a frame extent");
	return true;
}

static bool test_transactionality(void)
{
	struct v92_cpt_parameters valid = parameters(1, false);
	struct v92_cpt_parameters invalid = valid;
	uint8_t frame[V92_CPT_MAX_FRAME_BYTES];
	uint8_t saved[sizeof(frame)];
	size_t frame_bits = 123;

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved, frame, sizeof(saved));
	invalid.constellation_count = 0;
	CHECK(v92_cpt_encode(
		      &invalid, frame, sizeof(frame), &frame_bits) ==
		      V92_CPT_INVALID_PARAMETERS &&
	      memcmp(frame, saved, sizeof(frame)) == 0 && frame_bits == 123,
	      "invalid CPt parameters changed output");
	CHECK(v92_cpt_encode(
		      &valid, frame, v92_cpt_frame_bytes(&valid) - 1u,
		      &frame_bits) == V92_CPT_OUTPUT_TOO_SMALL &&
	      memcmp(frame, saved, sizeof(frame)) == 0 && frame_bits == 123,
	      "short CPt output changed state");
	CHECK(v92_cpt_encode(NULL, frame, sizeof(frame), &frame_bits) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      v92_cpt_encode(&valid, NULL, sizeof(frame), &frame_bits) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      v92_cpt_encode(&valid, frame, sizeof(frame), NULL) ==
		      V92_CPT_INVALID_ARGUMENT &&
	      memcmp(frame, saved, sizeof(frame)) == 0 && frame_bits == 123,
	      "invalid CPt arguments changed output");
	CHECK(decode_rejects_unchanged(
		      NULL, sizeof(frame), V92_CPT_MIN_FRAME_BITS,
		      V92_CPT_INVALID_ARGUMENT) &&
	      v92_cpt_decode(frame, sizeof(frame), V92_CPT_MIN_FRAME_BITS,
			     NULL) == V92_CPT_INVALID_ARGUMENT,
	      "invalid CPt decode arguments were accepted");
	return true;
}

static bool test_sparse_wire_maps(void)
{
	for (unsigned int differs = 0; differs < 2u; ++differs) {
		struct v92_cpt_parameters value = parameters(6u, differs != 0u);
		size_t payload_end = 136u + 6u * 136u * (differs + 1u);

		for (unsigned int subset = 32u; subset < 64u; ++subset) {
			struct v92_cpt_parameters decoded;
			uint8_t frame[V92_CPT_MAX_FRAME_BYTES];
			uint8_t rows[6];
			uint8_t count = 0;
			size_t bits = 0;

			CHECK(v92_cpt_encode(&value, frame, sizeof(frame), &bits) == V92_CPT_OK,
			      "could not encode sparse map fixture");
			for (uint8_t row = 0; row < 6u; ++row) {
				if ((subset & (1u << row)) != 0)
					rows[count++] = row;
			}
			for (size_t interval = 0; interval < 6u; ++interval) {
				size_t first = 103u + 4u * interval + (interval >= 4u);
				uint8_t row = rows[interval % count];

				for (size_t bit = 0; bit < 4u; ++bit)
					set_bit(frame, first + bit,
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
							set_bit(frame, 136u + (6u * plane + row) * 136u + bit, 0);
				}
			uint16_t crc = reference_crc(frame, payload_end);
			for (size_t bit = 0; bit < 16u; ++bit)
				set_bit(frame, payload_end + 1u + bit,
					(uint8_t)((crc >> (15u - bit)) & 1u));
			CHECK(v92_cpt_decode(frame, sizeof(frame), bits, &decoded) == V92_CPT_OK &&
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
			CHECK(v92_cpt_parameters_are_valid(&decoded),
			      "decoded sparse map is not canonical");
		}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "sparse wire maps", test_sparse_wire_maps },
		{ "frame dimensions", test_dimensions },
		{ "wire fields", test_wire_fields },
		{ "decode layouts", test_decode_layouts },
		{ "decode corruption", test_decode_corruption },
		{ "decode extent", test_decode_extent },
		{ "validation", test_validation },
		{ "transactionality", test_transactionality },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			return 1;
		}
		fprintf(stderr, "[       OK ] %s\n", tests[index].name);
	}
	return 0;
}
