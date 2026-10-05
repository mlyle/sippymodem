// SPDX-License-Identifier: BSD-3-Clause

#include "v8_menu.h"

#include <string.h>

enum {
	V8_CALL_FUNCTION_TAG = 0x01,
	V8_MODULATION_TAG = 0x05,
	V8_PCM_TAG = 0x07,
	V8_PROTOCOL_TAG = 0x0a,
	V8_PSTN_ACCESS_TAG = 0x0d,
	V8_EXTENSION_TAG = 0x10,
	V8_DATA_CALL_FUNCTION =
		V8_CALL_FUNCTION_TAG | (V8_CALL_DATA << 5),
	V8_LAPM_PROTOCOL = V8_PROTOCOL_TAG | (1u << 5),
};

/* V.8 (11/2000), Table 4: modn0, modn1 and modn2, indexed by wire bit.
 * Category/extension tags and modn0's PCM indicator are not modulations.
 * Later extension octets have no defined modulation bits (clause 6). */
static const uint16_t modulation_bits[3][8] = {
	{ [6] = V8_MODULATION_V34_DUPLEX,
	  [7] = V8_MODULATION_V34_HALF_DUPLEX },
	{ [0] = V8_MODULATION_V32,
	  [1] = V8_MODULATION_V22,
	  [2] = V8_MODULATION_V17,
	  [6] = V8_MODULATION_V29_HALF_DUPLEX,
	  [7] = V8_MODULATION_V27TER },
	{ [0] = V8_MODULATION_V26TER,
	  [1] = V8_MODULATION_V26BIS,
	  [2] = V8_MODULATION_V23_DUPLEX,
	  [6] = V8_MODULATION_V23_HALF_DUPLEX,
	  [7] = V8_MODULATION_V21 },
};

static uint8_t required_modulation_octets(uint16_t modulations)
{
	for (size_t octet = sizeof(modulation_bits) / sizeof(modulation_bits[0]);
	     octet > 0; --octet)
		for (unsigned bit = 0; bit < 8; ++bit)
			if ((modulations & modulation_bits[octet - 1u][bit]) != 0)
				return (uint8_t)octet;
	return 1;
}

static bool menu_is_valid(const struct v8_menu *menu)
{
	size_t category_count;

	if (menu == NULL)
		return false;
	category_count = 1u + menu->modulation_octets +
			 (menu->lapm ? 1u : 0u) +
			 (menu->pcm_present ? 1u : 0u) +
			 (menu->pstn_access_present ? 1u : 0u);
	return menu->call_function == V8_CALL_DATA &&
	       (menu->modulations & ~V8_MODULATION_ALL) == 0 &&
	       (menu->pcm_availability & ~V8_PCM_ALL) == 0 &&
	       (menu->pstn_access & ~V8_PSTN_ALL) == 0 &&
	       (menu->pcm_present || menu->pcm_availability == 0) &&
	       (menu->pstn_access_present || menu->pstn_access == 0) &&
	       (!menu->pcm_present || menu->pstn_access_present) &&
	       (!menu->pcm_present ||
		(menu->pcm_availability &
		 (V8_PCM_ANALOG | V8_PCM_DIGITAL | V8_PCM_V91)) == 0 ||
		(menu->modulations & V8_MODULATION_V34_DUPLEX) != 0) &&
	       menu->modulation_octets != 0 &&
	       menu->modulation_octets <=
		       V8_MAX_MODULATION_OCTETS &&
	       menu->modulation_octets >=
		       required_modulation_octets(menu->modulations) &&
	       category_count <= V8_MAX_INFORMATION_OCTETS;
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

enum v8_menu_result v8_pcm_call_menu_init(
	struct v8_menu *menu, uint8_t pcm_availability,
	uint8_t pstn_access, bool lapm)
{
	struct v8_menu initialized = {
		.call_function = V8_CALL_DATA,
		.modulations = V8_MODULATION_V34_DUPLEX,
		.modulation_octets = 1,
		.pcm_availability = pcm_availability,
		.pstn_access = pstn_access,
		.pcm_present = true,
		.pstn_access_present = true,
		.lapm = lapm,
	};

	if (menu == NULL ||
	    (pcm_availability & (V8_PCM_ANALOG | V8_PCM_DIGITAL | V8_PCM_V91)) == 0 ||
	    !menu_is_valid(&initialized))
		return V8_MENU_INVALID_ARGUMENT;
	*menu = initialized;
	return V8_MENU_OK;
}

static uint8_t encode_modulation_octet(uint16_t modulations, size_t index)
{
	uint8_t octet = index == 0 ? V8_MODULATION_TAG : V8_EXTENSION_TAG;

	if (index < sizeof(modulation_bits) / sizeof(modulation_bits[0]))
		for (unsigned bit = 0; bit < 8; ++bit)
			if ((modulations & modulation_bits[index][bit]) != 0)
				octet |= (uint8_t)(1u << bit);
	return octet;
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
	count = 1u + menu->modulation_octets + (menu->lapm ? 1u : 0u) +
		(menu->pstn_access_present ? 1u : 0u) +
		(menu->pcm_present ? 1u : 0u);
	if (capacity < count)
		return V8_MENU_BUFFER_TOO_SMALL;
	encoded[0] = V8_DATA_CALL_FUNCTION;
	for (size_t index = 0; index < menu->modulation_octets; ++index)
		encoded[index + 1u] = encode_modulation_octet(menu->modulations, index);
	if (menu->pcm_present)
		encoded[1] |= 1u << 5;
	{
		size_t index = 1u + menu->modulation_octets;

		if (menu->lapm)
			encoded[index++] = V8_LAPM_PROTOCOL;
		if (menu->pstn_access_present)
			encoded[index++] = (uint8_t)(
				V8_PSTN_ACCESS_TAG | menu->pstn_access << 5);
		if (menu->pcm_present)
			encoded[index] = (uint8_t)(
				V8_PCM_TAG | menu->pcm_availability << 5);
	}
	memcpy(information, encoded, count);
	*information_count = count;
	return V8_MENU_OK;
}

static bool extension_is_valid(uint8_t octet)
{
	/* b3 and b5 are zero; b4 distinguishes an extension. */
	return (octet & UINT8_C(0x38)) == V8_EXTENSION_TAG;
}

static uint16_t decode_modulation_octet(uint8_t octet, size_t index)
{
	uint16_t modulations = 0;

	if (index < sizeof(modulation_bits) / sizeof(modulation_bits[0]))
		for (unsigned bit = 0; bit < 8; ++bit)
			if ((octet & (1u << bit)) != 0)
				modulations |= modulation_bits[index][bit];
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
	bool pcm_indicator = false;
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
			if (tag == V8_MODULATION_TAG)
				decoded.modulations |= decode_modulation_octet(
					extension, extension_count + 1u);
			extension_count++;
		}
		if (tag == V8_CALL_FUNCTION_TAG)
			return V8_MENU_MALFORMED;
		if (tag == V8_MODULATION_TAG) {
			if (modulation_seen)
				return V8_MENU_MALFORMED;
			decoded.modulations |=
				decode_modulation_octet(category, 0);
			pcm_indicator = (category & (1u << 5)) != 0;
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
		} else if (tag == V8_PCM_TAG) {
			if (decoded.pcm_present)
				return V8_MENU_MALFORMED;
			decoded.pcm_availability = category >> 5;
			decoded.pcm_present = true;
		} else if (tag == V8_PSTN_ACCESS_TAG) {
			uint8_t access = category >> 5;

			/* Some deployed peers repeat the unchanged access category.
			 * It carries no new information; conflicting repetitions are
			 * still malformed. */
			if (decoded.pstn_access_present &&
			    decoded.pstn_access != access)
				return V8_MENU_MALFORMED;
			decoded.pstn_access = access;
			decoded.pstn_access_present = true;
		}
	}
	/* Some peers clear modn0's PCM indicator but retain a PCM category,
	 * including stale role bits. Honor the refusal: the extra category must
	 * never authorize PCM. Structural checks above still reject conflicting
	 * or duplicated categories; indicated PCM offers retain normal checks. */
	if (!pcm_indicator) {
		decoded.pcm_present = false;
		decoded.pcm_availability = 0;
	}
	if (!modulation_seen || pcm_indicator != decoded.pcm_present ||
	    !menu_is_valid(&decoded))
		return V8_MENU_MALFORMED;
	*menu = decoded;
	return V8_MENU_OK;
}

enum v8_menu_result v8_joint_menu_select(
	const struct v8_menu *call_menu,
	const struct v8_menu *answer_menu,
	struct v8_menu *joint_menu)
{
	struct v8_menu selected;
	struct v8_pcm_roles roles;

	if (joint_menu == NULL ||
	    v8_pcm_select_roles(call_menu, answer_menu, &roles) != V8_MENU_OK ||
	    call_menu->modulations == 0 || answer_menu->modulations == 0)
		return V8_MENU_INVALID_ARGUMENT;
	/* Resolve PCM availability before constructing JM, including when only
	 * one endpoint offers PCM. V.8 section 7.4 permits the PCM category in
	 * JM only when it was present in CM. Advertise the selected answerer
	 * role so the peer need not resolve an ambiguous dual-role JM. */
	selected = (struct v8_menu){
		.call_function = call_menu->call_function,
		.modulations = call_menu->modulations & answer_menu->modulations,
		.modulation_octets = call_menu->modulation_octets,
		.pcm_availability = roles.caller == V8_PCM_ROLE_DIGITAL &&
			roles.answerer == V8_PCM_ROLE_DIGITAL ? V8_PCM_V91 :
			roles.answerer == V8_PCM_ROLE_DIGITAL ? V8_PCM_DIGITAL :
			roles.answerer == V8_PCM_ROLE_ANALOG ? V8_PCM_ANALOG : 0,
		.pstn_access =
			(call_menu->pstn_access & V8_PSTN_CALL_CELLULAR) |
			(answer_menu->pstn_access &
			 (V8_PSTN_ANSWER_CELLULAR | V8_PSTN_DIGITAL)),
		.pcm_present = roles.caller != V8_PCM_ROLE_NONE,
		.pstn_access_present =
			answer_menu->pstn_access_present ||
			(call_menu->pstn_access_present &&
			 (call_menu->pstn_access & V8_PSTN_CALL_CELLULAR) != 0),
		.lapm = call_menu->lapm && answer_menu->lapm,
	};
	if (!menu_is_valid(&selected))
		return V8_MENU_INVALID_ARGUMENT;
	*joint_menu = selected;
	return V8_MENU_OK;
}

enum v8_menu_result v8_pcm_select_roles(
	const struct v8_menu *call_menu,
	const struct v8_menu *joint_menu,
	struct v8_pcm_roles *roles)
{
	struct v8_pcm_roles selected = { 0 };
	bool answer_can_be_digital;
	bool caller_can_be_digital;

	if (!menu_is_valid(call_menu) || !menu_is_valid(joint_menu) ||
	    roles == NULL)
		return V8_MENU_INVALID_ARGUMENT;
	if (!call_menu->pcm_present || !joint_menu->pcm_present) {
		*roles = selected;
		return V8_MENU_OK;
	}
	answer_can_be_digital =
		(call_menu->pcm_availability & V8_PCM_ANALOG) != 0 &&
		(joint_menu->pcm_availability & V8_PCM_DIGITAL) != 0 &&
		(joint_menu->pstn_access & V8_PSTN_DIGITAL) != 0;
	caller_can_be_digital =
		(call_menu->pcm_availability & V8_PCM_DIGITAL) != 0 &&
		(call_menu->pstn_access & V8_PSTN_DIGITAL) != 0 &&
		(joint_menu->pcm_availability & V8_PCM_ANALOG) != 0;
	if ((call_menu->pcm_availability & joint_menu->pcm_availability & V8_PCM_V91) &&
	    (call_menu->pstn_access & joint_menu->pstn_access & V8_PSTN_DIGITAL)) {
		selected.caller = selected.answerer = V8_PCM_ROLE_DIGITAL;
	} else if (answer_can_be_digital) {
		selected.caller = V8_PCM_ROLE_ANALOG;
		selected.answerer = V8_PCM_ROLE_DIGITAL;
	} else if (caller_can_be_digital) {
		selected.caller = V8_PCM_ROLE_DIGITAL;
		selected.answerer = V8_PCM_ROLE_ANALOG;
	}
	*roles = selected;
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
		       detector->confirmed_menu.pcm_availability ==
			       previous.pcm_availability &&
		       detector->confirmed_menu.pstn_access ==
			       previous.pstn_access &&
		       detector->confirmed_menu.pcm_present ==
			       previous.pcm_present &&
		       detector->confirmed_menu.pstn_access_present ==
			       previous.pstn_access_present &&
		       detector->confirmed_menu.lapm == previous.lapm;
	return detector->confirmed_menu.call_function == 0 &&
	       detector->confirmed_menu.modulations == 0 &&
	       detector->confirmed_menu.modulation_octets == 0 &&
	       detector->confirmed_menu.pcm_availability == 0 &&
	       detector->confirmed_menu.pstn_access == 0 &&
	       !detector->confirmed_menu.pcm_present &&
	       !detector->confirmed_menu.pstn_access_present &&
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
