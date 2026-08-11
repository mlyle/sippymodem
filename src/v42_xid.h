// SPDX-License-Identifier: BSD-3-Clause

#ifndef V42_XID_H
#define V42_XID_H

#include <stddef.h>
#include <stdint.h>

enum {
	V42_XID_GENERAL_FORMAT = 0x82,
};

/*
 * Data-link parameter fields exclude their group identifier and two-octet
 * group length. user_data is the complete unlengthened subfield, including
 * its 0xff group identifier. Decoded fields borrow the input storage.
 */
struct v42_xid_fields {
	const uint8_t *parameter_negotiation;
	size_t parameter_negotiation_length;
	const uint8_t *private_negotiation;
	size_t private_negotiation_length;
	const uint8_t *user_data;
	size_t user_data_length;
};

enum v42_xid_result {
	V42_XID_OK = 0,
	V42_XID_NEED_OUTPUT,
	V42_XID_MALFORMED,
	V42_XID_INVALID_ARGUMENT,
};

/*
 * Encode the complete XID information field in standard group order. A field
 * with zero length is omitted. Input fields and output must not overlap.
 * NEED_OUTPUT and errors leave output and output_length unchanged.
 */
enum v42_xid_result v42_xid_encode(
	const struct v42_xid_fields *fields,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length);

/*
 * Decode one complete XID information field. Unknown, well-formed data-link
 * groups are ignored. Groups must be strictly ascending, and the unlengthened
 * user-data subfield, when present, consumes the remainder of the field.
 * Present groups, including user data, must contain a complete parameter.
 * Errors leave fields unchanged.
 */
enum v42_xid_result v42_xid_decode(
	const uint8_t *input,
	size_t input_length,
	struct v42_xid_fields *fields);

const char *v42_xid_result_name(
	enum v42_xid_result result);

#endif
