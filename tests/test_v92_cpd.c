// SPDX-License-Identifier: BSD-3-Clause

#include <crc16_ccitt_false.h>
#include <packed_bits.h>
#include <v92_cpd.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum {
	FIXTURE_ALIGNMENT_BITS = 17,
	CPD_CRC_BITS = 16,
};

static bool parameters_equal(
	const struct v92_cpd_parameters *left,
	const struct v92_cpd_parameters *right)
{
	return left->four_times_gain_q0_16 ==
		       right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u &&
	       left->acknowledges_cpu == right->acknowledges_cpu &&
	       left->modulus_present == right->modulus_present &&
	       left->filters_present == right->filters_present &&
	       left->constellations_present ==
		       right->constellations_present &&
	       memcmp(left->modulus, right->modulus,
		      sizeof(left->modulus)) == 0 &&
	       memcmp(left->frame_constellation,
		      right->frame_constellation,
		      sizeof(left->frame_constellation)) == 0 &&
	       memcmp(left->filter, right->filter,
		      sizeof(left->filter)) == 0 &&
	       memcmp(left->constellation, right->constellation,
		      sizeof(left->constellation)) == 0;
}

static struct v92_cpd_parameters cleardown(void)
{
	return (struct v92_cpd_parameters){
		.four_times_gain_q0_16 = 1u,
		.acknowledges_cpu = true,
		.trellis = V92_CPD_TRELLIS_16_STATE,
	};
}

static struct v92_cpd_parameters selection(unsigned int sections)
{
	struct v92_cpd_parameters parameters = {
		.four_times_gain_q0_16 = UINT16_C(0x4321),
		.data_rate_number = V92_CPD_MAX_DATA_RATE_NUMBER,
		.trellis = V92_CPD_TRELLIS_64_STATE,
		.extend_e2u = true,
		.acknowledges_cpu = true,
		.modulus_present = (sections & 1u) != 0u,
		.filters_present = (sections & 2u) != 0u,
		.constellations_present = (sections & 4u) != 0u,
	};

	if (parameters.modulus_present) {
		for (size_t index = 0u;
		     index < V92_CPD_MODULUS_COUNT; ++index)
			parameters.modulus[index] = (uint8_t)index;
	}
	if (parameters.filters_present) {
		static const uint16_t counts[] = { 2u, 1u, 3u, 1u };
		static const int16_t coefficients[][3] = {
			{ INT16_MAX, INT16_MIN, 0 },
			{ -2, 0, 0 },
			{ 0, 1, -1 },
			{ 1234, 0, 0 },
		};

		for (size_t section = 0u;
		     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
			parameters.filter[section].count = counts[section];
			memcpy(parameters.filter[section].coefficient,
			       coefficients[section],
			       counts[section] * sizeof(coefficients[section][0]));
		}
	}
	if (parameters.constellations_present) {
		static const uint8_t mapping[] = { 0u, 1u, 2u, 0u, 1u, 2u };
		static const uint8_t counts[] = { 2u, 1u, 3u };
		static const int16_t levels[][3] = {
			{ 256, 768, 0 },
			{ 1024, 0, 0 },
			{ 128, 512, 4096 },
		};

		memcpy(parameters.frame_constellation, mapping, sizeof(mapping));
		for (size_t set = 0u; set < ARRAY_LENGTH(counts); ++set) {
			parameters.constellation[set].count = counts[set];
			memcpy(parameters.constellation[set].positive_linear,
			       levels[set],
			       counts[set] * sizeof(levels[set][0]));
		}
	}
	return parameters;
}

static struct v92_cpd_base_parameters base_parameters(
	const struct v92_cpd_parameters *parameters)
{
	return (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 =
			parameters->four_times_gain_q0_16,
		.data_rate_number = parameters->data_rate_number,
		.trellis = parameters->trellis,
		.extend_e2u = parameters->extend_e2u,
		.acknowledges_cpu = parameters->acknowledges_cpu,
	};
}

static uint16_t test_crc(const uint8_t *frame, size_t payload_end)
{
	uint16_t remainder = CRC16_CCITT_FALSE_INITIAL_REMAINDER;

	for (size_t bit = 18u; bit < payload_end; ++bit) {
		if (bit % V92_CPD_WORD_BITS == 0u)
			continue;
		remainder = crc16_ccitt_false_step(
			remainder, packed_msb_bit(frame, bit) != 0u);
	}
	return remainder;
}

static bool test_cleardown_wire_image(void)
{
	static const uint8_t expected[] = {
		0xff, 0xff, 0x80, 0x00, 0x50, 0x00,
		0x0a, 0x86, 0x80, 0x00, 0x00,
	};
	struct v92_cpd_parameters parameters = cleardown();
	struct v92_cpd_parameters decoded;
	uint8_t frame[sizeof(expected)];
	size_t frame_bits = 0u;

	CHECK(V92_CPD_FIXED_BITS == 51u && V92_CPD_TRAILER_BITS == 18u &&
	      v92_cpd_parameters_are_valid(&parameters) &&
	      v92_cpd_payload_bits(&parameters) == 51u &&
	      v92_cpd_frame_bits(&parameters, FIXTURE_ALIGNMENT_BITS) == 85u &&
	      v92_cpd_frame_bytes(&parameters, FIXTURE_ALIGNMENT_BITS) ==
		      sizeof(expected),
	      "cleardown dimensions changed");
	CHECK(v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS,
		      frame, sizeof(frame), &frame_bits) == V92_CPD_OK &&
	      frame_bits == 85u &&
	      memcmp(frame, expected, sizeof(expected)) == 0,
	      "Table 30 cleardown wire image changed");
	CHECK(v92_cpd_decode(
		      frame, sizeof(frame), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_OK &&
	      parameters_equal(&decoded, &parameters),
	      "cleardown CPd did not round-trip");
	return true;
}

static bool test_base_encoding(void)
{
	struct v92_cpd_parameters full = selection(0u);
	struct v92_cpd_base_parameters base = base_parameters(&full);
	struct v92_cpd_base_parameters invalid;
	uint8_t expected[V92_CPD_BASE_MAX_FRAME_BYTES];
	uint8_t actual[V92_CPD_BASE_MAX_FRAME_BYTES];
	uint8_t untouched[V92_CPD_BASE_MAX_FRAME_BYTES];
	uint8_t saved[sizeof(untouched)];
	size_t expected_bits;
	size_t actual_bits;
	size_t untouched_bits;

	CHECK(V92_CPD_BASE_MAX_FRAME_BITS == 110u &&
	      V92_CPD_BASE_MAX_FRAME_BYTES == 14u &&
	      v92_cpd_base_parameters_are_valid(&base),
	      "base CPd dimensions or parameters changed");
	for (unsigned int alignment = V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
	     alignment <= V92_CPD_DOWNSTREAM_DATA_MAX_BITS; ++alignment) {
		CHECK(v92_cpd_encode(
			      &full, alignment, expected, sizeof(expected),
			      &expected_bits) == V92_CPD_OK &&
		      v92_cpd_encode_base(
			      &base, alignment, actual, sizeof(actual),
			      &actual_bits) == V92_CPD_OK &&
		      actual_bits == expected_bits &&
		      memcmp(actual, expected, (actual_bits + 7u) / 8u) == 0,
		      "base CPd differed at alignment %u", alignment);
	}

	memset(untouched, 0xa5, sizeof(untouched));
	memcpy(saved, untouched, sizeof(saved));
	untouched_bits = SIZE_MAX;
	invalid = base;
	invalid.four_times_gain_q0_16 = 0u;
	CHECK(!v92_cpd_base_parameters_are_valid(NULL) &&
	      !v92_cpd_base_parameters_are_valid(&invalid),
	      "invalid base CPd gain was accepted");
	invalid = base;
	invalid.data_rate_number = V92_CPD_MAX_DATA_RATE_NUMBER + 1u;
	CHECK(!v92_cpd_base_parameters_are_valid(&invalid),
	      "invalid base CPd rate was accepted");
	invalid = base;
	invalid.trellis = (enum v92_cpd_trellis)3;
	CHECK(!v92_cpd_base_parameters_are_valid(&invalid),
	      "invalid base CPd trellis was accepted");
	invalid = base;
	invalid.four_times_gain_q0_16 = 0u;
	CHECK(
	      v92_cpd_encode_base(
		      &invalid, FIXTURE_ALIGNMENT_BITS, untouched,
		      sizeof(untouched), &untouched_bits) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      v92_cpd_encode_base(
		      &base, 8u, untouched, sizeof(untouched),
		      &untouched_bits) == V92_CPD_INVALID_ALIGNMENT &&
	      v92_cpd_encode_base(
		      &base, FIXTURE_ALIGNMENT_BITS, untouched, 1u,
		      &untouched_bits) == V92_CPD_OUTPUT_TOO_SMALL &&
	      v92_cpd_encode_base(
		      NULL, FIXTURE_ALIGNMENT_BITS, untouched,
		      sizeof(untouched), &untouched_bits) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_encode_base(
		      &base, FIXTURE_ALIGNMENT_BITS, NULL,
		      sizeof(untouched), &untouched_bits) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_encode_base(
		      &base, FIXTURE_ALIGNMENT_BITS, untouched,
		      sizeof(untouched), NULL) == V92_CPD_INVALID_ARGUMENT &&
	      untouched_bits == SIZE_MAX &&
	      memcmp(untouched, saved, sizeof(untouched)) == 0,
	      "rejected base CPd encoding changed output");
	return true;
}

static size_t expected_payload_bits(unsigned int sections)
{
	size_t bits = V92_CPD_FIXED_BITS;

	if ((sections & 1u) != 0u)
		bits += V92_CPD_MODULUS_BITS;
	if ((sections & 2u) != 0u)
		bits += V92_CPD_FILTER_HEADER_BITS + 7u * V92_CPD_WORD_BITS;
	if ((sections & 4u) != 0u)
		bits += V92_CPD_CONSTELLATION_HEADER_BITS +
			6u * V92_CPD_WORD_BITS;
	return bits;
}

static size_t expected_probe_bits(unsigned int sections)
{
	size_t bits = V92_CPD_FIXED_BITS;

	if ((sections & 1u) != 0u)
		bits += V92_CPD_MODULUS_BITS;
	if ((sections & 2u) != 0u) {
		bits += V92_CPD_FILTER_HEADER_BITS;
		if ((sections & 4u) != 0u)
			bits += 7u * V92_CPD_WORD_BITS;
	}
	if ((sections & 4u) != 0u)
		bits += V92_CPD_CONSTELLATION_HEADER_BITS;
	return bits;
}

static bool test_optional_section_matrix(void)
{
	unsigned int cases = 0u;

	for (unsigned int sections = 0u; sections < 8u; ++sections) {
		struct v92_cpd_parameters parameters = selection(sections);

		for (unsigned int alignment =
			     V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
		     alignment <= V92_CPD_DOWNSTREAM_DATA_MAX_BITS;
		     ++alignment) {
			struct v92_cpd_parameters decoded;
			uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
			uint8_t reencoded[V92_CPD_MAX_FRAME_BYTES];
			size_t frame_bits = 0u;
			size_t reencoded_bits = 0u;
			size_t expected_payload = expected_payload_bits(sections);
			size_t probe_ready = expected_probe_bits(sections);
			size_t expected_frame =
				(expected_payload + V92_CPD_TRAILER_BITS +
				 alignment - 1u) /
				alignment * alignment;

			CHECK(v92_cpd_parameters_are_valid(&parameters) &&
			      v92_cpd_payload_bits(&parameters) == expected_payload &&
			      v92_cpd_encode(
				      &parameters, alignment, frame,
				      sizeof(frame), &frame_bits) == V92_CPD_OK &&
			      frame_bits == expected_frame &&
			      v92_cpd_decode(
				      frame, sizeof(frame), frame_bits,
				      alignment, &decoded) == V92_CPD_OK &&
			      parameters_equal(&decoded, &parameters) &&
			      v92_cpd_encode(
				      &decoded, alignment, reencoded,
				      sizeof(reencoded), &reencoded_bits) == V92_CPD_OK &&
			      reencoded_bits == frame_bits &&
			      memcmp(frame, reencoded,
				     (frame_bits + 7u) / 8u) == 0,
			      "optional section case 0x%x alignment %u failed",
			      sections, alignment);
			for (size_t received = 0u; received <= frame_bits;
			     ++received) {
				size_t probed = SIZE_MAX;
				enum v92_cpd_result probe =
					v92_cpd_probe_frame_bits(
						frame, (received + 7u) / 8u,
						received, alignment, &probed);

				CHECK(probe == (received < probe_ready ?
						      V92_CPD_INCOMPLETE :
						      V92_CPD_OK) &&
				      (probe != V92_CPD_OK ||
				       probed == frame_bits) &&
				      (probe == V92_CPD_OK || probed == SIZE_MAX),
				      "CPd probe sections 0x%x alignment %u bit %zu failed",
				      sections, alignment, received);
			}
			cases++;
		}
	}
	CHECK(cases == 272u, "generated %u optional CPd cases", cases);
	return true;
}

static bool test_unselected_constellations(void)
{
	/* Table 30 selects six interval indices independently of its packed
	 * nonempty constellation list. A peer need not select every listed set.
	 * Mutate an ordinary wire image so neither decoding nor extent probing
	 * depends on our encoder accepting that distinction first. */
	for (unsigned int sections = 4u; sections < 8u; ++sections) {
		for (uint8_t selected = 0u; selected < 3u; ++selected) {
			for (unsigned int alignment = V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
			     alignment <= V92_CPD_DOWNSTREAM_DATA_MAX_BITS; ++alignment) {
				struct v92_cpd_parameters parameters = selection(sections), decoded;
				uint8_t frame[V92_CPD_MAX_FRAME_BYTES], reencoded[sizeof(frame)];
				size_t frame_bits, reencoded_bits, probed = SIZE_MAX;
				size_t header_end = expected_probe_bits(sections);
				size_t header = header_end - V92_CPD_CONSTELLATION_HEADER_BITS;
				size_t payload_end = expected_payload_bits(sections);

				CHECK(v92_cpd_encode(&parameters, alignment, frame, sizeof(frame),
					      &frame_bits) == V92_CPD_OK, "CPd fixture did not encode");
				for (size_t interval = 0u; interval < V92_CPD_FRAME_INTERVALS; ++interval) {
					size_t field = interval < 4u ? 1u + 4u * interval :
						V92_CPD_WORD_BITS + 1u + 4u * (interval - 4u);

					packed_msb_write_lsb_field(frame, header + field, 4u, selected);
					parameters.frame_constellation[interval] = selected;
				}
				packed_msb_write_msb_field(frame, payload_end + 1u, CPD_CRC_BITS,
					test_crc(frame, payload_end));
				CHECK(v92_cpd_probe_frame_bits(frame, sizeof(frame), header_end - 1u,
					      alignment, &probed) == V92_CPD_INCOMPLETE && probed == SIZE_MAX &&
				      v92_cpd_probe_frame_bits(frame, sizeof(frame), header_end,
					      alignment, &probed) == V92_CPD_OK && probed == frame_bits,
				      "CPd extent rejected spare sets: sections %u selected %u alignment %u",
				      sections, selected, alignment);
				CHECK(v92_cpd_decode(frame, sizeof(frame), frame_bits, alignment,
					      &decoded) == V92_CPD_OK && parameters_equal(&decoded, &parameters),
				      "CPd decode rejected or discarded unselected sets");
				CHECK(v92_cpd_encode(&parameters, alignment, reencoded, sizeof(reencoded),
					      &reencoded_bits) == V92_CPD_OK && reencoded_bits == frame_bits &&
				      memcmp(frame, reencoded, (frame_bits + 7u) / 8u) == 0,
				      "CPd encoder changed spare constellation wire image");
				/* Unselected sets remain CRC-covered and must still contain
				 * ordered nonzero points; they are not unvalidated padding. */
				size_t unused_point = header_end + 1u + (selected == 0u ? 2u * V92_CPD_WORD_BITS : 0u);
				uint32_t point = packed_msb_read_lsb_field(frame, unused_point, 16u);

				packed_msb_write_lsb_field(frame, unused_point, 16u, point + 1u);
				CHECK(v92_cpd_decode(frame, sizeof(frame), frame_bits, alignment,
					      &decoded) == V92_CPD_CRC_MISMATCH && parameters_equal(&decoded, &parameters),
				      "damaged spare constellation changed the decoded selection");
				packed_msb_write_lsb_field(frame, unused_point, 16u, 0u);
				packed_msb_write_msb_field(frame, payload_end + 1u, CPD_CRC_BITS,
					test_crc(frame, payload_end));
				CHECK(v92_cpd_decode(frame, sizeof(frame), frame_bits, alignment,
					      &decoded) == V92_CPD_INVALID_FRAME && parameters_equal(&decoded, &parameters),
				      "zero point in an unselected constellation was accepted");
				packed_msb_write_lsb_field(reencoded, header + 1u, 4u, 3u);
				packed_msb_write_msb_field(reencoded, payload_end + 1u, CPD_CRC_BITS,
					test_crc(reencoded, payload_end));
				probed = SIZE_MAX;
				CHECK(v92_cpd_probe_frame_bits(reencoded, sizeof(reencoded), header_end,
					      alignment, &probed) == V92_CPD_INVALID_FRAME && probed == SIZE_MAX &&
				      v92_cpd_decode(reencoded, sizeof(reencoded), frame_bits, alignment,
					      &decoded) == V92_CPD_INVALID_FRAME && parameters_equal(&decoded, &parameters),
				      "empty constellation reference was accepted alongside spare sets");
			}
		}
	}
	return true;
}

static struct v92_cpd_parameters maximum_selection(void)
{
	struct v92_cpd_parameters parameters = {
		.four_times_gain_q0_16 = UINT16_MAX,
		.data_rate_number = V92_CPD_MAX_DATA_RATE_NUMBER,
		.trellis = V92_CPD_TRELLIS_64_STATE,
		.extend_e2u = true,
		.acknowledges_cpu = true,
		.modulus_present = true,
		.filters_present = true,
		.constellations_present = true,
	};

	for (size_t index = 0u; index < V92_CPD_MODULUS_COUNT; ++index)
		parameters.modulus[index] = UINT8_MAX;
	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		struct v92_cpd_filter *filter = &parameters.filter[section];

		filter->count = V92_CPD_MAX_TOTAL_COEFFICIENTS /
			V92_CPD_FILTER_SECTION_COUNT;
		for (size_t index = 0u; index < filter->count; ++index)
			filter->coefficient[index] = (int16_t)((int)index - 48);
	}
	for (size_t set = 0u; set < V92_CPD_MAX_CONSTELLATIONS; ++set) {
		struct v92_cpd_constellation *constellation =
			&parameters.constellation[set];

		parameters.frame_constellation[set] = (uint8_t)set;
		constellation->count = V92_CPD_MAX_CONSTELLATION_POINTS;
		for (size_t index = 0u; index < constellation->count; ++index)
			constellation->positive_linear[index] =
				(int16_t)(index + 1u);
	}
	return parameters;
}

static bool test_maximum_extent(void)
{
	struct v92_cpd_parameters parameters = maximum_selection();
	struct v92_cpd_parameters decoded;
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
	size_t frame_bits = 0u;

	CHECK(v92_cpd_parameters_are_valid(&parameters) &&
	      v92_cpd_payload_bits(&parameters) ==
		      V92_CPD_MAX_PAYLOAD_BITS &&
	      v92_cpd_encode(
		      &parameters, V92_CPD_DOWNSTREAM_DATA_MAX_BITS,
		      frame, sizeof(frame), &frame_bits) == V92_CPD_OK &&
	      frame_bits == 19908u && frame_bits <= V92_CPD_MAX_FRAME_BITS &&
	      v92_cpd_decode(
		      frame, sizeof(frame), frame_bits,
		      V92_CPD_DOWNSTREAM_DATA_MAX_BITS,
		      &decoded) == V92_CPD_OK &&
	      parameters_equal(&decoded, &parameters),
	      "maximum CPd extent did not round-trip");
	return true;
}

static bool invalid_parameters_are_rejected(
	const struct v92_cpd_parameters *parameters)
{
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
	uint8_t saved[sizeof(frame)];
	size_t frame_bits = SIZE_MAX;

	memset(frame, 0xa5, sizeof(frame));
	memcpy(saved, frame, sizeof(saved));
	return !v92_cpd_parameters_are_valid(parameters) &&
	       v92_cpd_payload_bits(parameters) == 0u &&
	       v92_cpd_encode(
		       parameters, FIXTURE_ALIGNMENT_BITS,
		       frame, sizeof(frame), &frame_bits) ==
		       V92_CPD_INVALID_PARAMETERS &&
	       frame_bits == SIZE_MAX &&
	       memcmp(frame, saved, sizeof(frame)) == 0;
}

static bool test_parameter_validation(void)
{
	struct v92_cpd_parameters valid = selection(7u);
	struct v92_cpd_parameters invalid;

	invalid = valid;
	invalid.data_rate_number = V92_CPD_MAX_DATA_RATE_NUMBER + 1u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "out-of-range CPd rate was accepted");
	invalid = valid;
	invalid.trellis = (enum v92_cpd_trellis)3;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "reserved CPd trellis was accepted");
	invalid = valid;
	invalid.four_times_gain_q0_16 = 0u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "zero CPd gain was accepted");
	invalid = selection(0u);
	invalid.modulus[0] = 1u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "stale absent modulus was accepted");
	invalid = valid;
	invalid.filter[0].count =
		V92_CPD_MAX_SECTION_COEFFICIENTS + 1u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "oversized CPd filter section was accepted");
	invalid = valid;
	invalid.filter[0].count = V92_CPD_MAX_SECTION_COEFFICIENTS;
	invalid.filter[1].count =
		V92_CPD_MAX_TOTAL_COEFFICIENTS -
		V92_CPD_MAX_SECTION_COEFFICIENTS + 1u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "oversized CPd filter total was accepted");
	invalid = valid;
	invalid.filter[0].coefficient[invalid.filter[0].count] = 1;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "stale CPd filter coefficient was accepted");
	invalid = valid;
	invalid.constellation[0].positive_linear[1] =
		invalid.constellation[0].positive_linear[0];
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "unordered CPd constellation was accepted");
	invalid = valid;
	invalid.constellation[1].count = 0u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "gapped CPd constellation list was accepted");
	invalid = valid;
	invalid.frame_constellation[5] = 3u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "empty CPd constellation reference was accepted");
	invalid = selection(0u);
	invalid.frame_constellation[0] = 1u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "stale absent constellation map was accepted");
	invalid = valid;
	invalid.constellation[0].count =
		V92_CPD_MAX_CONSTELLATION_POINTS + 1u;
	CHECK(invalid_parameters_are_rejected(&invalid),
	      "oversized CPd constellation was accepted");
	return true;
}

static bool test_wire_rejection_and_contracts(void)
{
	struct v92_cpd_parameters parameters = cleardown();
	struct v92_cpd_parameters decoded;
	struct v92_cpd_parameters saved;
	uint8_t optional[64];
	uint8_t frame[16];
	uint8_t changed[16];
	uint8_t untouched[16];
	uint8_t untouched_saved[16];
	size_t frame_bits = 0u;
	size_t untouched_bits = SIZE_MAX;

	CHECK(v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS,
		      frame, sizeof(frame), &frame_bits) == V92_CPD_OK,
	      "wire rejection fixture did not encode");
	memset(&decoded, 0xa5, sizeof(decoded));
	memcpy(&saved, &decoded, sizeof(saved));
	CHECK(v92_cpd_decode(NULL, sizeof(frame), frame_bits,
			     FIXTURE_ALIGNMENT_BITS, &decoded) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_decode(frame, sizeof(frame), frame_bits,
			     FIXTURE_ALIGNMENT_BITS, NULL) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_decode(frame, 1u, frame_bits,
			     FIXTURE_ALIGNMENT_BITS, &decoded) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_decode(frame, sizeof(frame), frame_bits, 8u,
			     &decoded) == V92_CPD_INVALID_ALIGNMENT &&
	      memcmp(&decoded, &saved, sizeof(decoded)) == 0,
	      "invalid CPd decode call changed its destination");

	for (size_t bit = 0u; bit < 17u; ++bit) {
		memcpy(changed, frame, sizeof(changed));
		packed_msb_set_bit(changed, bit, false);
		CHECK(v92_cpd_probe_frame_bits(
			      changed, (V92_CPD_FIXED_BITS + 7u) / 8u,
			      V92_CPD_FIXED_BITS, FIXTURE_ALIGNMENT_BITS,
			      &untouched_bits) == V92_CPD_INVALID_FRAME &&
		      untouched_bits == SIZE_MAX &&
		      v92_cpd_decode(changed, sizeof(changed), frame_bits,
			      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME &&
		      memcmp(&decoded, &saved, sizeof(decoded)) == 0,
		      "CPd accepted a broken prefix bit %zu", bit);
	}

	memcpy(changed, frame, sizeof(changed));
	packed_msb_set_bit(changed, 30u, true);
	packed_msb_write_msb_field(
		changed, V92_CPD_FIXED_BITS + 1u, CPD_CRC_BITS,
		test_crc(changed, V92_CPD_FIXED_BITS));
	CHECK(v92_cpd_decode(
		      changed, sizeof(changed), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_OK &&
	      parameters_equal(&decoded, &parameters),
	      "CRC-covered reserved CPd field was interpreted");

	memcpy(changed, frame, sizeof(changed));
	packed_msb_set_bit(changed, 34u, true);
	CHECK(v92_cpd_decode(
		      changed, sizeof(changed), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME,
	      "CPd fixed row start was accepted");
	memcpy(changed, frame, sizeof(changed));
	changed[(V92_CPD_FIXED_BITS + 1u) / 8u] ^=
		(uint8_t)(1u <<
			  (7u - (V92_CPD_FIXED_BITS + 1u) % 8u));
	CHECK(v92_cpd_decode(
		      changed, sizeof(changed), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_CRC_MISMATCH,
	      "CPd CRC damage was not distinguished");
	saved = decoded;
	for (unsigned int last = 0u; last < 2u; ++last) {
		size_t bit = last ? frame_bits - 1u :
			V92_CPD_FIXED_BITS + 1u + CPD_CRC_BITS;

		memcpy(changed, frame, sizeof(changed));
		packed_msb_set_bit(changed, bit, true);
		CHECK(v92_cpd_decode(
			      changed, sizeof(changed), frame_bits,
			      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME &&
		      memcmp(&decoded, &saved, sizeof(decoded)) == 0,
		      "nonzero CPd fill bit %zu was accepted", bit);
	}
	memcpy(changed, frame, sizeof(changed));
	packed_msb_set_bit(changed, frame_bits, true);
	CHECK(v92_cpd_decode(
		      changed, sizeof(changed), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME,
	      "nonzero CPd storage padding was accepted");
	CHECK(v92_cpd_decode(
		      frame, sizeof(frame), 68u,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME &&
	      v92_cpd_decode(
		      frame, sizeof(frame), 102u,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME,
		      "truncated or overlong CPd extent was accepted");

	parameters = selection(2u);
	CHECK(v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS,
		      optional, sizeof(optional), &frame_bits) == V92_CPD_OK,
	      "filter bound fixture did not encode");
	packed_msb_write_lsb_field(
		optional, V92_CPD_FIXED_BITS + 1u, 9u,
		V92_CPD_MAX_SECTION_COEFFICIENTS + 1u);
	CHECK(v92_cpd_probe_frame_bits(
		      optional, sizeof(optional),
		      V92_CPD_FIXED_BITS + V92_CPD_FILTER_HEADER_BITS,
		      FIXTURE_ALIGNMENT_BITS, &untouched_bits) ==
		      V92_CPD_INVALID_FRAME &&
	      v92_cpd_decode(
		      optional, sizeof(optional), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME,
	      "oversized wire filter count was accepted");

	parameters = selection(4u);
	CHECK(v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS,
		      optional, sizeof(optional), &frame_bits) == V92_CPD_OK,
	      "constellation bound fixture did not encode");
	packed_msb_write_lsb_field(
		optional, V92_CPD_FIXED_BITS + 2u * V92_CPD_WORD_BITS + 1u,
		8u, V92_CPD_MAX_CONSTELLATION_POINTS + 1u);
	CHECK(v92_cpd_probe_frame_bits(
		      optional, sizeof(optional),
		      V92_CPD_FIXED_BITS + V92_CPD_CONSTELLATION_HEADER_BITS,
		      FIXTURE_ALIGNMENT_BITS, &untouched_bits) ==
		      V92_CPD_INVALID_FRAME &&
	      v92_cpd_decode(
		      optional, sizeof(optional), frame_bits,
		      FIXTURE_ALIGNMENT_BITS, &decoded) == V92_CPD_INVALID_FRAME,
	      "oversized wire constellation count was accepted");

	memset(untouched, 0xa5, sizeof(untouched));
	memcpy(untouched_saved, untouched, sizeof(untouched));
	CHECK(v92_cpd_encode(
		      NULL, FIXTURE_ALIGNMENT_BITS, untouched,
		      sizeof(untouched), &untouched_bits) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS, NULL,
		      sizeof(untouched), &untouched_bits) ==
		      V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS, untouched,
		      sizeof(untouched), NULL) == V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_encode(
		      &parameters, 43u, untouched,
		      sizeof(untouched), &untouched_bits) ==
		      V92_CPD_INVALID_ALIGNMENT &&
	      v92_cpd_encode(
		      &parameters, FIXTURE_ALIGNMENT_BITS, untouched,
		      v92_cpd_frame_bytes(
			      &parameters, FIXTURE_ALIGNMENT_BITS) - 1u,
		      &untouched_bits) == V92_CPD_OUTPUT_TOO_SMALL &&
	      untouched_bits == SIZE_MAX &&
	      memcmp(untouched, untouched_saved, sizeof(untouched)) == 0,
	      "rejected CPd encoding changed output");

	untouched_bits = SIZE_MAX;
	CHECK(v92_cpd_probe_frame_bits(
		      NULL, 0u, 0u, FIXTURE_ALIGNMENT_BITS,
		      &untouched_bits) == V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_probe_frame_bits(
		      frame, sizeof(frame), 0u, FIXTURE_ALIGNMENT_BITS,
		      NULL) == V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_probe_frame_bits(
		      frame, 0u, 1u, FIXTURE_ALIGNMENT_BITS,
		      &untouched_bits) == V92_CPD_INVALID_ARGUMENT &&
	      v92_cpd_probe_frame_bits(
		      frame, sizeof(frame), 0u, 8u,
		      &untouched_bits) == V92_CPD_INVALID_ALIGNMENT &&
	      v92_cpd_probe_frame_bits(
		      frame, sizeof(frame), V92_CPD_MAX_FRAME_BITS + 1u,
		      FIXTURE_ALIGNMENT_BITS, &untouched_bits) ==
		      V92_CPD_INVALID_FRAME &&
	      untouched_bits == SIZE_MAX,
	      "rejected CPd probe changed its output");

	for (int result = V92_CPD_OK; result <= V92_CPD_INCOMPLETE;
	     ++result) {
		CHECK(strcmp(
			      v92_cpd_result_name((enum v92_cpd_result)result),
			      "unknown") != 0,
		      "CPd result %d has no name", result);
	}
	CHECK(strcmp(
		      v92_cpd_result_name((enum v92_cpd_result)99),
		      "unknown") == 0,
	      "unknown CPd result name changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "cleardown wire image", test_cleardown_wire_image },
		{ "base encoding", test_base_encoding },
		{ "optional section matrix", test_optional_section_matrix },
		{ "unselected constellations", test_unselected_constellations },
		{ "maximum extent", test_maximum_extent },
		{ "parameter validation", test_parameter_validation },
		{ "wire rejection and contracts", test_wire_rejection_and_contracts },
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
