// SPDX-License-Identifier: BSD-3-Clause

#include <v8_menu.h>

#include "test.h"

#include <string.h>

static bool encode_equals(
	uint16_t modulations, bool lapm, const uint8_t *expected,
	size_t expected_count)
{
	struct v8_menu menu;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count = 0;

	return v8_call_menu_init(&menu, modulations, lapm) ==
		       V8_MENU_OK &&
	       v8_menu_encode(
		       &menu, information, sizeof(information),
		       &information_count) == V8_MENU_OK &&
	       information_count == expected_count &&
	       memcmp(information, expected, expected_count) == 0;
}

static bool decode_has_result(
	const uint8_t *information, size_t information_count,
	enum v8_menu_result expected);

static bool test_v21_data_menu(void)
{
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x90), UINT8_C(0x2a),
	};

	CHECK(encode_equals(
		      V8_MODULATION_V21, true,
		      expected, ARRAY_LENGTH(expected)),
	      "V.8 V.21 data menu changed");
	return true;
}

static bool test_modulation_mapping(void)
{
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0xc5), UINT8_C(0xd7), UINT8_C(0xd7),
	};

	CHECK(encode_equals(
		      V8_MODULATION_ALL, false,
		      expected, ARRAY_LENGTH(expected)),
	      "V.8 modulation mapping changed");
	return true;
}

static bool test_all_modulation_combinations(void)
{
	/* Table 4/V.8 wire masks, ordered by the public capability enum.
	 * Each value contains modn0:modn1:modn2 in the low 24 bits. These
	 * literal vectors check the wire result independently of round trips. */
	static const uint32_t wire_masks[] = {
		0x400000, 0x800000, 0x000100, 0x000200,
		0x000400, 0x004000, 0x008000, 0x000001,
		0x000002, 0x000004, 0x000040, 0x000080,
	};

	for (unsigned capabilities = 0; capabilities <= V8_MODULATION_ALL;
	     ++capabilities) {
		uint32_t wire = 0x051010;
		uint8_t expected[V8_MAX_INFORMATION_OCTETS] = { 0xc1 };
		uint8_t encoded[V8_MAX_INFORMATION_OCTETS];
		struct v8_menu menu = {
			.call_function = V8_CALL_DATA,
			.modulations = (uint16_t)capabilities,
			.modulation_octets = 1,
		};
		struct v8_menu decoded;
		size_t count;

		for (size_t bit = 0; bit < ARRAY_LENGTH(wire_masks); ++bit)
			if ((capabilities & (1u << bit)) != 0)
				wire |= wire_masks[bit];
		expected[1] = (uint8_t)(wire >> 16);
		expected[2] = (uint8_t)(wire >> 8);
		expected[3] = (uint8_t)wire;
		unsigned width = expected[3] != 0x10 ? 3u :
			(expected[2] != 0x10 ? 2u : 1u);
		if (capabilities != 0)
			CHECK(v8_call_menu_init(&menu, capabilities, false) == V8_MENU_OK &&
			      menu.modulation_octets == width,
			      "minimal width for capabilities=%x", capabilities);
		/* Preserve explicit extra extensions, including zero capabilities. */
		for (unsigned extra = 0;
		     extra <= V8_MAX_MODULATION_OCTETS - width; ++extra) {
			menu.modulation_octets = (uint8_t)(width + extra);
			for (unsigned i = width + 1u; i <= width + extra; ++i)
				expected[i] = 0x10;
			CHECK(v8_menu_encode(&menu, encoded, sizeof(encoded), &count) == V8_MENU_OK &&
			      count == 1u + width + extra && !memcmp(encoded, expected, count) &&
			      v8_menu_decode(expected, count, &decoded) == V8_MENU_OK &&
			      decoded.modulations == capabilities &&
			      decoded.modulation_octets == width + extra && !decoded.pcm_present &&
			      v8_menu_is_usable(&menu) == (capabilities != 0) &&
			      v8_menu_is_usable(&decoded) == (capabilities != 0),
			      "wire mapping capabilities=%x extra=%u", capabilities, extra);
		}
	}
	return true;
}

static bool test_minimal_modulation_width(void)
{
	static const uint8_t v34[] = { UINT8_C(0xc1), UINT8_C(0x45) };
	static const uint8_t v32[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x11),
	};
	static const uint8_t v21[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10), UINT8_C(0x90),
	};

	CHECK(encode_equals(
		      V8_MODULATION_V34_DUPLEX, false,
		      v34, ARRAY_LENGTH(v34)) &&
	      encode_equals(
		      V8_MODULATION_V32, false,
		      v32, ARRAY_LENGTH(v32)) &&
	      encode_equals(
		      V8_MODULATION_V21, false,
		      v21, ARRAY_LENGTH(v21)),
	      "V.8 modulation menu was not minimally extended");
	return true;
}

static bool test_extended_zero_capabilities(void)
{
	struct v8_menu menu = {
		.call_function = V8_CALL_DATA,
		.modulation_octets = 5,
	};
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x10), UINT8_C(0x10), UINT8_C(0x10),
	};
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t count = 0;

	CHECK(v8_menu_encode(
		      &menu, information, sizeof(information), &count) ==
		      V8_MENU_OK &&
	      count == ARRAY_LENGTH(expected) &&
	      memcmp(information, expected, sizeof(expected)) == 0,
	      "V.8 zero-capability extension width changed");
	CHECK(!v8_menu_is_usable(&menu),
	      "V.8 empty refusal menu was usable for negotiation");
	return true;
}

static bool test_menu_usability(void)
{
	CHECK(!v8_menu_is_usable(NULL), "V.8 null menu was usable");
	for (unsigned flags = 0; flags < 8; ++flags) {
		struct v8_menu menu = {
			.call_function = V8_CALL_DATA,
			.modulations = V8_MODULATION_V34_DUPLEX | V8_MODULATION_V21,
			.pcm_availability = (flags & 4u) ? V8_PCM_ANALOG : 0,
			.pstn_access = (flags & 2u) ? V8_PSTN_DIGITAL : 0,
			.pcm_present = (flags & 4u) != 0,
			.pstn_access_present = (flags & 2u) != 0,
			.lapm = (flags & 1u) != 0,
		};
		unsigned categories = 1u + menu.lapm + menu.pcm_present +
			menu.pstn_access_present;

		for (unsigned width = 0; width <= V8_MAX_MODULATION_OCTETS + 1u; ++width) {
			bool expected = width >= 3u && width + categories <= 16u &&
				(!menu.pcm_present || menu.pstn_access_present);

			menu.modulation_octets = (uint8_t)width;
			CHECK(v8_menu_is_usable(&menu) == expected,
			      "V.8 menu admission changed at width=%u flags=%u", width, flags);
		}
	}
	for (unsigned mutation = 0; mutation < 8; ++mutation) {
		struct v8_menu menu = {
			.call_function = V8_CALL_DATA,
			.modulations = V8_MODULATION_V34_DUPLEX | V8_MODULATION_V21,
			.modulation_octets = 3,
			.pcm_availability = V8_PCM_ANALOG,
			.pstn_access = V8_PSTN_DIGITAL,
			.pcm_present = true,
			.pstn_access_present = true,
		};

		switch (mutation) {
		case 0: menu.call_function = (enum v8_call_function)0; break;
		case 1: menu.modulations |= UINT16_C(0x1000); break;
		case 2: menu.pcm_availability = UINT8_C(0x08); break;
		case 3: menu.pstn_access = UINT8_C(0x08); break;
		case 4: menu.pcm_present = false; break;
		case 5: menu.pstn_access_present = false; break;
		case 6: menu.modulations = V8_MODULATION_V21; break;
		case 7: menu.modulations = 0; break;
		}
		CHECK(!v8_menu_is_usable(&menu),
		      "V.8 malformed menu mutation %u was usable", mutation);
	}
	return true;
}

static bool test_decode_round_trip(void)
{
	static const uint16_t capabilities[] = {
		V8_MODULATION_V34_DUPLEX,
		V8_MODULATION_V32,
		V8_MODULATION_V21,
		V8_MODULATION_ALL,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(capabilities); ++index) {
		struct v8_menu encoded;
		struct v8_menu decoded;
		uint8_t information[V8_MAX_INFORMATION_OCTETS];
		size_t information_count;
		bool lapm = (index & 1u) != 0;

		CHECK(v8_call_menu_init(
			      &encoded, capabilities[index], lapm) ==
			      V8_MENU_OK &&
		      v8_menu_encode(
			      &encoded, information, sizeof(information),
			      &information_count) == V8_MENU_OK &&
		      v8_menu_decode(
			      information, information_count, &decoded) ==
			      V8_MENU_OK &&
		      decoded.call_function == encoded.call_function &&
		      decoded.modulations == encoded.modulations &&
		      decoded.modulation_octets == encoded.modulation_octets &&
		      decoded.pcm_availability == 0 &&
		      decoded.pstn_access == 0 && !decoded.pcm_present &&
		      !decoded.pstn_access_present &&
		      decoded.lapm == encoded.lapm,
		      "V.8 menu round trip changed case %zu", index);
	}
	return true;
}

static bool test_pcm_categories(void)
{
	static const uint8_t digital_information[] = {
		UINT8_C(0xc1), UINT8_C(0x65), UINT8_C(0x2a),
		UINT8_C(0x8d), UINT8_C(0x67),
	};
	static const uint8_t analog_information[] = {
		UINT8_C(0xc1), UINT8_C(0x65), UINT8_C(0x0d),
		UINT8_C(0x27),
	};
	static const uint8_t repeated_analog_access[] = {
		UINT8_C(0xc1), UINT8_C(0x65), UINT8_C(0x0d),
		UINT8_C(0x0d), UINT8_C(0x27),
	};
	struct v8_menu menu;
	struct v8_menu decoded;
	uint8_t encoded[V8_MAX_INFORMATION_OCTETS];
	size_t encoded_count;

	CHECK(v8_pcm_call_menu_init(
		      &menu, V8_PCM_ANALOG | V8_PCM_DIGITAL,
		      V8_PSTN_DIGITAL, true) == V8_MENU_OK &&
	      v8_menu_encode(
		      &menu, encoded, sizeof(encoded), &encoded_count) ==
		      V8_MENU_OK &&
	      encoded_count == ARRAY_LENGTH(digital_information) &&
	      memcmp(encoded, digital_information, encoded_count) == 0 &&
	      v8_menu_decode(
		      encoded, encoded_count, &decoded) == V8_MENU_OK &&
	      decoded.modulations == V8_MODULATION_V34_DUPLEX &&
	      decoded.modulation_octets == 1 &&
	      decoded.pcm_availability ==
		      (V8_PCM_ANALOG | V8_PCM_DIGITAL) &&
	      decoded.pstn_access == V8_PSTN_DIGITAL &&
	      decoded.pcm_present && decoded.pstn_access_present &&
	      decoded.lapm,
	      "V.8 digital PCM categories changed");
	CHECK(v8_pcm_call_menu_init(
		      &menu, V8_PCM_ANALOG, 0, false) == V8_MENU_OK &&
	      v8_menu_encode(
		      &menu, encoded, sizeof(encoded), &encoded_count) ==
		      V8_MENU_OK &&
	      encoded_count == ARRAY_LENGTH(analog_information) &&
	      memcmp(encoded, analog_information, encoded_count) == 0 &&
	      v8_menu_decode(
		      encoded, encoded_count, &decoded) == V8_MENU_OK &&
	      decoded.pcm_availability == V8_PCM_ANALOG &&
	      decoded.pstn_access == 0 && decoded.pcm_present &&
	      decoded.pstn_access_present && !decoded.lapm,
	      "V.8 analog PSTN access presence was lost");
	CHECK(v8_menu_decode(
		      repeated_analog_access,
		      ARRAY_LENGTH(repeated_analog_access), &decoded) ==
		      V8_MENU_OK &&
	      decoded.modulations == V8_MODULATION_V34_DUPLEX &&
	      decoded.pcm_availability == V8_PCM_ANALOG &&
	      decoded.pstn_access == 0 && decoded.pcm_present &&
	      decoded.pstn_access_present,
	      "V.8 repeated identical PSTN access was not tolerated");
	return true;
}

static bool test_pcm_category_rejection(void)
{
	static const uint8_t orphan_indicator[] = {
		UINT8_C(0xc1), UINT8_C(0x65),
	};
	static const uint8_t missing_access[] = {
		UINT8_C(0xc1), UINT8_C(0x65), UINT8_C(0x27),
	};
	static const uint8_t missing_v34[] = {
		UINT8_C(0xc1), UINT8_C(0x25), UINT8_C(0x0d), UINT8_C(0x27),
	};
	static const uint8_t duplicate_pcm[] = {
		UINT8_C(0xc1), UINT8_C(0x65), UINT8_C(0x0d),
		UINT8_C(0x27), UINT8_C(0x27),
	};
	static const uint8_t conflicting_access[] = {
		UINT8_C(0xc1), UINT8_C(0x65), UINT8_C(0x0d),
		UINT8_C(0x8d), UINT8_C(0x27),
	};
	struct v8_menu unchanged = {
		.modulations = UINT16_MAX,
	};
	struct v8_menu saved = unchanged;

	CHECK(decode_has_result(
		      orphan_indicator, ARRAY_LENGTH(orphan_indicator),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      missing_access, ARRAY_LENGTH(missing_access),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      missing_v34, ARRAY_LENGTH(missing_v34),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      duplicate_pcm, ARRAY_LENGTH(duplicate_pcm),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      conflicting_access, ARRAY_LENGTH(conflicting_access),
		      V8_MENU_MALFORMED) &&
	      v8_pcm_call_menu_init(
		      &unchanged, 0, 0, false) == V8_MENU_INVALID_ARGUMENT &&
	      memcmp(&unchanged, &saved, sizeof(unchanged)) == 0 &&
	      v8_pcm_call_menu_init(
		      &unchanged, V8_PCM_ALL + 1u, 0, false) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_pcm_call_menu_init(
		      &unchanged, V8_PCM_ANALOG,
		      V8_PSTN_ALL + 1u, false) == V8_MENU_INVALID_ARGUMENT &&
	      v8_pcm_call_menu_init(
		      NULL, V8_PCM_ANALOG, 0, false) ==
		      V8_MENU_INVALID_ARGUMENT,
	      "V.8 PCM category coupling accepted malformed state");
	return true;
}

static bool test_future_categories(void)
{
	static const uint8_t information[] = {
		UINT8_C(0xc1),
		UINT8_C(0x03), UINT8_C(0x97),
		UINT8_C(0x05), UINT8_C(0x10), UINT8_C(0x90), UINT8_C(0x17),
		UINT8_C(0x2a),
	};
	static const uint8_t normalized[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x90), UINT8_C(0x10), UINT8_C(0x2a),
	};
	struct v8_menu menu;
	uint8_t encoded[V8_MAX_INFORMATION_OCTETS];
	size_t encoded_count;

	CHECK(v8_menu_decode(
		      information, ARRAY_LENGTH(information), &menu) ==
		      V8_MENU_OK &&
	      menu.modulations == V8_MODULATION_V21 &&
	      menu.modulation_octets == 4 && menu.lapm &&
	      v8_menu_encode(
		      &menu, encoded, sizeof(encoded), &encoded_count) ==
		      V8_MENU_OK &&
	      encoded_count == ARRAY_LENGTH(normalized) &&
	      memcmp(encoded, normalized, sizeof(normalized)) == 0,
	      "V.8 decoder did not preserve future extension width");
	return true;
}

static bool decode_has_result(
	const uint8_t *information, size_t information_count,
	enum v8_menu_result expected)
{
	struct v8_menu menu = {
		.call_function = V8_CALL_DATA,
		.modulations = UINT16_MAX,
		.modulation_octets = UINT8_MAX,
		.lapm = true,
	};
	struct v8_menu saved = menu;

	return v8_menu_decode(
		       information, information_count, &menu) == expected &&
	       memcmp(&menu, &saved, sizeof(menu)) == 0;
}

static bool test_decode_rejection(void)
{
	static const uint8_t wrong_first[] = { UINT8_C(0x05) };
	static const uint8_t unsupported_call[] = { UINT8_C(0x21) };
	static const uint8_t missing_modulation[] = {
		UINT8_C(0xc1), UINT8_C(0x2a),
	};
	static const uint8_t orphan_extension[] = {
		UINT8_C(0xc1), UINT8_C(0x10), UINT8_C(0x05),
	};
	static const uint8_t malformed_extension[] = {
		UINT8_C(0xc1), UINT8_C(0x03), UINT8_C(0x30), UINT8_C(0x05),
	};
	static const uint8_t duplicate_modulation[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x05),
	};
	static const uint8_t duplicate_protocol[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x2a), UINT8_C(0x2a),
	};
	static const uint8_t unexpected_protocol_extension[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x2a), UINT8_C(0x10),
	};
	uint8_t too_long[V8_MAX_INFORMATION_OCTETS + 1] = { 0 };

	CHECK(decode_has_result(NULL, 1, V8_MENU_INVALID_ARGUMENT) &&
	      decode_has_result(wrong_first, 0, V8_MENU_MALFORMED) &&
	      decode_has_result(
		      wrong_first, ARRAY_LENGTH(wrong_first),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      unsupported_call, ARRAY_LENGTH(unsupported_call),
		      V8_MENU_UNSUPPORTED_CALL_FUNCTION) &&
	      decode_has_result(
		      missing_modulation, ARRAY_LENGTH(missing_modulation),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      orphan_extension, ARRAY_LENGTH(orphan_extension),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      malformed_extension, ARRAY_LENGTH(malformed_extension),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      duplicate_modulation, ARRAY_LENGTH(duplicate_modulation),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      duplicate_protocol, ARRAY_LENGTH(duplicate_protocol),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      unexpected_protocol_extension,
		      ARRAY_LENGTH(unexpected_protocol_extension),
		      V8_MENU_MALFORMED) &&
	      decode_has_result(
		      too_long, ARRAY_LENGTH(too_long),
		      V8_MENU_MALFORMED),
	      "V.8 decoder accepted malformed information");
	return true;
}

static bool test_joint_menu_selection(void)
{
	struct v8_menu call_menu;
	struct v8_menu local_menu;
	struct v8_menu joint_menu;
	enum v8_modulation selected;
	uint16_t local_modulations =
		V8_MODULATION_V34_HALF_DUPLEX |
		V8_MODULATION_V32 |
		V8_MODULATION_V21;

	CHECK(v8_call_menu_init(
		      &call_menu, V8_MODULATION_ALL, true) ==
		      V8_MENU_OK &&
	      v8_call_menu_init(
		      &local_menu, local_modulations, true) ==
		      V8_MENU_OK &&
	      v8_joint_menu_select(
		      &call_menu, &local_menu, &joint_menu) ==
		      V8_MENU_OK &&
	      joint_menu.call_function == V8_CALL_DATA &&
	      joint_menu.modulations == local_modulations &&
	      joint_menu.modulation_octets == call_menu.modulation_octets &&
	      joint_menu.lapm &&
	      v8_menu_selected_modulation(
		      &call_menu, &joint_menu, &selected) == V8_MENU_OK &&
	      selected == V8_MODULATION_V34_HALF_DUPLEX,
	      "V.8 joint menu did not select the first common mode");
	return true;
}

static bool test_pcm_role_selection(void)
{
	struct v8_menu call_menu;
	struct v8_menu answer_menu;
	struct v8_menu joint_menu;
	struct v8_pcm_roles roles;

	CHECK(v8_pcm_call_menu_init(
		      &call_menu, V8_PCM_ANALOG, 0, false) == V8_MENU_OK &&
	      v8_pcm_call_menu_init(
		      &answer_menu, V8_PCM_DIGITAL,
		      V8_PSTN_DIGITAL, false) == V8_MENU_OK &&
	      v8_joint_menu_select(
		      &call_menu, &answer_menu, &joint_menu) == V8_MENU_OK &&
	      joint_menu.pcm_present &&
	      joint_menu.pcm_availability == V8_PCM_DIGITAL &&
	      joint_menu.pstn_access_present &&
	      joint_menu.pstn_access == V8_PSTN_DIGITAL &&
	      v8_pcm_select_roles(
		      &call_menu, &joint_menu, &roles) == V8_MENU_OK &&
	      roles.caller == V8_PCM_ROLE_ANALOG &&
	      roles.answerer == V8_PCM_ROLE_DIGITAL,
	      "V.8 did not choose a digital answer modem");
	CHECK(v8_pcm_call_menu_init(
		      &call_menu, V8_PCM_DIGITAL,
		      V8_PSTN_CALL_CELLULAR | V8_PSTN_DIGITAL,
		      false) == V8_MENU_OK &&
	      v8_pcm_call_menu_init(
		      &answer_menu, V8_PCM_ANALOG,
		      V8_PSTN_ANSWER_CELLULAR, false) == V8_MENU_OK &&
	      v8_joint_menu_select(
		      &call_menu, &answer_menu, &joint_menu) == V8_MENU_OK &&
	      joint_menu.pcm_present &&
	      joint_menu.pcm_availability == V8_PCM_ANALOG &&
	      joint_menu.pstn_access ==
		      (V8_PSTN_CALL_CELLULAR |
		       V8_PSTN_ANSWER_CELLULAR) &&
	      v8_pcm_select_roles(
		      &call_menu, &joint_menu, &roles) == V8_MENU_OK &&
	      roles.caller == V8_PCM_ROLE_DIGITAL &&
	      roles.answerer == V8_PCM_ROLE_ANALOG,
	      "V.8 did not choose a digital call modem");
	return true;
}

static bool test_pcm_role_fallback(void)
{
	struct v8_menu call_menu;
	struct v8_menu answer_menu;
	struct v8_menu joint_menu;
	struct v8_pcm_roles roles = {
		.caller = V8_PCM_ROLE_DIGITAL,
		.answerer = V8_PCM_ROLE_ANALOG,
	};
	struct v8_pcm_roles saved = roles;
	const struct v8_pcm_roles expected_saved = roles;

	CHECK(v8_pcm_call_menu_init(
		      &call_menu, V8_PCM_ANALOG | V8_PCM_DIGITAL,
		      V8_PSTN_DIGITAL, false) == V8_MENU_OK &&
	      v8_pcm_call_menu_init(
		      &answer_menu, V8_PCM_ANALOG | V8_PCM_DIGITAL,
		      V8_PSTN_DIGITAL, false) == V8_MENU_OK &&
	      v8_joint_menu_select(
		      &call_menu, &answer_menu, &joint_menu) == V8_MENU_OK &&
	      v8_pcm_select_roles(
		      &call_menu, &joint_menu, &roles) == V8_MENU_OK &&
	      roles.caller == V8_PCM_ROLE_ANALOG &&
	      roles.answerer == V8_PCM_ROLE_DIGITAL &&
	      joint_menu.pcm_availability == V8_PCM_DIGITAL,
	      "V.8 did not resolve the dual-digital role in its joint menu");
	CHECK(v8_pcm_call_menu_init(
		      &call_menu, V8_PCM_ANALOG, 0, false) == V8_MENU_OK &&
	      v8_pcm_call_menu_init(
		      &answer_menu, V8_PCM_DIGITAL, 0, false) == V8_MENU_OK &&
	      v8_joint_menu_select(
		      &call_menu, &answer_menu, &joint_menu) == V8_MENU_OK &&
	      !joint_menu.pcm_present && joint_menu.pcm_availability == 0 &&
	      v8_pcm_select_roles(
		      &call_menu, &joint_menu, &roles) == V8_MENU_OK &&
	      roles.caller == V8_PCM_ROLE_NONE &&
	      roles.answerer == V8_PCM_ROLE_NONE &&
	      v8_pcm_select_roles(
		      NULL, &joint_menu, &saved) == V8_MENU_INVALID_ARGUMENT &&
	      memcmp(&saved, &expected_saved, sizeof(saved)) == 0,
	      "V.8 did not fall back when no digital access was available");
	return true;
}

static bool test_pcm_with_non_pcm_peer(void)
{
	struct v8_menu pcm;
	struct v8_menu non_pcm;
	struct v8_menu joint;
	struct v8_pcm_roles roles;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t count;

	for (unsigned int digital = 0; digital < 2; ++digital) {
		CHECK(v8_pcm_call_menu_init(&pcm,
			      digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG,
			      digital ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK,
		      "PCM capability setup failed");
		for (unsigned int common = 0; common < 2; ++common) {
			CHECK(v8_call_menu_init(&non_pcm, common ?
				      V8_MODULATION_V34_DUPLEX : V8_MODULATION_V21,
				      true) == V8_MENU_OK,
			      "non-PCM capability setup failed");
			for (unsigned int pcm_caller = 0; pcm_caller < 2; ++pcm_caller) {
				const struct v8_menu *call = pcm_caller ? &pcm : &non_pcm;
				const struct v8_menu *answer = pcm_caller ? &non_pcm : &pcm;
				enum v8_modulation selected = V8_MODULATION_NONE;

				CHECK(v8_joint_menu_select(call, answer, &joint) == V8_MENU_OK &&
				      !joint.pcm_present && joint.pcm_availability == 0 &&
				      joint.modulation_octets == call->modulation_octets &&
				      joint.lapm &&
				      v8_menu_encode(&joint, information, sizeof(information),
					      &count) == V8_MENU_OK &&
				      (information[1] & 0x20) == 0 &&
				      v8_pcm_select_roles(call, &joint, &roles) == V8_MENU_OK &&
				      roles.caller == V8_PCM_ROLE_NONE &&
				      roles.answerer == V8_PCM_ROLE_NONE &&
				      v8_menu_selected_modulation(call, &joint, &selected) ==
					      (common ? V8_MENU_OK : V8_MENU_NO_COMMON_MODULATION) &&
				      selected == (common ? V8_MODULATION_V34_DUPLEX :
					      V8_MODULATION_NONE),
				      "mixed PCM menu failed: digital=%u common=%u pcm_caller=%u",
				      digital, common, pcm_caller);
			}
		}
	}
	return true;
}

static bool test_no_common_modulation(void)
{
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x10), UINT8_C(0x2a),
	};
	struct v8_menu call_menu;
	struct v8_menu local_menu;
	struct v8_menu joint_menu;
	enum v8_modulation selected =
		V8_MODULATION_V21;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	CHECK(v8_call_menu_init(
		      &call_menu, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_call_menu_init(
		      &local_menu, V8_MODULATION_V34_DUPLEX, true) ==
		      V8_MENU_OK &&
	      v8_joint_menu_select(
		      &call_menu, &local_menu, &joint_menu) ==
		      V8_MENU_OK &&
	      joint_menu.modulations == 0 &&
	      joint_menu.modulation_octets == 3 && joint_menu.lapm &&
	      v8_menu_encode(
		      &joint_menu, information, sizeof(information),
		      &information_count) == V8_MENU_OK &&
	      information_count == ARRAY_LENGTH(expected) &&
	      memcmp(information, expected, sizeof(expected)) == 0 &&
	      v8_menu_selected_modulation(
		      &call_menu, &joint_menu, &selected) ==
		      V8_MENU_NO_COMMON_MODULATION &&
	      selected == V8_MODULATION_V21,
	      "V.8 no-common-mode joint menu changed");
	return true;
}

static bool test_joint_validation(void)
{
	struct v8_menu call_menu;
	struct v8_menu local_menu;
	struct v8_menu joint_menu = {
		.modulations = UINT16_MAX,
	};
	struct v8_menu saved = joint_menu;
	enum v8_modulation selected =
		V8_MODULATION_V21;

	CHECK(v8_call_menu_init(
		      &call_menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_call_menu_init(
		      &local_menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK,
	      "V.8 joint validation fixtures did not initialize");
	call_menu.modulation_octets = 0;
	CHECK(v8_joint_menu_select(
		      &call_menu, &local_menu, &joint_menu) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      memcmp(&joint_menu, &saved, sizeof(joint_menu)) == 0 &&
	      v8_joint_menu_select(
		      NULL, &local_menu, &joint_menu) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_selected_modulation(
		      NULL, &local_menu, &selected) == V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_selected_modulation(
		      &local_menu, NULL, &selected) == V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_selected_modulation(
		      &local_menu, &local_menu, NULL) == V8_MENU_INVALID_ARGUMENT &&
	      strcmp(v8_menu_result_name(
			     V8_MENU_NO_COMMON_MODULATION),
		     "no common modulation") == 0,
	      "V.8 joint menu accepted invalid state");
	return true;
}

static bool feed_control_bits(
	struct v8_menu_detector *detector,
	const uint8_t *information, size_t information_count, size_t bit_count,
	struct v8_menu *menu, bool *confirmed)
{
	struct v8_control_generator generator;

	if (v8_control_generator_init(
		    &generator, information, information_count) !=
	    V8_FRAMING_OK)
		return false;
	for (size_t index = 0; index < bit_count; ++index) {
		bool complete;
		uint8_t bit;

		if (v8_control_next_bit(
			    &generator, &bit, &complete) !=
			    V8_FRAMING_OK ||
		    v8_menu_detect_bit(
			    detector, bit, menu, confirmed) !=
			    V8_MENU_OK)
			return false;
	}
	return true;
}

static bool test_empty_pcm_refusal(void)
{
	static const uint8_t information[] = {
		0xc1, 0x45, 0x2a, 0x0d, 0x07,
	};
	static const uint8_t missing_access[] = {
		0xc1, 0x45, 0x2a, 0x07,
	};
	static const uint8_t duplicate_pcm[] = {
		0xc1, 0x45, 0x2a, 0x0d, 0x07, 0x07,
	};
	struct v8_menu_detector detector;
	struct v8_menu detected, caller, joint;
	struct v8_pcm_roles roles;
	uint8_t encoded[V8_MAX_INFORMATION_OCTETS];
	size_t count;
	size_t sequence_bits = V8_PREAMBLE_BITS +
		ARRAY_LENGTH(information) * V8_FRAMED_OCTET_BITS;
	bool confirmed;

	CHECK(v8_menu_detector_init(&detector) == V8_MENU_OK &&
	      feed_control_bits(&detector, information, ARRAY_LENGTH(information),
				2u * sequence_bits + V8_PREAMBLE_BITS,
				&detected, &confirmed) && confirmed &&
	      detected.modulations == V8_MODULATION_V34_DUPLEX &&
	      detected.lapm && !detected.pcm_present &&
	      detected.pcm_availability == 0 && detected.pstn_access_present &&
	      detected.pstn_access == 0,
	      "V.8 repeated empty PCM refusal was rejected");
	for (unsigned int digital = 0; digital < 2; ++digital) {
		CHECK(v8_pcm_call_menu_init(&caller,
				digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG,
				digital ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK &&
		      v8_joint_menu_select(&caller, &detected, &joint) == V8_MENU_OK &&
		      joint.modulations == V8_MODULATION_V34_DUPLEX && joint.lapm &&
		      !joint.pcm_present && joint.pcm_availability == 0 &&
		      v8_pcm_select_roles(&caller, &joint, &roles) == V8_MENU_OK &&
		      roles.caller == V8_PCM_ROLE_NONE &&
		      roles.answerer == V8_PCM_ROLE_NONE &&
		      v8_menu_encode(&joint, encoded, sizeof(encoded), &count) ==
			      V8_MENU_OK && (encoded[1] & 0x20) == 0,
		      "V.8 empty refusal incorrectly authorized a PCM role");
	}
	CHECK(v8_menu_decode(missing_access, ARRAY_LENGTH(missing_access),
			     &detected) == V8_MENU_OK &&
	      !detected.pcm_present && !detected.pstn_access_present &&
	      decode_has_result(duplicate_pcm, ARRAY_LENGTH(duplicate_pcm),
			       V8_MENU_MALFORMED),
	      "V.8 empty refusal bypassed category validation");
	return true;
}

static bool test_unindicated_pcm_roles(void)
{
	/* A repeated deployed JM retains a digital-role category even though
	 * modn0 declines PCM. Conventional modes remain usable. */
	uint8_t information[] = { 0xc1, 0x45, 0x12, 0x10, 0x2a, 0x0d, 0x47 };
	static const uint8_t duplicate[] = { 0xc1, 0x45, 0x0d, 0x47, 0x47 };
	struct v8_menu decoded, caller, joint;
	struct v8_pcm_roles roles;

	for (unsigned availability = 0; availability <= V8_PCM_ALL; ++availability) {
		information[6] = (uint8_t)(0x07 | availability << 5);
		CHECK(v8_menu_decode(information, ARRAY_LENGTH(information), &decoded) == V8_MENU_OK &&
		      decoded.modulations == (V8_MODULATION_V34_DUPLEX | V8_MODULATION_V22) &&
		      decoded.lapm && !decoded.pcm_present && decoded.pcm_availability == 0,
		      "unindicated PCM role %u blocked conventional negotiation", availability);
		for (unsigned digital = 0; digital < 2; ++digital) {
			CHECK(v8_pcm_call_menu_init(&caller,
				      digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG,
				      digital ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK &&
			      v8_joint_menu_select(&caller, &decoded, &joint) == V8_MENU_OK &&
			      v8_pcm_select_roles(&caller, &joint, &roles) == V8_MENU_OK &&
			      roles.caller == V8_PCM_ROLE_NONE && roles.answerer == V8_PCM_ROLE_NONE,
			      "unindicated PCM category granted a role");
		}
	}
	CHECK(decode_has_result(duplicate, ARRAY_LENGTH(duplicate), V8_MENU_MALFORMED),
	      "unindicated PCM normalization hid a duplicate category");
	return true;
}

static bool test_empty_pcm_refusal_without_access(void)
{
	static const uint8_t information[] = {
		0xc1, 0x45, 0x13, 0x94, 0x2a, 0x07, 0x10,
	};
	struct v8_menu_detector detector;
	struct v8_menu detected, caller, joint;
	struct v8_pcm_roles roles;
	size_t sequence_bits = V8_PREAMBLE_BITS +
		ARRAY_LENGTH(information) * V8_FRAMED_OCTET_BITS;
	bool confirmed;

	CHECK(v8_menu_detector_init(&detector) == V8_MENU_OK &&
	      feed_control_bits(&detector, information, ARRAY_LENGTH(information),
				2u * sequence_bits + V8_PREAMBLE_BITS,
				&detected, &confirmed) && confirmed &&
	      detected.modulations == (V8_MODULATION_V34_DUPLEX | V8_MODULATION_V32 |
		      V8_MODULATION_V22 | V8_MODULATION_V23_DUPLEX | V8_MODULATION_V21) &&
	      detected.lapm && !detected.pcm_present &&
	      detected.pcm_availability == 0 && !detected.pstn_access_present,
	      "empty PCM extension without PSTN access blocked repeated JM");
	for (unsigned int digital = 0; digital < 2; ++digital) {
		CHECK(v8_pcm_call_menu_init(&caller,
				digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG,
				digital ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK &&
		      v8_joint_menu_select(&caller, &detected, &joint) == V8_MENU_OK &&
		      joint.modulations == V8_MODULATION_V34_DUPLEX && joint.lapm &&
		      !joint.pcm_present && !joint.pstn_access_present &&
		      v8_pcm_select_roles(&caller, &joint, &roles) == V8_MENU_OK &&
		      roles.caller == V8_PCM_ROLE_NONE && roles.answerer == V8_PCM_ROLE_NONE,
		      "empty PCM refusal granted a role for caller %u", digital);
	}
	return true;
}

static bool test_repeated_menu_confirmation(void)
{
	struct v8_menu offered;
	struct v8_menu detected = {
		.modulations = UINT16_MAX,
	};
	struct v8_menu saved = detected;
	struct v8_menu_detector detector;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count;
	size_t sequence_bits;
	bool confirmed = true;

	CHECK(v8_call_menu_init(
		      &offered, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_menu_encode(
		      &offered, information, sizeof(information),
		      &information_count) == V8_MENU_OK &&
	      v8_menu_detector_init(&detector) ==
		      V8_MENU_OK,
	      "V.8 repeated-menu endpoints did not initialize");
	sequence_bits = V8_PREAMBLE_BITS +
			information_count * V8_FRAMED_OCTET_BITS;
	confirmed = false;
	CHECK(feed_control_bits(
		      &detector, information, information_count,
		      2u * sequence_bits, &detected, &confirmed) &&
	      !confirmed && memcmp(&detected, &saved, sizeof(detected)) == 0 &&
	      feed_control_bits(
		      &detector, information, information_count,
		      V8_PREAMBLE_BITS, &detected, &confirmed) &&
	      confirmed && detected.modulations == offered.modulations &&
	      detected.modulation_octets == offered.modulation_octets &&
	      detected.lapm,
	      "V.8 detector did not require two complete identical menus");
	return true;
}

static bool test_mismatched_menu_restart(void)
{
	struct v8_menu first;
	struct v8_menu second;
	struct v8_menu detected = { 0 };
	struct v8_menu_detector detector;
	uint8_t first_information[V8_MAX_INFORMATION_OCTETS];
	uint8_t second_information[V8_MAX_INFORMATION_OCTETS];
	size_t first_count;
	size_t second_count;
	size_t first_bits;
	size_t second_bits;
	bool confirmed = false;

	CHECK(v8_call_menu_init(
		      &first, V8_MODULATION_V32, false) ==
		      V8_MENU_OK &&
	      v8_call_menu_init(
		      &second, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_menu_encode(
		      &first, first_information, sizeof(first_information),
		      &first_count) == V8_MENU_OK &&
	      v8_menu_encode(
		      &second, second_information, sizeof(second_information),
		      &second_count) == V8_MENU_OK &&
	      v8_menu_detector_init(&detector) ==
		      V8_MENU_OK,
	      "V.8 mismatch endpoints did not initialize");
	first_bits = V8_PREAMBLE_BITS +
		     first_count * V8_FRAMED_OCTET_BITS;
	second_bits = V8_PREAMBLE_BITS +
		      second_count * V8_FRAMED_OCTET_BITS;
	CHECK(feed_control_bits(
		      &detector, first_information, first_count, first_bits,
		      &detected, &confirmed) &&
	      feed_control_bits(
		      &detector, second_information, second_count,
		      2u * second_bits, &detected, &confirmed) &&
	      !confirmed &&
	      feed_control_bits(
		      &detector, second_information, second_count,
		      V8_PREAMBLE_BITS, &detected, &confirmed) &&
	      confirmed &&
	      detected.modulations == V8_MODULATION_V21,
	      "V.8 mismatch did not restart consecutive confirmation");
	return true;
}

static bool test_malformed_menu_recovery(void)
{
	static const uint8_t malformed[] = {
		UINT8_C(0xc1), UINT8_C(0x2a),
	};
	struct v8_menu valid;
	struct v8_menu detected = { 0 };
	struct v8_menu_detector detector;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count;
	size_t malformed_bits =
		V8_PREAMBLE_BITS +
		ARRAY_LENGTH(malformed) * V8_FRAMED_OCTET_BITS;
	size_t valid_bits;
	bool confirmed = false;

	CHECK(v8_call_menu_init(
		      &valid, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_menu_encode(
		      &valid, information, sizeof(information),
		      &information_count) == V8_MENU_OK &&
	      v8_menu_detector_init(&detector) ==
		      V8_MENU_OK,
	      "V.8 malformed recovery endpoints did not initialize");
	valid_bits = V8_PREAMBLE_BITS +
		     information_count * V8_FRAMED_OCTET_BITS;
	CHECK(feed_control_bits(
		      &detector, malformed, ARRAY_LENGTH(malformed),
		      2u * malformed_bits + V8_PREAMBLE_BITS,
		      &detected, &confirmed) &&
	      !confirmed &&
	      feed_control_bits(
		      &detector, information, information_count,
		      2u * valid_bits + V8_PREAMBLE_BITS,
		      &detected, &confirmed) &&
	      confirmed && detected.modulations == valid.modulations,
	      "V.8 detector did not recover after malformed repeats");
	return true;
}

static bool test_menu_detector_validation(void)
{
	struct v8_menu_detector detector;
	struct v8_menu_detector saved;
	struct v8_menu menu = {
		.modulations = UINT16_MAX,
	};
	struct v8_menu saved_menu = menu;
	bool confirmed = true;

	CHECK(v8_menu_detector_init(&detector) ==
		      V8_MENU_OK,
	      "V.8 menu detector validation did not initialize");
	detector.current_count = V8_MAX_INFORMATION_OCTETS + 1u;
	saved = detector;
	CHECK(v8_menu_detector_init(NULL) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_detect_bit(
		      &detector, 0, &menu, &confirmed) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
	      memcmp(&menu, &saved_menu, sizeof(menu)) == 0 && confirmed &&
	      v8_menu_detect_bit(NULL, 0, &menu, &confirmed) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_detect_bit(&saved, 2, &menu, &confirmed) ==
		      V8_MENU_INVALID_ARGUMENT,
	      "V.8 menu detector accepted invalid state");
	v8_menu_detector_reset(&detector);
	CHECK(!detector.sequence_open && detector.current_count == 0 &&
		      !detector.confirmed,
	      "V.8 menu detector reset retained state");
	return true;
}

static bool test_validation(void)
{
	struct v8_menu menu;
	struct v8_menu invalid;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	uint8_t saved_information[sizeof(information)];
	size_t count = SIZE_MAX;

	memset(information, UINT8_C(0xa5), sizeof(information));
	memcpy(saved_information, information, sizeof(information));
	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, true) ==
		      V8_MENU_OK,
	      "V.8 validation menu did not initialize");
	invalid = menu;
	invalid.modulation_octets = 2;
	CHECK(v8_call_menu_init(NULL, 1, false) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_call_menu_init(&menu, 0, false) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_call_menu_init(
		      &menu, V8_MODULATION_ALL + 1u, false) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_encode(
		      &invalid, information, sizeof(information), &count) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      memcmp(information, saved_information, sizeof(information)) == 0 &&
	      count == SIZE_MAX,
	      "V.8 menu accepted invalid capabilities or changed output");
	invalid = menu;
	CHECK(v8_menu_encode(
		      &invalid, information, 4, &count) ==
		      V8_MENU_BUFFER_TOO_SMALL &&
	      memcmp(information, saved_information, sizeof(information)) == 0 &&
	      count == SIZE_MAX &&
	      v8_menu_encode(NULL, information, 5, &count) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_encode(&invalid, NULL, 5, &count) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      v8_menu_encode(&invalid, information, 5, NULL) ==
		      V8_MENU_INVALID_ARGUMENT &&
	      strcmp(v8_menu_result_name(
			     V8_MENU_BUFFER_TOO_SMALL),
		     "buffer too small") == 0,
	      "V.8 menu validation was not transactional");
	return true;
}

static bool test_bidirectional_pcm_roles(void)
{
	struct v8_menu caller, answerer, joint, decoded;
	struct v8_pcm_roles roles;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t count;
	for (unsigned mask = 0; mask < 16; ++mask) {
		CHECK(v8_pcm_call_menu_init(&caller,
		      V8_PCM_ANALOG | V8_PCM_DIGITAL | ((mask & 1) ? V8_PCM_V91 : 0),
		      (mask & 4) ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK &&
		      v8_pcm_call_menu_init(&answerer,
		      V8_PCM_ANALOG | V8_PCM_DIGITAL | ((mask & 2) ? V8_PCM_V91 : 0),
		      (mask & 8) ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK &&
		      v8_joint_menu_select(&caller, &answerer, &joint) == V8_MENU_OK &&
		      v8_menu_encode(&joint, information, sizeof(information), &count) == V8_MENU_OK &&
		      v8_menu_decode(information, count, &decoded) == V8_MENU_OK &&
		      v8_pcm_select_roles(&caller, &decoded, &roles) == V8_MENU_OK,
		      "digital PCM menu did not round trip, mask=%u", mask);
		bool duplex = roles.caller == V8_PCM_ROLE_DIGITAL && roles.answerer == V8_PCM_ROLE_DIGITAL;
		CHECK(duplex == (mask == 15), "V.91 selected without mutual digital capability, mask=%u", mask);
		if (mask == 15)
			CHECK(decoded.pcm_availability == V8_PCM_V91, "joint menu retained ambiguous legacy roles");
		else if (mask & 8)
			CHECK(roles.caller == V8_PCM_ROLE_ANALOG && roles.answerer == V8_PCM_ROLE_DIGITAL,
			      "legacy digital answerer preference changed");
	}
	CHECK(v8_pcm_call_menu_init(&caller, V8_PCM_V91, V8_PSTN_DIGITAL, false) == V8_MENU_OK &&
	      v8_joint_menu_select(&caller, &caller, &joint) == V8_MENU_OK &&
	      v8_pcm_select_roles(&caller, &joint, &roles) == V8_MENU_OK &&
	      roles.caller == V8_PCM_ROLE_DIGITAL && roles.answerer == V8_PCM_ROLE_DIGITAL,
	      "explicit V.91 menus require an unrelated PCM role");
	return true;
}

static bool test_v91_wire_without_v34(void)
{
	/* V.8 (11/2000), 6.3 and 7.4: V.34 is required for pcm0 b5/b6,
	 * not b7 (V.91). Zero legacy capabilities can still select V.91. */
	const uint8_t pcm_only[] = { 0xc1, 0x25, 0x8d, 0x87 };
	const uint8_t with_v21[] = { 0xc1, 0x25, 0x10, 0x90, 0x8d, 0x87 };
	const struct { const uint8_t *wire; size_t count; uint16_t fallback; } cases[] = {
		{ pcm_only, sizeof(pcm_only), 0 },
		{ with_v21, sizeof(with_v21), V8_MODULATION_V21 },
	};
	struct v8_menu call, peer, joint;
	struct v8_pcm_roles roles;
	uint8_t encoded[V8_MAX_INFORMATION_OCTETS];
	size_t count;
	enum v8_modulation selected;

	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		CHECK(v8_menu_decode(cases[i].wire, cases[i].count, &call) == V8_MENU_OK &&
		      call.modulations == cases[i].fallback && v8_menu_is_usable(&call) &&
		      v8_menu_encode(&call, encoded, sizeof(encoded), &count) == V8_MENU_OK &&
		      count == cases[i].count && !memcmp(encoded, cases[i].wire, count),
		      "V.91 wire requires an unrelated V.34 bit, case %zu", i);
		CHECK(v8_joint_menu_select(&call, &call, &joint) == V8_MENU_OK &&
		      v8_menu_selected_modulation(&call, &joint, &selected) == V8_MENU_OK &&
		      selected == cases[i].fallback &&
		      v8_menu_selection_is_valid(&call, &joint, selected) &&
		      v8_pcm_select_roles(&call, &joint, &roles) == V8_MENU_OK &&
		      roles.caller == V8_PCM_ROLE_DIGITAL && roles.answerer == V8_PCM_ROLE_DIGITAL,
		      "V.91 PCM selection lost its legacy fallback");
		for (unsigned role = V8_PCM_ANALOG; role <= V8_PCM_DIGITAL; role <<= 1) {
			peer = call;
			peer.pcm_availability = role;
			CHECK(v8_menu_encode(&peer, encoded, sizeof(encoded), &count) ==
			      V8_MENU_INVALID_ARGUMENT, "V.90/V.92 accepted missing V.34 support");
		}
	}
	CHECK(v8_menu_decode(pcm_only, sizeof(pcm_only), &call) == V8_MENU_OK &&
	      v8_call_menu_init(&peer, V8_MODULATION_V34_DUPLEX, false) == V8_MENU_OK &&
	      v8_joint_menu_select(&call, &peer, &joint) == V8_MENU_OK &&
	      !joint.pcm_present && !joint.modulations && !v8_menu_is_usable(&joint),
	      "non-V.91 peer selected a PCM-only offer");
	selected = V8_MODULATION_V21;
	CHECK(v8_menu_selected_modulation(&call, &joint, &selected) ==
	      V8_MENU_NO_COMMON_MODULATION && selected == V8_MODULATION_V21 &&
	      !v8_menu_selection_is_valid(&call, &joint, V8_MODULATION_NONE),
	      "empty refusal became a successful NONE selection");
	peer = call;
	peer.pstn_access = 0;
	CHECK(!v8_menu_is_usable(&peer) &&
	      v8_joint_menu_select(&call, &peer, &joint) == V8_MENU_OK &&
	      v8_menu_selected_modulation(&call, &joint, &selected) ==
	      V8_MENU_NO_COMMON_MODULATION && selected == V8_MODULATION_V21,
	      "V.91 selected without mutual digital access");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "V.21 data menu", test_v21_data_menu },
		{ "modulation mapping", test_modulation_mapping },
		{ "all modulation combinations", test_all_modulation_combinations },
		{ "minimal modulation width", test_minimal_modulation_width },
		{ "extended zero capabilities", test_extended_zero_capabilities },
		{ "menu usability", test_menu_usability },
		{ "decode round trip", test_decode_round_trip },
		{ "PCM categories", test_pcm_categories },
		{ "PCM category rejection", test_pcm_category_rejection },
		{ "future categories", test_future_categories },
		{ "decode rejection", test_decode_rejection },
		{ "joint menu selection", test_joint_menu_selection },
		{ "PCM role selection", test_pcm_role_selection },
		{ "bidirectional digital PCM", test_bidirectional_pcm_roles },
		{ "V.91 wire without V.34", test_v91_wire_without_v34 },
		{ "PCM role fallback", test_pcm_role_fallback },
		{ "PCM with non-PCM peer", test_pcm_with_non_pcm_peer },
		{ "no common modulation", test_no_common_modulation },
		{ "joint validation", test_joint_validation },
		{ "empty PCM refusal", test_empty_pcm_refusal },
		{ "unindicated PCM roles", test_unindicated_pcm_roles },
		{ "empty PCM refusal without access", test_empty_pcm_refusal_without_access },
		{ "repeated menu confirmation", test_repeated_menu_confirmation },
		{ "mismatched menu restart", test_mismatched_menu_restart },
		{ "malformed menu recovery", test_malformed_menu_recovery },
		{ "menu detector validation", test_menu_detector_validation },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
