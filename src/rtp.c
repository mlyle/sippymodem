// SPDX-License-Identifier: BSD-3-Clause

#include "rtp.h"

#include <string.h>

enum {
	RTP_VERSION = 2,
	RTP_VERSION_SHIFT = 6,
	RTP_PADDING_FLAG = 0x20,
	RTP_EXTENSION_FLAG = 0x10,
	RTP_CSRC_COUNT_MASK = 0x0f,
	RTP_MARKER_FLAG = 0x80,
	RTP_EXTENSION_HEADER_SIZE = 4,
};

static uint16_t read_u16be(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t read_u32be(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

static void write_u16be(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

static void write_u32be(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

enum rtp_result rtp_packet_parse(
	const uint8_t *packet,
	size_t packet_size,
	struct rtp_packet_view *view)
{
	struct rtp_packet_view parsed;
	size_t header_size;
	size_t padding_size = 0;

	if (packet == NULL || view == NULL)
		return RTP_INVALID_ARGUMENT;
	if (packet_size < RTP_HEADER_SIZE)
		return RTP_PACKET_TOO_SHORT;
	if (packet[0] >> RTP_VERSION_SHIFT != RTP_VERSION)
		return RTP_UNSUPPORTED_VERSION;

	header_size = RTP_HEADER_SIZE +
		      (size_t)(packet[0] & RTP_CSRC_COUNT_MASK) * 4;
	if (packet_size < header_size)
		return RTP_TRUNCATED_CSRC_LIST;

	if ((packet[0] & RTP_EXTENSION_FLAG) != 0) {
		size_t extension_size;

		if (packet_size - header_size < RTP_EXTENSION_HEADER_SIZE)
			return RTP_TRUNCATED_EXTENSION;
		extension_size = RTP_EXTENSION_HEADER_SIZE +
				 (size_t)read_u16be(packet + header_size + 2) * 4;
		if (packet_size - header_size < extension_size)
			return RTP_TRUNCATED_EXTENSION;
		header_size += extension_size;
	}

	if ((packet[0] & RTP_PADDING_FLAG) != 0) {
		padding_size = packet[packet_size - 1];
		if (padding_size == 0 || padding_size > packet_size - header_size)
			return RTP_INVALID_PADDING;
	}

	parsed.header.payload_type =
		(uint8_t)(packet[1] & RTP_MAX_PAYLOAD_TYPE);
	parsed.header.marker = (packet[1] & RTP_MARKER_FLAG) != 0;
	parsed.header.sequence = read_u16be(packet + 2);
	parsed.header.timestamp = read_u32be(packet + 4);
	parsed.header.ssrc = read_u32be(packet + 8);
	parsed.payload = packet + header_size;
	parsed.payload_size = packet_size - header_size - padding_size;
	parsed.header_size = header_size;
	parsed.padding_size = padding_size;
	*view = parsed;
	return RTP_OK;
}

enum rtp_result rtp_packet_write(
	uint8_t *packet,
	size_t capacity,
	const struct rtp_header *header,
	const uint8_t *payload,
	size_t payload_size,
	size_t *packet_size)
{
	if (packet == NULL || header == NULL || packet_size == NULL ||
	    (payload == NULL && payload_size != 0) ||
	    header->payload_type > RTP_MAX_PAYLOAD_TYPE)
		return RTP_INVALID_ARGUMENT;
	if (capacity < RTP_HEADER_SIZE ||
	    payload_size > capacity - RTP_HEADER_SIZE)
		return RTP_BUFFER_TOO_SMALL;

	if (payload_size != 0)
		memmove(packet + RTP_HEADER_SIZE, payload,
			payload_size);
	packet[0] = RTP_VERSION << RTP_VERSION_SHIFT;
	packet[1] = header->payload_type |
		    (header->marker ? RTP_MARKER_FLAG : 0);
	write_u16be(packet + 2, header->sequence);
	write_u32be(packet + 4, header->timestamp);
	write_u32be(packet + 8, header->ssrc);
	*packet_size = RTP_HEADER_SIZE + payload_size;
	return RTP_OK;
}

const char *rtp_result_name(enum rtp_result result)
{
	switch (result) {
	case RTP_OK:
		return "ok";
	case RTP_INVALID_ARGUMENT:
		return "invalid argument";
	case RTP_PACKET_TOO_SHORT:
		return "packet too short";
	case RTP_UNSUPPORTED_VERSION:
		return "unsupported RTP version";
	case RTP_TRUNCATED_CSRC_LIST:
		return "truncated CSRC list";
	case RTP_TRUNCATED_EXTENSION:
		return "truncated RTP extension";
	case RTP_INVALID_PADDING:
		return "invalid RTP padding";
	case RTP_BUFFER_TOO_SMALL:
		return "output buffer too small";
	}
	return "unknown RTP result";
}
