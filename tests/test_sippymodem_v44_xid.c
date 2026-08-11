// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_xid.h>

#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	V44_XID_CAPABILITY_ONLY_SIZE = 9,
};

static bool parameters_equal(
	const struct sippymodem_v44_parameters *left,
	const struct sippymodem_v44_parameters *right)
{
	return left->codeword_limit == right->codeword_limit &&
	       left->max_string_length == right->max_string_length &&
	       left->history_size == right->history_size;
}

static bool offers_equal(
	const struct sippymodem_v44_offer *left,
	const struct sippymodem_v44_offer *right)
{
	return left->transmit.enabled == right->transmit.enabled &&
	       parameters_equal(
		       &left->transmit.parameters,
		       &right->transmit.parameters) &&
	       left->receive.enabled == right->receive.enabled &&
	       parameters_equal(
		       &left->receive.parameters, &right->receive.parameters);
}

static bool test_default_wire_image(void)
{
	static const uint8_t expected[] = {
		0xff,
		0x40, 0x03, 'V', '4', '4',
		0x41, 0x01, 0x00,
		0x42, 0x01, 0x03,
		0x43, 0x02, 0x04, 0x00,
		0x44, 0x02, 0x04, 0x00,
		0x45, 0x01, 0xff,
		0x46, 0x01, 0xff,
		0x47, 0x02, 0x0c, 0x00,
		0x48, 0x02, 0x0c, 0x00,
	};
	const struct sippymodem_v44_offer offer =
		sippymodem_v44_default_offer();
	struct sippymodem_v44_offer decoded = { 0 };
	uint8_t wire[sizeof(expected)];
	size_t wire_length = 0;

	CHECK(sizeof(expected) == SIPPYMODEM_V44_XID_OFFER_SIZE,
	      "V.44 XID public size changed");
	CHECK(sippymodem_v44_xid_encode(
		      &offer, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V44_XID_OK &&
	      wire_length == sizeof(expected) &&
	      memcmp(wire, expected, sizeof(expected)) == 0,
	      "default V.44 XID wire image changed");
	CHECK(sippymodem_v44_xid_decode(
		      wire, wire_length, &decoded) == SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&decoded, &offer),
	      "default V.44 XID offer did not round-trip");
	return true;
}

static bool test_directional_selection(void)
{
	const struct sippymodem_v44_offer local = {
		.transmit = {
			.enabled = true,
			.parameters = { 4096, 200, 4096 },
		},
		.receive = {
			.enabled = false,
			.parameters = { 2048, 100, 2000 },
		},
	};
	const struct sippymodem_v44_offer peer = {
		.transmit = {
			.enabled = true,
			.parameters = { 1024, 150, 3000 },
		},
		.receive = {
			.enabled = true,
			.parameters = { 8192, 80, 1000 },
		},
	};
	const struct sippymodem_v44_offer expected = {
		.transmit = {
			.enabled = true,
			.parameters = { 4096, 80, 1000 },
		},
		.receive = {
			.enabled = false,
			.parameters = { 1024, 100, 2000 },
		},
	};
	struct sippymodem_v44_offer selected = { 0 };
	struct sippymodem_v44_offer round_trip = { 0 };
	uint8_t wire[SIPPYMODEM_V44_XID_OFFER_SIZE];
	size_t wire_length = 0;

	CHECK(sippymodem_v44_xid_negotiate(&local, &peer, &selected) ==
		      SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&selected, &expected),
	      "V.44 directional negotiation selected the wrong limits");
	CHECK(sippymodem_v44_xid_encode(
		      &local, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V44_XID_OK &&
	      sippymodem_v44_xid_decode(
		      wire, wire_length, &round_trip) ==
		      SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&round_trip, &local),
	      "directional V.44 XID offer did not round-trip");
	selected = local;
	CHECK(sippymodem_v44_xid_negotiate(
		      &selected, &peer, &selected) == SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&selected, &expected),
	      "V.44 negotiation was not safe in place");
	return true;
}

static bool test_capability_only_defaults(void)
{
	static const uint8_t capability_only[] = {
		0xff, 0x40, 0x03, 'V', '4', '4', 0x41, 0x01, 0x01,
	};
	const struct sippymodem_v44_offer expected =
		sippymodem_v44_default_offer();
	struct sippymodem_v44_offer decoded = { 0 };

	CHECK(sizeof(capability_only) == V44_XID_CAPABILITY_ONLY_SIZE &&
	      sippymodem_v44_xid_decode(
		      capability_only, sizeof(capability_only), &decoded) ==
		      SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&decoded, &expected),
	      "capability-only V.44 XID did not select default parameters");
	return true;
}

static bool test_extensions_and_rejection(void)
{
	const struct sippymodem_v44_offer offer =
		sippymodem_v44_default_offer();
	const struct sippymodem_v44_offer unchanged = {
		.transmit = { .parameters = { 256, 32, 512 } },
		.receive = { .parameters = { 256, 32, 512 } },
	};
	struct sippymodem_v44_offer decoded;
	uint8_t wire[40];
	uint8_t malformed[sizeof(wire)];
	size_t wire_length = 0;

	CHECK(sippymodem_v44_xid_encode(
		      &offer, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V44_XID_OK,
	      "could not build V.44 XID rejection fixture");
	memcpy(malformed, wire, wire_length);
	malformed[wire_length] = 0x49;
	malformed[wire_length + 1U] = 1;
	malformed[wire_length + 2U] = 0xa5;
	decoded = unchanged;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length + 3U, &decoded) ==
		      SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&decoded, &offer),
	      "V.44 XID parser rejected an unknown extension");

	for (size_t length = 0; length < wire_length; ++length) {
		if (length == V44_XID_CAPABILITY_ONLY_SIZE)
			continue;
		decoded = unchanged;
		CHECK(sippymodem_v44_xid_decode(wire, length, &decoded) ==
			      SIPPYMODEM_V44_XID_MALFORMED &&
		      offers_equal(&decoded, &unchanged),
		      "V.44 XID truncation at %zu was not transactional", length);
	}

	memcpy(malformed, wire, wire_length);
	malformed[0] = 0xfe;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted the wrong user-data identifier");
	memcpy(malformed, wire, wire_length);
	malformed[3] = 'X';
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted the wrong parameter-set identifier");
	memcpy(malformed, wire, wire_length);
	malformed[2] = 2;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted a wrong parameter length");
	memcpy(malformed, wire, wire_length);
	malformed[8] = 1;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&decoded, &offer),
	      "V.44 XID treated later Parameter Mode support as a demand");
	malformed[8] = 0xc0;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_OK &&
	      offers_equal(&decoded, &offer),
	      "V.44 XID did not ignore packet capability bits on a modem link");
	malformed[8] = 2;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted a reserved capability bit");
	memcpy(malformed, wire, wire_length);
	malformed[11] = 4;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted a reserved request bit");
	memcpy(malformed, wire, wire_length);
	malformed[14] = 0;
	malformed[15] = 255;
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted an out-of-range resource proposal");
	memcpy(malformed, wire, wire_length);
	memmove(malformed + 20, malformed + 23, wire_length - 23U);
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length - 3U, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted a missing required parameter");
	memcpy(malformed, wire, wire_length);
	memcpy(malformed + wire_length, wire + 6, 3);
	CHECK(sippymodem_v44_xid_decode(
		      malformed, wire_length + 3U, &decoded) ==
		      SIPPYMODEM_V44_XID_MALFORMED,
	      "V.44 XID accepted a duplicate parameter");
	return true;
}

static bool test_api_contract(void)
{
	static const char *const names[] = {
		"ok", "need output", "malformed", "invalid argument",
	};
	struct sippymodem_v44_offer offer = sippymodem_v44_default_offer();
	struct sippymodem_v44_offer unchanged = offer;
	uint8_t wire[SIPPYMODEM_V44_XID_OFFER_SIZE];
	size_t wire_length = 77;

	memset(wire, 0xa5, sizeof(wire));
	CHECK(sippymodem_v44_xid_encode(
		      &offer, wire, sizeof(wire) - 1U, &wire_length) ==
		      SIPPYMODEM_V44_XID_NEED_OUTPUT &&
	      wire_length == 77 && wire[0] == 0xa5,
	      "V.44 XID short output changed caller state");
	offer.transmit.parameters.codeword_limit = 0;
	CHECK(sippymodem_v44_xid_encode(
		      &offer, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_encode(
		      NULL, wire, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_encode(
		      &unchanged, NULL, sizeof(wire), &wire_length) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_encode(
		      &unchanged, wire, sizeof(wire), NULL) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT,
	      "V.44 XID encoder accepted an invalid argument");
	CHECK(sippymodem_v44_xid_decode(NULL, 0, &offer) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_decode(wire, sizeof(wire), NULL) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_negotiate(NULL, &unchanged, &offer) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_negotiate(&unchanged, NULL, &offer) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT &&
	      sippymodem_v44_xid_negotiate(&unchanged, &unchanged, NULL) ==
		      SIPPYMODEM_V44_XID_INVALID_ARGUMENT,
	      "V.44 XID parser or selector accepted an invalid argument");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      sippymodem_v44_xid_result_name(
				      (enum sippymodem_v44_xid_result)index),
			      names[index]) == 0,
		      "V.44 XID result %zu has the wrong name", index);
	CHECK(strcmp(
		      sippymodem_v44_xid_result_name(
			      (enum sippymodem_v44_xid_result)99),
		      "unknown") == 0,
	      "unknown V.44 XID result has the wrong name");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "default wire image", test_default_wire_image },
		{ "directional selection", test_directional_selection },
		{ "capability-only defaults", test_capability_only_defaults },
		{ "extensions and rejection", test_extensions_and_rejection },
		{ "API contract", test_api_contract },
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
