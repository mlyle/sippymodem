// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_INFO0_H
#define PCM_INFO0_H

#include "pcm_protocol.h"
#include "v92_info0.h"

union pcm_info0_record {
	struct v34_info0 analog;
	struct v90_info0d digital;
	struct v92_info0a v92_analog;
	struct v92_info0d v92_digital;
};

/* The caller supplies valid protocol/role tags and the selected record. */
static inline const struct v34_info0 *pcm_info0_common(
	const union pcm_info0_record *record, enum pcm_protocol protocol,
	enum v90_modem_role role)
{
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       &record->analog : &record->digital.common;
	return role == V90_ANALOG_MODEM ?
		       &record->v92_analog.v90 : &record->v92_digital.v90.common;
}

static inline struct v34_info0 *pcm_info0_mutable_common(
	union pcm_info0_record *record, enum pcm_protocol protocol,
	enum v90_modem_role role)
{
	/* The record is mutable; only the const view's qualifier is removed. */
	return (struct v34_info0 *)pcm_info0_common(record, protocol, role);
}

/* A received V.92-format record may decline V.92 so startup can fall back.
 * Acknowledgment is an exchange property, not a capability constraint. */
static inline bool pcm_info0_is_valid(enum pcm_protocol protocol,
	enum v90_modem_role role, const union pcm_info0_record *record)
{
	if (record == NULL || !pcm_protocol_is_valid(protocol) ||
	    !pcm_modem_role_is_valid(role))
		return false;
	if (protocol == PCM_PROTOCOL_V90)
		return role == V90_ANALOG_MODEM ?
			       v90_info0_is_valid(&record->analog) :
			       v90_info0d_is_valid(&record->digital);
	return role == V90_ANALOG_MODEM ?
		       v92_info0a_is_valid(&record->v92_analog) :
		       v92_info0d_is_valid(&record->v92_digital);
}

/* Retained local offers precede acknowledgment and must advertise the
 * selected protocol. The call and Phase 2 owners keep the same invariant. */
static inline bool pcm_info0_offer_is_valid(enum pcm_protocol protocol,
	enum v90_modem_role role, const union pcm_info0_record *record)
{
	return pcm_info0_is_valid(protocol, role, record) &&
	       !pcm_info0_common(record, protocol, role)->acknowledges_peer_info0 &&
	       (protocol == PCM_PROTOCOL_V90 ||
		(role == V90_ANALOG_MODEM ? record->v92_analog.v92_capable :
					  record->v92_digital.v92_capable));
}

#endif /* PCM_INFO0_H */
