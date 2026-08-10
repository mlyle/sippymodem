// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_shell_mapper.h>

#include "test.h"

#include <string.h>

enum {
	MAX_2_RING_SUM_COUNT = 2 * SIPPYMODEM_V34_MAX_RING_COUNT - 1,
	MAX_4_RING_SUM_COUNT = 4 * SIPPYMODEM_V34_MAX_RING_COUNT - 3,
};

struct inverse_counts {
	uint32_t pair[MAX_2_RING_SUM_COUNT];
	uint32_t quartet[MAX_4_RING_SUM_COUNT];
	uint8_t pair_count;
	uint8_t quartet_count;
};

struct symbol_rate_fixture {
	enum sippymodem_v34_symbol_rate symbol_rate;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ SIPPYMODEM_V34_SYMBOL_RATE_2400, 1, 9 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2743, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_2800, 2, 11 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3000, 2, 12 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3200, 2, 13 },
	{ SIPPYMODEM_V34_SYMBOL_RATE_3429, 2, 14 },
};

static void generate_inverse_counts(
	uint8_t ring_count, struct inverse_counts *counts)
{
	memset(counts, 0, sizeof(*counts));
	counts->pair_count = (uint8_t)(2u * ring_count - 1u);
	counts->quartet_count = (uint8_t)(4u * ring_count - 3u);
	for (uint8_t first = 0; first < ring_count; ++first) {
		for (uint8_t second = 0; second < ring_count; ++second)
			counts->pair[first + second]++;
	}
	for (uint8_t first = 0; first < ring_count; ++first) {
		for (uint8_t second = 0; second < ring_count; ++second) {
			for (uint8_t third = 0; third < ring_count; ++third) {
				for (uint8_t fourth = 0; fourth < ring_count;
				     ++fourth) {
					counts->quartet[
						first + second + third + fourth]++;
				}
			}
		}
	}
}

static uint64_t partition_prefix(
	const uint32_t *weights, uint8_t count, uint8_t total,
	uint8_t selected_left)
{
	uint64_t prefix = 0;

	for (uint8_t left = 0; left < selected_left; ++left) {
		uint8_t right;

		if (left > total)
			break;
		right = (uint8_t)(total - left);
		if (left < count && right < count)
			prefix += (uint64_t)weights[left] * weights[right];
	}
	return prefix;
}

static uint64_t convolution_count(
	const uint32_t *weights, uint8_t count, uint8_t total)
{
	uint64_t result = 0;

	for (uint8_t left = 0; left < count && left <= total; ++left) {
		uint8_t right = (uint8_t)(total - left);

		if (right < count)
			result += (uint64_t)weights[left] * weights[right];
	}
	return result;
}

static uint8_t inverse_pair_selector(
	uint8_t ring_count, uint8_t sum, const uint8_t pair[2])
{
	return sum < ring_count ? pair[0] :
		(uint8_t)(ring_count - 1u - pair[1]);
}

static bool recover_shell_label(
	const struct sippymodem_v34_ring_indices *indices,
	uint8_t ring_count, const struct inverse_counts *counts,
	uint64_t *shell_label)
{
	uint8_t pair_sum[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	uint8_t selector[SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME];
	uint8_t sum_4d_first;
	uint8_t sum_4d_second;
	uint8_t sum_8d;
	uint64_t rank_pair_first;
	uint64_t rank_pair_second;
	uint64_t rank_4d_first;
	uint64_t rank_4d_second;
	uint64_t rank_8d;
	uint64_t prefix_8d = 0;

	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		const uint8_t *pair = indices->index[interval];

		if (pair[0] >= ring_count || pair[1] >= ring_count)
			return false;
		pair_sum[interval] = (uint8_t)(pair[0] + pair[1]);
		selector[interval] = inverse_pair_selector(
			ring_count, pair_sum[interval], pair);
		if (selector[interval] >= counts->pair[pair_sum[interval]])
			return false;
	}

	sum_4d_first = (uint8_t)(pair_sum[0] + pair_sum[1]);
	sum_4d_second = (uint8_t)(pair_sum[2] + pair_sum[3]);
	sum_8d = (uint8_t)(sum_4d_first + sum_4d_second);
	rank_pair_first = selector[0] +
		(uint64_t)counts->pair[pair_sum[0]] * selector[1];
	rank_pair_second = selector[2] +
		(uint64_t)counts->pair[pair_sum[2]] * selector[3];
	rank_4d_first = partition_prefix(
		counts->pair, counts->pair_count, sum_4d_first,
		pair_sum[0]) + rank_pair_first;
	rank_4d_second = partition_prefix(
		counts->pair, counts->pair_count, sum_4d_second,
		pair_sum[2]) + rank_pair_second;
	rank_8d = partition_prefix(
		counts->quartet, counts->quartet_count, sum_8d,
		sum_4d_first) + rank_4d_first +
		(uint64_t)counts->quartet[sum_4d_first] * rank_4d_second;
	for (uint8_t sum = 0; sum < sum_8d; ++sum) {
		prefix_8d += convolution_count(
			counts->quartet, counts->quartet_count, sum);
	}
	*shell_label = prefix_8d + rank_8d;
	return true;
}

static bool verify_label(
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct inverse_counts *counts, uint32_t shell_label)
{
	struct sippymodem_v34_ring_indices indices;
	uint64_t recovered = 0;

	CHECK(sippymodem_v34_shell_map(
		      parameters, shell_label, &indices) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_OK,
	      "K=%u M=%u rejected shell label %u",
	      parameters->shell_mapper_bits, parameters->ring_count,
	      shell_label);
	CHECK(sippymodem_v34_ring_indices_are_valid(&indices, parameters),
	      "K=%u M=%u produced an invalid ring index for %u",
	      parameters->shell_mapper_bits, parameters->ring_count,
	      shell_label);
	CHECK(recover_shell_label(
		      &indices, parameters->ring_count, counts, &recovered) &&
	      recovered == shell_label,
	      "K=%u M=%u mapped %u to independently ranked %llu",
	      parameters->shell_mapper_bits, parameters->ring_count,
	      shell_label, (unsigned long long)recovered);
	return true;
}

static bool test_small_shell_order(void)
{
	static const struct {
		uint32_t shell_label;
		uint8_t one_interval;
		uint8_t one_symbol;
	} single_ring_cases[] = {
		{ 1, 3, 1 },
		{ 2, 3, 0 },
		{ 4, 2, 0 },
		{ 8, 0, 0 },
	};
	const struct sippymodem_v34_mapping_parameters parameters = {
		.constellation_points = 8,
		.mapping_frame_bits = 20,
		.shell_mapper_bits = 8,
		.point_index_bits_per_2d_symbol = 0,
		.ring_count = 2,
		.expanded_shaping = false,
	};
	struct sippymodem_v34_ring_indices indices;

	CHECK(sippymodem_v34_mapping_parameters_is_valid(&parameters),
	      "small shell fixture has invalid dimensions");
	CHECK(sippymodem_v34_shell_map(&parameters, 0, &indices) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_OK,
	      "zero shell label was rejected");
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		CHECK(indices.index[interval][0] == 0u &&
		      indices.index[interval][1] == 0u,
		      "zero shell label selected a nonzero ring");
	}

	for (size_t fixture = 0;
	     fixture < ARRAY_LENGTH(single_ring_cases); ++fixture) {
		CHECK(sippymodem_v34_shell_map(
			      &parameters, single_ring_cases[fixture].shell_label,
			      &indices) == SIPPYMODEM_V34_SHELL_MAPPER_OK,
		      "single-ring shell fixture %zu was rejected", fixture);
		for (uint8_t interval = 0;
		     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
		     ++interval) {
			for (uint8_t symbol = 0;
			     symbol < SIPPYMODEM_V34_RING_INDICES_PER_4D_INTERVAL;
			     ++symbol) {
				uint8_t expected =
					interval == single_ring_cases[fixture]
							    .one_interval &&
						symbol == single_ring_cases[fixture]
							  .one_symbol;

				CHECK(indices.index[interval][symbol] == expected,
				      "single-ring shell fixture %zu changed order",
				      fixture);
			}
		}
	}
	CHECK(sippymodem_v34_shell_map(&parameters, 255, &indices) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_OK,
	      "last two-ring shell label was rejected");
	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		CHECK(indices.index[interval][0] == 1u &&
		      indices.index[interval][1] == 1u,
		      "last two-ring shell label did not select every outer ring");
	}
	return true;
}

static bool test_all_supported_parameters(void)
{
	struct inverse_counts counts[SIPPYMODEM_V34_MAX_RING_COUNT + 1];
	bool counts_ready[SIPPYMODEM_V34_MAX_RING_COUNT + 1] = { false };
	bool combination_seen[SIPPYMODEM_V34_MAX_SHELL_MAPPER_BITS + 1]
			     [SIPPYMODEM_V34_MAX_RING_COUNT + 1] = { false };
	unsigned int geometry_variants = 0;
	unsigned int unique_combinations = 0;
	uint64_t labels_checked = 0;

	for (size_t symbol_index = 0;
	     symbol_index < ARRAY_LENGTH(SYMBOL_RATES); ++symbol_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[symbol_index];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct sippymodem_v34_frame_geometry geometry;

				CHECK(sippymodem_v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0) ==
					      SIPPYMODEM_V34_FRAME_SCHEDULE_OK,
				      "geometry %zu/%u/%u did not initialize",
				      symbol_index, rate, auxiliary);
				for (unsigned int expanded = 0; expanded < 2;
				     ++expanded) {
					struct sippymodem_v34_mapping_parameters
						parameters;
					uint32_t label_limit;
					uint32_t maximum;

					CHECK(sippymodem_v34_mapping_parameters_init(
						      &parameters, &geometry,
						      expanded != 0) ==
						      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
					      "parameters %zu/%u/%u/%u failed",
					      symbol_index, rate, auxiliary, expanded);
					if (!counts_ready[parameters.ring_count]) {
						generate_inverse_counts(
							parameters.ring_count,
							&counts[parameters.ring_count]);
						counts_ready[parameters.ring_count] = true;
					}
					label_limit =
						parameters.shell_mapper_bits == 0 ? 1u :
						UINT32_C(1) <<
							parameters.shell_mapper_bits;
					maximum = label_limit - 1u;
					CHECK(verify_label(
						      &parameters,
						      &counts[parameters.ring_count], 0) &&
					      verify_label(
						      &parameters,
						      &counts[parameters.ring_count], maximum),
					      "geometry endpoint mapping failed");
					labels_checked += 2;
					geometry_variants++;

					if (combination_seen
						    [parameters.shell_mapper_bits]
						    [parameters.ring_count])
						continue;
					combination_seen[parameters.shell_mapper_bits]
							[parameters.ring_count] = true;
					unique_combinations++;
					if (parameters.shell_mapper_bits <= 12u) {
						for (uint32_t label = 0;
						     label < label_limit; ++label) {
							CHECK(verify_label(
							      &parameters,
							      &counts[parameters.ring_count],
							      label),
							      "exhaustive mapping failed");
							labels_checked++;
						}
					} else {
						uint32_t samples[] = {
							0,
							1,
							maximum / 4u,
							maximum / 2u,
							(uint32_t)(
								3u * (uint64_t)maximum / 4u),
							maximum - 1u,
							maximum,
						};

						for (size_t sample = 0;
						     sample < ARRAY_LENGTH(samples);
						     ++sample) {
							CHECK(verify_label(
							      &parameters,
							      &counts[parameters.ring_count],
							      samples[sample]),
							      "sampled mapping failed");
							labels_checked++;
						}
					}
				}
			}
		}
	}
	CHECK(geometry_variants >= 250u && unique_combinations >= 30u &&
	      labels_checked >= 10000u,
	      "shell coverage shrank to %u geometries, %u combinations, %llu labels",
	      geometry_variants, unique_combinations,
	      (unsigned long long)labels_checked);
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_mapping_parameters parameters = {
		.constellation_points = 8,
		.mapping_frame_bits = 20,
		.shell_mapper_bits = 8,
		.point_index_bits_per_2d_symbol = 0,
		.ring_count = 2,
		.expanded_shaping = false,
	};
	struct sippymodem_v34_ring_indices indices;
	struct sippymodem_v34_ring_indices saved;

	memset(&indices, 0x5a, sizeof(indices));
	saved = indices;
	CHECK(sippymodem_v34_shell_map(NULL, 0, &indices) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_shell_map(&parameters, 0, NULL) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_INVALID_ARGUMENT &&
	      sippymodem_v34_shell_map(&parameters, 256, &indices) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&indices, &saved, sizeof(indices)) == 0,
	      "invalid shell mapping changed its destination");
	parameters.ring_count++;
	CHECK(sippymodem_v34_shell_map(&parameters, 0, &indices) ==
		      SIPPYMODEM_V34_SHELL_MAPPER_INVALID_ARGUMENT &&
	      memcmp(&indices, &saved, sizeof(indices)) == 0,
	      "corrupt parameters changed shell output");

	parameters.ring_count = 2;
	memset(&indices, 0, sizeof(indices));
	CHECK(sippymodem_v34_ring_indices_are_valid(&indices, &parameters),
	      "valid inner ring indices were rejected");
	indices.index[2][1] = parameters.ring_count;
	CHECK(!sippymodem_v34_ring_indices_are_valid(&indices, &parameters) &&
	      !sippymodem_v34_ring_indices_are_valid(NULL, &parameters) &&
	      !sippymodem_v34_ring_indices_are_valid(&indices, NULL),
	      "invalid ring indices were accepted");
	CHECK(strcmp(sippymodem_v34_shell_mapper_result_name(
		     SIPPYMODEM_V34_SHELL_MAPPER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_shell_mapper_result_name(
		     SIPPYMODEM_V34_SHELL_MAPPER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_shell_mapper_result_name(
		     (enum sippymodem_v34_shell_mapper_result)2),
		     "unknown") == 0,
	      "shell mapper result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "small shell order", test_small_shell_order },
		{ "all supported parameters", test_all_supported_parameters },
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
