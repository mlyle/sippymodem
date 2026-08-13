// SPDX-License-Identifier: BSD-3-Clause

#include <v90_info1.h>
#include <v92_pcm_info1a.h>

#include "test.h"

#include <string.h>

static struct v92_pcm_info1a selection(void)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 256,
		.maximum_section_coefficients = 128,
		.local_md_length_276_symbol_units = 0,
		.training_ucode = 78,
	};
}

static bool selections_equal(
	const struct v92_pcm_info1a *left,
	const struct v92_pcm_info1a *right)
{
	return left->filter_sections == right->filter_sections &&
	       left->maximum_total_coefficients ==
		       right->maximum_total_coefficients &&
	       left->maximum_section_coefficients ==
		       right->maximum_section_coefficients &&
	       left->local_md_length_276_symbol_units ==
		       right->local_md_length_276_symbol_units &&
	       left->training_ucode == right->training_ucode;
}

static void replace_field(
	uint8_t *payload, size_t first_bit, size_t bit_count, uint16_t value)
{
	for (size_t index = 0; index < bit_count; ++index) {
		uint8_t mask = (uint8_t)(
			1u << (7u - (first_bit + index) % 8u));

		payload[(first_bit + index) / 8u] &= (uint8_t)~mask;
		if ((value & (uint16_t)(1u << index)) != 0)
			payload[(first_bit + index) / 8u] |= mask;
	}
}

static bool test_wire_image(void)
{
	static const uint8_t expected[] = {
		0x20, 0x03, 0x91, 0xbf, 0xfc,
	};
	struct v92_pcm_info1a selected = selection();
	struct v92_pcm_info1a decoded;
	struct v90_info1a conventional;
	uint8_t payload[V92_PCM_INFO1A_PAYLOAD_BYTES];

	CHECK(V92_PCM_INFO1A_PAYLOAD_BITS == 38 &&
	      V92_PCM_INFO1A_PAYLOAD_BYTES == sizeof(expected) &&
	      v92_pcm_info1a_is_valid(&selected) &&
	      v92_pcm_info1a_encode(&selected, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK &&
	      memcmp(payload, expected, sizeof(payload)) == 0 &&
	      v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_OK &&
	      selections_equal(&selected, &decoded),
	      "V.92 PCM-upstream INFO1a wire image changed");
	CHECK(v90_info1a_decode(
		      payload, sizeof(payload), V90_INFO1A_PAYLOAD_BITS,
		      &conventional) == V90_INFO1_INVALID_PAYLOAD,
	      "PCM-upstream INFO1a was accepted as conventional upstream");
	return true;
}

static bool test_generated_round_trips(void)
{
	uint8_t payload[V92_PCM_INFO1A_PAYLOAD_BYTES];
	uint8_t reencoded[V92_PCM_INFO1A_PAYLOAD_BYTES];
	unsigned int cases = 0;

	for (unsigned int filter = V92_PCM_FILTER_P1_Z2;
	     filter <= V92_PCM_FILTER_Z1_P1_P2_Z2; ++filter) {
		for (uint16_t total = V92_PCM_INFO1A_MIN_TOTAL_COEFFICIENTS;
		     total <= V92_PCM_INFO1A_MAX_TOTAL_COEFFICIENTS;
		     total += V92_PCM_INFO1A_COEFFICIENT_STEP) {
			for (uint16_t section =
				     V92_PCM_INFO1A_MIN_SECTION_COEFFICIENTS;
			     section <=
				     V92_PCM_INFO1A_MAX_SECTION_COEFFICIENTS;
			     section += V92_PCM_INFO1A_COEFFICIENT_STEP) {
				static const uint8_t md_lengths[] = { 0, 127 };
				static const uint8_t ucodes[] = { 67, 127 };

				if (section > total)
					continue;
				for (size_t edge = 0;
				     edge < ARRAY_LENGTH(md_lengths); ++edge) {
					struct v92_pcm_info1a selected = {
						.filter_sections =
							(enum v92_pcm_filter_sections)
								filter,
						.maximum_total_coefficients = total,
						.maximum_section_coefficients =
							section,
						.local_md_length_276_symbol_units =
							md_lengths[edge],
						.training_ucode = ucodes[edge],
					};
					struct v92_pcm_info1a decoded;

					CHECK(v92_pcm_info1a_encode(
						      &selected, payload,
						      sizeof(payload)) ==
						      V92_PCM_INFO1A_OK &&
					      v92_pcm_info1a_decode(
						      payload, sizeof(payload),
						      V92_PCM_INFO1A_PAYLOAD_BITS,
						      &decoded) ==
						      V92_PCM_INFO1A_OK &&
					      selections_equal(&selected, &decoded) &&
					      v92_pcm_info1a_encode(
						      &decoded, reencoded,
						      sizeof(reencoded)) ==
						      V92_PCM_INFO1A_OK &&
					      memcmp(payload, reencoded,
						     sizeof(payload)) == 0,
					      "generated INFO1a case %u failed",
					      cases);
					cases++;
				}
			}
		}
	}
	CHECK(cases == 104, "generated %u INFO1a cases, expected 104", cases);
	return true;
}

static bool test_reserved_bits(void)
{
	struct v92_pcm_info1a selected = selection();
	struct v92_pcm_info1a decoded;
	uint8_t payload[V92_PCM_INFO1A_PAYLOAD_BYTES];

	CHECK(v92_pcm_info1a_encode(&selected, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK,
	      "reserved-bit fixture did not encode");
	replace_field(payload, 20, 2, 3);
	replace_field(payload, 28, 10, 0);
	CHECK(v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_OK &&
	      selections_equal(&selected, &decoded),
	      "reserved INFO1a bits were interpreted");
	CHECK(v92_pcm_info1a_encode(&decoded, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK,
	      "reserved INFO1a fields did not re-encode");
	CHECK((payload[2] & UINT8_C(0x0c)) == 0,
	      "reserved-zero INFO1a bits were not canonicalized");
	for (size_t bit = 28; bit < 38; ++bit)
		CHECK((payload[bit / 8u] &
		       (uint8_t)(1u << (7u - bit % 8u))) != 0,
		      "reserved-one INFO1a bit %zu was not canonicalized", bit);
	return true;
}

static bool invalid_selection_is_rejected(
	const struct v92_pcm_info1a *invalid)
{
	uint8_t payload[V92_PCM_INFO1A_PAYLOAD_BYTES] = {
		0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	};
	uint8_t saved[sizeof(payload)];

	memcpy(saved, payload, sizeof(saved));
	return !v92_pcm_info1a_is_valid(invalid) &&
	       v92_pcm_info1a_encode(invalid, payload, sizeof(payload)) ==
		       V92_PCM_INFO1A_INVALID_SELECTION &&
	       memcmp(payload, saved, sizeof(payload)) == 0;
}

static bool test_validation(void)
{
	struct v92_pcm_info1a valid = selection();
	struct v92_pcm_info1a invalid = valid;
	struct v92_pcm_info1a decoded = selection();
	struct v92_pcm_info1a saved_decoded = decoded;
	uint8_t payload[V92_PCM_INFO1A_PAYLOAD_BYTES + 1u] = {
		0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	};
	uint8_t saved_payload[sizeof(payload)];

	invalid.filter_sections = (enum v92_pcm_filter_sections)4;
	CHECK(invalid_selection_is_rejected(&invalid),
	      "invalid filter layout was accepted");
	invalid = valid;
	invalid.maximum_total_coefficients = 200;
	CHECK(invalid_selection_is_rejected(&invalid),
	      "off-grid total coefficient limit was accepted");
	invalid = valid;
	invalid.maximum_section_coefficients = 192;
	invalid.maximum_total_coefficients = 192;
	CHECK(v92_pcm_info1a_is_valid(&invalid),
	      "equal total and section coefficient limits were rejected");
	invalid.maximum_section_coefficients = 256;
	CHECK(invalid_selection_is_rejected(&invalid),
	      "section limit greater than total was accepted");
	invalid = valid;
	invalid.local_md_length_276_symbol_units = 128;
	CHECK(invalid_selection_is_rejected(&invalid),
	      "out-of-range MD length was accepted");
	invalid = valid;
	invalid.training_ucode = 66;
	CHECK(invalid_selection_is_rejected(&invalid),
	      "low training Ucode was accepted");
	invalid.training_ucode = 128;
	CHECK(invalid_selection_is_rejected(&invalid),
	      "high training Ucode was accepted");

	memcpy(saved_payload, payload, sizeof(payload));
	CHECK(v92_pcm_info1a_encode(NULL, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_INVALID_ARGUMENT &&
	      v92_pcm_info1a_encode(&valid, NULL, sizeof(payload)) ==
		      V92_PCM_INFO1A_INVALID_ARGUMENT &&
	      v92_pcm_info1a_encode(
		      &valid, payload, V92_PCM_INFO1A_PAYLOAD_BYTES - 1u) ==
		      V92_PCM_INFO1A_INVALID_ARGUMENT &&
	      memcmp(payload, saved_payload, sizeof(payload)) == 0,
	      "INFO1a encoding accepted an invalid argument or changed output");

	CHECK(v92_pcm_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK,
	      "decode fixture did not encode");
	replace_field(payload, 22, 3, 5);
	CHECK(v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_INVALID_PAYLOAD,
	      "non-PCM analog symbol rate was accepted");
	CHECK(v92_pcm_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK,
	      "decode fixture did not reset");
	replace_field(payload, 25, 3, 5);
	CHECK(v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_INVALID_PAYLOAD,
	      "non-PCM digital symbol rate was accepted");
	CHECK(v92_pcm_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK,
	      "decode fixture did not reset");
	replace_field(payload, 2, 2, 0);
	replace_field(payload, 4, 2, 3);
	CHECK(v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_INVALID_PAYLOAD,
	      "inconsistent coefficient limits were accepted");
	CHECK(v92_pcm_info1a_encode(&valid, payload, sizeof(payload)) ==
		      V92_PCM_INFO1A_OK,
	      "padding fixture did not reset");
	payload[V92_PCM_INFO1A_PAYLOAD_BYTES - 1u] |= 1u;
	CHECK(v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_INVALID_PAYLOAD &&
	      v92_pcm_info1a_decode(
		      payload, sizeof(payload),
		      V92_PCM_INFO1A_PAYLOAD_BITS - 1u, &decoded) ==
		      V92_PCM_INFO1A_INVALID_PAYLOAD &&
	      v92_pcm_info1a_decode(
		      NULL, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      &decoded) == V92_PCM_INFO1A_INVALID_ARGUMENT &&
	      v92_pcm_info1a_decode(
		      payload, sizeof(payload), V92_PCM_INFO1A_PAYLOAD_BITS,
		      NULL) == V92_PCM_INFO1A_INVALID_ARGUMENT &&
	      v92_pcm_info1a_decode(
		      payload, V92_PCM_INFO1A_PAYLOAD_BYTES - 1u,
		      V92_PCM_INFO1A_PAYLOAD_BITS, &decoded) ==
		      V92_PCM_INFO1A_INVALID_ARGUMENT &&
	      selections_equal(&decoded, &saved_decoded) &&
	      !v92_pcm_info1a_is_valid(NULL) &&
	      strcmp(v92_pcm_info1a_result_name(
		     V92_PCM_INFO1A_INVALID_PAYLOAD), "invalid payload") == 0 &&
	      strcmp(v92_pcm_info1a_result_name(
		     (enum v92_pcm_info1a_result)4), "unknown") == 0,
	      "INFO1a decoding accepted malformed input or changed output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "wire image", test_wire_image },
		{ "generated round trips", test_generated_round_trips },
		{ "reserved bits", test_reserved_bits },
		{ "validation", test_validation },
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
