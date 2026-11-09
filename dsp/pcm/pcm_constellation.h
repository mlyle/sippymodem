// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_CONSTELLATION_H
#define PCM_CONSTELLATION_H

#include "integer_math.h"
#include "memory_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	PCM_CONSTELLATION_UCODES = 128,
	PCM_CONSTELLATION_MASK_BYTES = PCM_CONSTELLATION_UCODES / 8,
	PCM_CONSTELLATION_FRAME_INTERVALS = 6,
	PCM_CONSTELLATION_MAX_ROWS = 6,
};

/* Bit n represents companded codeword n. Keeping the set packed avoids a
 * 128-entry level table while preserving the normative Ucode ordering. */
struct pcm_constellation {
	uint8_t ucode_mask[PCM_CONSTELLATION_MASK_BYTES];
};

/* Row tables are packed bitmap planes; copies and comparisons include no
 * padding or non-membership state between consecutive masks. */
_Static_assert(sizeof(struct pcm_constellation) == PCM_CONSTELLATION_MASK_BYTES,
	       "constellation rows must contain only packed membership bits");

/* Callers supply owned storage and a validated Ucode below 128. */
static inline void pcm_constellation_add(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static inline bool pcm_constellation_contains(
	const struct pcm_constellation *constellation, uint8_t ucode)
{
	return constellation != NULL && ucode < PCM_CONSTELLATION_UCODES &&
	       (constellation->ucode_mask[ucode / 8u] &
		(uint8_t)(1u << (ucode % 8u))) != 0;
}

/* First present Ucode at or above first, or 128 when exhausted. NULL
 * denotes the implicit full alphabet used by direct-channel design. Walk
 * packed membership bytes within the 128-code range; first >= 128 is exhausted and never dereferences the mask. */
static inline unsigned pcm_constellation_next(
	const struct pcm_constellation *constellation, unsigned first)
{
	if (first >= PCM_CONSTELLATION_UCODES)
		return PCM_CONSTELLATION_UCODES;
	if (constellation == NULL)
		return first;
	unsigned byte = first / 8u;
	unsigned bits = constellation->ucode_mask[byte];

	if ((bits & (1u << (first % 8u))) != 0)
		return first;
	bits &= 0xffu << (first % 8u);
	for (;;) {
		if (bits != 0)
			return byte * 8u + (unsigned)__builtin_ctz(bits);
		if (++byte == PCM_CONSTELLATION_MASK_BYTES)
			return PCM_CONSTELLATION_UCODES;
		bits = constellation->ucode_mask[byte];
	}
}

static inline uint8_t pcm_constellation_count(
	const struct pcm_constellation *constellation)
{
	unsigned int count = 0;

	if (constellation == NULL)
		return 0;
	for (size_t index = 0; index < PCM_CONSTELLATION_MASK_BYTES; ++index)
		count += bit_count_u8(constellation->ucode_mask[index]);
	return (uint8_t)count;
}

/* Mapper labels enumerate present Ucodes from 127 down to zero. Leave the
 * output untouched when the queried Ucode is absent. */
static inline bool pcm_constellation_descending_rank(
	const struct pcm_constellation *constellation, uint8_t ucode,
	uint8_t *rank)
{
	size_t byte;
	unsigned int count;
	uint8_t higher;

	if (rank == NULL || !pcm_constellation_contains(constellation, ucode))
		return false;
	byte = ucode / 8u;
	higher = (uint8_t)(constellation->ucode_mask[byte] &
			   (uint8_t)(0xffu << ((ucode % 8u) + 1u)));
	count = bit_count_u8(higher);
	for (size_t index = byte + 1u; index < PCM_CONSTELLATION_MASK_BYTES; ++index)
		count += bit_count_u8(constellation->ucode_mask[index]);
	*rank = (uint8_t)count;
	return true;
}

/* Inverse of descending_rank: select the label-th present Ucode without
 * scanning the empty codeword space. A missing label leaves output untouched. */
static inline bool pcm_constellation_descending_select(
	const struct pcm_constellation *constellation, uint8_t label,
	uint8_t *ucode)
{
	if (constellation == NULL || ucode == NULL)
		return false;
	for (size_t byte = PCM_CONSTELLATION_MASK_BYTES; byte-- > 0;) {
		uint8_t bits = constellation->ucode_mask[byte];
		uint8_t count = bit_count_u8(bits);

		if (label >= count) {
			label = (uint8_t)(label - count);
			continue;
		}
		for (unsigned int bit = 8u; bit-- > 0;) {
			if ((bits & (uint8_t)(1u << bit)) == 0)
				continue;
			if (label == 0u) {
				*ucode = (uint8_t)(byte * 8u + bit);
				return true;
			}
			--label;
		}
	}
	return false;
}

static inline bool pcm_constellation_is_empty(
	const struct pcm_constellation *constellation)
{
	return constellation != NULL && memory_region_is_zero(
		constellation->ucode_mask, sizeof(constellation->ucode_mask));
}

/* First identical row, or count when absent. Owners bound count by six and
 * supply the queried row; packed masks contain no padding. */
static inline unsigned pcm_constellation_find(
	const struct pcm_constellation *rows, unsigned count,
	const struct pcm_constellation *value)
{
	for (unsigned row = 0; row < count; ++row)
		if (memcmp(rows + row, value, sizeof(*rows)) == 0)
			return row;
	return count;
}

/* Map each interval to its first identical row. Input masks and the six
 * output indices occupy separate storage; NULL denotes one common implicit
 * alphabet. Owners retain each interval's coding/power weights. No pointer
 * or mutable cache survives the operation. */
static inline void pcm_constellation_sources(
	const struct pcm_constellation *intervals,
	unsigned source[PCM_CONSTELLATION_FRAME_INTERVALS])
{
	for (unsigned slot = 0; slot < PCM_CONSTELLATION_FRAME_INTERVALS; ++slot)
		source[slot] = intervals == NULL ? 0 :
			pcm_constellation_find(intervals, slot, intervals + slot);
}

/* Referenced-row bitmap for a six-interval map, or zero for missing storage,
 * an invalid row count, or an out-of-range index. Every valid map references
 * at least one row, so zero is unambiguous. Check before shifting by an
 * untrusted index; sparse maps remain valid for wire framing/compaction. */
static inline uint8_t pcm_constellation_map_used(
	const uint8_t map[PCM_CONSTELLATION_FRAME_INTERVALS], uint8_t count)
{
	uint8_t used = 0;

	if (map == NULL || count == 0 || count > PCM_CONSTELLATION_MAX_ROWS)
		return 0;
	for (size_t interval = 0; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
		if (map[interval] >= count)
			return 0;
		used |= (uint8_t)(1u << map[interval]);
	}
	return used;
}

/* Pack six interval sets into the smallest ordered row table. Identical
 * intervals share one row. Input may alias rows; rejected sets change nothing. */
static inline bool pcm_constellations_pack(
	const struct pcm_constellation intervals[PCM_CONSTELLATION_FRAME_INTERVALS],
	struct pcm_constellation rows[PCM_CONSTELLATION_MAX_ROWS],
	uint8_t map[PCM_CONSTELLATION_FRAME_INTERVALS], uint8_t *count)
{
	struct pcm_constellation packed[PCM_CONSTELLATION_MAX_ROWS] = { 0 };
	uint8_t indices[PCM_CONSTELLATION_FRAME_INTERVALS], used = 0;

	if (intervals == NULL || rows == NULL || map == NULL || count == NULL)
		return false;
	for (unsigned interval = 0; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
		if (pcm_constellation_is_empty(&intervals[interval]))
			return false;
		unsigned row = pcm_constellation_find(packed, used, intervals + interval);
		if (row == used)
			packed[used++] = intervals[interval];
		indices[interval] = (uint8_t)row;
	}
	memcpy(rows, packed, sizeof(packed));
	memcpy(map, indices, sizeof(indices));
	*count = used;
	return true;
}

/* Wire records include every row through the greatest map index, even
 * when an intermediate row is unused. After framing and CRC validation,
 * compact referenced rows so all three control messages share the mapper's
 * canonical representation. Check before changing the caller's arrays. */
static inline bool pcm_constellations_compact(
	struct pcm_constellation transmit[PCM_CONSTELLATION_MAX_ROWS],
	struct pcm_constellation codec[PCM_CONSTELLATION_MAX_ROWS],
	uint8_t frame_constellation[PCM_CONSTELLATION_FRAME_INTERVALS],
	uint8_t *count, bool codec_differs)
{
	uint8_t used = 0;
	uint8_t remap[PCM_CONSTELLATION_MAX_ROWS] = { 0 };
	uint8_t compact_count = 0;

	if (transmit == NULL || codec == NULL || count == NULL)
		return false;
	used = pcm_constellation_map_used(frame_constellation, *count);
	if (used == 0)
		return false;
	for (size_t index = 0; index < *count; ++index) {
		if ((used & (1u << index)) != 0 &&
		    (pcm_constellation_is_empty(&transmit[index]) ||
		     (codec_differs && pcm_constellation_is_empty(&codec[index]))))
			return false;
	}
	for (size_t index = 0; index < *count; ++index) {
		if ((used & (1u << index)) == 0)
			continue;
		remap[index] = compact_count;
		transmit[compact_count] = transmit[index];
		if (codec_differs)
			codec[compact_count] = codec[index];
		++compact_count;
	}
	for (size_t index = 0; index < PCM_CONSTELLATION_MAX_ROWS; ++index) {
		if (index >= compact_count)
			transmit[index] = (struct pcm_constellation){ { 0 } };
		if (index >= compact_count || !codec_differs)
			codec[index] = (struct pcm_constellation){ { 0 } };
	}
	for (size_t interval = 0;
	     interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval)
		frame_constellation[interval] = remap[frame_constellation[interval]];
	*count = compact_count;
	return true;
}

/* V.90 CP and V.92 CPt/CPu use the same six-interval layout. Every declared
 * row must be referenced and nonempty; unused rows, including the whole
 * optional codec plane when absent, must be empty. */
static inline bool pcm_constellations_are_canonical(
	const struct pcm_constellation transmit[PCM_CONSTELLATION_MAX_ROWS],
	const struct pcm_constellation codec[PCM_CONSTELLATION_MAX_ROWS],
	const uint8_t frame_constellation[PCM_CONSTELLATION_FRAME_INTERVALS],
	uint8_t count, bool codec_differs)
{
	uint8_t used = 0;

	if (transmit == NULL || codec == NULL)
		return false;
	used = pcm_constellation_map_used(frame_constellation, count);
	if (used == 0 || used != (uint8_t)((1u << count) - 1u))
		return false;
	for (size_t index = 0; index < PCM_CONSTELLATION_MAX_ROWS; ++index) {
		bool active = index < count;

		if (pcm_constellation_is_empty(&transmit[index]) == active ||
		    pcm_constellation_is_empty(&codec[index]) ==
			    (active && codec_differs))
			return false;
	}
	return true;
}

#endif /* PCM_CONSTELLATION_H */
