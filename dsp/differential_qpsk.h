// SPDX-License-Identifier: BSD-3-Clause

#ifndef DIFFERENTIAL_QPSK_H
#define DIFFERENTIAL_QPSK_H

#include <stdint.h>

enum {
	QPSK_QUADRANT_COUNT = 4,
};

enum differential_qpsk_result {
	DIFFERENTIAL_QPSK_OK,
	DIFFERENTIAL_QPSK_INVALID_ARGUMENT,
};

enum differential_qpsk_result
differential_qpsk_encode(
	uint8_t previous_quadrant, uint8_t dibit, uint8_t *quadrant);
enum differential_qpsk_result
differential_qpsk_decode(
	uint8_t previous_quadrant, uint8_t quadrant, uint8_t *dibit);

const char *differential_qpsk_result_name(
	enum differential_qpsk_result result);

#endif
