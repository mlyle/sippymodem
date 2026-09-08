// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpu.h>

#include "test.h"

#include <string.h>

enum {
	CPU_INFORMATION_FIRST_BIT = 18,
	CPU_RATE_FIRST_BIT = 21,
	CPU_SIGN_REDUNDANCY_FIRST_BIT = 31,
	CPU_ACKNOWLEDGMENT_BIT = 33,
	CPU_LAW_BIT = 35,
	CPU_LOOKAHEAD_FIRST_BIT = 49,
	CPU_RATIO_FIRST_BIT = 52,
	CPU_A1_FIRST_BIT = 69,
	CPU_A2_FIRST_BIT = 77,
	CPU_B1_FIRST_BIT = 86,
	CPU_B2_FIRST_BIT = 94,
	CPU_MAP_FIRST_BIT = 103,
	CPU_CODEC_DIFFERENCE_BIT = 128,
};

static uint8_t bit_at(const uint8_t *packed, size_t bit)
{
	return (uint8_t)((packed[bit / 8u] >> (7u - bit % 8u)) & 1u);
}

static void set_bit(uint8_t *packed, size_t bit, uint8_t value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit % 8u));

	if (value != 0u)
		packed[bit / 8u] |= mask;
	else
		packed[bit / 8u] &= (uint8_t)~mask;
}

static void write_lsb_field(
	uint8_t *packed, size_t first_bit, size_t width, uint32_t value)
{
	for (size_t bit = 0u; bit < width; ++bit)
		set_bit(
			packed, first_bit + bit,
			(uint8_t)((value >> bit) & 1u));
}

static void write_msb_field(
	uint8_t *packed, size_t first_bit, size_t width, uint32_t value)
{
	for (size_t bit = 0u; bit < width; ++bit) {
		size_t shift = width - bit - 1u;

		set_bit(
			packed, first_bit + bit,
			(uint8_t)((value >> shift) & 1u));
	}
}

static uint32_t lsb_field(
	const uint8_t *packed, size_t first_bit, size_t width)
{
	uint32_t value = 0u;

	for (size_t bit = 0u; bit < width; ++bit)
		value |= (uint32_t)bit_at(packed, first_bit + bit) << bit;
	return value;
}

static uint32_t msb_field(
	const uint8_t *packed, size_t first_bit, size_t width)
{
	uint32_t value = 0u;

	for (size_t bit = 0u; bit < width; ++bit)
		value = (value << 1) | bit_at(packed, first_bit + bit);
	return value;
}

static bool bits_are(
	const uint8_t *packed, size_t first_bit,
	size_t bit_count, uint8_t expected)
{
	for (size_t bit = 0u; bit < bit_count; ++bit) {
		if (bit_at(packed, first_bit + bit) != expected)
			return false;
	}
	return true;
}

static uint16_t reference_crc(
	const uint8_t *packed, size_t payload_end)
{
	uint16_t remainder = UINT16_C(0xffff);

	for (size_t bit = CPU_INFORMATION_FIRST_BIT;
	     bit < payload_end; ++bit) {
		uint16_t feedback;

		if (bit % 17u == 0u)
			continue;
		feedback = (uint16_t)(((remainder >> 15) ^
			bit_at(packed, bit)) & 1u);
		remainder = (uint16_t)(remainder << 1);
		if (feedback != 0u)
			remainder ^= UINT16_C(0x1021);
	}
	return remainder;
}

static size_t map_first_bit(size_t interval)
{
	return CPU_MAP_FIRST_BIT + interval * 4u +
	       (interval >= 4u ? 1u : 0u);
}

static size_t row_mask_bit(size_t row, uint8_t ucode)
{
	return V92_CPU_HEADER_BITS + row * V92_CPU_CONSTELLATION_BITS +
	       (size_t)(ucode / 16u) * 17u + 1u + ucode % 16u;
}

static void rewrite_crc(uint8_t *frame, size_t payload_end)
{
	write_msb_field(
		frame, payload_end + 1u, 16u,
		reference_crc(frame, payload_end));
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpu_parameters parameters(
	uint8_t count, bool codec_differs)
{
	struct v92_cpu_parameters value = {
		.law = G711_LAW_PCMA,
		.trn1d_ratio_q3_13 = UINT16_C(0x9abc),
		.shaping_a1_q1_6 = -64,
		.shaping_a2_q1_6 = 63,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = -127,
		.data_rate_number = V92_CPU_MAX_DATA_RATE_NUMBER,
		.sign_redundancy = 3u,
		.spectral_lookahead_frames = 2u,
		.constellation_count = count,
		.acknowledges_cpd = true,
		.codec_constellations_differ = codec_differs,
	};

	for (uint8_t index = 0u; index < count; ++index) {
		add_ucode(&value.transmit_constellations[index], index);
		add_ucode(
			&value.transmit_constellations[index],
			(uint8_t)(31u + 7u * index));
		if (codec_differs) {
			add_ucode(
				&value.codec_constellations[index],
				(uint8_t)(96u - 3u * index));
		}
	}
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		value.frame_constellation[interval] =
			(uint8_t)(interval % count);
	}
	return value;
}

static bool parameters_are_equal(
	const struct v92_cpu_parameters *left,
	const struct v92_cpu_parameters *right)
{
	return left->law == right->law &&
	       left->trn1d_ratio_q3_13 == right->trn1d_ratio_q3_13 &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead_frames ==
		       right->spectral_lookahead_frames &&
	       left->constellation_count == right->constellation_count &&
	       left->acknowledges_cpd == right->acknowledges_cpd &&
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

static bool test_dimensions(void)
{
	const struct v92_cpu_parameters valid = parameters(1u, false);

	CHECK(V92_CPU_MIN_FRAME_BITS == 290 &&
	      V92_CPU_MAX_FRAME_BITS == 1857 &&
	      V92_CPU_MAX_FRAME_BYTES == 233,
	      "CPu public bounds changed");
	for (unsigned int width = 0u; width <= 73u; ++width) {
		bool allowed = width == 24u ||
			(width >= 36u && width <= 72u && width % 2u == 0u);
		CHECK((v92_cpu_frame_bits(&valid, width) != 0u) == allowed,
		      "CPu incorrectly accepted or rejected width %u", width);
	}
	for (uint8_t count = 1u;
	     count <= V92_CPU_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int differs = 0u; differs < 2u; ++differs) {
			struct v92_cpu_parameters value =
				parameters(count, differs != 0u);
			uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
			size_t sets = (differs != 0u ? 2u : 1u) * count;
			size_t unfilled = V92_CPU_HEADER_BITS +
				sets * V92_CPU_CONSTELLATION_BITS +
				V92_CPU_MIN_TRAILER_BITS;

			for (unsigned int bits = V92_CONTROL_UPSTREAM_4_POINT_BITS;
			     bits <= V92_CONTROL_UPSTREAM_DATA_MAX_BITS; bits += 2u) {
				struct v92_cpu_parameters decoded;
				size_t encoded_bits = 0u;
				size_t probed_bits = 0u;
				size_t multiple = bits;
				size_t expected =
					(unfilled + multiple - 1u) / multiple * multiple;

				if (bits != 24u && bits < 36u)
					continue;

				CHECK(v92_cpu_parameters_are_valid(&value) &&
				      v92_cpu_frame_bits(&value, bits) == expected &&
				      v92_cpu_frame_bytes(&value, bits) ==
					      (expected + 7u) / 8u &&
				      v92_cpu_encode(
					      &value, bits, frame, sizeof(frame),
					      &encoded_bits) == V92_CPU_OK &&
				      encoded_bits == expected &&
				      v92_cpu_probe_frame_bits(
					      frame,
					      V92_CPU_HEADER_BITS / 8u,
					      V92_CPU_HEADER_BITS, bits,
					      &probed_bits) == V92_CPU_OK &&
				      probed_bits == expected &&
				      v92_cpu_decode(
					      frame, (expected + 7u) / 8u,
					      expected, bits, &decoded) == V92_CPU_OK &&
				      parameters_are_equal(&decoded, &value),
				      "CPu %u/%u/%u has the wrong extent",
				      count, differs, bits);
			}
		}
	}
	return true;
}

static bool probe_is_transactional(
	const uint8_t *frame, size_t frame_capacity,
	size_t received_bits, unsigned int mapping_frame_bits,
	enum v92_cpu_result expected)
{
	size_t extent = 123u;

	return v92_cpu_probe_frame_bits(
		       frame, frame_capacity, received_bits,
		       mapping_frame_bits, &extent) == expected &&
	       extent == 123u;
}

static bool test_extent_probe_failures(void)
{
	struct v92_cpu_parameters value = parameters(1u, false);
	uint8_t encoded[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	uint8_t changed[V92_CPU_MAX_FRAME_BYTES];
	size_t frame_bits = 0u;
	size_t inferred = 0u;

	CHECK(v92_cpu_encode(
		      &value, 36u, encoded, sizeof(encoded), &frame_bits) ==
		      V92_CPU_OK &&
	      v92_cpu_probe_frame_bits(
		      encoded, V92_CPU_HEADER_BITS / 8u,
		      V92_CPU_HEADER_BITS, 36u, &inferred) == V92_CPU_OK &&
	      inferred == frame_bits,
	      "CPu extent probe did not infer a complete header");
	for (size_t received = 0u;
	     received < V92_CPU_HEADER_BITS; ++received) {
		CHECK(probe_is_transactional(
			      encoded, (received + 7u) / 8u, received,
			      36u, V92_CPU_INCOMPLETE),
		      "CPu extent probe mishandled split %zu", received);
	}
	CHECK(probe_is_transactional(
		      encoded, V92_CPU_HEADER_BITS / 8u - 1u,
		      V92_CPU_HEADER_BITS, 36u, V92_CPU_INVALID_ARGUMENT) &&
	      probe_is_transactional(
		      encoded, sizeof(encoded), V92_CPU_MAX_FRAME_BITS + 1u,
		      36u, V92_CPU_INVALID_FRAME) &&
	      probe_is_transactional(
		      encoded, sizeof(encoded), V92_CPU_HEADER_BITS,
		      1u, V92_CPU_INVALID_FRAME) &&
	      v92_cpu_probe_frame_bits(
		      NULL, 0u, 0u, 36u, &inferred) ==
		      V92_CPU_INVALID_ARGUMENT &&
	      v92_cpu_probe_frame_bits(
		      encoded, sizeof(encoded), V92_CPU_HEADER_BITS,
		      36u, NULL) == V92_CPU_INVALID_ARGUMENT,
	      "CPu extent probe accepted an invalid boundary");

	memcpy(changed, encoded, sizeof(changed));
	set_bit(changed, 0u, 0u);
	CHECK(probe_is_transactional(
		      changed, sizeof(changed), V92_CPU_HEADER_BITS,
		      36u, V92_CPU_INVALID_FRAME),
	      "CPu extent probe accepted a broken prefix");
	memcpy(changed, encoded, sizeof(changed));
	write_lsb_field(changed, CPU_RATE_FIRST_BIT, 5u, 31u);
	CHECK(probe_is_transactional(
		      changed, sizeof(changed), V92_CPU_HEADER_BITS,
		      36u, V92_CPU_INVALID_FRAME),
	      "CPu extent probe accepted an invalid rate");
	memcpy(changed, encoded, sizeof(changed));
	write_lsb_field(changed, map_first_bit(0u), 4u, 6u);
	CHECK(probe_is_transactional(
		      changed, sizeof(changed), V92_CPU_HEADER_BITS,
		      36u, V92_CPU_INVALID_FRAME),
	      "CPu extent probe accepted an invalid map");
	CHECK(probe_is_transactional(
		      encoded, sizeof(encoded), frame_bits + 1u,
		      36u, V92_CPU_INVALID_FRAME),
	      "CPu extent probe accepted input past its inferred end");
	return true;
}

static bool test_scalar_validation(void)
{
	struct v92_cpu_parameters valid = parameters(1u, false);
	struct v92_cpu_parameters changed;

	CHECK(!v92_cpu_parameters_are_valid(NULL) &&
	      v92_cpu_frame_bits(NULL, 24u) == 0u,
	      "CPu accepted null parameters");
	for (uint8_t rate = 0u;
	     rate <= V92_CPU_MAX_DATA_RATE_NUMBER; ++rate) {
		changed = valid;
		changed.data_rate_number = rate;
		CHECK(v92_cpu_parameters_are_valid(&changed),
		      "CPu rejected data-rate number %u", rate);
	}
	changed = valid;
	changed.data_rate_number = V92_CPU_MAX_DATA_RATE_NUMBER + 1u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an invalid rate");
	changed = valid;
	changed.law = (enum g711_law)99;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an invalid law");
	changed = valid;
	changed.sign_redundancy = 4u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted invalid sign redundancy");
	changed = valid;
	changed.spectral_lookahead_frames = 4u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted invalid lookahead");
	CHECK(v92_cpu_frame_bits(&valid, 1u) == 0u &&
	      v92_cpu_frame_bits(&valid, 5u) == 0u &&
	      v92_cpu_frame_bytes(&valid, 1u) == 0u,
	      "CPu accepted an invalid modulation width");
	return true;
}

static bool test_constellation_validation(void)
{
	struct v92_cpu_parameters valid = parameters(2u, false);
	struct v92_cpu_parameters changed;

	changed = valid;
	changed.constellation_count = 0u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted no constellations");
	changed = valid;
	changed.constellation_count = V92_CPU_MAX_CONSTELLATIONS + 1u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted too many constellations");
	changed = valid;
	changed.frame_constellation[0] = 2u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an out-of-range map");
	changed = valid;
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval)
		changed.frame_constellation[interval] = 0u;
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an unused constellation");
	changed = valid;
	memset(&changed.transmit_constellations[0], 0,
	       sizeof(changed.transmit_constellations[0]));
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an empty active constellation");
	changed = valid;
	add_ucode(&changed.transmit_constellations[2], 7u);
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an inactive constellation");
	changed = valid;
	add_ucode(&changed.codec_constellations[0], 7u);
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted an unadvertised codec constellation");
	changed = parameters(2u, true);
	memset(&changed.codec_constellations[0], 0,
	       sizeof(changed.codec_constellations[0]));
	CHECK(!v92_cpu_parameters_are_valid(&changed),
	      "CPu accepted a missing codec constellation");
	return true;
}

static bool fixed_fields_are_correct(
	const uint8_t *frame,
	const struct v92_cpu_parameters *value)
{
	return bits_are(frame, 0u, 17u, 1u) &&
	       bit_at(frame, 17u) == 0u &&
	       bit_at(frame, 18u) == 0u &&
	       lsb_field(frame, 19u, 2u) == 1u &&
	       lsb_field(frame, CPU_RATE_FIRST_BIT, 5u) ==
		       value->data_rate_number &&
	       bits_are(frame, 26u, 5u, 0u) &&
	       lsb_field(frame, CPU_SIGN_REDUNDANCY_FIRST_BIT, 2u) ==
		       value->sign_redundancy &&
	       bit_at(frame, CPU_ACKNOWLEDGMENT_BIT) ==
		       (uint8_t)value->acknowledges_cpd &&
	       bit_at(frame, 34u) == 0u &&
	       bit_at(frame, CPU_LAW_BIT) ==
		       (uint8_t)(value->law == G711_LAW_PCMA) &&
	       bits_are(frame, 36u, 13u, 0u) &&
	       lsb_field(frame, CPU_LOOKAHEAD_FIRST_BIT, 2u) ==
		       value->spectral_lookahead_frames &&
	       bit_at(frame, 51u) == 0u &&
	       lsb_field(frame, CPU_RATIO_FIRST_BIT, 16u) ==
		       value->trn1d_ratio_q3_13 &&
	       bit_at(frame, 68u) == 0u &&
	       lsb_field(frame, CPU_A1_FIRST_BIT, 8u) ==
		       (uint8_t)value->shaping_a1_q1_6 &&
	       lsb_field(frame, CPU_A2_FIRST_BIT, 8u) ==
		       (uint8_t)value->shaping_a2_q1_6 &&
	       bit_at(frame, 85u) == 0u &&
	       lsb_field(frame, CPU_B1_FIRST_BIT, 8u) ==
		       (uint8_t)value->shaping_b1_q1_6 &&
	       lsb_field(frame, CPU_B2_FIRST_BIT, 8u) ==
		       (uint8_t)value->shaping_b2_q1_6 &&
	       bit_at(frame, 102u) == 0u &&
	       bit_at(frame, 119u) == 0u &&
	       bit_at(frame, CPU_CODEC_DIFFERENCE_BIT) ==
		       (uint8_t)value->codec_constellations_differ &&
	       bits_are(frame, 129u, 7u, 0u);
}

static bool constellation_rows_are_correct(
	const uint8_t *frame,
	const struct v92_cpu_parameters *value)
{
	for (size_t set = 0u; set < 2u; ++set) {
		const struct pcm_constellation *constellations = set == 0u ?
			value->transmit_constellations :
			value->codec_constellations;

		for (size_t index = 0u;
		     index < value->constellation_count; ++index) {
			size_t row = set * value->constellation_count + index;
			size_t first = V92_CPU_HEADER_BITS +
				row * V92_CPU_CONSTELLATION_BITS;

			for (size_t group = 0u; group < 8u; ++group) {
				CHECK(bit_at(frame, first + group * 17u) == 0u,
				      "CPu row %zu group %zu start is not zero",
				      row, group);
			}
			for (uint8_t ucode = 0u;
			     ucode < PCM_CONSTELLATION_UCODES; ++ucode) {
				size_t bit = first + (size_t)(ucode / 16u) *
					17u + 1u + ucode % 16u;
				bool expected = pcm_constellation_contains(
					&constellations[index], ucode);

				CHECK(bit_at(frame, bit) == (uint8_t)expected,
				      "CPu row %zu Ucode %u is wrong",
				      row, ucode);
			}
		}
	}
	return true;
}

static bool test_wire_fields(void)
{
	struct v92_cpu_parameters value =
		parameters(V92_CPU_MAX_CONSTELLATIONS, true);
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES + 1u];
	size_t frame_bits = 0u;
	size_t payload_end = V92_CPU_HEADER_BITS +
		2u * value.constellation_count *
		V92_CPU_CONSTELLATION_BITS;
	uint16_t expected_crc;

	memset(frame, 0xa5, sizeof(frame));
	CHECK(v92_cpu_encode(
		      &value, 66u, frame, V92_CPU_MAX_FRAME_BYTES,
		      &frame_bits) == V92_CPU_OK,
	      "maximum CPu encoding failed");
	CHECK(frame_bits == 1848u && frame_bits <= V92_CPU_MAX_FRAME_BITS &&
	      frame[(frame_bits + 7u) / 8u] == 0xa5,
	      "maximum CPu extent is wrong");
	CHECK(fixed_fields_are_correct(frame, &value),
	      "CPu fixed fields are wrong");
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		CHECK(lsb_field(frame, map_first_bit(interval), 4u) ==
			      value.frame_constellation[interval],
		      "CPu interval %zu map is wrong", interval);
	}
	CHECK(constellation_rows_are_correct(frame, &value),
	      "CPu constellation row is wrong");
	expected_crc = reference_crc(frame, payload_end);
	CHECK(bit_at(frame, payload_end) == 0u &&
	      msb_field(frame, payload_end + 1u, 16u) == expected_crc,
	      "CPu CRC field is wrong");
	CHECK(bits_are(
		      frame, payload_end + 17u,
		      frame_bits - payload_end - 17u, 0u),
	      "CPu fill is not zero");
	return true;
}

static bool test_compact_row_layout(void)
{
	struct v92_cpu_parameters value = parameters(2u, true);
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;

	value.law = G711_LAW_PCMU;
	CHECK(v92_cpu_encode(
		      &value, 24u, frame, sizeof(frame), &frame_bits) ==
		      V92_CPU_OK,
	      "compact CPu encoding failed");
	CHECK(frame_bits == v92_cpu_frame_bits(&value, 24u) &&
	      fixed_fields_are_correct(frame, &value) &&
	      constellation_rows_are_correct(frame, &value),
	      "CPu rows were not packed contiguously");
	return true;
}

static bool test_transactionality(void)
{
	struct v92_cpu_parameters value = parameters(1u, false);
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES + 1u];
	uint8_t unchanged[sizeof(frame)];
	size_t frame_bits = 123u;
	size_t expected_bits = v92_cpu_frame_bits(&value, 36u);
	size_t expected_bytes = v92_cpu_frame_bytes(&value, 36u);

	memset(frame, 0xa5, sizeof(frame));
	memcpy(unchanged, frame, sizeof(frame));
	CHECK(v92_cpu_encode(
		      &value, 1u, frame, sizeof(frame), &frame_bits) ==
		      V92_CPU_INVALID_PARAMETERS &&
	      frame_bits == 123u &&
	      memcmp(frame, unchanged, sizeof(frame)) == 0,
	      "invalid CPu encoding changed output");
	CHECK(v92_cpu_encode(
		      &value, 36u, frame, expected_bytes - 1u,
		      &frame_bits) == V92_CPU_OUTPUT_TOO_SMALL &&
	      frame_bits == 123u &&
	      memcmp(frame, unchanged, sizeof(frame)) == 0,
	      "short CPu encoding changed output");
	CHECK(v92_cpu_encode(
		      &value, 36u, frame, expected_bytes, &frame_bits) ==
		      V92_CPU_OK &&
	      frame_bits == expected_bits &&
	      expected_bits % 8u != 0u &&
	      bits_are(
		      frame, expected_bits,
		      expected_bytes * 8u - expected_bits, 0u) &&
	      frame[expected_bytes] == 0xa5,
	      "valid CPu encoding crossed its extent");
	CHECK(v92_cpu_encode(
		      NULL, 36u, frame, sizeof(frame), &frame_bits) ==
		      V92_CPU_INVALID_ARGUMENT &&
	      v92_cpu_encode(
		      &value, 36u, NULL, sizeof(frame), &frame_bits) ==
		      V92_CPU_INVALID_ARGUMENT &&
	      v92_cpu_encode(
		      &value, 36u, frame, sizeof(frame), NULL) ==
		      V92_CPU_INVALID_ARGUMENT,
	      "CPu accepted null arguments");
	return true;
}

static bool decode_is_transactional(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bits, unsigned int mapping_frame_bits,
	enum v92_cpu_result expected)
{
	struct v92_cpu_parameters decoded;
	struct v92_cpu_parameters unchanged;

	memset(&decoded, 0x5a, sizeof(decoded));
	memcpy(&unchanged, &decoded, sizeof(decoded));
	return v92_cpu_decode(
		       frame, frame_capacity, frame_bits,
		       mapping_frame_bits, &decoded) == expected &&
	       memcmp(&decoded, &unchanged, sizeof(decoded)) == 0;
}

static bool test_decoder_failures(void)
{
	struct v92_cpu_parameters value = parameters(2u, true);
	struct v92_cpu_parameters decoded;
	uint8_t encoded[V92_CPU_MAX_FRAME_BYTES];
	uint8_t changed[V92_CPU_MAX_FRAME_BYTES];
	size_t frame_bits = 0u;
	size_t frame_bytes;
	size_t payload_end = V92_CPU_HEADER_BITS +
		2u * value.constellation_count *
		V92_CPU_CONSTELLATION_BITS;

	CHECK(v92_cpu_encode(
		      &value, 36u, encoded, sizeof(encoded), &frame_bits) ==
		      V92_CPU_OK,
	      "decoder fixture encoding failed");
	frame_bytes = (frame_bits + 7u) / 8u;
	CHECK(v92_cpu_decode(
		      NULL, frame_bytes, frame_bits, 36u, &decoded) ==
		      V92_CPU_INVALID_ARGUMENT &&
	      v92_cpu_decode(
		      encoded, frame_bytes, frame_bits, 36u, NULL) ==
		      V92_CPU_INVALID_ARGUMENT,
	      "CPu decoder accepted null arguments");
	CHECK(decode_is_transactional(
		      encoded, frame_bytes - 1u, frame_bits, 36u,
		      V92_CPU_INVALID_ARGUMENT) &&
	      decode_is_transactional(
		      encoded, frame_bytes, frame_bits, 1u,
		      V92_CPU_INVALID_FRAME) &&
	      decode_is_transactional(
		      encoded, frame_bytes, frame_bits - 1u, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted an invalid extent");

	memcpy(changed, encoded, frame_bytes);
	set_bit(changed, 0u, 0u);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted a broken preamble");
	memcpy(changed, encoded, frame_bytes);
	set_bit(changed, V92_CPU_HEADER_BITS, 1u);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted a row start bit");
	memcpy(changed, encoded, frame_bytes);
	set_bit(changed, payload_end + 17u, 1u);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted a nonzero fill bit");

	memcpy(changed, encoded, frame_bytes);
	set_bit(changed, 26u, 1u);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_CRC_MISMATCH),
	      "CPu decoder missed a CRC error");
	rewrite_crc(changed, payload_end);
	CHECK(v92_cpu_decode(
		      changed, frame_bytes, frame_bits, 36u, &decoded) ==
		      V92_CPU_OK && parameters_are_equal(&decoded, &value),
	      "CPu decoder did not ignore a covered reserved bit");

	memcpy(changed, encoded, frame_bytes);
	write_lsb_field(changed, CPU_RATE_FIRST_BIT, 5u, 31u);
	rewrite_crc(changed, payload_end);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted an invalid data rate");
	memcpy(changed, encoded, frame_bytes);
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval)
		write_lsb_field(changed, map_first_bit(interval), 4u, 1u);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted a gapped interval map");
	memcpy(changed, encoded, frame_bytes);
	for (uint8_t ucode = 0u;
	     ucode < PCM_CONSTELLATION_UCODES; ++ucode)
		set_bit(changed, row_mask_bit(0u, ucode), 0u);
	rewrite_crc(changed, payload_end);
	CHECK(decode_is_transactional(
		      changed, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted an empty active constellation");
	return true;
}

static bool test_decoder_storage_padding(void)
{
	struct v92_cpu_parameters value = parameters(1u, false);
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	size_t frame_bytes;

	CHECK(v92_cpu_encode(
		      &value, 36u, frame, sizeof(frame), &frame_bits) ==
		      V92_CPU_OK && frame_bits % 8u != 0u,
	      "storage-padding fixture encoding failed");
	frame_bytes = (frame_bits + 7u) / 8u;
	set_bit(frame, frame_bits, 1u);
	CHECK(decode_is_transactional(
		      frame, frame_bytes, frame_bits, 36u,
		      V92_CPU_INVALID_FRAME),
	      "CPu decoder accepted nonzero storage padding");
	return true;
}

static bool test_result_names(void)
{
	CHECK(strcmp(v92_cpu_result_name(V92_CPU_OK), "ok") == 0 &&
	      strcmp(v92_cpu_result_name(V92_CPU_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v92_cpu_result_name(V92_CPU_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v92_cpu_result_name(V92_CPU_OUTPUT_TOO_SMALL),
		     "output too small") == 0 &&
	      strcmp(v92_cpu_result_name(V92_CPU_INVALID_FRAME),
		     "invalid frame") == 0 &&
	      strcmp(v92_cpu_result_name(V92_CPU_CRC_MISMATCH),
		     "CRC mismatch") == 0 &&
	      strcmp(v92_cpu_result_name(V92_CPU_INCOMPLETE),
		     "incomplete") == 0 &&
	      strcmp(v92_cpu_result_name((enum v92_cpu_result)99),
		     "unknown") == 0,
	      "CPu result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "dimensions", test_dimensions },
		{ "extent probe failures", test_extent_probe_failures },
		{ "scalar validation", test_scalar_validation },
		{ "constellation validation", test_constellation_validation },
		{ "wire fields", test_wire_fields },
		{ "compact row layout", test_compact_row_layout },
		{ "transactionality", test_transactionality },
		{ "decoder failures", test_decoder_failures },
		{ "decoder storage padding", test_decoder_storage_padding },
		{ "result names", test_result_names },
	};
	unsigned int failures = 0u;

	for (size_t index = 0u; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0u ? 0 : 1;
}
