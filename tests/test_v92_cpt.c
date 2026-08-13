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
	CHECK(strcmp(v92_cpt_result_name(V92_CPT_OK), "ok") == 0 &&
	      strcmp(v92_cpt_result_name(V92_CPT_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v92_cpt_result_name(V92_CPT_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v92_cpt_result_name(V92_CPT_OUTPUT_TOO_SMALL),
		     "output too small") == 0 &&
	      strcmp(v92_cpt_result_name((enum v92_cpt_result)99),
		     "unknown") == 0,
	      "CPt result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "frame dimensions", test_dimensions },
		{ "wire fields", test_wire_fields },
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
