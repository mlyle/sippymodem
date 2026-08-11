// SPDX-License-Identifier: BSD-3-Clause

#include "sdp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *codec_encoding_name(enum media_codec codec)
{
	switch (codec) {
	case MEDIA_CODEC_PCMU:
		return "PCMU";
	case MEDIA_CODEC_PCMA:
		return "PCMA";
	case MEDIA_CODEC_CLEARMODE:
		return "CLEARMODE";
	}
	return NULL;
}

static bool description_is_valid(
	const struct sdp_audio_description *description)
{
	int default_payload_type;

	if (description == NULL || description->rtp_port == 0 ||
	    description->rtcp_port == 0 || description->packet_duration_ms == 0 ||
	    description->packet_duration_ms >
		    SDP_MAX_PACKET_DURATION_MS ||
	    description->sample_rate != MEDIA_SAMPLE_RATE ||
	    description->channels != 1 ||
	    codec_encoding_name(description->codec) == NULL)
		return false;
	default_payload_type =
		media_codec_default_payload_type(description->codec);
	return default_payload_type >= 0 &&
	       description->payload_type == (uint8_t)default_payload_type;
}

enum sdp_result sdp_audio_description_init(
	struct sdp_audio_description *description,
	enum media_codec codec, uint16_t rtp_port,
	unsigned int packet_duration_ms, bool include_vbd_attributes)
{
	struct sdp_audio_description initialized;
	int payload_type = media_codec_default_payload_type(codec);

	if (description == NULL || payload_type < 0 || rtp_port == 0 ||
	    packet_duration_ms == 0 ||
	    packet_duration_ms > SDP_MAX_PACKET_DURATION_MS)
		return SDP_INVALID_ARGUMENT;

	memset(&initialized, 0, sizeof(initialized));
	initialized.codec = codec;
	initialized.payload_type = (uint8_t)payload_type;
	initialized.rtp_port = rtp_port;
	/* No separate RTCP socket exists; advertise the RTP port explicitly. */
	initialized.rtcp_port = rtp_port;
	initialized.packet_duration_ms = (uint16_t)packet_duration_ms;
	initialized.sample_rate = MEDIA_SAMPLE_RATE;
	initialized.channels = 1;
	initialized.include_vbd_attributes = include_vbd_attributes;
	*description = initialized;
	return SDP_OK;
}

const char *sdp_audio_encoding_name(
	const struct sdp_audio_description *description)
{
	return description_is_valid(description)
		       ? codec_encoding_name(description->codec)
		       : NULL;
}

const char *sdp_audio_attribute_name(
	enum sdp_audio_attribute attribute)
{
	switch (attribute) {
	case SDP_AUDIO_RTPMAP:
		return "rtpmap";
	case SDP_AUDIO_PTIME:
		return "ptime";
	case SDP_AUDIO_MAXPTIME:
		return "maxptime";
	case SDP_AUDIO_RTCP:
		return "rtcp";
	case SDP_AUDIO_SILENCE_SUPPRESSION:
		return "silenceSupp";
	case SDP_AUDIO_GPMD:
		return "gpmd";
	case SDP_AUDIO_ATTRIBUTE_COUNT:
		break;
	}
	return NULL;
}

static enum sdp_result finish_attribute(
	char *output, size_t capacity, int length)
{
	if (length < 0 || (size_t)length >= capacity) {
		output[0] = '\0';
		return SDP_OUTPUT_TOO_SMALL;
	}
	return SDP_OK;
}

enum sdp_result sdp_audio_format_attribute(
	const struct sdp_audio_description *description,
	enum sdp_audio_attribute attribute,
	char *output, size_t capacity)
{
	if (output == NULL || capacity == 0)
		return SDP_INVALID_ARGUMENT;
	output[0] = '\0';
	if (!description_is_valid(description) ||
	    sdp_audio_attribute_name(attribute) == NULL)
		return SDP_INVALID_ARGUMENT;

	switch (attribute) {
	case SDP_AUDIO_RTPMAP:
		return finish_attribute(
			output, capacity,
			snprintf(output, capacity, "%u %s/%u",
				 description->payload_type,
				 codec_encoding_name(description->codec),
				 description->sample_rate));
	case SDP_AUDIO_PTIME:
	case SDP_AUDIO_MAXPTIME:
		return finish_attribute(
			output, capacity,
			snprintf(output, capacity, "%u",
				 description->packet_duration_ms));
	case SDP_AUDIO_RTCP:
		return finish_attribute(
			output, capacity,
			snprintf(output, capacity, "%u", description->rtcp_port));
	case SDP_AUDIO_SILENCE_SUPPRESSION:
		if (!description->include_vbd_attributes)
			return SDP_NOT_PRESENT;
		return finish_attribute(
			output, capacity,
			snprintf(output, capacity, "%s", "off - - - -"));
	case SDP_AUDIO_GPMD:
		if (!description->include_vbd_attributes)
			return SDP_NOT_PRESENT;
		return finish_attribute(
			output, capacity,
			snprintf(output, capacity, "%u vbd=yes",
				 description->payload_type));
	case SDP_AUDIO_ATTRIBUTE_COUNT:
		break;
	}
	return SDP_INVALID_ARGUMENT;
}

static bool codec_has_static_payload(enum media_codec codec)
{
	return codec == MEDIA_CODEC_PCMU || codec == MEDIA_CODEC_PCMA;
}

bool sdp_audio_accepts_format(
	const struct sdp_audio_description *description,
	const struct sdp_audio_format *format)
{
	if (!description_is_valid(description) || format == NULL ||
	    format->payload_type != description->payload_type)
		return false;
	if (format->encoding_name == NULL || format->encoding_name[0] == '\0')
		return codec_has_static_payload(description->codec) &&
		       format->sample_rate == 0 && format->channels == 0;
	return strcasecmp(format->encoding_name,
			  codec_encoding_name(description->codec)) == 0 &&
	       format->sample_rate == description->sample_rate &&
	       (format->channels == 0 || format->channels == description->channels);
}

bool sdp_attribute_is_media_owned(const char *name)
{
	static const char *const owned_attributes[] = {
		"rtpmap",
		"fmtp",
		"rtcp-fb",
		"rtcp",
		"ssrc",
		"ssrc-group",
		"ptime",
		"maxptime",
		"silenceSupp",
		"gpmd",
	};

	if (name == NULL)
		return false;
	for (size_t index = 0;
	     index < sizeof(owned_attributes) / sizeof(owned_attributes[0]);
	     ++index) {
		if (strcasecmp(name, owned_attributes[index]) == 0)
			return true;
	}
	return false;
}

const char *sdp_result_name(enum sdp_result result)
{
	switch (result) {
	case SDP_OK:
		return "ok";
	case SDP_NOT_PRESENT:
		return "attribute not present";
	case SDP_INVALID_ARGUMENT:
		return "invalid argument";
	case SDP_OUTPUT_TOO_SMALL:
		return "output buffer too small";
	}
	return "unknown SDP result";
}
