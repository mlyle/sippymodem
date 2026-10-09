// SPDX-License-Identifier: BSD-3-Clause

#include "sip_uri.h"

#include "net_socket.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool ascii_equal_ignoring_case(char left, char right)
{
	if (left >= 'A' && left <= 'Z')
		left = (char)(left + ('a' - 'A'));
	if (right >= 'A' && right <= 'Z')
		right = (char)(right + ('a' - 'A'));
	return left == right;
}

static bool starts_with_ignoring_case(const char *text, const char *prefix)
{
	while (*prefix != '\0') {
		if (*text == '\0' || !ascii_equal_ignoring_case(*text, *prefix))
			return false;
		text++;
		prefix++;
	}
	return true;
}

static bool text_is_header_safe(const char *text)
{
	for (; *text != '\0'; ++text) {
		unsigned char octet = (unsigned char)*text;

		if (octet <= 0x20 || octet == 0x7f)
			return false;
	}
	return true;
}

static bool ascii_is_alphanumeric(char character)
{
	return (character >= 'a' && character <= 'z') ||
	       (character >= 'A' && character <= 'Z') ||
	       (character >= '0' && character <= '9');
}

static bool ascii_is_hexadecimal(char character)
{
	return (character >= 'a' && character <= 'f') ||
	       (character >= 'A' && character <= 'F') ||
	       (character >= '0' && character <= '9');
}

static bool contact_user_is_valid(const char *user)
{
	static const char user_punctuation[] = "-_.!~*'()&=+$,;?/";

	for (size_t index = 0; user[index] != '\0'; ++index) {
		if (ascii_is_alphanumeric(user[index]) ||
		    strchr(user_punctuation, user[index]) != NULL)
			continue;
		if (user[index] == '%' && user[index + 1] != '\0' &&
		    user[index + 2] != '\0' &&
		    ascii_is_hexadecimal(user[index + 1]) &&
		    ascii_is_hexadecimal(user[index + 2])) {
			index += 2;
			continue;
		}
		return false;
	}
	return true;
}

static enum sip_uri_result copy_parts(
	char *output, size_t capacity, const char *const *parts,
	size_t part_count)
{
	size_t total = 0;

	for (size_t index = 0; index < part_count; ++index) {
		size_t length = strlen(parts[index]);

		if (total >= capacity || length >= capacity - total) {
			output[0] = '\0';
			return SIP_URI_TOO_LONG;
		}
		total += length;
	}

	total = 0;
	for (size_t index = 0; index < part_count; ++index) {
		size_t length = strlen(parts[index]);

		memcpy(output + total, parts[index], length);
		total += length;
	}
	output[total] = '\0';
	return SIP_URI_OK;
}

enum sip_uri_result sip_uri_format_target(
	char *output, size_t capacity, const char *dial_string,
	const char *configured_target, const char *domain)
{
	const char *parts[4];
	const char *target;
	const char *at;
	size_t part_count;

	if (output == NULL || capacity == 0)
		return SIP_URI_INVALID_ARGUMENT;
	output[0] = '\0';
	target = configured_target;
	if (dial_string != NULL && dial_string[0] != '\0')
		target = dial_string;
	if (target == NULL || target[0] == '\0')
		return SIP_URI_NOT_CONFIGURED;
	if (!text_is_header_safe(target))
		return SIP_URI_INVALID_TEXT;

	if (starts_with_ignoring_case(target, "sip:") ||
	    starts_with_ignoring_case(target, "sips:")) {
		const char *colon = strchr(target, ':');

		if (colon[1] == '\0')
			return SIP_URI_INVALID_TEXT;
		parts[0] = target;
		part_count = 1;
	} else if ((at = strchr(target, '@')) != NULL) {
		if (at == target || at[1] == '\0' || strchr(at + 1, '@') != NULL)
			return SIP_URI_INVALID_TEXT;
		parts[0] = "sip:";
		parts[1] = target;
		part_count = 2;
	} else {
		if (domain == NULL || domain[0] == '\0')
			return SIP_URI_MISSING_DOMAIN;
		if (!text_is_header_safe(domain))
			return SIP_URI_INVALID_TEXT;
		parts[0] = "sip:";
		parts[1] = target;
		parts[2] = "@";
		parts[3] = domain;
		part_count = 4;
	}

	return copy_parts(output, capacity, parts, part_count);
}

static bool hostname_is_valid(const char *host)
{
	for (; *host != '\0'; ++host) {
		if (!ascii_is_alphanumeric(*host) && *host != '.' && *host != '-' &&
		    *host != '_')
			return false;
	}
	return true;
}

static bool contact_host_is_valid(const char *host, bool *is_bracketed)
{
	struct in6_addr ipv6_address;
	char unbracketed[INET6_ADDRSTRLEN];
	const char *address = host;
	size_t length = strlen(host);

	*is_bracketed = length >= 2 && host[0] == '[' && host[length - 1] == ']';
	if (*is_bracketed) {
		size_t address_length = length - 2;

		address = host + 1;
		if (address_length == 0 || address_length >= sizeof(unbracketed))
			return false;
		memcpy(unbracketed, address, address_length);
		unbracketed[address_length] = '\0';
		address = unbracketed;
	}

	if (strchr(address, ':') != NULL)
		return inet_pton(AF_INET6, address, &ipv6_address) == 1;
	return !*is_bracketed && hostname_is_valid(address);
}

enum sip_uri_result sip_uri_format_contact(
	char *output, size_t capacity, const char *user,
	const char *public_address, uint16_t port)
{
	char port_text[6];
	const char *parts[9];
	bool bracketed;
	size_t part_count = 0;

	if (output == NULL || capacity == 0)
		return SIP_URI_INVALID_ARGUMENT;
	output[0] = '\0';
	if (user == NULL || user[0] == '\0' || public_address == NULL ||
	    public_address[0] == '\0')
		return SIP_URI_NOT_CONFIGURED;
	if (port == 0)
		return SIP_URI_INVALID_ARGUMENT;
	if (!text_is_header_safe(user) || !text_is_header_safe(public_address) ||
	    !contact_user_is_valid(user) ||
	    !contact_host_is_valid(public_address, &bracketed))
		return SIP_URI_INVALID_TEXT;

	(void)snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);
	parts[part_count++] = "sip:";
	parts[part_count++] = user;
	parts[part_count++] = "@";
	if (strchr(public_address, ':') != NULL && !bracketed)
		parts[part_count++] = "[";
	parts[part_count++] = public_address;
	if (strchr(public_address, ':') != NULL && !bracketed)
		parts[part_count++] = "]";
	parts[part_count++] = ":";
	parts[part_count++] = port_text;
	parts[part_count++] = ";ob";
	return copy_parts(output, capacity, parts, part_count);
}

const char *sip_uri_result_name(
	enum sip_uri_result result)
{
	switch (result) {
	case SIP_URI_OK:
		return "ok";
	case SIP_URI_NOT_CONFIGURED:
		return "not configured";
	case SIP_URI_INVALID_ARGUMENT:
		return "invalid argument";
	case SIP_URI_INVALID_TEXT:
		return "invalid URI text";
	case SIP_URI_MISSING_DOMAIN:
		return "missing SIP domain";
	case SIP_URI_TOO_LONG:
		return "URI is too long";
	}
	return "unknown URI result";
}
