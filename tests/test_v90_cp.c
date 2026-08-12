// SPDX-License-Identifier: BSD-3-Clause

#include <v90_cp.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void add_ucode(
	struct v90_cp_constellation *constellation, uint8_t ucode)
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
	struct v90_cp_constellation constellation = { 0 };

	add_ucode(&constellation, 0);
	add_ucode(&constellation, 63);
	add_ucode(&constellation, 127);
	CHECK(v90_cp_constellation_contains(&constellation, 0) &&
	      v90_cp_constellation_contains(&constellation, 63) &&
	      v90_cp_constellation_contains(&constellation, 127) &&
	      !v90_cp_constellation_contains(&constellation, 1) &&
	      !v90_cp_constellation_contains(&constellation, 128) &&
	      !v90_cp_constellation_contains(NULL, 0),
	      "CP constellation membership changed");
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

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "dimensions and boundaries", test_dimensions_and_boundaries },
		{ "constellation membership", test_constellation_membership },
		{ "scalar validation", test_scalar_validation },
		{ "canonical masks", test_canonical_masks },
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
