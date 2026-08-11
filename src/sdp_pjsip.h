// SPDX-License-Identifier: BSD-3-Clause

#ifndef SDP_PJSIP_H
#define SDP_PJSIP_H

#include "sdp.h"

#include <pjmedia/sdp.h>

#include <stdbool.h>

/* Translate the backend-neutral modem audio policy into a local SDP. */
pj_status_t sdp_pjsip_apply(
	pjmedia_sdp_session *local_sdp, pj_pool_t *pool, bool is_answer,
	const struct sdp_audio_description *description,
	const char *public_address);

#endif
