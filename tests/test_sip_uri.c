// SPDX-License-Identifier: BSD-3-Clause

#include <sip_uri.h>

#include "test.h"

#include <string.h>

static bool expect_target(const char *expected, const char *dial_string,
			  const char *configured_target, const char *domain)
{
	char output[128];
	enum sippymodem_sip_uri_result result;

	result = sippymodem_sip_uri_format_target(
		output, sizeof(output), dial_string, configured_target, domain);
	CHECK(result == SIPPYMODEM_SIP_URI_OK, "target failed: %s",
	      sippymodem_sip_uri_result_name(result));
	CHECK(strcmp(output, expected) == 0, "got '%s', expected '%s'", output,
	      expected);
	return true;
}

static bool test_target_forms_and_precedence(void)
{
	return expect_target("SIPS:alice@example.net;transport=tls",
			     "SIPS:alice@example.net;transport=tls", NULL, NULL) &&
	       expect_target("sip:bob@example.net", "bob@example.net", NULL,
			     NULL) &&
	       expect_target("sip:5551212@example.net", "5551212",
			     "ignored", "example.net") &&
	       expect_target("sip:operator@example.org", NULL, "operator",
			     "example.org");
}

static bool expect_target_error(enum sippymodem_sip_uri_result expected,
				const char *dial_string,
				const char *configured_target,
				const char *domain)
{
	char output[16] = "stale";
	enum sippymodem_sip_uri_result result;

	result = sippymodem_sip_uri_format_target(
		output, sizeof(output), dial_string, configured_target, domain);
	CHECK(result == expected, "got %s, expected %s",
	      sippymodem_sip_uri_result_name(result),
	      sippymodem_sip_uri_result_name(expected));
	CHECK(output[0] == '\0', "failed target left stale output");
	return true;
}

static bool test_target_validation(void)
{
	char one_byte[1] = { 'x' };

	CHECK(expect_target_error(SIPPYMODEM_SIP_URI_NOT_CONFIGURED, NULL, NULL,
				  "example.net"),
	      "empty target result was wrong");
	CHECK(expect_target_error(SIPPYMODEM_SIP_URI_MISSING_DOMAIN, "alice",
				  NULL, NULL),
	      "missing domain result was wrong");
	CHECK(expect_target_error(SIPPYMODEM_SIP_URI_INVALID_TEXT,
				  "alice\r\n@example.net", NULL, NULL),
	      "header injection was accepted");
	CHECK(expect_target_error(SIPPYMODEM_SIP_URI_INVALID_TEXT, "@host",
				  NULL, NULL),
	      "empty user was accepted");
	CHECK(expect_target_error(SIPPYMODEM_SIP_URI_TOO_LONG,
				  "alice@example.net", NULL, NULL),
	      "small buffer was accepted");
	CHECK(sippymodem_sip_uri_format_target(one_byte, sizeof(one_byte), "x@y",
					       NULL, NULL) ==
		      SIPPYMODEM_SIP_URI_TOO_LONG &&
		      one_byte[0] == '\0',
	      "one-byte output did not fail safely");
	return true;
}

static bool expect_contact(const char *expected, const char *address)
{
	char output[128];
	enum sippymodem_sip_uri_result result;

	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice", address, 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_OK, "contact failed: %s",
	      sippymodem_sip_uri_result_name(result));
	CHECK(strcmp(output, expected) == 0, "got '%s', expected '%s'", output,
	      expected);
	return true;
}

static bool test_contact_address_forms(void)
{
	return expect_contact("sip:alice@198.51.100.4:5060;ob",
			      "198.51.100.4") &&
	       expect_contact("sip:alice@[2001:db8::4]:5060;ob",
			      "2001:db8::4") &&
	       expect_contact("sip:alice@[2001:db8::4]:5060;ob",
			      "[2001:db8::4]");
}

static bool test_contact_validation(void)
{
	char output[12] = "stale";
	enum sippymodem_sip_uri_result result;

	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), NULL, "198.51.100.4", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_NOT_CONFIGURED && output[0] == '\0',
	      "optional contact was not reported cleanly");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice", "host\r\n.invalid", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_INVALID_TEXT && output[0] == '\0',
	      "unsafe public address was accepted");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice", "[2001:db8::4", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_INVALID_TEXT && output[0] == '\0',
	      "unbalanced IPv6 brackets were accepted");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice:password", "198.51.100.4", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_INVALID_TEXT && output[0] == '\0',
	      "contact user changed URI structure");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice%", "198.51.100.4", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_INVALID_TEXT && output[0] == '\0',
	      "incomplete user escape was accepted");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice", "example.net>sip:intruder", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_INVALID_TEXT && output[0] == '\0',
	      "invalid contact host was accepted");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice", "198.51.100.4", 0);
	CHECK(result == SIPPYMODEM_SIP_URI_INVALID_ARGUMENT && output[0] == '\0',
	      "zero contact port was accepted");
	result = sippymodem_sip_uri_format_contact(
		output, sizeof(output), "alice", "198.51.100.4", 5060);
	CHECK(result == SIPPYMODEM_SIP_URI_TOO_LONG && output[0] == '\0',
	      "small contact buffer did not fail safely");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "target forms and precedence", test_target_forms_and_precedence },
		{ "target validation", test_target_validation },
		{ "contact address forms", test_contact_address_forms },
		{ "contact validation", test_contact_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
