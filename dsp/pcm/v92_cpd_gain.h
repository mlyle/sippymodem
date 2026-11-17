// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_GAIN_H
#define V92_CPD_GAIN_H

#include <stdint.h>

/* CPd carries F=4G in Q0.16, so G=F/2^18. Choose the representable
 * F whose scaled mean squared B1u prefilter output is nearest unity. */
uint16_t v92_cpd_gain_for_b1u_energy(uint64_t energy);

#endif /* V92_CPD_GAIN_H */
