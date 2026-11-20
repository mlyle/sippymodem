// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_FRAME_BOUNDS_H
#define V92_FRAME_BOUNDS_H

#include "integer_math.h"
#include "v92_pcm_mapping.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_BOUND_DATA_NORMAL,
	V92_BOUND_DATA_REFLECTED,
	V92_BOUND_RM,
	V92_BOUND_RM_PRIME,
	V92_BOUND_MODES,
};

/* Modulus-output request digits; owners validate the nonzero modulus. */
static inline uint8_t v92_rm_digit(uint8_t modulus, unsigned symbol, bool prime)
{
	return (symbol % 4u < 2u) != prime ? (uint8_t)(modulus - 1u) : 0u;
}

/* Frame-local lower bounds in the supplied point-cost units. No input pointer
 * survives preparation. Keep the current four-symbol group's parity; relax
 * the unknown input state of later groups. Payload modes restrict only the
 * most significant digit, a superset of valid mixed-radix words. Control
 * modes require their actual digits, including their future point costs. */
struct v92_frame_bounds {
	uint64_t tail[V92_BOUND_MODES][V92_PCM_MAPPING_FRAME_SYMBOLS][2];
};

/* Combine one parity-constrained symbol with its retained suffix. */
static inline void v92_frame_bounds_store(struct v92_frame_bounds *bounds,
	unsigned mode, unsigned symbol, const uint64_t minimum[2])
{
	for (unsigned parity = 0; parity < 2; ++parity) {
		if (symbol % 4u == 3u) {
			uint64_t later = 0;
			if (symbol + 1u < V92_PCM_MAPPING_FRAME_SYMBOLS) {
				const uint64_t *next = bounds->tail[mode][symbol + 1u];
				later = next[0] < next[1] ? next[0] : next[1];
			}
			bounds->tail[mode][symbol][parity] = add_u64_saturating(minimum[parity], later);
		} else {
			const uint64_t *next = bounds->tail[mode][symbol + 1u];
			uint64_t even = add_u64_saturating(minimum[0], next[parity]);
			uint64_t odd = add_u64_saturating(minimum[1], next[parity ^ 1u]);
			bounds->tail[mode][symbol][parity] = even < odd ? even : odd;
		}
	}
}

/* Read only the prescribed digit's equivalence classes. Fourth-symbol
 * classes have separate parity offsets and twice the modulus stride. */
static inline void v92_frame_bounds_control(const uint64_t *costs,
	unsigned count, unsigned modulus, unsigned digit, bool fourth, uint64_t minimum[2])
{
	minimum[0] = minimum[1] = UINT64_MAX;
	unsigned stride = fourth ? 2u : 1u;
	for (unsigned offset = 0; offset < stride; ++offset) {
		int first = -(int)count;
		unsigned step = stride * modulus;
		first += nonnegative_remainder_s32((int)(offset + stride * digit) - first, step);
		for (int eta = first; eta < (int)count; eta += (int)step) {
			unsigned parity = (unsigned)eta & 1u;
			uint64_t cost = costs[eta + count];
			if (cost < minimum[parity]) minimum[parity] = cost;
		}
	}
}

/* Owned geometry: counts are 1..128, moduli are nonzero, maximum_last is below
 * the last modulus, and each cost row covers twice its count. group_tail is
 * the ordinary within-group parity bound from the same costs/geometry.
 * point_minimum holds each unrestricted row's even and odd minima.
 * All arguments are disjoint. No input pointer survives preparation. */
static inline void v92_frame_bounds_prepare(struct v92_frame_bounds *bounds,
	const uint64_t costs[V92_PCM_MAPPING_FRAME_SYMBOLS][2 * V92_CPD_MAX_CONSTELLATION_POINTS],
	const uint64_t group_tail[V92_PCM_MAPPING_FRAME_SYMBOLS][2],
	const uint64_t point_minimum[V92_PCM_MAPPING_FRAME_SYMBOLS][2],
	const uint8_t counts[V92_PCM_MAPPING_FRAME_SYMBOLS],
	const uint8_t moduli[V92_PCM_MAPPING_FRAME_SYMBOLS], uint8_t maximum_last)
{
	/* Only the most significant digit restricts these payload bounds.
	 * Other symbols in its group reuse the point minima already computed
	 * for ordinary group parity. */
	const unsigned last = V92_PCM_MAPPING_FRAME_SYMBOLS - 1u;
	uint64_t minimum[2][2] = { { UINT64_MAX, UINT64_MAX }, { UINT64_MAX, UINT64_MAX } };
	int first = -(int)counts[last];
	int coordinate = (first - (int)((unsigned)first & 1u)) / 2;
	unsigned digit = nonnegative_remainder_s32(coordinate, moduli[last]);
	for (int eta = first; eta < counts[last]; ++eta) {
		unsigned parity = (unsigned)eta & 1u;
		uint64_t cost = costs[last][eta + counts[last]];
		if (digit <= maximum_last && cost < minimum[0][parity])
			minimum[0][parity] = cost;
		if (moduli[last] - 1u - digit <= maximum_last && cost < minimum[1][parity])
			minimum[1][parity] = cost;
		if (parity && ++digit == moduli[last]) digit = 0;
	}
	for (unsigned sign = 0; sign < 2; ++sign) {
		v92_frame_bounds_store(bounds, sign, last, minimum[sign]);
		for (unsigned symbol = last; symbol-- > last - 3u;)
			v92_frame_bounds_store(bounds, sign, symbol, point_minimum[symbol]);
		for (unsigned group = 2; group-- > 0;) {
			const uint64_t *next = bounds->tail[sign][4u * (group + 1u)];
			uint64_t later = next[0] < next[1] ? next[0] : next[1];
			for (unsigned i = 0; i < 4; ++i)
				for (unsigned parity = 0; parity < 2; ++parity)
					bounds->tail[sign][4u * group + i][parity] =
						add_u64_saturating(group_tail[4u * group + i][parity], later);
		}
	}
	for (unsigned symbol = V92_PCM_MAPPING_FRAME_SYMBOLS; symbol-- > 0;)
		for (unsigned request = 0; request < 2; ++request) {
			uint64_t minimum[2];
			v92_frame_bounds_control(costs[symbol], counts[symbol], moduli[symbol],
				v92_rm_digit(moduli[symbol], symbol, request != 0), symbol % 4u == 3u, minimum);
			v92_frame_bounds_store(bounds, V92_BOUND_RM + request, symbol, minimum);
		}
}

/* Exclude consumed costs. parity is the outstanding group parity after this
 * symbol. A caller admits a control mode only while its complete prefix still
 * matches; ordinary payload remains a possible continuation independently. */
static inline uint64_t v92_frame_bounds_remaining(const struct v92_frame_bounds *bounds,
	unsigned symbol, unsigned parity, unsigned mode)
{
	if (symbol + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS)
		return 0;
	const uint64_t *next = bounds->tail[mode][symbol + 1u];
	return symbol % 4u == 3u ? (next[0] < next[1] ? next[0] : next[1]) : next[parity];
}

#endif
