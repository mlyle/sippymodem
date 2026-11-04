// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_MODULI_H
#define PCM_MODULI_H

#include "pcm_rate.h"
#include "wide_uint.h"
#include <stddef.h>

_Static_assert(PCM_UPSTREAM_FRAME_SYMBOLS <= 16, "byte-modulus product must fit 128 bits");

/* A PCM frame has at most twelve byte-sized moduli, so their product fits
 * in 96 bits. Geometry and rate policy remain with the mapper. Check every
 * factor; a zero makes capacity zero even when zero bits are requested. Avoid
 * logarithms and overflowing a 64-bit product at upstream rates. */
static inline bool pcm_moduli_have_capacity(
	const uint8_t *moduli, size_t symbols, unsigned bits)
{
	if (moduli == NULL || symbols == 0 || symbols > PCM_UPSTREAM_FRAME_SYMBOLS ||
	    bits >= 8u * symbols)
		return false;
	/* Eight byte-sized factors fit uint64_t. Widen once for longer frames,
	 * keeping ordinary six-symbol PCM validation on native arithmetic. */
	uint64_t prefix = 1;
	size_t narrow = symbols < 8 ? symbols : 8;
	for (size_t symbol = 0; symbol < narrow; ++symbol) {
		prefix *= moduli[symbol];
	}
	if (symbols <= 8)
		return prefix >= (UINT64_C(1) << bits);
	struct wide_uint capacity = wide_from_u64(prefix);
	for (size_t symbol = narrow; symbol < symbols; ++symbol) {
		(void)wide_multiply_u32(capacity, moduli[symbol], &capacity);
	}
	return wide_compare(capacity, wide_shift_left(wide_from_u64(1), bits)) >= 0;
}

#endif
