// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_H
#define RTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RTP_HEADER_SIZE = 12,
	RTP_MAX_PAYLOAD_TYPE = 127,
};

enum rtp_result {
	RTP_OK,
	RTP_INVALID_ARGUMENT,
	RTP_PACKET_TOO_SHORT,
	RTP_UNSUPPORTED_VERSION,
	RTP_TRUNCATED_CSRC_LIST,
	RTP_TRUNCATED_EXTENSION,
	RTP_INVALID_PADDING,
	RTP_BUFFER_TOO_SMALL,
};

struct rtp_header {
	uint8_t payload_type;
	bool marker;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;
};

/* Pointers in a parsed view borrow storage from the input packet and remain
 * valid only while that packet is unchanged. */
struct rtp_packet_view {
	struct rtp_header header;
	const uint8_t *payload;
	size_t payload_size;
	size_t header_size;
	size_t padding_size;
};

enum rtp_result rtp_packet_parse(
	const uint8_t *packet,
	size_t packet_size,
	struct rtp_packet_view *view);

/* The writer emits the fixed RTP header used by the media path. Payload storage
 * may overlap the output buffer; packet_size is updated only on success. */
enum rtp_result rtp_packet_write(
	uint8_t *packet,
	size_t capacity,
	const struct rtp_header *header,
	const uint8_t *payload,
	size_t payload_size,
	size_t *packet_size);

const char *rtp_result_name(enum rtp_result result);

#endif
