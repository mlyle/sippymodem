// SPDX-License-Identifier: BSD-3-Clause

#ifndef LAPM_XID_FIXTURE_H
#define LAPM_XID_FIXTURE_H

#include <lapm_v44_session.h>
#include <string.h>

static inline bool lapm_v44_offers_equal(
	const struct v44_offer *left,
	const struct v44_offer *right)
{
	return left->transmit.enabled == right->transmit.enabled &&
	       left->transmit.parameters.codeword_limit ==
		       right->transmit.parameters.codeword_limit &&
	       left->transmit.parameters.max_string_length ==
		       right->transmit.parameters.max_string_length &&
	       left->transmit.parameters.history_size ==
		       right->transmit.parameters.history_size &&
	       left->receive.enabled == right->receive.enabled &&
	       left->receive.parameters.codeword_limit ==
		       right->receive.parameters.codeword_limit &&
	       left->receive.parameters.max_string_length ==
		       right->receive.parameters.max_string_length &&
	       left->receive.parameters.history_size ==
		       right->receive.parameters.history_size;
}

static inline bool lapm_v44_sessions_equal(
	const struct lapm_v44_session *left,
	const struct lapm_v44_session *right)
{
	return left->config.enabled == right->config.enabled &&
	       left->config.native_v44_ready == right->config.native_v44_ready &&
	       lapm_v44_offers_equal(&left->config.local_offer,
				    &right->config.local_offer) &&
	       left->phase == right->phase && left->revision == right->revision &&
	       left->initialized == right->initialized &&
	       left->parameters_retried == right->parameters_retried &&
	       lapm_v44_offers_equal(&left->command_offer, &right->command_offer) &&
	       lapm_v44_offers_equal(&left->negotiated_offer,
				    &right->negotiated_offer) &&
	       lapm_v44_offers_equal(&left->response_offer, &right->response_offer) &&
	       left->command_length == right->command_length &&
	       memcmp(left->command, right->command, sizeof(left->command)) == 0 &&
	       left->peer_command_length == right->peer_command_length &&
	       memcmp(left->peer_command, right->peer_command,
		      sizeof(left->peer_command)) == 0 &&
	       left->response_length == right->response_length &&
	       memcmp(left->response, right->response, sizeof(left->response)) == 0;
}

/* A valid envelope containing the forbidden dual compression selection. */
static const uint8_t lapm_dual_compression_response[] = {
	0x82, 0x80, 0x00, 0x13, 0x03, 0x03, 0x8a, 0x89, 0x00, 0x05, 0x02, 0x04,
	0x00, 0x06, 0x02, 0x04, 0x00, 0x07, 0x01, 0x0c, 0x08, 0x01, 0x0d, 0xf0,
	0x00, 0x0f, 0x00, 0x03, 0x56, 0x34, 0x32, 0x01, 0x01, 0x03, 0x02, 0x02,
	0x02, 0x00, 0x03, 0x01, 0x10, 0xff, 0x40, 0x03, 0x56, 0x34, 0x34, 0x41,
	0x01, 0x00, 0x42, 0x01, 0x03, 0x43, 0x02, 0x04, 0x00, 0x44, 0x02, 0x04,
	0x00, 0x45, 0x01, 0xff, 0x46, 0x01, 0x64, 0x47, 0x02, 0x08, 0x00, 0x48,
	0x02, 0x08, 0x00,
};

#endif
