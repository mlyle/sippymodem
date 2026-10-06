// SPDX-License-Identifier: BSD-3-Clause

#ifndef LAPM_STATE_FIXTURE_H
#define LAPM_STATE_FIXTURE_H

#include <modem.h>
#include "lapm_xid_fixture.h"

static inline bool lapm_states_equal(
	const struct lapm_state *left, const struct lapm_state *right)
{
	if (!lapm_v44_sessions_equal(&left->v44_session, &right->v44_session))
		return false;
	struct lapm_state left_copy, right_copy;
	memcpy(&left_copy, left, sizeof(left_copy));
	memcpy(&right_copy, right, sizeof(right_copy));
	/* The remaining owned LAPM storage is cleared by lapm_init. */
	memset(&left_copy.v44_session, 0, sizeof(left_copy.v44_session));
	memset(&right_copy.v44_session, 0, sizeof(right_copy.v44_session));
	return memcmp(&left_copy, &right_copy, sizeof(left_copy)) == 0;
}

#endif
