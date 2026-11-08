// SPDX-License-Identifier: BSD-3-Clause

#include "g711.h"
#include "integer_math.h"

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

	/* Segment is the bit width above the first segment's low eight bits. */
	unsigned int segment = bit_width_unsigned(magnitude >> 8);
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

	unsigned int segment = bit_width_unsigned(magnitude >> 5);
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

static struct g711_interval octet_interval(enum g711_law law, uint8_t octet)
{
	int lower, upper;

	if (law == G711_LAW_PCMU) {
		unsigned code = (uint8_t)~octet & 0x7fu;
		unsigned shift = (code >> 4u) + 3u;

		/* Invert the encoder's biased segment bin. The top bin includes
		 * saturation. Keep the exact signed-zero character's input range. */
		lower = (int)((16u + (code & 15u)) << shift) - G711_PCMU_BIAS;
		upper = (int)((17u + (code & 15u)) << shift) - G711_PCMU_BIAS - 1;
		if (code == 0u) {
			lower = octet & 0x80u ? 0 : -3;
			upper = octet & 0x80u ? 3 : -1;
		} else if (!(octet & 0x80u)) {
			int positive_lower = lower;

			lower = code == 127u ? INT16_MIN : -upper;
			upper = -positive_lower;
		} else if (code == 127u) {
			upper = INT16_MAX;
		}
	} else {
		unsigned code = (octet ^ G711_PCMA_POSITIVE_MASK) & 0x7fu;
		unsigned segment = code >> 4u;
		unsigned width = segment == 0u ? 16u : 1u << (segment + 3u);

		/* A-law's two inner segments share a 16-sample width. Restore
		 * the discarded low input bits before complementing a negative bin. */
		lower = (int)(((code & 15u) + (segment == 0u ? 0u : 16u)) * width);
		upper = lower + (int)width - 1;
		if (!(octet & 0x80u)) {
			int positive_lower = lower;

			lower = -upper - 1;
			upper = -positive_lower - 1;
		}
	}
	return (struct g711_interval){ (int16_t)lower, (int16_t)upper };
}

bool g711_octet_interval(
	enum g711_law law, uint8_t octet, struct g711_interval *interval)
{
	if (!g711_law_is_valid(law) || interval == NULL)
		return false;
	*interval = octet_interval(law, octet);
	return true;
}

bool g711_quantization_interval(
	enum g711_law law, int16_t sample, struct g711_interval *interval)
{
	if (!g711_law_is_valid(law) || interval == NULL)
		return false;
	*interval = octet_interval(law, g711_encode(law, sample));
	/* This API identifies reconstructed levels, so retain its union of the
	 * two PCMU zero cells even when the caller's original sign is known. */
	if (law == G711_LAW_PCMU && sample >= -3 && sample <= 3)
		*interval = (struct g711_interval){ -3, 3 };
	return true;
}

bool g711_octet_interval_lsb(enum g711_law law, uint8_t octet,
	enum g711_lsb_mode mode, struct g711_interval *interval)
{
	if (!g711_law_is_valid(law) || !g711_lsb_mode_is_valid(mode) || interval == NULL)
		return false;
	if (mode == G711_LSB_UNCHANGED)
		return g711_octet_interval(law, octet, interval);
	if (g711_apply_lsb(octet, mode) != octet)
		return false;
	struct g711_interval a = octet_interval(law, octet & 0xfeu);
	struct g711_interval b = octet_interval(law, octet | 1u);

	*interval = (struct g711_interval){
		.lower = a.lower < b.lower ? a.lower : b.lower,
		.upper = a.upper > b.upper ? a.upper : b.upper,
	};
	return true;
}

bool g711_level_interval_lsb(enum g711_law law, int16_t level,
	enum g711_lsb_mode mode, struct g711_interval *interval)
{
	struct g711_interval next;

	if (!g711_law_is_valid(law) || !g711_lsb_mode_is_valid(mode) || interval == NULL ||
	    g711_quantize(law, level) != level)
		return false;
	if (mode == G711_LSB_UNCHANGED)
		return g711_quantization_interval(law, level, interval);
	if (!g711_octet_interval_lsb(law, g711_encode(law, level), mode, &next))
		return false;
	/* For forced one, the signed zero outputs also merge their adjacent
	 * +/-8 cells. A decoded zero alone cannot recover the wire sign. */
	if (law == G711_LAW_PCMU && level == 0)
		next.lower = (int16_t)-next.upper;
	*interval = next;
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
	encoded = g711_ucode_positive_octet(law, ucode);
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
