// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v8_menu.h>

#include "test.h"

#include <string.h>

static bool encode_equals(
	uint16_t modulations, bool lapm, const uint8_t *expected,
	size_t expected_count)
{
	struct sippymodem_v8_menu menu;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count = 0;

	return sippymodem_v8_call_menu_init(&menu, modulations, lapm) ==
		       SIPPYMODEM_V8_MENU_OK &&
	       sippymodem_v8_menu_encode(
		       &menu, information, sizeof(information),
		       &information_count) == SIPPYMODEM_V8_MENU_OK &&
	       information_count == expected_count &&
	       memcmp(information, expected, expected_count) == 0;
}

static bool test_v21_data_menu(void)
{
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x90), UINT8_C(0x2a),
	};

	CHECK(encode_equals(
		      SIPPYMODEM_V8_MODULATION_V21, true,
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
		      SIPPYMODEM_V8_MODULATION_ALL, false,
		      expected, ARRAY_LENGTH(expected)),
	      "V.8 modulation mapping changed");
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
		      SIPPYMODEM_V8_MODULATION_V34_DUPLEX, false,
		      v34, ARRAY_LENGTH(v34)) &&
	      encode_equals(
		      SIPPYMODEM_V8_MODULATION_V32, false,
		      v32, ARRAY_LENGTH(v32)) &&
	      encode_equals(
		      SIPPYMODEM_V8_MODULATION_V21, false,
		      v21, ARRAY_LENGTH(v21)),
	      "V.8 modulation menu was not minimally extended");
	return true;
}

static bool test_extended_zero_capabilities(void)
{
	struct sippymodem_v8_menu menu = {
		.call_function = SIPPYMODEM_V8_CALL_DATA,
		.modulation_octets = 5,
	};
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x10), UINT8_C(0x10), UINT8_C(0x10),
	};
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t count = 0;

	CHECK(sippymodem_v8_menu_encode(
		      &menu, information, sizeof(information), &count) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      count == ARRAY_LENGTH(expected) &&
	      memcmp(information, expected, sizeof(expected)) == 0,
	      "V.8 zero-capability extension width changed");
	return true;
}

static bool test_decode_round_trip(void)
{
	static const uint16_t capabilities[] = {
		SIPPYMODEM_V8_MODULATION_V34_DUPLEX,
		SIPPYMODEM_V8_MODULATION_V32,
		SIPPYMODEM_V8_MODULATION_V21,
		SIPPYMODEM_V8_MODULATION_ALL,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(capabilities); ++index) {
		struct sippymodem_v8_menu encoded;
		struct sippymodem_v8_menu decoded;
		uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
		size_t information_count;
		bool lapm = (index & 1u) != 0;

		CHECK(sippymodem_v8_call_menu_init(
			      &encoded, capabilities[index], lapm) ==
			      SIPPYMODEM_V8_MENU_OK &&
		      sippymodem_v8_menu_encode(
			      &encoded, information, sizeof(information),
			      &information_count) == SIPPYMODEM_V8_MENU_OK &&
		      sippymodem_v8_menu_decode(
			      information, information_count, &decoded) ==
			      SIPPYMODEM_V8_MENU_OK &&
		      decoded.call_function == encoded.call_function &&
		      decoded.modulations == encoded.modulations &&
		      decoded.modulation_octets == encoded.modulation_octets &&
		      decoded.lapm == encoded.lapm,
		      "V.8 menu round trip changed case %zu", index);
	}
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
	struct sippymodem_v8_menu menu;
	uint8_t encoded[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t encoded_count;

	CHECK(sippymodem_v8_menu_decode(
		      information, ARRAY_LENGTH(information), &menu) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      menu.modulations == SIPPYMODEM_V8_MODULATION_V21 &&
	      menu.modulation_octets == 4 && menu.lapm &&
	      sippymodem_v8_menu_encode(
		      &menu, encoded, sizeof(encoded), &encoded_count) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      encoded_count == ARRAY_LENGTH(normalized) &&
	      memcmp(encoded, normalized, sizeof(normalized)) == 0,
	      "V.8 decoder did not preserve future extension width");
	return true;
}

static bool decode_has_result(
	const uint8_t *information, size_t information_count,
	enum sippymodem_v8_menu_result expected)
{
	struct sippymodem_v8_menu menu = {
		.call_function = SIPPYMODEM_V8_CALL_DATA,
		.modulations = UINT16_MAX,
		.modulation_octets = UINT8_MAX,
		.lapm = true,
	};
	struct sippymodem_v8_menu saved = menu;

	return sippymodem_v8_menu_decode(
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
	uint8_t too_long[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS + 1] = { 0 };

	CHECK(decode_has_result(NULL, 1, SIPPYMODEM_V8_MENU_INVALID_ARGUMENT) &&
	      decode_has_result(wrong_first, 0, SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      wrong_first, ARRAY_LENGTH(wrong_first),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      unsupported_call, ARRAY_LENGTH(unsupported_call),
		      SIPPYMODEM_V8_MENU_UNSUPPORTED_CALL_FUNCTION) &&
	      decode_has_result(
		      missing_modulation, ARRAY_LENGTH(missing_modulation),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      orphan_extension, ARRAY_LENGTH(orphan_extension),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      malformed_extension, ARRAY_LENGTH(malformed_extension),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      duplicate_modulation, ARRAY_LENGTH(duplicate_modulation),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      duplicate_protocol, ARRAY_LENGTH(duplicate_protocol),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      unexpected_protocol_extension,
		      ARRAY_LENGTH(unexpected_protocol_extension),
		      SIPPYMODEM_V8_MENU_MALFORMED) &&
	      decode_has_result(
		      too_long, ARRAY_LENGTH(too_long),
		      SIPPYMODEM_V8_MENU_MALFORMED),
	      "V.8 decoder accepted malformed information");
	return true;
}

static bool test_joint_menu_selection(void)
{
	struct sippymodem_v8_menu call_menu;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu joint_menu;
	enum sippymodem_v8_modulation selected;
	uint16_t local_modulations =
		SIPPYMODEM_V8_MODULATION_V34_HALF_DUPLEX |
		SIPPYMODEM_V8_MODULATION_V32 |
		SIPPYMODEM_V8_MODULATION_V21;

	CHECK(sippymodem_v8_call_menu_init(
		      &call_menu, SIPPYMODEM_V8_MODULATION_ALL, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_call_menu_init(
		      &local_menu, local_modulations, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_joint_menu_select(
		      &call_menu, &local_menu, &joint_menu) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      joint_menu.call_function == SIPPYMODEM_V8_CALL_DATA &&
	      joint_menu.modulations == local_modulations &&
	      joint_menu.modulation_octets == call_menu.modulation_octets &&
	      joint_menu.lapm &&
	      sippymodem_v8_menu_selected_modulation(
		      &joint_menu, &selected) == SIPPYMODEM_V8_MENU_OK &&
	      selected == SIPPYMODEM_V8_MODULATION_V34_HALF_DUPLEX,
	      "V.8 joint menu did not select the first common mode");
	return true;
}

static bool test_no_common_modulation(void)
{
	static const uint8_t expected[] = {
		UINT8_C(0xc1), UINT8_C(0x05), UINT8_C(0x10),
		UINT8_C(0x10), UINT8_C(0x2a),
	};
	struct sippymodem_v8_menu call_menu;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu joint_menu;
	enum sippymodem_v8_modulation selected =
		SIPPYMODEM_V8_MODULATION_V21;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;

	CHECK(sippymodem_v8_call_menu_init(
		      &call_menu, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_call_menu_init(
		      &local_menu, SIPPYMODEM_V8_MODULATION_V34_DUPLEX, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_joint_menu_select(
		      &call_menu, &local_menu, &joint_menu) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      joint_menu.modulations == 0 &&
	      joint_menu.modulation_octets == 3 && joint_menu.lapm &&
	      sippymodem_v8_menu_encode(
		      &joint_menu, information, sizeof(information),
		      &information_count) == SIPPYMODEM_V8_MENU_OK &&
	      information_count == ARRAY_LENGTH(expected) &&
	      memcmp(information, expected, sizeof(expected)) == 0 &&
	      sippymodem_v8_menu_selected_modulation(
		      &joint_menu, &selected) ==
		      SIPPYMODEM_V8_MENU_NO_COMMON_MODULATION &&
	      selected == SIPPYMODEM_V8_MODULATION_V21,
	      "V.8 no-common-mode joint menu changed");
	return true;
}

static bool test_joint_validation(void)
{
	struct sippymodem_v8_menu call_menu;
	struct sippymodem_v8_menu local_menu;
	struct sippymodem_v8_menu joint_menu = {
		.modulations = UINT16_MAX,
	};
	struct sippymodem_v8_menu saved = joint_menu;
	enum sippymodem_v8_modulation selected =
		SIPPYMODEM_V8_MODULATION_V21;

	CHECK(sippymodem_v8_call_menu_init(
		      &call_menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_call_menu_init(
		      &local_menu, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK,
	      "V.8 joint validation fixtures did not initialize");
	call_menu.modulations = 0;
	CHECK(sippymodem_v8_joint_menu_select(
		      &call_menu, &local_menu, &joint_menu) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      memcmp(&joint_menu, &saved, sizeof(joint_menu)) == 0 &&
	      sippymodem_v8_joint_menu_select(
		      NULL, &local_menu, &joint_menu) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_selected_modulation(
		      NULL, &selected) == SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_selected_modulation(
		      &local_menu, NULL) == SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      strcmp(sippymodem_v8_menu_result_name(
			     SIPPYMODEM_V8_MENU_NO_COMMON_MODULATION),
		     "no common modulation") == 0,
	      "V.8 joint menu accepted invalid state");
	return true;
}

static bool feed_control_bits(
	struct sippymodem_v8_menu_detector *detector,
	const uint8_t *information, size_t information_count, size_t bit_count,
	struct sippymodem_v8_menu *menu, bool *confirmed)
{
	struct sippymodem_v8_control_generator generator;

	if (sippymodem_v8_control_generator_init(
		    &generator, information, information_count) !=
	    SIPPYMODEM_V8_FRAMING_OK)
		return false;
	for (size_t index = 0; index < bit_count; ++index) {
		bool complete;
		uint8_t bit;

		if (sippymodem_v8_control_next_bit(
			    &generator, &bit, &complete) !=
			    SIPPYMODEM_V8_FRAMING_OK ||
		    sippymodem_v8_menu_detect_bit(
			    detector, bit, menu, confirmed) !=
			    SIPPYMODEM_V8_MENU_OK)
			return false;
	}
	return true;
}

static bool test_repeated_menu_confirmation(void)
{
	struct sippymodem_v8_menu offered;
	struct sippymodem_v8_menu detected = {
		.modulations = UINT16_MAX,
	};
	struct sippymodem_v8_menu saved = detected;
	struct sippymodem_v8_menu_detector detector;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;
	size_t sequence_bits;
	bool confirmed = true;

	CHECK(sippymodem_v8_call_menu_init(
		      &offered, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_encode(
		      &offered, information, sizeof(information),
		      &information_count) == SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_detector_init(&detector) ==
		      SIPPYMODEM_V8_MENU_OK,
	      "V.8 repeated-menu endpoints did not initialize");
	sequence_bits = SIPPYMODEM_V8_PREAMBLE_BITS +
			information_count * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	confirmed = false;
	CHECK(feed_control_bits(
		      &detector, information, information_count,
		      2u * sequence_bits, &detected, &confirmed) &&
	      !confirmed && memcmp(&detected, &saved, sizeof(detected)) == 0 &&
	      feed_control_bits(
		      &detector, information, information_count,
		      SIPPYMODEM_V8_PREAMBLE_BITS, &detected, &confirmed) &&
	      confirmed && detected.modulations == offered.modulations &&
	      detected.modulation_octets == offered.modulation_octets &&
	      detected.lapm,
	      "V.8 detector did not require two complete identical menus");
	return true;
}

static bool test_mismatched_menu_restart(void)
{
	struct sippymodem_v8_menu first;
	struct sippymodem_v8_menu second;
	struct sippymodem_v8_menu detected = { 0 };
	struct sippymodem_v8_menu_detector detector;
	uint8_t first_information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	uint8_t second_information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t first_count;
	size_t second_count;
	size_t first_bits;
	size_t second_bits;
	bool confirmed = false;

	CHECK(sippymodem_v8_call_menu_init(
		      &first, SIPPYMODEM_V8_MODULATION_V32, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_call_menu_init(
		      &second, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_encode(
		      &first, first_information, sizeof(first_information),
		      &first_count) == SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_encode(
		      &second, second_information, sizeof(second_information),
		      &second_count) == SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_detector_init(&detector) ==
		      SIPPYMODEM_V8_MENU_OK,
	      "V.8 mismatch endpoints did not initialize");
	first_bits = SIPPYMODEM_V8_PREAMBLE_BITS +
		     first_count * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	second_bits = SIPPYMODEM_V8_PREAMBLE_BITS +
		      second_count * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	CHECK(feed_control_bits(
		      &detector, first_information, first_count, first_bits,
		      &detected, &confirmed) &&
	      feed_control_bits(
		      &detector, second_information, second_count,
		      2u * second_bits, &detected, &confirmed) &&
	      !confirmed &&
	      feed_control_bits(
		      &detector, second_information, second_count,
		      SIPPYMODEM_V8_PREAMBLE_BITS, &detected, &confirmed) &&
	      confirmed &&
	      detected.modulations == SIPPYMODEM_V8_MODULATION_V21,
	      "V.8 mismatch did not restart consecutive confirmation");
	return true;
}

static bool test_malformed_menu_recovery(void)
{
	static const uint8_t malformed[] = {
		UINT8_C(0xc1), UINT8_C(0x2a),
	};
	struct sippymodem_v8_menu valid;
	struct sippymodem_v8_menu detected = { 0 };
	struct sippymodem_v8_menu_detector detector;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;
	size_t malformed_bits =
		SIPPYMODEM_V8_PREAMBLE_BITS +
		ARRAY_LENGTH(malformed) * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	size_t valid_bits;
	bool confirmed = false;

	CHECK(sippymodem_v8_call_menu_init(
		      &valid, SIPPYMODEM_V8_MODULATION_V21, false) ==
		      SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_encode(
		      &valid, information, sizeof(information),
		      &information_count) == SIPPYMODEM_V8_MENU_OK &&
	      sippymodem_v8_menu_detector_init(&detector) ==
		      SIPPYMODEM_V8_MENU_OK,
	      "V.8 malformed recovery endpoints did not initialize");
	valid_bits = SIPPYMODEM_V8_PREAMBLE_BITS +
		     information_count * SIPPYMODEM_V8_FRAMED_OCTET_BITS;
	CHECK(feed_control_bits(
		      &detector, malformed, ARRAY_LENGTH(malformed),
		      2u * malformed_bits + SIPPYMODEM_V8_PREAMBLE_BITS,
		      &detected, &confirmed) &&
	      !confirmed &&
	      feed_control_bits(
		      &detector, information, information_count,
		      2u * valid_bits + SIPPYMODEM_V8_PREAMBLE_BITS,
		      &detected, &confirmed) &&
	      confirmed && detected.modulations == valid.modulations,
	      "V.8 detector did not recover after malformed repeats");
	return true;
}

static bool test_menu_detector_validation(void)
{
	struct sippymodem_v8_menu_detector detector;
	struct sippymodem_v8_menu_detector saved;
	struct sippymodem_v8_menu menu = {
		.modulations = UINT16_MAX,
	};
	struct sippymodem_v8_menu saved_menu = menu;
	bool confirmed = true;

	CHECK(sippymodem_v8_menu_detector_init(&detector) ==
		      SIPPYMODEM_V8_MENU_OK,
	      "V.8 menu detector validation did not initialize");
	detector.current_count = SIPPYMODEM_V8_MAX_INFORMATION_OCTETS + 1u;
	saved = detector;
	CHECK(sippymodem_v8_menu_detector_init(NULL) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_detect_bit(
		      &detector, 0, &menu, &confirmed) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
	      memcmp(&menu, &saved_menu, sizeof(menu)) == 0 && confirmed &&
	      sippymodem_v8_menu_detect_bit(NULL, 0, &menu, &confirmed) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_detect_bit(&saved, 2, &menu, &confirmed) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT,
	      "V.8 menu detector accepted invalid state");
	sippymodem_v8_menu_detector_reset(&detector);
	CHECK(!detector.sequence_open && detector.current_count == 0 &&
		      !detector.confirmed,
	      "V.8 menu detector reset retained state");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v8_menu menu;
	struct sippymodem_v8_menu invalid;
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	uint8_t saved_information[sizeof(information)];
	size_t count = SIZE_MAX;

	memset(information, UINT8_C(0xa5), sizeof(information));
	memcpy(saved_information, information, sizeof(information));
	CHECK(sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_V21, true) ==
		      SIPPYMODEM_V8_MENU_OK,
	      "V.8 validation menu did not initialize");
	invalid = menu;
	invalid.modulation_octets = 2;
	CHECK(sippymodem_v8_call_menu_init(NULL, 1, false) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_call_menu_init(&menu, 0, false) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_call_menu_init(
		      &menu, SIPPYMODEM_V8_MODULATION_ALL + 1u, false) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_encode(
		      &invalid, information, sizeof(information), &count) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      memcmp(information, saved_information, sizeof(information)) == 0 &&
	      count == SIZE_MAX,
	      "V.8 menu accepted invalid capabilities or changed output");
	invalid = menu;
	CHECK(sippymodem_v8_menu_encode(
		      &invalid, information, 4, &count) ==
		      SIPPYMODEM_V8_MENU_BUFFER_TOO_SMALL &&
	      memcmp(information, saved_information, sizeof(information)) == 0 &&
	      count == SIZE_MAX &&
	      sippymodem_v8_menu_encode(NULL, information, 5, &count) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_encode(&invalid, NULL, 5, &count) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      sippymodem_v8_menu_encode(&invalid, information, 5, NULL) ==
		      SIPPYMODEM_V8_MENU_INVALID_ARGUMENT &&
	      strcmp(sippymodem_v8_menu_result_name(
			     SIPPYMODEM_V8_MENU_BUFFER_TOO_SMALL),
		     "buffer too small") == 0,
	      "V.8 menu validation was not transactional");
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
		{ "minimal modulation width", test_minimal_modulation_width },
		{ "extended zero capabilities", test_extended_zero_capabilities },
		{ "decode round trip", test_decode_round_trip },
		{ "future categories", test_future_categories },
		{ "decode rejection", test_decode_rejection },
		{ "joint menu selection", test_joint_menu_selection },
		{ "no common modulation", test_no_common_modulation },
		{ "joint validation", test_joint_validation },
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
