// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_DIRECT_H
#define V92_CPD_DIRECT_H

#include "g711.h"
#include "v92_cpd.h"

#include <stdbool.h>
#include <stdint.h>

/* Build the complete initial CPd for a direct PCM-upstream mapping. The
 * generated modulus, filter, and constellation sections are owned by the
 * result; later parameter-mode updates may use the compact CPd form.
 * Rejected calls leave parameters unchanged. */
enum v92_cpd_result v92_cpd_direct_init(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, bool extend_e2u);

#endif /* V92_CPD_DIRECT_H */
