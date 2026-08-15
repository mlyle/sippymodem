// SPDX-License-Identifier: BSD-3-Clause

#include "v92_j.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RECORD_IDENTIFIER_BIT = 28,
	TRAINING_CONSTELLATION_BIT = 29,
	RENEGOTIATION_CONSTELLATION_BIT = 30,
	LOOKAHEAD_FIRST_BIT = 30,
};

bool v92_constellation_size_is_valid(
	enum v92_constellation_size constellation)
{
	return constellation == V92_CONSTELLATION_4_POINT ||
	       constellation == V92_CONSTELLATION_8_POINT;
}

bool v92_jd_is_valid(const struct v92_jd *jd)
{
	return jd != NULL &&
	       (jd->data_rate_capability_mask &
		~(uint32_t)V92_JD_RATE_CAPABILITY_MASK) == 0 &&
	       jd->maximum_spectral_shaping_lookahead >=
		       V92_JD_MINIMUM_LOOKAHEAD &&
	       jd->maximum_spectral_shaping_lookahead <=
		       V92_JD_MAXIMUM_LOOKAHEAD;
}

bool v92_jp_is_valid(const struct v92_jp *jp)
{
	return jp != NULL &&
	       v92_constellation_size_is_valid(jp->training_constellation) &&
	       v92_constellation_size_is_valid(
		       jp->rate_renegotiation_constellation);
}

static enum v92_j_result frame_result(enum pcm_j_frame_result result)
{
	switch (result) {
	case PCM_J_FRAME_OK:
		return V92_J_OK;
	case PCM_J_FRAME_INVALID_ARGUMENT:
		return V92_J_INVALID_ARGUMENT;
	case PCM_J_FRAME_INVALID_FRAME:
		return V92_J_INVALID_FRAME;
	case PCM_J_FRAME_CRC_MISMATCH:
		return V92_J_CRC_MISMATCH;
	}
	return V92_J_INVALID_FRAME;
}

static uint32_t jd_payload(const struct v92_jd *jd)
{
	return jd->data_rate_capability_mask |
	       (uint32_t)jd->maximum_spectral_shaping_lookahead <<
		       LOOKAHEAD_FIRST_BIT;
}

enum v92_j_result v92_jd_encode(
	const struct v92_jd *jd, uint8_t *frame, size_t frame_capacity)
{
	if (jd == NULL || frame == NULL || frame_capacity < V92_J_FRAME_BYTES)
		return V92_J_INVALID_ARGUMENT;
	if (!v92_jd_is_valid(jd))
		return V92_J_INVALID_PARAMETERS;
	return frame_result(
		pcm_j_frame_encode(jd_payload(jd), frame, frame_capacity));
}

enum v92_j_result v92_jd_decode(
	const uint8_t *frame, size_t frame_capacity, struct v92_jd *jd)
{
	struct v92_jd decoded;
	enum pcm_j_frame_result result;
	uint32_t payload;

	if (frame == NULL || jd == NULL || frame_capacity < V92_J_FRAME_BYTES)
		return V92_J_INVALID_ARGUMENT;
	result = pcm_j_frame_decode(frame, frame_capacity, &payload);
	if (result != PCM_J_FRAME_OK)
		return frame_result(result);
	if ((payload & (UINT32_C(1) << RECORD_IDENTIFIER_BIT)) != 0)
		return V92_J_WRONG_RECORD;
	decoded = (struct v92_jd){
		.data_rate_capability_mask =
			payload & V92_JD_RATE_CAPABILITY_MASK,
		.maximum_spectral_shaping_lookahead =
			(uint8_t)(payload >> LOOKAHEAD_FIRST_BIT),
	};
	if (!v92_jd_is_valid(&decoded))
		return V92_J_INVALID_FRAME;
	*jd = decoded;
	return V92_J_OK;
}

static uint32_t jp_payload(const struct v92_jp *jp)
{
	uint32_t payload =
		(uint32_t)jp->su_phase_extension_q16 |
		UINT32_C(1) << RECORD_IDENTIFIER_BIT;

	if (jp->training_constellation == V92_CONSTELLATION_8_POINT)
		payload |= UINT32_C(1) << TRAINING_CONSTELLATION_BIT;
	if (jp->rate_renegotiation_constellation ==
	    V92_CONSTELLATION_8_POINT)
		payload |= UINT32_C(1) << RENEGOTIATION_CONSTELLATION_BIT;
	return payload;
}

enum v92_j_result v92_jp_encode(
	const struct v92_jp *jp, uint8_t *frame, size_t frame_capacity)
{
	if (jp == NULL || frame == NULL || frame_capacity < V92_J_FRAME_BYTES)
		return V92_J_INVALID_ARGUMENT;
	if (!v92_jp_is_valid(jp))
		return V92_J_INVALID_PARAMETERS;
	return frame_result(
		pcm_j_frame_encode(jp_payload(jp), frame, frame_capacity));
}

enum v92_j_result v92_jp_decode(
	const uint8_t *frame, size_t frame_capacity, struct v92_jp *jp)
{
	struct v92_jp decoded;
	enum pcm_j_frame_result result;
	uint32_t payload;

	if (frame == NULL || jp == NULL || frame_capacity < V92_J_FRAME_BYTES)
		return V92_J_INVALID_ARGUMENT;
	result = pcm_j_frame_decode(frame, frame_capacity, &payload);
	if (result != PCM_J_FRAME_OK)
		return frame_result(result);
	if ((payload & (UINT32_C(1) << RECORD_IDENTIFIER_BIT)) == 0)
		return V92_J_WRONG_RECORD;
	decoded = (struct v92_jp){
		.su_phase_extension_q16 =
			(uint16_t)payload,
		.training_constellation =
			(payload & (UINT32_C(1) <<
				    TRAINING_CONSTELLATION_BIT)) != 0 ?
				V92_CONSTELLATION_8_POINT :
				V92_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation =
			(payload & (UINT32_C(1) <<
				    RENEGOTIATION_CONSTELLATION_BIT)) != 0 ?
				V92_CONSTELLATION_8_POINT :
				V92_CONSTELLATION_4_POINT,
	};
	if (!v92_jp_is_valid(&decoded))
		return V92_J_INVALID_FRAME;
	*jp = decoded;
	return V92_J_OK;
}

const char *v92_j_result_name(enum v92_j_result result)
{
	switch (result) {
	case V92_J_OK:
		return "ok";
	case V92_J_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_J_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_J_INVALID_FRAME:
		return "invalid frame";
	case V92_J_CRC_MISMATCH:
		return "CRC mismatch";
	case V92_J_WRONG_RECORD:
		return "wrong record";
	}
	return "unknown";
}
