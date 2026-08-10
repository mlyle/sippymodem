// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_DIFFERENTIAL_QPSK_H
#define SIPPYMODEM_DIFFERENTIAL_QPSK_H

#include <stdint.h>

enum {
	SIPPYMODEM_QPSK_QUADRANT_COUNT = 4,
};

enum sippymodem_differential_qpsk_result {
	SIPPYMODEM_DIFFERENTIAL_QPSK_OK,
	SIPPYMODEM_DIFFERENTIAL_QPSK_INVALID_ARGUMENT,
};

enum sippymodem_differential_qpsk_result
sippymodem_differential_qpsk_encode(
	uint8_t previous_quadrant, uint8_t dibit, uint8_t *quadrant);
enum sippymodem_differential_qpsk_result
sippymodem_differential_qpsk_decode(
	uint8_t previous_quadrant, uint8_t quadrant, uint8_t *dibit);

const char *sippymodem_differential_qpsk_result_name(
	enum sippymodem_differential_qpsk_result result);

#endif
