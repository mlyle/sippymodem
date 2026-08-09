// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SDP_H
#define SIPPYMODEM_SDP_H

#include "sippymodem_media.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_SDP_MAX_PACKET_DURATION_MS = 1000,
};

enum sippymodem_sdp_result {
	SIPPYMODEM_SDP_OK = 0,
	SIPPYMODEM_SDP_NOT_PRESENT,
	SIPPYMODEM_SDP_INVALID_ARGUMENT,
	SIPPYMODEM_SDP_OUTPUT_TOO_SMALL,
};

enum sippymodem_sdp_audio_attribute {
	SIPPYMODEM_SDP_AUDIO_RTPMAP = 0,
	SIPPYMODEM_SDP_AUDIO_PTIME,
	SIPPYMODEM_SDP_AUDIO_MAXPTIME,
	SIPPYMODEM_SDP_AUDIO_RTCP,
	SIPPYMODEM_SDP_AUDIO_SILENCE_SUPPRESSION,
	SIPPYMODEM_SDP_AUDIO_GPMD,
	SIPPYMODEM_SDP_AUDIO_ATTRIBUTE_COUNT,
};

struct sippymodem_sdp_audio_description {
	enum sippymodem_media_codec codec;
	uint8_t payload_type;
	uint16_t rtp_port;
	uint16_t rtcp_port;
	uint16_t packet_duration_ms;
	uint16_t sample_rate;
	uint8_t channels;
	bool include_vbd_attributes;
};

/* Missing rtpmap is represented by a null name and zero rate/channels. */
struct sippymodem_sdp_audio_format {
	uint8_t payload_type;
	const char *encoding_name;
	unsigned int sample_rate;
	unsigned int channels;
};

enum sippymodem_sdp_result sippymodem_sdp_audio_description_init(
	struct sippymodem_sdp_audio_description *description,
	enum sippymodem_media_codec codec, uint16_t rtp_port,
	unsigned int packet_duration_ms, bool include_vbd_attributes);

const char *sippymodem_sdp_audio_encoding_name(
	const struct sippymodem_sdp_audio_description *description);
const char *sippymodem_sdp_audio_attribute_name(
	enum sippymodem_sdp_audio_attribute attribute);

/* Format only an attribute value; the signaling backend owns SDP syntax. */
enum sippymodem_sdp_result sippymodem_sdp_audio_format_attribute(
	const struct sippymodem_sdp_audio_description *description,
	enum sippymodem_sdp_audio_attribute attribute,
	char *output, size_t capacity);

bool sippymodem_sdp_audio_accepts_format(
	const struct sippymodem_sdp_audio_description *description,
	const struct sippymodem_sdp_audio_format *format);

/* Attributes replaced when a signaling backend installs modem media policy. */
bool sippymodem_sdp_attribute_is_media_owned(const char *name);

const char *sippymodem_sdp_result_name(enum sippymodem_sdp_result result);

#endif
