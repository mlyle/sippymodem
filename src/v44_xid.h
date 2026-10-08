// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_XID_H
#define V44_XID_H

#include "v44_stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V44_XID_OFFER_SIZE = 34,
};

struct v44_direction_offer {
	bool enabled;
	struct v44_parameters parameters;
};

/* Transmit and receive are relative to the entity represented by the offer. */
struct v44_offer {
	struct v44_direction_offer transmit;
	struct v44_direction_offer receive;
};

static inline bool v44_offer_has_direction(const struct v44_offer *offer)
{
	return offer->transmit.enabled || offer->receive.enabled;
}

enum v44_xid_result {
	V44_XID_OK = 0,
	V44_XID_NEED_OUTPUT,
	V44_XID_MALFORMED,
	V44_XID_INVALID_ARGUMENT,
};

struct v44_offer v44_default_offer(void);

/* Encode the complete V.42 XID User Data Subfield for a V.44 offer. */
enum v44_xid_result v44_xid_encode(
	const struct v44_offer *offer,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Decode one complete V.44 XID User Data Subfield. Unknown parameters are
 * ignored after the required parameter-set identifier for extensibility. A
 * capability-only subfield selects the default parameters. If any proposal
 * parameter is present, the complete proposal set must be present.
 *
 * The capability bit for later Parameter Mode is accepted but does not cause
 * that mode to be entered; this codec selects any parameters carried in XID.
 */
enum v44_xid_result v44_xid_decode(
	const uint8_t *input,
	size_t input_length,
	struct v44_offer *offer);

/*
 * Select parameters from the local perspective. A direction is enabled only
 * when the sender and corresponding receiver both request it. Each selected
 * resource limit is the lesser of the two peers' proposals.
 */
enum v44_xid_result v44_xid_negotiate(
	const struct v44_offer *local,
	const struct v44_offer *peer,
	struct v44_offer *negotiated);

#endif
