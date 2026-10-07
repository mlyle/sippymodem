// SPDX-License-Identifier: BSD-3-Clause

#include "v90_jd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RATE_FIRST_BIT = 0,
	TRAINING_CONSTELLATION_BIT = 28,
	RENEGOTIATION_CONSTELLATION_BIT = 29,
	LOOKAHEAD_FIRST_BIT = 30,
};

static bool constellation_is_valid(
	enum v34_constellation_size constellation)
{
	return constellation == V34_CONSTELLATION_4_POINT ||
	       constellation == V34_CONSTELLATION_16_POINT;
}

bool v90_jd_parameters_are_valid(
	const struct v90_jd_parameters *parameters)
{
	return parameters != NULL &&
	       (parameters->data_rate_capability_mask &
		~(uint32_t)V90_JD_RATE_CAPABILITY_MASK) == 0 &&
	       constellation_is_valid(parameters->training_constellation) &&
	       constellation_is_valid(
		       parameters->rate_renegotiation_constellation) &&
	       parameters->maximum_spectral_shaping_lookahead >=
		       V90_JD_MINIMUM_LOOKAHEAD &&
	       parameters->maximum_spectral_shaping_lookahead <=
		       V90_JD_MAXIMUM_LOOKAHEAD;
}

static enum v90_jd_result frame_result(enum pcm_j_frame_result result)
{
	switch (result) {
	case PCM_J_FRAME_OK:
		return V90_JD_OK;
	case PCM_J_FRAME_INVALID_ARGUMENT:
		return V90_JD_INVALID_ARGUMENT;
	case PCM_J_FRAME_INVALID_FRAME:
		return V90_JD_INVALID_FRAME;
	case PCM_J_FRAME_CRC_MISMATCH:
		return V90_JD_CRC_MISMATCH;
	}
	return V90_JD_INVALID_FRAME;
}

static uint32_t encode_payload(
	const struct v90_jd_parameters *parameters)
{
	uint32_t payload = parameters->data_rate_capability_mask <<
			   RATE_FIRST_BIT;

	if (parameters->training_constellation ==
	    V34_CONSTELLATION_16_POINT)
		payload |= UINT32_C(1) << TRAINING_CONSTELLATION_BIT;
	if (parameters->rate_renegotiation_constellation ==
	    V34_CONSTELLATION_16_POINT)
		payload |= UINT32_C(1) << RENEGOTIATION_CONSTELLATION_BIT;
	payload |= (uint32_t)parameters->maximum_spectral_shaping_lookahead <<
		   LOOKAHEAD_FIRST_BIT;
	return payload;
}

enum v90_jd_result v90_jd_encode(
	const struct v90_jd_parameters *parameters,
	uint8_t *frame, size_t frame_capacity)
{
	if (parameters == NULL || frame == NULL ||
	    frame_capacity < V90_JD_FRAME_BYTES)
		return V90_JD_INVALID_ARGUMENT;
	if (!v90_jd_parameters_are_valid(parameters))
		return V90_JD_INVALID_PARAMETERS;
	return frame_result(pcm_j_frame_encode(
		encode_payload(parameters), frame, frame_capacity));
}

static struct v90_jd_parameters decode_payload(uint32_t payload)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask =
			payload & V90_JD_RATE_CAPABILITY_MASK,
		.training_constellation =
			(payload & (UINT32_C(1) <<
				    TRAINING_CONSTELLATION_BIT)) != 0 ?
				V34_CONSTELLATION_16_POINT :
				V34_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation =
			(payload & (UINT32_C(1) <<
				    RENEGOTIATION_CONSTELLATION_BIT)) != 0 ?
				V34_CONSTELLATION_16_POINT :
				V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead =
			(uint8_t)(payload >> LOOKAHEAD_FIRST_BIT),
	};
}

enum v90_jd_result v90_jd_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v90_jd_parameters *parameters)
{
	struct v90_jd_parameters decoded;
	enum pcm_j_frame_result result;
	uint32_t payload;

	if (frame == NULL || parameters == NULL ||
	    frame_capacity < V90_JD_FRAME_BYTES)
		return V90_JD_INVALID_ARGUMENT;
	result = pcm_j_frame_decode(frame, frame_capacity, &payload);
	if (result != PCM_J_FRAME_OK)
		return frame_result(result);
	decoded = decode_payload(payload);
	if (!v90_jd_parameters_are_valid(&decoded))
		return V90_JD_INVALID_FRAME;
	*parameters = decoded;
	return V90_JD_OK;
}
