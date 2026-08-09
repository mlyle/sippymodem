// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_g711.h"

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

uint8_t sippymodem_g711_encode_pcmu(int16_t sample)
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

int16_t sippymodem_g711_decode_pcmu(uint8_t octet)
{
	uint8_t code = (uint8_t)~octet;
	int32_t magnitude = ((int32_t)(code & 0x0f) << 3) + G711_PCMU_BIAS;

	magnitude <<= (code & 0x70) >> 4;
	if ((code & 0x80) != 0)
		return (int16_t)(G711_PCMU_BIAS - magnitude);
	return (int16_t)(magnitude - G711_PCMU_BIAS);
}

uint8_t sippymodem_g711_encode_pcma(int16_t sample)
{
	uint32_t magnitude;
	uint8_t mask;

	/* A-law quantizes the input in units of eight.  Subtracting one before
	 * scaling keeps the negative intervals symmetric without right-shifting
	 * a negative integer, whose result is implementation-defined in C. */
	if (sample < 0) {
		magnitude = ((uint32_t)(-(int32_t)sample) - 1) >> 3;
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

int16_t sippymodem_g711_decode_pcma(uint8_t octet)
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
