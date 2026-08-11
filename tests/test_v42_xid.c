// SPDX-License-Identifier: BSD-3-Clause

#include <v42_xid.h>
#include <v44_xid.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool fields_equal(
	const struct sippymodem_v42_xid_fields *left,
	const struct sippymodem_v42_xid_fields *right)
{
	return left->parameter_negotiation == right->parameter_negotiation &&
	       left->parameter_negotiation_length ==
		       right->parameter_negotiation_length &&
	       left->private_negotiation == right->private_negotiation &&
	       left->private_negotiation_length ==
		       right->private_negotiation_length &&
	       left->user_data == right->user_data &&
	       left->user_data_length == right->user_data_length;
}

static bool test_standard_envelope(void)
{
	static const uint8_t parameters[] = {
		0x03, 0x04, 0x8a, 0x89, 0x00, 0x00,
		0x05, 0x02, 0x04, 0x00,
		0x06, 0x02, 0x04, 0x00,
		0x07, 0x01, 0x0f,
		0x08, 0x01, 0x0f,
	};
	static const uint8_t private_parameters[] = {
		0x00, 0x03, 'V', '4', '2',
		0x01, 0x01, 0x03,
		0x02, 0x02, 0x04, 0x00,
		0x03, 0x01, 0x20,
	};
	const struct sippymodem_v44_offer v44_offer =
		sippymodem_v44_default_offer();
	struct sippymodem_v44_offer decoded_v44 = { 0 };
	uint8_t v44[SIPPYMODEM_V44_XID_OFFER_SIZE];
	uint8_t wire[128];
	uint8_t rebuilt[sizeof(wire)];
	struct sippymodem_v42_xid_fields fields;
	struct sippymodem_v42_xid_fields decoded = { 0 };
	size_t v44_length = 0;
	size_t wire_length = 0;
	size_t rebuilt_length = 0;
	const size_t private_offset = 1U + 3U + sizeof(parameters);
	const size_t user_offset =
		private_offset + 3U + sizeof(private_parameters);

	CHECK(sippymodem_v44_xid_encode(
		      &v44_offer, v44, sizeof(v44), &v44_length) ==
		      SIPPYMODEM_V44_XID_OK &&
	      v44_length == sizeof(v44),
	      "could not build a V.44 user-data subfield");
	fields = (struct sippymodem_v42_xid_fields) {
		.parameter_negotiation = parameters,
		.parameter_negotiation_length = sizeof(parameters),
		.private_negotiation = private_parameters,
		.private_negotiation_length = sizeof(private_parameters),
		.user_data = v44,
		.user_data_length = v44_length,
	};
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_OK &&
	      wire_length == user_offset + sizeof(v44) &&
	      wire[0] == SIPPYMODEM_V42_XID_GENERAL_FORMAT &&
	      wire[1] == 0x80 && wire[2] == 0 &&
	      wire[3] == sizeof(parameters) &&
	      wire[private_offset] == 0xf0 &&
	      wire[private_offset + 1U] == 0 &&
	      wire[private_offset + 2U] == sizeof(private_parameters) &&
	      memcmp(wire + user_offset, v44, sizeof(v44)) == 0,
	      "V.42 XID groups were not encoded in standard order");
	CHECK(sippymodem_v42_xid_decode(
		      wire, wire_length, &decoded) == SIPPYMODEM_V42_XID_OK &&
	      decoded.parameter_negotiation == wire + 4 &&
	      decoded.parameter_negotiation_length == sizeof(parameters) &&
	      decoded.private_negotiation == wire + private_offset + 3U &&
	      decoded.private_negotiation_length ==
		      sizeof(private_parameters) &&
	      decoded.user_data == wire + user_offset &&
	      decoded.user_data_length == sizeof(v44) &&
	      sippymodem_v44_xid_decode(
		      decoded.user_data, decoded.user_data_length,
		      &decoded_v44) == SIPPYMODEM_V44_XID_OK,
	      "V.42 XID envelope did not expose its bounded subfields");
	CHECK(sippymodem_v42_xid_encode(
		      &decoded, rebuilt, sizeof(rebuilt), &rebuilt_length) ==
		      SIPPYMODEM_V42_XID_OK &&
	      rebuilt_length == wire_length &&
	      memcmp(rebuilt, wire, wire_length) == 0,
	      "known V.42 XID groups did not round trip");
	return true;
}

static bool test_optional_and_unknown_groups(void)
{
	static const uint8_t unknown[] = {
		0x82,
		0x20, 0x00, 0x03, 0x90, 0x01, 0x55,
		0xff, 0xfe, 0x01, 0xa5,
	};
	static const uint8_t expected_user_only[] = {
		0x82, 0xff, 0xfe, 0x01, 0xa5,
	};
	const struct sippymodem_v42_xid_fields empty = { 0 };
	struct sippymodem_v42_xid_fields decoded = { 0 };
	uint8_t wire[sizeof(unknown)];
	size_t wire_length = 0;

	CHECK(sippymodem_v42_xid_encode(
		      &empty, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_OK &&
	      wire_length == 1 &&
	      wire[0] == SIPPYMODEM_V42_XID_GENERAL_FORMAT &&
	      sippymodem_v42_xid_decode(wire, wire_length, &decoded) ==
		      SIPPYMODEM_V42_XID_OK &&
	      decoded.parameter_negotiation == NULL &&
	      decoded.private_negotiation == NULL &&
	      decoded.user_data == NULL,
	      "empty optional V.42 XID groups were not omitted");
	CHECK(sippymodem_v42_xid_decode(
		      unknown, sizeof(unknown), &decoded) ==
		      SIPPYMODEM_V42_XID_OK &&
	      decoded.parameter_negotiation == NULL &&
	      decoded.private_negotiation == NULL &&
	      decoded.user_data == unknown + 7 &&
	      decoded.user_data_length == 4,
	      "unknown V.42 data-link group was not ignored");
	CHECK(sippymodem_v42_xid_encode(
		      &decoded, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_OK &&
	      wire_length == sizeof(expected_user_only) &&
	      memcmp(wire, expected_user_only, sizeof(expected_user_only)) == 0,
	      "known V.42 XID fields changed after an unknown group");
	return true;
}

static bool test_malformed_envelopes(void)
{
	static const uint8_t wrong_format[] = { 0x81 };
	static const uint8_t short_group[] = { 0x82, 0x80, 0x00 };
	static const uint8_t long_group[] = {
		0x82, 0x80, 0x00, 0x03, 0x03, 0x01,
	};
	static const uint8_t empty_group[] = { 0x82, 0x80, 0x00, 0x00 };
	static const uint8_t short_parameter[] = {
		0x82, 0x80, 0x00, 0x01, 0x03,
	};
	static const uint8_t long_parameter[] = {
		0x82, 0x80, 0x00, 0x03, 0x03, 0x02, 0x00,
	};
	static const uint8_t duplicate_group[] = {
		0x82,
		0x80, 0x00, 0x03, 0x03, 0x01, 0x00,
		0x80, 0x00, 0x03, 0x03, 0x01, 0x00,
	};
	static const uint8_t descending_groups[] = {
		0x82,
		0xf0, 0x00, 0x03, 0x00, 0x01, 0x00,
		0x80, 0x00, 0x03, 0x03, 0x01, 0x00,
	};
	static const uint8_t empty_user_data[] = { 0x82, 0xff };
	static const uint8_t short_user_parameter[] = {
		0x82, 0xff, 0x40, 0x03, 'V',
	};
	static const struct {
		const uint8_t *wire;
		size_t length;
	} malformed[] = {
		{ wrong_format, sizeof(wrong_format) },
		{ short_group, sizeof(short_group) },
		{ long_group, sizeof(long_group) },
		{ empty_group, sizeof(empty_group) },
		{ short_parameter, sizeof(short_parameter) },
		{ long_parameter, sizeof(long_parameter) },
		{ duplicate_group, sizeof(duplicate_group) },
		{ descending_groups, sizeof(descending_groups) },
		{ empty_user_data, sizeof(empty_user_data) },
		{ short_user_parameter, sizeof(short_user_parameter) },
	};
	const struct sippymodem_v42_xid_fields unchanged = {
		.parameter_negotiation = wrong_format,
		.parameter_negotiation_length = 11,
		.private_negotiation = short_group,
		.private_negotiation_length = 13,
		.user_data = empty_user_data,
		.user_data_length = 17,
	};

	for (size_t index = 0; index < ARRAY_LENGTH(malformed); ++index) {
		struct sippymodem_v42_xid_fields decoded = unchanged;

		CHECK(sippymodem_v42_xid_decode(
			      malformed[index].wire, malformed[index].length,
			      &decoded) == SIPPYMODEM_V42_XID_MALFORMED &&
		      fields_equal(&decoded, &unchanged),
		      "malformed V.42 XID case %zu changed caller state", index);
	}
	return true;
}

static bool test_wide_group_length(void)
{
	uint8_t parameter[257];
	uint8_t wire[1U + 3U + sizeof(parameter)];
	struct sippymodem_v42_xid_fields fields;
	struct sippymodem_v42_xid_fields decoded;
	size_t wire_length = 0;

	parameter[0] = 0x7e;
	parameter[1] = 255;
	for (size_t index = 2; index < sizeof(parameter); ++index)
		parameter[index] = (uint8_t)(index * 37U + 11U);
	fields = (struct sippymodem_v42_xid_fields) {
		.parameter_negotiation = parameter,
		.parameter_negotiation_length = sizeof(parameter),
	};
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_OK &&
	      wire_length == sizeof(wire) &&
	      wire[0] == SIPPYMODEM_V42_XID_GENERAL_FORMAT &&
	      wire[1] == 0x80 && wire[2] == 0x01 && wire[3] == 0x01 &&
	      memcmp(wire + 4, parameter, sizeof(parameter)) == 0,
	      "wide V.42 XID group length was encoded incorrectly");
	CHECK(sippymodem_v42_xid_decode(
		      wire, wire_length, &decoded) == SIPPYMODEM_V42_XID_OK &&
	      decoded.parameter_negotiation == wire + 4 &&
	      decoded.parameter_negotiation_length == sizeof(parameter),
	      "wide V.42 XID group did not decode");
	decoded = fields;
	CHECK(sippymodem_v42_xid_decode(
		      wire, wire_length - 1U, &decoded) ==
		      SIPPYMODEM_V42_XID_MALFORMED &&
	      fields_equal(&decoded, &fields),
	      "truncated wide V.42 XID group changed caller state");
	return true;
}

static bool test_api_and_backpressure(void)
{
	static const uint8_t parameter[] = { 0x03, 0x01, 0x00 };
	static const uint8_t bad_user[] = { 0xfe, 0x01, 0x00 };
	struct sippymodem_v42_xid_fields fields = {
		.parameter_negotiation = parameter,
		.parameter_negotiation_length = sizeof(parameter),
	};
	uint8_t wire[7];
	size_t wire_length = 77;
	struct sippymodem_v42_xid_fields decoded = { 0 };
	static const char *const names[] = {
		"ok", "need output", "malformed", "invalid argument",
	};

	memset(wire, 0xa5, sizeof(wire));
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire) - 1U, &wire_length) ==
		      SIPPYMODEM_V42_XID_NEED_OUTPUT &&
	      wire_length == 77 && wire[0] == 0xa5 &&
	      sippymodem_v42_xid_encode(
		      &fields, NULL, 0, &wire_length) ==
		      SIPPYMODEM_V42_XID_NEED_OUTPUT &&
	      wire_length == 77,
	      "V.42 XID output backpressure changed caller state");
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_OK &&
	      wire_length == sizeof(wire),
	      "V.42 XID exact output capacity was rejected");
	fields.parameter_negotiation = NULL;
	memset(wire, 0xa5, sizeof(wire));
	wire_length = 77;
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT &&
	      wire_length == 77 && wire[0] == 0xa5,
	      "invalid V.42 XID input changed caller output");
	fields.parameter_negotiation = parameter;
	fields.parameter_negotiation_length = (size_t)UINT16_MAX + 1U;
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT,
	      "V.42 XID accepted an unencodable group length");
	fields = (struct sippymodem_v42_xid_fields) {
		.user_data = bad_user,
		.user_data_length = sizeof(bad_user),
	};
	CHECK(sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT &&
	      sippymodem_v42_xid_encode(
		      NULL, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT &&
	      sippymodem_v42_xid_encode(
		      &fields, wire, sizeof(wire), NULL) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT &&
	      sippymodem_v42_xid_decode(NULL, 0, &decoded) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT &&
	      sippymodem_v42_xid_decode(wire, sizeof(wire), NULL) ==
		      SIPPYMODEM_V42_XID_INVALID_ARGUMENT &&
	      sippymodem_v42_xid_decode(wire, 0, &decoded) ==
		      SIPPYMODEM_V42_XID_MALFORMED,
	      "V.42 XID API accepted an invalid argument");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      sippymodem_v42_xid_result_name(
				      (enum sippymodem_v42_xid_result)index),
			      names[index]) == 0,
		      "V.42 XID result %zu has the wrong name", index);
	CHECK(strcmp(
		      sippymodem_v42_xid_result_name(
			      (enum sippymodem_v42_xid_result)99),
		      "unknown") == 0,
	      "unknown V.42 XID result has the wrong name");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "standard envelope", test_standard_envelope },
		{ "optional and unknown groups",
		  test_optional_and_unknown_groups },
		{ "malformed envelopes", test_malformed_envelopes },
		{ "wide group length", test_wide_group_length },
		{ "API and backpressure", test_api_and_backpressure },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
