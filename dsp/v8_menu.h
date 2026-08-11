// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_MENU_H
#define V8_MENU_H

#include "v8_framing.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v8_call_function {
	V8_CALL_DATA = 6,
};

enum v8_modulation {
	V8_MODULATION_NONE = 0,
	V8_MODULATION_V34_DUPLEX = 1u << 0,
	V8_MODULATION_V34_HALF_DUPLEX = 1u << 1,
	V8_MODULATION_V32 = 1u << 2,
	V8_MODULATION_V22 = 1u << 3,
	V8_MODULATION_V17 = 1u << 4,
	V8_MODULATION_V29_HALF_DUPLEX = 1u << 5,
	V8_MODULATION_V27TER = 1u << 6,
	V8_MODULATION_V26TER = 1u << 7,
	V8_MODULATION_V26BIS = 1u << 8,
	V8_MODULATION_V23_DUPLEX = 1u << 9,
	V8_MODULATION_V23_HALF_DUPLEX = 1u << 10,
	V8_MODULATION_V21 = 1u << 11,
	V8_MODULATION_ALL =
		(1u << 12) - 1u,
};

enum {
	V8_MAX_MODULATION_OCTETS =
		V8_MAX_INFORMATION_OCTETS - 1,
};

enum v8_menu_result {
	V8_MENU_OK,
	V8_MENU_INVALID_ARGUMENT,
	V8_MENU_BUFFER_TOO_SMALL,
	V8_MENU_MALFORMED,
	V8_MENU_UNSUPPORTED_CALL_FUNCTION,
	V8_MENU_NO_COMMON_MODULATION,
};

struct v8_menu {
	enum v8_call_function call_function;
	uint16_t modulations;
	uint8_t modulation_octets;
	bool lapm;
};

struct v8_menu_detector {
	struct v8_control_receiver framing;
	uint8_t current[V8_MAX_INFORMATION_OCTETS];
	uint8_t previous[V8_MAX_INFORMATION_OCTETS];
	struct v8_menu confirmed_menu;
	uint8_t current_count;
	uint8_t previous_count;
	bool sequence_open;
	bool current_overflow;
	bool have_previous;
	bool confirmed;
};

enum v8_menu_result v8_call_menu_init(
	struct v8_menu *menu, uint16_t modulations, bool lapm);
enum v8_menu_result v8_menu_encode(
	const struct v8_menu *menu, uint8_t *information,
	size_t capacity, size_t *information_count);
enum v8_menu_result v8_menu_decode(
	const uint8_t *information, size_t information_count,
	struct v8_menu *menu);
enum v8_menu_result v8_joint_menu_select(
	const struct v8_menu *call_menu,
	const struct v8_menu *local_menu,
	struct v8_menu *joint_menu);
enum v8_menu_result v8_menu_selected_modulation(
	const struct v8_menu *menu,
	enum v8_modulation *modulation);

enum v8_menu_result v8_menu_detector_init(
	struct v8_menu_detector *detector);
void v8_menu_detector_reset(
	struct v8_menu_detector *detector);
enum v8_menu_result v8_menu_detect_bit(
	struct v8_menu_detector *detector, uint8_t bit,
	struct v8_menu *menu, bool *confirmed);

const char *v8_menu_result_name(
	enum v8_menu_result result);

#endif
