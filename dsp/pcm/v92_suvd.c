// SPDX-License-Identifier: BSD-3-Clause

#include "v92_suvd.h"

enum {
	SUVD_REQUEST_SILENCE_PAYLOAD_BIT = 13,
	SUVD_ACKNOWLEDGMENT_PAYLOAD_BIT = 14,
};

bool v92_suvd_parameters_are_valid(
	const struct v92_suvd_parameters *parameters)
{
	return parameters != NULL;
}

size_t v92_suvd_frame_bits(uint8_t mapping_frame_bits)
{
	if (mapping_frame_bits < V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS ||
	    mapping_frame_bits > V92_STATUS_UPDATE_DOWNSTREAM_DATA_MAX_BITS)
		return 0u;
	return v92_status_update_frame_bits(mapping_frame_bits);
}

size_t v92_suvd_frame_bytes(uint8_t mapping_frame_bits)
{
	return (v92_suvd_frame_bits(mapping_frame_bits) + 7u) / 8u;
}

static enum v92_suvd_result encode_result(
	enum v92_status_update_result result)
{
	switch (result) {
	case V92_STATUS_UPDATE_OK:
		return V92_SUVD_OK;
	case V92_STATUS_UPDATE_INVALID_ARGUMENT:
		return V92_SUVD_INVALID_ARGUMENT;
	case V92_STATUS_UPDATE_OUTPUT_TOO_SMALL:
		return V92_SUVD_OUTPUT_TOO_SMALL;
	case V92_STATUS_UPDATE_INVALID_ALIGNMENT:
	case V92_STATUS_UPDATE_INVALID_FRAME:
	case V92_STATUS_UPDATE_CRC_MISMATCH:
		break;
	}
	return V92_SUVD_INVALID_ARGUMENT;
}

enum v92_suvd_result v92_suvd_encode(
	const struct v92_suvd_parameters *parameters,
	uint8_t mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count)
{
	uint16_t payload;

	if (!v92_suvd_parameters_are_valid(parameters) || frame == NULL ||
	    frame_bit_count == NULL || v92_suvd_frame_bits(mapping_frame_bits) == 0u)
		return V92_SUVD_INVALID_ARGUMENT;
	payload = (uint16_t)(
		(uint16_t)parameters->request_silence <<
		SUVD_REQUEST_SILENCE_PAYLOAD_BIT);
	payload |= (uint16_t)(
		(uint16_t)parameters->acknowledges_cpu <<
		SUVD_ACKNOWLEDGMENT_PAYLOAD_BIT);
	return encode_result(v92_status_update_encode(
		payload, mapping_frame_bits,
		frame, frame_capacity, frame_bit_count));
}

static enum v92_suvd_result decode_result(
	enum v92_status_update_result result)
{
	switch (result) {
	case V92_STATUS_UPDATE_OK:
		return V92_SUVD_OK;
	case V92_STATUS_UPDATE_INVALID_ARGUMENT:
		return V92_SUVD_INVALID_ARGUMENT;
	case V92_STATUS_UPDATE_INVALID_FRAME:
		return V92_SUVD_INVALID_FRAME;
	case V92_STATUS_UPDATE_CRC_MISMATCH:
		return V92_SUVD_CRC_MISMATCH;
	case V92_STATUS_UPDATE_INVALID_ALIGNMENT:
	case V92_STATUS_UPDATE_OUTPUT_TOO_SMALL:
		break;
	}
	return V92_SUVD_INVALID_FRAME;
}

enum v92_suvd_result v92_suvd_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, uint8_t mapping_frame_bits,
	struct v92_suvd_parameters *parameters)
{
	struct v92_suvd_parameters decoded;
	enum v92_status_update_result result;
	uint16_t payload = 0u;

	if (frame == NULL || parameters == NULL)
		return V92_SUVD_INVALID_ARGUMENT;
	if (v92_suvd_frame_bits(mapping_frame_bits) == 0u)
		return V92_SUVD_INVALID_FRAME;
	result = v92_status_update_decode(
		frame, frame_capacity, frame_bit_count,
		mapping_frame_bits, &payload);
	if (result != V92_STATUS_UPDATE_OK)
		return decode_result(result);
	decoded = (struct v92_suvd_parameters){
		.request_silence =
			((payload >> SUVD_REQUEST_SILENCE_PAYLOAD_BIT) & 1u) != 0u,
		.acknowledges_cpu =
			((payload >> SUVD_ACKNOWLEDGMENT_PAYLOAD_BIT) & 1u) != 0u,
	};
	*parameters = decoded;
	return V92_SUVD_OK;
}

const char *v92_suvd_result_name(enum v92_suvd_result result)
{
	switch (result) {
	case V92_SUVD_OK:
		return "ok";
	case V92_SUVD_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_SUVD_OUTPUT_TOO_SMALL:
		return "output too small";
	case V92_SUVD_INVALID_FRAME:
		return "invalid frame";
	case V92_SUVD_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
