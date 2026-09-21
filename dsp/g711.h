// SPDX-License-Identifier: BSD-3-Clause

#ifndef G711_H
#define G711_H

#include <stdbool.h>
#include <stdint.h>

enum g711_law {
	G711_LAW_PCMU,
	G711_LAW_PCMA,
};

static inline bool g711_law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

uint8_t g711_encode_pcmu(int16_t sample);
int16_t g711_decode_pcmu(uint8_t octet);

uint8_t g711_encode_pcma(int16_t sample);
int16_t g711_decode_pcma(uint8_t octet);

/* Reconstructed linear level after encoding. law must be valid; PCMU signed
 * zeros both reconstruct as zero, while PCMA zero reconstructs as +8. */
static inline int16_t g711_quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

struct g711_interval {
	int16_t lower, upper;
};

/* Inclusive 16-bit input range with the same encode/decode result as sample.
 * This is a reconstructed-level interval, not an exact-octet interval: the
 * two PCMU zero codewords share one interval. Invalid calls leave it alone. */
bool g711_quantization_interval(
	enum g711_law law, int16_t sample, struct g711_interval *interval);

/* V.90 and V.92 identify magnitudes with a seven-bit universal code.  The
 * sample form preserves the otherwise ambiguous PCMU negative-zero codeword
 * as -1 so a later G.711 encoder reproduces the exact octet. */
bool g711_ucode_octet(
	enum g711_law law, uint8_t ucode, bool positive,
	uint8_t *octet);
bool g711_ucode_sample(
	enum g711_law law, uint8_t ucode, bool positive,
	int16_t *sample);

#endif
