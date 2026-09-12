// SPDX-License-Identifier: BSD-3-Clause

#include "g711.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

enum {
	G711_SEGMENT_COUNT = 8,
	G711_PCMU_BIAS = 0x84,
	G711_PCMA_POSITIVE_MASK = 0xd5,
	G711_PCMA_NEGATIVE_MASK = 0x55,
	G711_PCMU_POSITIVE_MASK = 0xff,
	G711_PCMU_NEGATIVE_MASK = 0x7f,
};

static unsigned int g711_segment(uint32_t magnitude,
				 unsigned int first_segment_end)
{
	unsigned int segment = 0;

	while (magnitude > first_segment_end &&
	       segment < G711_SEGMENT_COUNT) {
		magnitude >>= 1;
		segment++;
	}
	return segment;
}

uint8_t g711_encode_pcmu(int16_t sample)
{
	uint32_t magnitude;
	uint8_t mask;

	if (sample < 0) {
		magnitude = (uint32_t)(-(int32_t)sample) + G711_PCMU_BIAS;
		mask = G711_PCMU_NEGATIVE_MASK;
	} else {
		magnitude = (uint32_t)sample + G711_PCMU_BIAS;
		mask = G711_PCMU_POSITIVE_MASK;
	}

	unsigned int segment = g711_segment(magnitude, 0xff);
	if (segment == G711_SEGMENT_COUNT)
		return (uint8_t)(0x7f ^ mask);

	uint8_t code = (uint8_t)((segment << 4) |
				 ((magnitude >> (segment + 3)) & 0x0f));
	return (uint8_t)(code ^ mask);
}

int16_t g711_decode_pcmu(uint8_t octet)
{
	uint8_t code = (uint8_t)~octet;
	int32_t magnitude = ((int32_t)(code & 0x0f) << 3) + G711_PCMU_BIAS;

	magnitude <<= (code & 0x70) >> 4;
	if ((code & 0x80) != 0)
		return (int16_t)(G711_PCMU_BIAS - magnitude);
	return (int16_t)(magnitude - G711_PCMU_BIAS);
}

uint8_t g711_encode_pcma(int16_t sample)
{
	uint32_t magnitude;
	uint8_t mask;

	/* G.711 uses one's-complement magnitude for negative samples, then
	 * discards the low three bits of the 16-bit linear input.  Complement
	 * before scaling; subtracting eight shifts the quantizer intervals. */
	if (sample < 0) {
		magnitude = (uint32_t)(-(int32_t)sample - 1) >> 3;
		mask = G711_PCMA_NEGATIVE_MASK;
	} else {
		magnitude = (uint32_t)sample >> 3;
		mask = G711_PCMA_POSITIVE_MASK;
	}

	unsigned int segment = g711_segment(magnitude, 0x1f);
	if (segment == G711_SEGMENT_COUNT)
		return (uint8_t)(0x7f ^ mask);

	uint8_t code = (uint8_t)(segment << 4);
	if (segment < 2)
		code |= (uint8_t)((magnitude >> 1) & 0x0f);
	else
		code |= (uint8_t)((magnitude >> segment) & 0x0f);
	return (uint8_t)(code ^ mask);
}

int16_t g711_decode_pcma(uint8_t octet)
{
	uint8_t code = (uint8_t)(octet ^ G711_PCMA_NEGATIVE_MASK);
	int32_t magnitude = (int32_t)(code & 0x0f) << 4;
	unsigned int segment = (code & 0x70) >> 4;

	if (segment == 0)
		magnitude += 8;
	else if (segment == 1)
		magnitude += 0x108;
	else
		magnitude = (magnitude + 0x108) << (segment - 1);

	return (int16_t)(((code & 0x80) != 0) ? magnitude : -magnitude);
}

bool g711_quantization_interval(
	enum g711_law law, int16_t sample, struct g711_interval *interval)
{
	if (!g711_law_is_valid(law) || interval == NULL)
		return false;
	int lower, upper;

	if (law == G711_LAW_PCMU) {
		unsigned code = (uint8_t)~g711_encode_pcmu(sample) & 0x7fu;
		unsigned shift = (code >> 4u) + 3u;

		/* Invert the encoder's biased segment bin. The top bin includes
		 * saturation; both signed zero codewords reconstruct to one level. */
		lower = (int)((16u + (code & 15u)) << shift) - G711_PCMU_BIAS;
		upper = (int)((17u + (code & 15u)) << shift) - G711_PCMU_BIAS - 1;
		if (code == 0u) {
			lower = -3;
			upper = 3;
		} else if (sample < 0) {
			int positive_lower = lower;

			lower = code == 127u ? INT16_MIN : -upper;
			upper = -positive_lower;
		} else if (code == 127u) {
			upper = INT16_MAX;
		}
	} else {
		unsigned code = (g711_encode_pcma(sample) ^ G711_PCMA_POSITIVE_MASK) & 0x7fu;
		unsigned segment = code >> 4u;
		unsigned width = segment == 0u ? 16u : 1u << (segment + 3u);

		/* A-law's two inner segments share a 16-sample width. Restore
		 * the discarded low input bits before complementing a negative bin. */
		lower = (int)(((code & 15u) + (segment == 0u ? 0u : 16u)) * width);
		upper = lower + (int)width - 1;
		if (sample < 0) {
			int positive_lower = lower;

			lower = -upper - 1;
			upper = -positive_lower - 1;
		}
	}
	*interval = (struct g711_interval){ (int16_t)lower, (int16_t)upper };
	return true;
}

bool g711_ucode_octet(
	enum g711_law law, uint8_t ucode, bool positive,
	uint8_t *octet)
{
	uint8_t encoded;

	if (!g711_law_is_valid(law) || ucode > UINT8_C(127) ||
	    octet == NULL)
		return false;
	encoded = law == G711_LAW_PCMA ?
			(uint8_t)(ucode ^ UINT8_C(0xd5)) :
			(uint8_t)~ucode;
	*octet = positive ? encoded : (uint8_t)(encoded ^ UINT8_C(0x80));
	return true;
}

bool g711_ucode_sample(
	enum g711_law law, uint8_t ucode, bool positive,
	int16_t *sample)
{
	uint8_t octet;

	if (sample == NULL ||
	    !g711_ucode_octet(law, ucode, positive, &octet))
		return false;
	if (law == G711_LAW_PCMU) {
		/* Both PCMU zeros decode to zero.  -1 is the smallest input that
		 * encodes as the negative-zero character, 0x7f. */
		*sample = octet == UINT8_C(0x7f) ? -1 :
						 g711_decode_pcmu(octet);
	} else {
		*sample = g711_decode_pcma(octet);
	}
	return true;
}
