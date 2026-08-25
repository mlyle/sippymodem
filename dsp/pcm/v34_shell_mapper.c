// SPDX-License-Identifier: BSD-3-Clause

#include "v34_shell_mapper.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_MAX_2_RING_SUM = 2 * (V34_MAX_RING_COUNT - 1),
	V34_MAX_4_RING_SUM = 2 * V34_MAX_2_RING_SUM,
	V34_MAX_2_RING_SUM_COUNT = V34_MAX_2_RING_SUM + 1,
	V34_MAX_4_RING_SUM_COUNT = V34_MAX_4_RING_SUM + 1,
};

struct shell_coefficients {
	/* g2 and g4 count ordered pairs and quartets at each ring sum. */
	uint32_t g2[V34_MAX_2_RING_SUM_COUNT];
	uint32_t g4[V34_MAX_4_RING_SUM_COUNT];
	uint8_t g2_count;
	uint8_t g4_count;
};

static uint64_t convolution_at(
	const uint32_t *weights, uint8_t count, uint8_t sum)
{
	uint64_t total = 0;

	for (uint8_t left = 0; left < count && left <= sum; ++left) {
		uint8_t right = (uint8_t)(sum - left);

		if (right < count)
			total += (uint64_t)weights[left] * weights[right];
	}
	return total;
}

static bool generate_coefficients(
	uint8_t ring_count, struct shell_coefficients *coefficients)
{
	coefficients->g2_count = (uint8_t)(2u * ring_count - 1u);
	coefficients->g4_count =
		(uint8_t)(2u * coefficients->g2_count - 1u);
	for (uint8_t sum = 0; sum < coefficients->g2_count; ++sum) {
		coefficients->g2[sum] = sum < ring_count ?
			(uint32_t)sum + 1u :
			(uint32_t)(2u * ring_count - 1u - sum);
	}
	for (uint8_t sum = 0; sum < coefficients->g4_count; ++sum) {
		uint64_t count = convolution_at(
			coefficients->g2, coefficients->g2_count, sum);

		if (count > UINT32_MAX)
			return false;
		coefficients->g4[sum] = (uint32_t)count;
	}
	return true;
}

static bool select_partition(
	const uint32_t *weights, uint8_t count, uint8_t total,
	uint64_t rank, uint8_t *left_total, uint64_t *residual)
{
	for (uint8_t left = 0; left < count && left <= total; ++left) {
		uint8_t right = (uint8_t)(total - left);
		uint64_t block;

		if (right >= count)
			continue;
		block = (uint64_t)weights[left] * weights[right];
		if (rank < block) {
			*left_total = left;
			*residual = rank;
			return true;
		}
		rank -= block;
	}
	return false;
}

static bool select_eight_ring_sum(
	const struct shell_coefficients *coefficients, uint32_t shell_label,
	uint8_t *sum, uint64_t *residual)
{
	uint64_t cumulative = 0;
	uint8_t sum_count = (uint8_t)(2u * coefficients->g4_count - 1u);

	for (uint8_t candidate = 0; candidate < sum_count; ++candidate) {
		uint64_t count = convolution_at(
			coefficients->g4, coefficients->g4_count, candidate);

		if ((uint64_t)shell_label < cumulative + count) {
			*sum = candidate;
			*residual = (uint64_t)shell_label - cumulative;
			return true;
		}
		cumulative += count;
	}
	return false;
}

static void split_ring_pair(
	uint8_t ring_count, uint8_t sum, uint64_t selector,
	uint8_t pair[V34_RING_INDICES_PER_4D_INTERVAL])
{
	if (sum < ring_count) {
		pair[0] = (uint8_t)selector;
		pair[1] = (uint8_t)(sum - pair[0]);
	} else {
		pair[1] = (uint8_t)(ring_count - 1u - selector);
		pair[0] = (uint8_t)(sum - pair[1]);
	}
}

static uint64_t partition_prefix(
	const uint32_t *weights, uint8_t count, uint8_t total,
	uint8_t selected_left)
{
	uint64_t prefix = 0;

	for (uint8_t left = 0; left < selected_left && left < count &&
	     left <= total; ++left) {
		uint8_t right = (uint8_t)(total - left);

		if (right < count)
			prefix += (uint64_t)weights[left] * weights[right];
	}
	return prefix;
}

static uint8_t ring_pair_selector(
	uint8_t ring_count, uint8_t sum,
	const uint8_t pair[V34_RING_INDICES_PER_4D_INTERVAL])
{
	return sum < ring_count ? pair[0] :
		(uint8_t)(ring_count - 1u - pair[1]);
}

static bool rank_ring_quartet(
	const struct shell_coefficients *coefficients, uint8_t ring_count,
	const struct v34_ring_indices *indices,
	uint8_t first_interval, uint8_t *sum, uint64_t *rank)
{
	const uint8_t *first_pair = indices->index[first_interval];
	const uint8_t *second_pair = indices->index[first_interval + 1u];
	uint8_t first_sum = (uint8_t)(first_pair[0] + first_pair[1]);
	uint8_t second_sum = (uint8_t)(second_pair[0] + second_pair[1]);
	uint8_t first_selector = ring_pair_selector(
		ring_count, first_sum, first_pair);
	uint8_t second_selector = ring_pair_selector(
		ring_count, second_sum, second_pair);
	uint8_t quartet_sum = (uint8_t)(first_sum + second_sum);

	if (first_sum >= coefficients->g2_count ||
	    second_sum >= coefficients->g2_count ||
	    quartet_sum >= coefficients->g4_count ||
	    first_selector >= coefficients->g2[first_sum] ||
	    second_selector >= coefficients->g2[second_sum])
		return false;
	*sum = quartet_sum;
	*rank = partition_prefix(
		coefficients->g2, coefficients->g2_count, quartet_sum,
		first_sum) + first_selector +
		(uint64_t)coefficients->g2[first_sum] * second_selector;
	return *rank < coefficients->g4[quartet_sum];
}

static bool recover_shell_label(
	const struct shell_coefficients *coefficients, uint8_t ring_count,
	const struct v34_ring_indices *indices, uint64_t *shell_label)
{
	uint8_t first_four_sum;
	uint8_t second_four_sum;
	uint8_t eight_ring_sum;
	uint64_t first_four_rank;
	uint64_t second_four_rank;
	uint64_t rank;

	if (!rank_ring_quartet(
		    coefficients, ring_count, indices, 0, &first_four_sum,
		    &first_four_rank) ||
	    !rank_ring_quartet(
		    coefficients, ring_count, indices, 2, &second_four_sum,
		    &second_four_rank))
		return false;
	eight_ring_sum = (uint8_t)(first_four_sum + second_four_sum);
	rank = partition_prefix(
		coefficients->g4, coefficients->g4_count, eight_ring_sum,
		first_four_sum) + first_four_rank +
		(uint64_t)coefficients->g4[first_four_sum] * second_four_rank;
	for (uint8_t sum = 0; sum < eight_ring_sum; ++sum)
		rank += convolution_at(
			coefficients->g4, coefficients->g4_count, sum);
	*shell_label = rank;
	return true;
}

static bool map_shell_label(
	const struct shell_coefficients *coefficients, uint8_t ring_count,
	uint32_t shell_label, struct v34_ring_indices *indices)
{
	uint64_t eight_ring_rank;
	uint64_t partitioned_rank;
	uint64_t first_four_rank;
	uint64_t second_four_rank;
	uint64_t first_pair_rank;
	uint64_t second_pair_rank;
	uint8_t eight_ring_sum;
	uint8_t first_four_sum;
	uint8_t first_pair_sum;
	uint8_t third_pair_sum;

	if (!select_eight_ring_sum(
		    coefficients, shell_label, &eight_ring_sum,
		    &eight_ring_rank) ||
	    !select_partition(
		    coefficients->g4, coefficients->g4_count,
		    eight_ring_sum, eight_ring_rank, &first_four_sum,
		    &partitioned_rank))
		return false;
	first_four_rank =
		partitioned_rank % coefficients->g4[first_four_sum];
	second_four_rank =
		partitioned_rank / coefficients->g4[first_four_sum];

	if (!select_partition(
		    coefficients->g2, coefficients->g2_count,
		    first_four_sum, first_four_rank, &first_pair_sum,
		    &first_pair_rank) ||
	    !select_partition(
		    coefficients->g2, coefficients->g2_count,
		    (uint8_t)(eight_ring_sum - first_four_sum),
		    second_four_rank, &third_pair_sum,
		    &second_pair_rank))
		return false;

	split_ring_pair(
		ring_count, first_pair_sum,
		first_pair_rank % coefficients->g2[first_pair_sum],
		indices->index[0]);
	split_ring_pair(
		ring_count, (uint8_t)(first_four_sum - first_pair_sum),
		first_pair_rank / coefficients->g2[first_pair_sum],
		indices->index[1]);
	split_ring_pair(
		ring_count, third_pair_sum,
		second_pair_rank % coefficients->g2[third_pair_sum],
		indices->index[2]);
	split_ring_pair(
		ring_count,
		(uint8_t)(eight_ring_sum - first_four_sum - third_pair_sum),
		second_pair_rank / coefficients->g2[third_pair_sum],
		indices->index[3]);
	return true;
}

bool v34_ring_indices_are_valid(
	const struct v34_ring_indices *indices,
	const struct v34_mapping_parameters *parameters)
{
	if (indices == NULL ||
	    !v34_mapping_parameters_is_valid(parameters))
		return false;
	for (uint8_t interval = 0;
	     interval < V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		for (uint8_t symbol = 0;
		     symbol < V34_RING_INDICES_PER_4D_INTERVAL;
		     ++symbol) {
			if (indices->index[interval][symbol] >=
			    parameters->ring_count)
				return false;
		}
	}
	return true;
}

static uint32_t shell_label_limit(
	const struct v34_mapping_parameters *parameters)
{
	return parameters->shell_mapper_bits == 0 ? 1u :
		UINT32_C(1) << parameters->shell_mapper_bits;
}

enum v34_shell_mapper_result v34_shell_map(
	const struct v34_mapping_parameters *parameters,
	uint32_t shell_label, struct v34_ring_indices *indices)
{
	struct v34_ring_indices generated = { 0 };
	struct shell_coefficients coefficients;
	uint32_t label_limit;

	if (!v34_mapping_parameters_is_valid(parameters) ||
	    indices == NULL)
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	label_limit = shell_label_limit(parameters);
	if (shell_label >= label_limit ||
	    !generate_coefficients(parameters->ring_count, &coefficients) ||
	    !map_shell_label(
		    &coefficients, parameters->ring_count, shell_label,
		    &generated) ||
	    !v34_ring_indices_are_valid(&generated, parameters))
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	*indices = generated;
	return V34_SHELL_MAPPER_OK;
}

enum v34_shell_mapper_result v34_shell_unmap(
	const struct v34_mapping_parameters *parameters,
	const struct v34_ring_indices *indices,
	uint32_t *shell_label)
{
	struct shell_coefficients coefficients;
	uint64_t recovered;

	if (!v34_ring_indices_are_valid(indices, parameters) ||
	    shell_label == NULL ||
	    !generate_coefficients(parameters->ring_count, &coefficients) ||
	    !recover_shell_label(
		    &coefficients, parameters->ring_count, indices, &recovered) ||
	    recovered >= shell_label_limit(parameters))
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	*shell_label = (uint32_t)recovered;
	return V34_SHELL_MAPPER_OK;
}

enum v34_shell_mapper_result v34_shell_unmap_decision(
	const struct v34_mapping_parameters *parameters,
	const struct v34_ring_indices *indices,
	uint8_t shell_bits, uint32_t *shell_label)
{
	struct shell_coefficients coefficients;
	uint64_t recovered;
	uint32_t label_limit;

	if (!v34_ring_indices_are_valid(indices, parameters) ||
	    shell_bits > parameters->shell_mapper_bits || shell_label == NULL ||
	    !generate_coefficients(parameters->ring_count, &coefficients) ||
	    !recover_shell_label(
		    &coefficients, parameters->ring_count, indices, &recovered))
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	label_limit = shell_bits == 0 ? 1u : UINT32_C(1) << shell_bits;
	*shell_label = (uint32_t)(recovered % label_limit);
	return V34_SHELL_MAPPER_OK;
}

const char *v34_shell_mapper_result_name(
	enum v34_shell_mapper_result result)
{
	switch (result) {
	case V34_SHELL_MAPPER_OK:
		return "ok";
	case V34_SHELL_MAPPER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
