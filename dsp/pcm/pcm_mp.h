// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_MP_H
#define PCM_MP_H

#include "integer_math.h"
#include "packed_bits.h"
#include "v34_precoder.h"
#include "v34_trellis.h"

/* V.34 and V.90 share the four-bit maximum rate at wire bits 24..27
 * (2400 bit/s units) and bits 29..33: two LSB-first trellis bits, nonlinear
 * encoding, expanded shaping and acknowledgment. The rate describes the
 * answer-to-call direction in V.34 and upstream in V.90; acknowledgment refers
 * to the peer MP or CP respectively. Owners validate rates/trellis/storage
 * and retain the auxiliary/reserved bit 28 and other header/CRC fields.
 * Each frame must cover at least wire bit 33; readers preserve reserved
 * field values for the record owner to reject. No pointer is retained. */
struct pcm_mp_modulation {
	uint8_t maximum_rate;
	enum v34_trellis_selection trellis;
	bool nonlinear, expanded, acknowledges;
};

static inline void pcm_mp_write_modulation(
	uint8_t *frame, struct pcm_mp_modulation modulation)
{
	packed_msb_write_lsb_field(frame, 24u, 4u, modulation.maximum_rate);
	packed_msb_write_lsb_field(frame, 29u, 2u, (uint16_t)modulation.trellis);
	packed_msb_set_bit(frame, 31u, modulation.nonlinear);
	packed_msb_set_bit(frame, 32u, modulation.expanded);
	packed_msb_set_bit(frame, 33u, modulation.acknowledges);
}

static inline struct pcm_mp_modulation pcm_mp_read_modulation(const uint8_t *frame)
{
	return (struct pcm_mp_modulation){
		.maximum_rate = (uint8_t)packed_msb_read_lsb_field(frame, 24u, 4u),
		.trellis = (enum v34_trellis_selection)
			packed_msb_read_lsb_field(frame, 29u, 2u),
		.nonlinear = packed_msb_bit(frame, 31u) != 0,
		.expanded = packed_msb_bit(frame, 32u) != 0,
		.acknowledges = packed_msb_bit(frame, 33u) != 0,
	};
}

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
