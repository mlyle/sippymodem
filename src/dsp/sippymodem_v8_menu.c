// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v8_menu.h"

#include <string.h>

enum {
	V8_CALL_FUNCTION_TAG = 0x01,
	V8_MODULATION_TAG = 0x05,
	V8_PROTOCOL_TAG = 0x0a,
	V8_EXTENSION_TAG = 0x10,
	V8_DATA_CALL_FUNCTION =
		V8_CALL_FUNCTION_TAG | (SIPPYMODEM_V8_CALL_DATA << 5),
	V8_LAPM_PROTOCOL = V8_PROTOCOL_TAG | (1u << 5),
};

static uint8_t required_modulation_octets(uint16_t modulations)
{
	if ((modulations & (SIPPYMODEM_V8_MODULATION_V26TER |
			    SIPPYMODEM_V8_MODULATION_V26BIS |
			    SIPPYMODEM_V8_MODULATION_V23_DUPLEX |
			    SIPPYMODEM_V8_MODULATION_V23_HALF_DUPLEX |
			    SIPPYMODEM_V8_MODULATION_V21)) != 0)
		return 3;
	if ((modulations & (SIPPYMODEM_V8_MODULATION_V32 |
			    SIPPYMODEM_V8_MODULATION_V22 |
			    SIPPYMODEM_V8_MODULATION_V17 |
			    SIPPYMODEM_V8_MODULATION_V29_HALF_DUPLEX |
			    SIPPYMODEM_V8_MODULATION_V27TER)) != 0)
		return 2;
	return 1;
}

static bool menu_is_valid(const struct sippymodem_v8_menu *menu)
{
	return menu != NULL &&
	       menu->call_function == SIPPYMODEM_V8_CALL_DATA &&
	       (menu->modulations & ~SIPPYMODEM_V8_MODULATION_ALL) == 0 &&
	       menu->modulation_octets != 0 &&
	       menu->modulation_octets <=
		       SIPPYMODEM_V8_MAX_MODULATION_OCTETS &&
	       menu->modulation_octets >=
		       required_modulation_octets(menu->modulations) &&
	       1u + menu->modulation_octets + (menu->lapm ? 1u : 0u) <=
		       SIPPYMODEM_V8_MAX_INFORMATION_OCTETS;
}

enum sippymodem_v8_menu_result sippymodem_v8_call_menu_init(
	struct sippymodem_v8_menu *menu, uint16_t modulations, bool lapm)
{
	struct sippymodem_v8_menu initialized = {
		.call_function = SIPPYMODEM_V8_CALL_DATA,
		.modulations = modulations,
		.modulation_octets = required_modulation_octets(modulations),
		.lapm = lapm,
	};

	if (menu == NULL || modulations == 0 ||
	    !menu_is_valid(&initialized))
		return SIPPYMODEM_V8_MENU_INVALID_ARGUMENT;
	*menu = initialized;
	return SIPPYMODEM_V8_MENU_OK;
}

static uint8_t first_modulation_octet(uint16_t modulations)
{
	return (uint8_t)(
		V8_MODULATION_TAG |
		((modulations & SIPPYMODEM_V8_MODULATION_V34_DUPLEX) != 0 ?
			 1u << 6 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V34_HALF_DUPLEX) != 0 ?
			 1u << 7 : 0));
}

static uint8_t second_modulation_octet(uint16_t modulations)
{
	return (uint8_t)(
		V8_EXTENSION_TAG |
		((modulations & SIPPYMODEM_V8_MODULATION_V32) != 0 ? 1u : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V22) != 0 ? 1u << 1 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V17) != 0 ? 1u << 2 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V29_HALF_DUPLEX) != 0 ?
			 1u << 6 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V27TER) != 0 ?
			 1u << 7 : 0));
}

static uint8_t third_modulation_octet(uint16_t modulations)
{
	return (uint8_t)(
		V8_EXTENSION_TAG |
		((modulations & SIPPYMODEM_V8_MODULATION_V26TER) != 0 ? 1u : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V26BIS) != 0 ?
			 1u << 1 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V23_DUPLEX) != 0 ?
			 1u << 2 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V23_HALF_DUPLEX) != 0 ?
			 1u << 6 : 0) |
		((modulations & SIPPYMODEM_V8_MODULATION_V21) != 0 ?
			 1u << 7 : 0));
}

enum sippymodem_v8_menu_result sippymodem_v8_menu_encode(
	const struct sippymodem_v8_menu *menu, uint8_t *information,
	size_t capacity, size_t *information_count)
{
	uint8_t encoded[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS] = { 0 };
	size_t count;

	if (!menu_is_valid(menu) || information == NULL ||
	    information_count == NULL)
		return SIPPYMODEM_V8_MENU_INVALID_ARGUMENT;
	count = 1u + menu->modulation_octets + (menu->lapm ? 1u : 0u);
	if (capacity < count)
		return SIPPYMODEM_V8_MENU_BUFFER_TOO_SMALL;
	encoded[0] = V8_DATA_CALL_FUNCTION;
	encoded[1] = first_modulation_octet(menu->modulations);
	for (size_t index = 2; index <= menu->modulation_octets; ++index) {
		if (index == 2)
			encoded[index] = second_modulation_octet(menu->modulations);
		else if (index == 3)
			encoded[index] = third_modulation_octet(menu->modulations);
		else
			encoded[index] = V8_EXTENSION_TAG;
	}
	if (menu->lapm)
		encoded[count - 1u] = V8_LAPM_PROTOCOL;
	memcpy(information, encoded, count);
	*information_count = count;
	return SIPPYMODEM_V8_MENU_OK;
}

const char *sippymodem_v8_menu_result_name(
	enum sippymodem_v8_menu_result result)
{
	switch (result) {
	case SIPPYMODEM_V8_MENU_OK:
		return "ok";
	case SIPPYMODEM_V8_MENU_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V8_MENU_BUFFER_TOO_SMALL:
		return "buffer too small";
	}
	return "unknown";
}
