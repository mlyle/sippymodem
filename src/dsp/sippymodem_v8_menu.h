// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_MENU_H
#define SIPPYMODEM_V8_MENU_H

#include "sippymodem_v8_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sippymodem_v8_call_function {
	SIPPYMODEM_V8_CALL_DATA = 6,
};

enum sippymodem_v8_modulation {
	SIPPYMODEM_V8_MODULATION_V34_DUPLEX = 1u << 0,
	SIPPYMODEM_V8_MODULATION_V34_HALF_DUPLEX = 1u << 1,
	SIPPYMODEM_V8_MODULATION_V32 = 1u << 2,
	SIPPYMODEM_V8_MODULATION_V22 = 1u << 3,
	SIPPYMODEM_V8_MODULATION_V17 = 1u << 4,
	SIPPYMODEM_V8_MODULATION_V29_HALF_DUPLEX = 1u << 5,
	SIPPYMODEM_V8_MODULATION_V27TER = 1u << 6,
	SIPPYMODEM_V8_MODULATION_V26TER = 1u << 7,
	SIPPYMODEM_V8_MODULATION_V26BIS = 1u << 8,
	SIPPYMODEM_V8_MODULATION_V23_DUPLEX = 1u << 9,
	SIPPYMODEM_V8_MODULATION_V23_HALF_DUPLEX = 1u << 10,
	SIPPYMODEM_V8_MODULATION_V21 = 1u << 11,
	SIPPYMODEM_V8_MODULATION_ALL =
		(1u << 12) - 1u,
};

enum {
	SIPPYMODEM_V8_MAX_MODULATION_OCTETS =
		SIPPYMODEM_V8_MAX_INFORMATION_OCTETS - 1,
};

enum sippymodem_v8_menu_result {
	SIPPYMODEM_V8_MENU_OK,
	SIPPYMODEM_V8_MENU_INVALID_ARGUMENT,
	SIPPYMODEM_V8_MENU_BUFFER_TOO_SMALL,
	SIPPYMODEM_V8_MENU_MALFORMED,
	SIPPYMODEM_V8_MENU_UNSUPPORTED_CALL_FUNCTION,
};

struct sippymodem_v8_menu {
	enum sippymodem_v8_call_function call_function;
	uint16_t modulations;
	uint8_t modulation_octets;
	bool lapm;
};

enum sippymodem_v8_menu_result sippymodem_v8_call_menu_init(
	struct sippymodem_v8_menu *menu, uint16_t modulations, bool lapm);
enum sippymodem_v8_menu_result sippymodem_v8_menu_encode(
	const struct sippymodem_v8_menu *menu, uint8_t *information,
	size_t capacity, size_t *information_count);
enum sippymodem_v8_menu_result sippymodem_v8_menu_decode(
	const uint8_t *information, size_t information_count,
	struct sippymodem_v8_menu *menu);

const char *sippymodem_v8_menu_result_name(
	enum sippymodem_v8_menu_result result);

#endif
