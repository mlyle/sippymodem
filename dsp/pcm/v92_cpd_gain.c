// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpd_gain.h"

#include "integer_math.h"
#include "v92_pcm_frame.h"

/* F^2 * energy should equal the number of B1u symbols times 2^36.
 * The rounding comparison can multiply that target by up to 2^17. */
_Static_assert(V92_B1U_SYMBOLS <= (UINT64_MAX >> 53),
	"B1u gain rounding products must fit uint64_t");

uint16_t v92_cpd_gain_for_b1u_energy(uint64_t energy)
{
	const uint64_t target = (uint64_t)V92_B1U_SYMBOLS << 36;

	if (energy == 0u)
		return 0u;
	if (energy > target)
		return 1u;
	uint64_t quotient = target / energy;
	if (quotient >= (uint64_t)UINT16_MAX * UINT16_MAX)
		return UINT16_MAX;

	/* The floor root of the integer quotient is also the floor root of
	 * the exact ratio. At an exact energy tie, keep the smaller F. */
	uint32_t lower = integer_square_root_u32((uint32_t)quotient);
	uint64_t below = target - (uint64_t)lower * lower * energy;
	uint64_t step = (2u * lower + 1u) * energy;

	return (uint16_t)(lower + (2u * below > step));
}
