// SPDX-License-Identifier: BSD-3-Clause

#include "v34_shell_mapper.h"
#include "v34_shell_counts.h"

#include <limits.h>
#include <stddef.h>

_Static_assert((int)V34_MAX_RING_COUNT == (int)V34_SHELL_COUNT_MAX_RINGS,
	       "Regenerate shell counts when the ring limit changes");

enum {
	V34_MAX_2_RING_SUM = 2 * (V34_MAX_RING_COUNT - 1),
	V34_MAX_4_RING_SUM = 2 * V34_MAX_2_RING_SUM,
	V34_MAX_2_RING_SUM_COUNT = V34_MAX_2_RING_SUM + 1,
	V34_MAX_4_RING_SUM_COUNT = V34_MAX_4_RING_SUM + 1,
	V34_RECEIVE_SHELL_WORD_COUNT = 128,
};

struct shell_coefficients {
	/* g2 and g4 count ordered pairs and quartets at each ring sum. */
	uint32_t g2[V34_RECEIVE_SHELL_WORD_COUNT];
	uint32_t g4[V34_RECEIVE_SHELL_WORD_COUNT];
	uint8_t g2_count;
	uint8_t g4_count;
};

/* Each M-ring row contains prefixes 0 through 8*(M-1)+1. */
static uint64_t eight_ring_prefix(uint8_t rings, uint16_t limit)
{
	uint32_t previous = rings - 1u;
	uint32_t offset = 4u * previous * previous - 2u * previous;
	uint16_t end = (uint16_t)(8u * previous + 1u);

	if (limit > end)
		limit = end;
	return V34_EIGHT_RING_PREFIXES[offset + limit];
}

static void generate_coefficients(
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
	/* Reflect four bounded ring indices to t <= 2M-2. Their count is
	 * C(t+3,3) - 4*C(t-M+3,3): at most one index can exceed M-1 on
	 * this half. The second term is zero for t < M. Validated M <= 18
	 * bounds the largest cubic product at 37*36*35 = 46620. */
	uint32_t maximum_sum = coefficients->g4_count - 1u;
	for (uint8_t sum = 0; sum < coefficients->g4_count; ++sum) {
		uint32_t reflected = sum <= maximum_sum / 2u ?
			sum : maximum_sum - sum;
		uint32_t ways = (reflected + 1u) * (reflected + 2u) *
			(reflected + 3u);
		if (reflected >= ring_count) {
			uint32_t excess = reflected - ring_count;
			ways -= 4u * (excess + 1u) * (excess + 2u) *
				(excess + 3u);
		}
		coefficients->g4[sum] = ways / 6u;
	}
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
	uint8_t rings = (uint8_t)((coefficients->g2_count + 1u) / 2u);
	uint8_t low = 0;
	uint8_t high = (uint8_t)(2u * coefficients->g4_count - 1u);

	if ((uint64_t)shell_label >= eight_ring_prefix(rings, high))
		return false;
	while (low + 1u < high) {
		uint8_t middle = (uint8_t)(low + (high - low) / 2u);

		if (eight_ring_prefix(rings, middle) <= shell_label)
			low = middle;
		else
			high = middle;
	}
	*sum = low;
	*residual = (uint64_t)shell_label - eight_ring_prefix(rings, low);
	return true;
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

static uint32_t receive_correlation(
	const uint32_t weights[V34_RECEIVE_SHELL_WORD_COUNT],
	uint16_t count, uint16_t reverse_index)
{
	uint32_t total = 0;

	for (uint16_t forward = 0; forward < count; ++forward)
		total += weights[forward] * weights[reverse_index - forward];
	return total;
}

static int32_t receive_edge_weight(
	uint16_t limit, uint16_t start, uint16_t edge,
	uint16_t inner, uint16_t tail)
{
	if ((uint32_t)edge - start >= limit)
		return (int32_t)limit - tail;
	return inner;
}

static int32_t receive_start_weight(
	uint16_t limit, uint16_t start, uint16_t head, uint16_t lead)
{
	if (start >= limit)
		return (int32_t)limit - lead;
	return head;
}

static uint32_t receive_shell_metric(
	const struct shell_coefficients *coefficients, uint16_t sum)
{
	uint8_t rings = (uint8_t)((coefficients->g2_count + 1u) / 2u);
	uint64_t cumulative;

	if (sum >= (uint16_t)(2u * coefficients->g4_count - 1u) ||
	    sum >= V34_RECEIVE_SHELL_WORD_COUNT)
		return UINT32_MAX;
	cumulative = eight_ring_prefix(rings, sum);
	return cumulative <= INT32_MAX ? (uint32_t)cumulative : UINT32_MAX;
}

static bool recover_received_shell_word(
	const struct shell_coefficients *coefficients, uint8_t ring_count,
	const struct v34_ring_prefixes *prefixes, uint32_t *shell_word)
{
	const uint16_t *first = prefixes->prefix[0];
	const uint16_t *second = prefixes->prefix[2];
	uint16_t limit = (uint16_t)(ring_count - 1u);
	uint32_t first_pair_sum = (uint32_t)first[0] + first[1];
	uint32_t first_quartet_sum = first_pair_sum +
		prefixes->prefix[1][0] + prefixes->prefix[1][1];
	uint32_t second_pair_sum = (uint32_t)second[0] + second[1];
	uint32_t second_quartet_sum = second_pair_sum +
		prefixes->prefix[3][0] + prefixes->prefix[3][1];
	uint32_t total_sum = first_quartet_sum + second_quartet_sum;
	uint16_t first_pair;
	uint16_t first_quartet;
	uint16_t second_pair;
	uint16_t second_quartet;
	uint16_t total;
	int32_t first_pair_weight;
	int32_t first_start_weight;
	int32_t second_pair_weight;
	int32_t second_start_weight;
	uint32_t first_prefix;
	uint32_t second_prefix;
	uint32_t quartet_prefix;
	uint32_t second_rank;
	uint32_t generated;

	if (first_pair_sum >= V34_RECEIVE_SHELL_WORD_COUNT ||
	    first_quartet_sum >= V34_RECEIVE_SHELL_WORD_COUNT ||
	    second_pair_sum >= V34_RECEIVE_SHELL_WORD_COUNT ||
	    second_quartet_sum >= V34_RECEIVE_SHELL_WORD_COUNT ||
	    total_sum >= V34_RECEIVE_SHELL_WORD_COUNT)
		return false;
	first_pair = (uint16_t)first_pair_sum;
	first_quartet = (uint16_t)first_quartet_sum;
	second_pair = (uint16_t)second_pair_sum;
	second_quartet = (uint16_t)second_quartet_sum;
	total = (uint16_t)total_sum;
	first_pair_weight = receive_edge_weight(
		limit, first_pair, first_quartet,
		prefixes->prefix[1][0], prefixes->prefix[1][1]);
	first_start_weight = receive_start_weight(
		limit, first_pair, first[0], first[1]);
	second_pair_weight = receive_edge_weight(
		limit, second_pair, second_quartet,
		prefixes->prefix[3][0], prefixes->prefix[3][1]);
	second_start_weight = receive_start_weight(
		limit, second_pair, second[0], second[1]);
	first_prefix = receive_correlation(
		coefficients->g2, first_pair, first_quartet);
	second_prefix = receive_correlation(
		coefficients->g2, second_pair, second_quartet);
	quartet_prefix = receive_correlation(
		coefficients->g4, first_quartet, total);
	second_rank = coefficients->g2[second_pair] *
		(uint32_t)second_pair_weight +
		(uint32_t)second_start_weight + second_prefix;
	generated = coefficients->g4[first_quartet] * second_rank +
		first_prefix + coefficients->g2[first_pair] *
		(uint32_t)first_pair_weight +
		(uint32_t)first_start_weight + quartet_prefix +
		receive_shell_metric(coefficients, total);
	*shell_word = generated;
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
	struct shell_coefficients coefficients = { 0 };
	uint32_t label_limit;

	if (!v34_mapping_parameters_is_valid(parameters) ||
	    indices == NULL)
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	label_limit = shell_label_limit(parameters);
	if (shell_label >= label_limit)
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	/* The innermost shell contains just the all-zero tuple. */
	if (shell_label == 0) {
		*indices = (struct v34_ring_indices){ 0 };
		return V34_SHELL_MAPPER_OK;
	}
	generate_coefficients(parameters->ring_count, &coefficients);
	if (!map_shell_label(
		    &coefficients, parameters->ring_count, shell_label,
		    &generated) ||
	    !v34_ring_indices_are_valid(&generated, parameters))
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	*indices = generated;
	return V34_SHELL_MAPPER_OK;
}

enum v34_shell_mapper_result v34_shell_unmap_decision(
	const struct v34_mapping_parameters *parameters,
	const struct v34_ring_prefixes *prefixes,
	uint8_t shell_bits, uint32_t *shell_label)
{
	struct shell_coefficients coefficients = { 0 };
	uint32_t recovered;
	uint32_t label_limit;

	if (!v34_mapping_parameters_is_valid(parameters) || prefixes == NULL ||
	    shell_bits > parameters->shell_mapper_bits || shell_label == NULL)
		return V34_SHELL_MAPPER_INVALID_ARGUMENT;
	/* Every recovered word is zero modulo the empty bit field. */
	if (shell_bits == 0) {
		*shell_label = 0;
		return V34_SHELL_MAPPER_OK;
	}
	generate_coefficients(parameters->ring_count, &coefficients);
	/* A corrupted survivor can exceed the bounded receive work tables.  The
	 * receive mapping represents that span with an all-one shell word and
	 * continues decoding rather than turning channel errors into state
	 * errors. */
	if (!recover_received_shell_word(
		    &coefficients, parameters->ring_count, prefixes, &recovered))
		recovered = UINT32_MAX;
	label_limit = UINT32_C(1) << shell_bits;
	*shell_label = recovered % label_limit;
	return V34_SHELL_MAPPER_OK;
}
