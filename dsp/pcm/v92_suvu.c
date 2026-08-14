// SPDX-License-Identifier: BSD-3-Clause

#include "v92_suvu.h"

#include "v92_status_update.h"

enum {
	SUVU_WAIT_FOR_CPU_PAYLOAD_BIT = 7,
	SUVU_LEVEL_FIRST_PAYLOAD_BIT = 8,
	SUVU_LEVEL_BITS = 5,
	SUVU_REQUEST_SILENCE_PAYLOAD_BIT = 13,
	SUVU_ACKNOWLEDGMENT_PAYLOAD_BIT = 14,
};

static unsigned int alignment_bits(
	enum v92_constellation_size constellation)
{
	switch (constellation) {
	case V92_CONSTELLATION_4_POINT:
		return V92_STATUS_UPDATE_4_POINT_ALIGNMENT;
	case V92_CONSTELLATION_8_POINT:
		return V92_STATUS_UPDATE_8_POINT_ALIGNMENT;
	}
	return 0u;
}

size_t v92_suvu_frame_bits(enum v92_constellation_size constellation)
{
	return v92_status_update_frame_bits(alignment_bits(constellation));
}

size_t v92_suvu_frame_bytes(enum v92_constellation_size constellation)
{
	size_t bits = v92_suvu_frame_bits(constellation);

	return bits == 0u ? 0u : (bits + 7u) / 8u;
}

bool v92_suvu_parameters_are_valid(
	const struct v92_suvu_parameters *parameters)
{
	return parameters != NULL &&
	       parameters->prefilter_level_db_q2_2 >= -16 &&
	       parameters->prefilter_level_db_q2_2 <= 15;
}

static enum v92_suvu_result encode_result(
	enum v92_status_update_result result)
{
	switch (result) {
	case V92_STATUS_UPDATE_OK:
		return V92_SUVU_OK;
	case V92_STATUS_UPDATE_INVALID_ARGUMENT:
		return V92_SUVU_INVALID_ARGUMENT;
	case V92_STATUS_UPDATE_INVALID_ALIGNMENT:
		return V92_SUVU_INVALID_PARAMETERS;
	case V92_STATUS_UPDATE_OUTPUT_TOO_SMALL:
		return V92_SUVU_OUTPUT_TOO_SMALL;
	case V92_STATUS_UPDATE_INVALID_FRAME:
	case V92_STATUS_UPDATE_CRC_MISMATCH:
		break;
	}
	return V92_SUVU_INVALID_PARAMETERS;
}

enum v92_suvu_result v92_suvu_encode(
	const struct v92_suvu_parameters *parameters,
	enum v92_constellation_size constellation,
	uint8_t *frame, size_t frame_capacity, size_t *frame_bit_count)
{
	unsigned int alignment = alignment_bits(constellation);
	uint8_t raw_level;
	uint16_t payload;

	if (parameters == NULL || frame == NULL || frame_bit_count == NULL)
		return V92_SUVU_INVALID_ARGUMENT;
	if (!v92_suvu_parameters_are_valid(parameters) ||
	    alignment == 0u)
		return V92_SUVU_INVALID_PARAMETERS;
	raw_level = parameters->prefilter_level_db_q2_2 < 0 ?
		(uint8_t)(parameters->prefilter_level_db_q2_2 + 32) :
		(uint8_t)parameters->prefilter_level_db_q2_2;
	payload = (uint16_t)(
		(uint16_t)raw_level << SUVU_LEVEL_FIRST_PAYLOAD_BIT);
	payload |= (uint16_t)(
		(uint16_t)parameters->wait_for_cpu <<
		SUVU_WAIT_FOR_CPU_PAYLOAD_BIT);
	payload |= (uint16_t)(
		(uint16_t)parameters->request_silence <<
		SUVU_REQUEST_SILENCE_PAYLOAD_BIT);
	payload |= (uint16_t)(
		(uint16_t)parameters->acknowledges_cpd <<
		SUVU_ACKNOWLEDGMENT_PAYLOAD_BIT);
	return encode_result(v92_status_update_encode(
		payload, alignment, frame, frame_capacity, frame_bit_count));
}

static enum v92_suvu_result decode_result(
	enum v92_status_update_result result)
{
	switch (result) {
	case V92_STATUS_UPDATE_OK:
		return V92_SUVU_OK;
	case V92_STATUS_UPDATE_INVALID_ARGUMENT:
		return V92_SUVU_INVALID_ARGUMENT;
	case V92_STATUS_UPDATE_INVALID_ALIGNMENT:
	case V92_STATUS_UPDATE_INVALID_FRAME:
		return V92_SUVU_INVALID_FRAME;
	case V92_STATUS_UPDATE_CRC_MISMATCH:
		return V92_SUVU_CRC_MISMATCH;
	case V92_STATUS_UPDATE_OUTPUT_TOO_SMALL:
		break;
	}
	return V92_SUVU_INVALID_FRAME;
}

enum v92_suvu_result v92_suvu_decode(
	const uint8_t *frame, size_t frame_capacity, size_t frame_bit_count,
	enum v92_constellation_size constellation,
	struct v92_suvu_parameters *parameters)
{
	struct v92_suvu_parameters decoded;
	enum v92_status_update_result result;
	unsigned int alignment = alignment_bits(constellation);
	uint16_t payload = 0u;
	uint8_t raw_level;

	if (frame == NULL || parameters == NULL)
		return V92_SUVU_INVALID_ARGUMENT;
	result = v92_status_update_decode(
		frame, frame_capacity, frame_bit_count,
		alignment, &payload);
	if (result != V92_STATUS_UPDATE_OK)
		return decode_result(result);
	raw_level = (uint8_t)(
		(payload >> SUVU_LEVEL_FIRST_PAYLOAD_BIT) &
		((1u << SUVU_LEVEL_BITS) - 1u));
	decoded = (struct v92_suvu_parameters){
		.prefilter_level_db_q2_2 = raw_level < 16u ?
			(int8_t)raw_level : (int8_t)((int)raw_level - 32),
		.wait_for_cpu =
			((payload >> SUVU_WAIT_FOR_CPU_PAYLOAD_BIT) & 1u) != 0u,
		.request_silence =
			((payload >> SUVU_REQUEST_SILENCE_PAYLOAD_BIT) & 1u) != 0u,
		.acknowledges_cpd =
			((payload >> SUVU_ACKNOWLEDGMENT_PAYLOAD_BIT) & 1u) != 0u,
	};
	if (!v92_suvu_parameters_are_valid(&decoded))
		return V92_SUVU_INVALID_FRAME;
	*parameters = decoded;
	return V92_SUVU_OK;
}

const char *v92_suvu_result_name(enum v92_suvu_result result)
{
	switch (result) {
	case V92_SUVU_OK:
		return "ok";
	case V92_SUVU_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_SUVU_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_SUVU_OUTPUT_TOO_SMALL:
		return "output too small";
	case V92_SUVU_INVALID_FRAME:
		return "invalid frame";
	case V92_SUVU_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
