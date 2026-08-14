// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpu.h>

#include "test.h"

#include <string.h>

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

static bool test_dimensions(void)
{
	CHECK(V92_CPU_MIN_FRAME_BITS == 312 &&
	      V92_CPU_MAX_FRAME_BITS == 1824 &&
	      V92_CPU_MAX_FRAME_BYTES == 228,
	      "CPu public bounds changed");
	for (uint8_t count = 1u;
	     count <= V92_CPU_MAX_CONSTELLATIONS; ++count) {
		for (unsigned int differs = 0u; differs < 2u; ++differs) {
			struct v92_cpu_parameters value =
				parameters(count, differs != 0u);
			size_t sets = (differs != 0u ? 2u : 1u) * count;
			size_t unfilled = V92_CPU_HEADER_BITS +
				sets * V92_CPU_CONSTELLATION_BITS +
				V92_CPU_MIN_TRAILER_BITS;

			for (unsigned int bits = V92_CPU_MIN_BITS_PER_SYMBOL;
			     bits <= V92_CPU_MAX_BITS_PER_SYMBOL; ++bits) {
				size_t multiple =
					bits * V92_CPU_SYMBOLS_PER_FRAME;
				size_t expected =
					(unfilled + multiple - 1u) / multiple * multiple;

				CHECK(v92_cpu_parameters_are_valid(&value) &&
				      v92_cpu_frame_bits(&value, bits) == expected &&
				      v92_cpu_frame_bytes(&value, bits) ==
					      (expected + 7u) / 8u,
				      "CPu %u/%u/%u has the wrong extent",
				      count, differs, bits);
			}
		}
	}
	return true;
}

static bool test_scalar_validation(void)
{
	struct v92_cpu_parameters valid = parameters(1u, false);
	struct v92_cpu_parameters changed;

	CHECK(!v92_cpu_parameters_are_valid(NULL) &&
	      v92_cpu_frame_bits(NULL, 2u) == 0u,
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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "dimensions", test_dimensions },
		{ "scalar validation", test_scalar_validation },
		{ "constellation validation", test_constellation_validation },
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
