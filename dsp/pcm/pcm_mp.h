// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_MP_H
#define PCM_MP_H

#include "integer_math.h"
#include "packed_bits.h"
#include "v34_precoder.h"

/* V.34 and V.90 Type 1 MP share three complex Q14 precoder coefficients.
 * Each component is a 16-bit two's-complement word sent LSB first after a
 * zero start bit: real/imaginary pairs begin at wire bits 52/69, 86/103,
 * and 120/137. Record owners validate storage and coefficient bounds;
 * these helpers preserve the header, start bits, reserved word and CRC. */
static inline void pcm_mp_write_precoder(uint8_t *frame,
	const struct v34_q14_coefficient coefficients[V34_PRECODER_TAP_COUNT])
{
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		size_t bit = 52u + tap * 34u;

		packed_msb_write_lsb_field(frame, bit, 16u, (uint16_t)coefficients[tap].real);
		packed_msb_write_lsb_field(frame, bit + 17u, 16u, (uint16_t)coefficients[tap].imaginary);
	}
}

static inline void pcm_mp_read_precoder(const uint8_t *frame,
	struct v34_q14_coefficient coefficients[V34_PRECODER_TAP_COUNT])
{
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		size_t bit = 52u + tap * 34u;

		coefficients[tap].real = wrap_s16_from_s32(
			(int32_t)packed_msb_read_lsb_field(frame, bit, 16u));
		coefficients[tap].imaginary = wrap_s16_from_s32(
			(int32_t)packed_msb_read_lsb_field(frame, bit + 17u, 16u));
	}
}

#endif /* PCM_MP_H */
