// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RTP_H
#define SIPPYMODEM_RTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_RTP_HEADER_SIZE = 12,
	SIPPYMODEM_RTP_MAX_PAYLOAD_TYPE = 127,
};

enum sippymodem_rtp_result {
	SIPPYMODEM_RTP_OK,
	SIPPYMODEM_RTP_INVALID_ARGUMENT,
	SIPPYMODEM_RTP_PACKET_TOO_SHORT,
	SIPPYMODEM_RTP_UNSUPPORTED_VERSION,
	SIPPYMODEM_RTP_TRUNCATED_CSRC_LIST,
	SIPPYMODEM_RTP_TRUNCATED_EXTENSION,
	SIPPYMODEM_RTP_INVALID_PADDING,
	SIPPYMODEM_RTP_BUFFER_TOO_SMALL,
};

struct sippymodem_rtp_header {
	uint8_t payload_type;
	bool marker;
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;
};

/* Pointers in a parsed view borrow storage from the input packet and remain
 * valid only while that packet is unchanged. */
struct sippymodem_rtp_packet_view {
	struct sippymodem_rtp_header header;
	const uint8_t *payload;
	size_t payload_size;
	size_t header_size;
	size_t padding_size;
};

enum sippymodem_rtp_result sippymodem_rtp_packet_parse(
	const uint8_t *packet,
	size_t packet_size,
	struct sippymodem_rtp_packet_view *view);

/* The writer emits the fixed RTP header used by sippymodem.  Payload storage
 * may overlap the output buffer; packet_size is updated only on success. */
enum sippymodem_rtp_result sippymodem_rtp_packet_write(
	uint8_t *packet,
	size_t capacity,
	const struct sippymodem_rtp_header *header,
	const uint8_t *payload,
	size_t payload_size,
	size_t *packet_size);

const char *sippymodem_rtp_result_name(enum sippymodem_rtp_result result);

#endif
