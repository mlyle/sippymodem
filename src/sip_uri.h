// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIP_URI_H
#define SIP_URI_H

#include <stddef.h>
#include <stdint.h>

enum sip_uri_result {
	SIP_URI_OK = 0,
	SIP_URI_NOT_CONFIGURED,
	SIP_URI_INVALID_ARGUMENT,
	SIP_URI_INVALID_TEXT,
	SIP_URI_MISSING_DOMAIN,
	SIP_URI_TOO_LONG,
};

/*
 * Format the destination for an outgoing call. A nonempty dial string takes
 * precedence over the configured target. Complete sip: and sips: URIs pass
 * through unchanged; user@host and local-user forms gain the missing parts.
 * Inputs must not overlap output.
 */
enum sip_uri_result sip_uri_format_target(
	char *output, size_t capacity, const char *dial_string,
	const char *configured_target, const char *domain);

/*
 * Format a contact URI for a known public transport address. Missing user or
 * address is an optional, unconfigured contact rather than malformed input.
 * Inputs must not overlap output.
 */
enum sip_uri_result sip_uri_format_contact(
	char *output, size_t capacity, const char *user,
	const char *public_address, uint16_t port);

const char *sip_uri_result_name(
	enum sip_uri_result result);

#endif
