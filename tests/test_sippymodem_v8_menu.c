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
