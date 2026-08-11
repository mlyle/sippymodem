// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SIP_URI_H
#define SIPPYMODEM_SIP_URI_H

#include <stddef.h>
#include <stdint.h>

enum sippymodem_sip_uri_result {
	SIPPYMODEM_SIP_URI_OK = 0,
	SIPPYMODEM_SIP_URI_NOT_CONFIGURED,
	SIPPYMODEM_SIP_URI_INVALID_ARGUMENT,
	SIPPYMODEM_SIP_URI_INVALID_TEXT,
	SIPPYMODEM_SIP_URI_MISSING_DOMAIN,
	SIPPYMODEM_SIP_URI_TOO_LONG,
};

/*
 * Format the destination for an outgoing call. A nonempty dial string takes
 * precedence over the configured target. Complete sip: and sips: URIs pass
 * through unchanged; user@host and local-user forms gain the missing parts.
 * Inputs must not overlap output.
 */
enum sippymodem_sip_uri_result sippymodem_sip_uri_format_target(
	char *output, size_t capacity, const char *dial_string,
	const char *configured_target, const char *domain);

/*
 * Format a contact URI for a known public transport address. Missing user or
 * address is an optional, unconfigured contact rather than malformed input.
 * Inputs must not overlap output.
 */
enum sippymodem_sip_uri_result sippymodem_sip_uri_format_contact(
	char *output, size_t capacity, const char *user,
	const char *public_address, uint16_t port);

const char *sippymodem_sip_uri_result_name(
	enum sippymodem_sip_uri_result result);

#endif
