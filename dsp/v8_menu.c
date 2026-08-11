// SPDX-License-Identifier: BSD-3-Clause

#include "v8_menu.h"

#include <string.h>

enum {
	V8_CALL_FUNCTION_TAG = 0x01,
	V8_MODULATION_TAG = 0x05,
	V8_PROTOCOL_TAG = 0x0a,
	V8_EXTENSION_TAG = 0x10,
	V8_DATA_CALL_FUNCTION =
		V8_CALL_FUNCTION_TAG | (V8_CALL_DATA << 5),
	V8_LAPM_PROTOCOL = V8_PROTOCOL_TAG | (1u << 5),
};

static uint8_t required_modulation_octets(uint16_t modulations)
{
	if ((modulations & (V8_MODULATION_V26TER |
			    V8_MODULATION_V26BIS |
			    V8_MODULATION_V23_DUPLEX |
			    V8_MODULATION_V23_HALF_DUPLEX |
			    V8_MODULATION_V21)) != 0)
		return 3;
	if ((modulations & (V8_MODULATION_V32 |
			    V8_MODULATION_V22 |
			    V8_MODULATION_V17 |
			    V8_MODULATION_V29_HALF_DUPLEX |
			    V8_MODULATION_V27TER)) != 0)
		return 2;
	return 1;
}

static bool menu_is_valid(const struct v8_menu *menu)
{
	return menu != NULL &&
	       menu->call_function == V8_CALL_DATA &&
	       (menu->modulations & ~V8_MODULATION_ALL) == 0 &&
	       menu->modulation_octets != 0 &&
	       menu->modulation_octets <=
		       V8_MAX_MODULATION_OCTETS &&
	       menu->modulation_octets >=
		       required_modulation_octets(menu->modulations) &&
	       1u + menu->modulation_octets + (menu->lapm ? 1u : 0u) <=
		       V8_MAX_INFORMATION_OCTETS;
}

enum v8_menu_result v8_call_menu_init(
	struct v8_menu *menu, uint16_t modulations, bool lapm)
{
	struct v8_menu initialized = {
		.call_function = V8_CALL_DATA,
		.modulations = modulations,
		.modulation_octets = required_modulation_octets(modulations),
		.lapm = lapm,
	};

	if (menu == NULL || modulations == 0 ||
	    !menu_is_valid(&initialized))
		return V8_MENU_INVALID_ARGUMENT;
	*menu = initialized;
	return V8_MENU_OK;
}

static uint8_t first_modulation_octet(uint16_t modulations)
{
	return (uint8_t)(
		V8_MODULATION_TAG |
		((modulations & V8_MODULATION_V34_DUPLEX) != 0 ?
			 1u << 6 : 0) |
		((modulations & V8_MODULATION_V34_HALF_DUPLEX) != 0 ?
			 1u << 7 : 0));
}

static uint8_t second_modulation_octet(uint16_t modulations)
{
	return (uint8_t)(
		V8_EXTENSION_TAG |
		((modulations & V8_MODULATION_V32) != 0 ? 1u : 0) |
		((modulations & V8_MODULATION_V22) != 0 ? 1u << 1 : 0) |
		((modulations & V8_MODULATION_V17) != 0 ? 1u << 2 : 0) |
		((modulations & V8_MODULATION_V29_HALF_DUPLEX) != 0 ?
			 1u << 6 : 0) |
		((modulations & V8_MODULATION_V27TER) != 0 ?
			 1u << 7 : 0));
}

static uint8_t third_modulation_octet(uint16_t modulations)
{
	return (uint8_t)(
		V8_EXTENSION_TAG |
		((modulations & V8_MODULATION_V26TER) != 0 ? 1u : 0) |
		((modulations & V8_MODULATION_V26BIS) != 0 ?
			 1u << 1 : 0) |
		((modulations & V8_MODULATION_V23_DUPLEX) != 0 ?
			 1u << 2 : 0) |
		((modulations & V8_MODULATION_V23_HALF_DUPLEX) != 0 ?
			 1u << 6 : 0) |
		((modulations & V8_MODULATION_V21) != 0 ?
			 1u << 7 : 0));
}

enum v8_menu_result v8_menu_encode(
	const struct v8_menu *menu, uint8_t *information,
	size_t capacity, size_t *information_count)
{
	uint8_t encoded[V8_MAX_INFORMATION_OCTETS] = { 0 };
	size_t count;

	if (!menu_is_valid(menu) || information == NULL ||
	    information_count == NULL)
		return V8_MENU_INVALID_ARGUMENT;
	count = 1u + menu->modulation_octets + (menu->lapm ? 1u : 0u);
	if (capacity < count)
		return V8_MENU_BUFFER_TOO_SMALL;
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
	return V8_MENU_OK;
}

static bool extension_is_valid(uint8_t octet)
{
	/* b3 and b5 are zero; b4 distinguishes an extension. */
	return (octet & UINT8_C(0x38)) == V8_EXTENSION_TAG;
}

static uint16_t decode_first_modulation_octet(uint8_t octet)
{
	uint16_t modulations = 0;

	if ((octet & (1u << 6)) != 0)
		modulations |= V8_MODULATION_V34_DUPLEX;
	if ((octet & (1u << 7)) != 0)
		modulations |= V8_MODULATION_V34_HALF_DUPLEX;
	return modulations;
}

static uint16_t decode_second_modulation_octet(uint8_t octet)
{
	uint16_t modulations = 0;

	if ((octet & 1u) != 0)
		modulations |= V8_MODULATION_V32;
	if ((octet & (1u << 1)) != 0)
		modulations |= V8_MODULATION_V22;
	if ((octet & (1u << 2)) != 0)
		modulations |= V8_MODULATION_V17;
	if ((octet & (1u << 6)) != 0)
		modulations |= V8_MODULATION_V29_HALF_DUPLEX;
	if ((octet & (1u << 7)) != 0)
		modulations |= V8_MODULATION_V27TER;
	return modulations;
}

static uint16_t decode_third_modulation_octet(uint8_t octet)
{
	uint16_t modulations = 0;

	if ((octet & 1u) != 0)
		modulations |= V8_MODULATION_V26TER;
	if ((octet & (1u << 1)) != 0)
		modulations |= V8_MODULATION_V26BIS;
	if ((octet & (1u << 2)) != 0)
		modulations |= V8_MODULATION_V23_DUPLEX;
	if ((octet & (1u << 6)) != 0)
		modulations |= V8_MODULATION_V23_HALF_DUPLEX;
	if ((octet & (1u << 7)) != 0)
		modulations |= V8_MODULATION_V21;
	return modulations;
}

enum v8_menu_result v8_menu_decode(
	const uint8_t *information, size_t information_count,
	struct v8_menu *menu)
{
	struct v8_menu decoded = {
		.call_function = V8_CALL_DATA,
	};
	bool modulation_seen = false;
	bool protocol_seen = false;
	size_t index = 1;

	if (information == NULL || menu == NULL)
		return V8_MENU_INVALID_ARGUMENT;
	if (information_count == 0 ||
	    information_count > V8_MAX_INFORMATION_OCTETS ||
	    (information[0] & UINT8_C(0x1f)) != V8_CALL_FUNCTION_TAG)
		return V8_MENU_MALFORMED;
	if ((information[0] >> 5) != V8_CALL_DATA)
		return V8_MENU_UNSUPPORTED_CALL_FUNCTION;
	while (index < information_count) {
		uint8_t category = information[index++];
		uint8_t tag;
		size_t extension_count = 0;

		if ((category & V8_EXTENSION_TAG) != 0)
			return V8_MENU_MALFORMED;
		tag = category & UINT8_C(0x0f);
		while (index < information_count &&
		       (information[index] & V8_EXTENSION_TAG) != 0) {
			uint8_t extension = information[index++];

			if (!extension_is_valid(extension))
				return V8_MENU_MALFORMED;
			if (tag == V8_MODULATION_TAG) {
				if (extension_count == 0)
					decoded.modulations |=
						decode_second_modulation_octet(extension);
				else if (extension_count == 1)
					decoded.modulations |=
						decode_third_modulation_octet(extension);
			}
			extension_count++;
		}
		if (tag == V8_CALL_FUNCTION_TAG)
			return V8_MENU_MALFORMED;
		if (tag == V8_MODULATION_TAG) {
			if (modulation_seen)
				return V8_MENU_MALFORMED;
			decoded.modulations |=
				decode_first_modulation_octet(category);
			decoded.modulation_octets =
				(uint8_t)(extension_count + 1u);
			modulation_seen = true;
		} else if (tag == V8_PROTOCOL_TAG) {
			uint8_t option = category >> 5;

			if (protocol_seen ||
			    (extension_count != 0 && option != 7))
				return V8_MENU_MALFORMED;
			decoded.lapm = option == 1;
			protocol_seen = true;
		}
	}
	if (!modulation_seen || !menu_is_valid(&decoded))
		return V8_MENU_MALFORMED;
	*menu = decoded;
	return V8_MENU_OK;
}

enum v8_menu_result v8_joint_menu_select(
	const struct v8_menu *call_menu,
	const struct v8_menu *local_menu,
	struct v8_menu *joint_menu)
{
	struct v8_menu selected;

	if (!menu_is_valid(call_menu) || !menu_is_valid(local_menu) ||
	    call_menu->modulations == 0 || local_menu->modulations == 0 ||
	    joint_menu == NULL)
		return V8_MENU_INVALID_ARGUMENT;
	selected = (struct v8_menu){
		.call_function = call_menu->call_function,
		.modulations = call_menu->modulations & local_menu->modulations,
		.modulation_octets = call_menu->modulation_octets,
		.lapm = call_menu->lapm && local_menu->lapm,
	};
	if (!menu_is_valid(&selected))
		return V8_MENU_INVALID_ARGUMENT;
	*joint_menu = selected;
	return V8_MENU_OK;
}

enum v8_menu_result v8_menu_selected_modulation(
	const struct v8_menu *menu,
	enum v8_modulation *modulation)
{
	uint16_t selected = V8_MODULATION_V34_DUPLEX;

	if (!menu_is_valid(menu) || modulation == NULL)
		return V8_MENU_INVALID_ARGUMENT;
	if (menu->modulations == 0)
		return V8_MENU_NO_COMMON_MODULATION;
	while ((menu->modulations & selected) == 0)
		selected <<= 1;
	*modulation = (enum v8_modulation)selected;
	return V8_MENU_OK;
}

static bool menu_detector_is_valid(
	const struct v8_menu_detector *detector)
{
	struct v8_menu previous;

	if (detector == NULL ||
	    detector->current_count > V8_MAX_INFORMATION_OCTETS ||
	    detector->previous_count > V8_MAX_INFORMATION_OCTETS ||
	    (!detector->sequence_open &&
	     (detector->current_count != 0 || detector->current_overflow)) ||
	    (detector->current_overflow &&
	     detector->current_count != V8_MAX_INFORMATION_OCTETS) ||
	    detector->have_previous != (detector->previous_count != 0) ||
	    (detector->have_previous && !detector->sequence_open))
		return false;
	if (detector->have_previous &&
	    v8_menu_decode(
		    detector->previous, detector->previous_count, &previous) !=
		    V8_MENU_OK)
		return false;
	if (detector->confirmed)
		return detector->have_previous && detector->current_count == 0 &&
		       !detector->current_overflow &&
		       detector->confirmed_menu.call_function ==
			       previous.call_function &&
		       detector->confirmed_menu.modulations ==
			       previous.modulations &&
		       detector->confirmed_menu.modulation_octets ==
			       previous.modulation_octets &&
		       detector->confirmed_menu.lapm == previous.lapm;
	return detector->confirmed_menu.call_function == 0 &&
	       detector->confirmed_menu.modulations == 0 &&
	       detector->confirmed_menu.modulation_octets == 0 &&
	       !detector->confirmed_menu.lapm;
}

enum v8_menu_result v8_menu_detector_init(
	struct v8_menu_detector *detector)
{
	struct v8_menu_detector initialized = { 0 };

	if (detector == NULL ||
	    v8_control_receiver_init(&initialized.framing) !=
		    V8_FRAMING_OK ||
	    !menu_detector_is_valid(&initialized))
		return V8_MENU_INVALID_ARGUMENT;
	*detector = initialized;
	return V8_MENU_OK;
}

void v8_menu_detector_reset(
	struct v8_menu_detector *detector)
{
	if (detector != NULL)
		(void)v8_menu_detector_init(detector);
}

static void reject_current_sequence(
	struct v8_menu_detector *detector)
{
	detector->previous_count = 0;
	detector->have_previous = false;
}

static void finish_current_sequence(
	struct v8_menu_detector *detector)
{
	struct v8_menu decoded;

	if (detector->current_overflow || detector->current_count == 0 ||
	    v8_menu_decode(
		    detector->current, detector->current_count, &decoded) !=
		    V8_MENU_OK) {
		reject_current_sequence(detector);
		return;
	}
	if (detector->have_previous &&
	    detector->previous_count == detector->current_count &&
	    memcmp(detector->previous, detector->current,
		   detector->current_count) == 0) {
		detector->confirmed_menu = decoded;
		detector->confirmed = true;
		return;
	}
	memcpy(detector->previous, detector->current, detector->current_count);
	detector->previous_count = detector->current_count;
	detector->have_previous = true;
}

static void observe_control_event(
	struct v8_menu_detector *detector,
	const struct v8_control_event *event)
{
	if (event->type == V8_CONTROL_SEQUENCE_START) {
		if (detector->sequence_open)
			finish_current_sequence(detector);
		detector->current_count = 0;
		detector->current_overflow = false;
		detector->sequence_open = true;
	} else if (event->type == V8_CONTROL_INFORMATION_OCTET &&
		   detector->sequence_open) {
		if (detector->current_count <
		    V8_MAX_INFORMATION_OCTETS) {
			detector->current[detector->current_count++] = event->octet;
		} else {
			detector->current_overflow = true;
		}
	}
}

enum v8_menu_result v8_menu_detect_bit(
	struct v8_menu_detector *detector, uint8_t bit,
	struct v8_menu *menu, bool *confirmed)
{
	struct v8_menu_detector next;
	struct v8_control_event event;

	if (!menu_detector_is_valid(detector) || bit > 1 || menu == NULL ||
	    confirmed == NULL)
		return V8_MENU_INVALID_ARGUMENT;
	if (detector->confirmed) {
		*menu = detector->confirmed_menu;
		*confirmed = true;
		return V8_MENU_OK;
	}
	next = *detector;
	if (v8_control_receive_bit(
		    &next.framing, bit, &event) != V8_FRAMING_OK)
		return V8_MENU_INVALID_ARGUMENT;
	observe_control_event(&next, &event);
	if (!menu_detector_is_valid(&next))
		return V8_MENU_INVALID_ARGUMENT;
	*detector = next;
	*confirmed = next.confirmed;
	if (next.confirmed)
		*menu = next.confirmed_menu;
	return V8_MENU_OK;
}

const char *v8_menu_result_name(
	enum v8_menu_result result)
{
	switch (result) {
	case V8_MENU_OK:
		return "ok";
	case V8_MENU_INVALID_ARGUMENT:
		return "invalid argument";
	case V8_MENU_BUFFER_TOO_SMALL:
		return "buffer too small";
	case V8_MENU_MALFORMED:
		return "malformed menu";
	case V8_MENU_UNSUPPORTED_CALL_FUNCTION:
		return "unsupported call function";
	case V8_MENU_NO_COMMON_MODULATION:
		return "no common modulation";
	}
	return "unknown";
}
