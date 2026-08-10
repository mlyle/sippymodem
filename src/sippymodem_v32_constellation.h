// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_CONSTELLATION_H
#define SIPPYMODEM_V32_CONSTELLATION_H

#include "sippymodem_qam.h"

#include <stdint.h>

enum {
	SIPPYMODEM_V32_CONSTELLATION_SYMBOLS = 16,
	SIPPYMODEM_V32_4800_SIGNAL_POINTS = 4,
};

enum sippymodem_v32_constellation_result {
	SIPPYMODEM_V32_CONSTELLATION_OK,
	SIPPYMODEM_V32_CONSTELLATION_INVALID_ARGUMENT,
};

/* Generate the normalized 4-by-4 V.32 signal-point geometry. */
enum sippymodem_v32_constellation_result
sippymodem_v32_constellation_point(
	uint8_t symbol, struct sippymodem_qam_point *point);

/* Return the four constant-energy points used at 4800 bit/s. */
enum sippymodem_v32_constellation_result
sippymodem_v32_4800_constellation_point(
	uint8_t quadrant, struct sippymodem_qam_point *point);

const char *sippymodem_v32_constellation_result_name(
	enum sippymodem_v32_constellation_result result);

#endif
