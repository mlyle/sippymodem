// SPDX-License-Identifier: BSD-3-Clause

#ifndef SDP_H
#define SDP_H

#include "media.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SDP_MAX_PACKET_DURATION_MS = 1000,
};

enum sdp_result {
	SDP_OK = 0,
	SDP_NOT_PRESENT,
	SDP_INVALID_ARGUMENT,
	SDP_OUTPUT_TOO_SMALL,
};

enum sdp_audio_attribute {
	SDP_AUDIO_RTPMAP = 0,
	SDP_AUDIO_PTIME,
	SDP_AUDIO_MAXPTIME,
	SDP_AUDIO_RTCP,
	SDP_AUDIO_SILENCE_SUPPRESSION,
	SDP_AUDIO_GPMD,
	SDP_AUDIO_ATTRIBUTE_COUNT,
};

struct sdp_audio_description {
	enum media_codec codec;
	uint8_t payload_type;
	uint16_t rtp_port;
	uint16_t rtcp_port;
	uint16_t packet_duration_ms;
	uint16_t sample_rate;
	uint8_t channels;
	bool include_vbd_attributes;
};

/* Missing rtpmap is represented by a null name and zero rate/channels. */
struct sdp_audio_format {
	uint8_t payload_type;
	const char *encoding_name;
	unsigned int sample_rate;
	unsigned int channels;
};

enum sdp_result sdp_audio_description_init(
	struct sdp_audio_description *description,
	enum media_codec codec, uint16_t rtp_port,
	unsigned int packet_duration_ms, bool include_vbd_attributes);

const char *sdp_audio_encoding_name(
	const struct sdp_audio_description *description);
const char *sdp_audio_attribute_name(
	enum sdp_audio_attribute attribute);

/* Format only an attribute value; the signaling backend owns SDP syntax. */
enum sdp_result sdp_audio_format_attribute(
	const struct sdp_audio_description *description,
	enum sdp_audio_attribute attribute,
	char *output, size_t capacity);

bool sdp_audio_accepts_format(
	const struct sdp_audio_description *description,
	const struct sdp_audio_format *format);

/* Attributes replaced when a signaling backend installs modem media policy. */
bool sdp_attribute_is_media_owned(const char *name);

const char *sdp_result_name(enum sdp_result result);

#endif
