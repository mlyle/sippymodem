// SPDX-License-Identifier: BSD-3-Clause

#include <rtp.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct rtp_header example_header = {
	.payload_type = 0,
	.marker = true,
	.sequence = 0x1234,
	.timestamp = 0x56789abc,
	.ssrc = 0xdef01234,
};

static bool test_write_and_parse(void)
{
	static const uint8_t payload[] = { 0xff, 0x7e, 0x80, 0x00 };
	static const uint8_t expected[] = {
		0x80, 0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
		0xde, 0xf0, 0x12, 0x34, 0xff, 0x7e, 0x80, 0x00,
	};
	uint8_t packet[sizeof(expected)];
	struct rtp_packet_view view;
	size_t packet_size = 0;

	CHECK(rtp_packet_write(packet, sizeof(packet),
					   &example_header, payload,
					   sizeof(payload), &packet_size) ==
		      RTP_OK,
	      "failed to write RTP packet");
	CHECK(packet_size == sizeof(expected), "wrong RTP packet size");
	CHECK(memcmp(packet, expected, sizeof(expected)) == 0,
	      "RTP packet bytes are wrong");
	CHECK(rtp_packet_parse(packet, packet_size, &view) ==
		      RTP_OK,
	      "failed to parse written RTP packet");
	CHECK(view.header.payload_type == example_header.payload_type &&
		      view.header.marker == example_header.marker &&
		      view.header.sequence == example_header.sequence &&
		      view.header.timestamp == example_header.timestamp &&
		      view.header.ssrc == example_header.ssrc,
	      "parsed RTP header is wrong");
	CHECK(view.header_size == RTP_HEADER_SIZE &&
		      view.padding_size == 0,
	      "parsed RTP layout is wrong");
	CHECK(view.payload_size == sizeof(payload) &&
		      memcmp(view.payload, payload, sizeof(payload)) == 0,
	      "parsed RTP payload is wrong");
	return true;
}

static bool test_optional_headers_and_padding(void)
{
	static const uint8_t packet[] = {
		0xb2, 0x08, 0xab, 0xcd, 0x01, 0x02, 0x03, 0x04,
		0x11, 0x22, 0x33, 0x44,
		0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
		0xbe, 0xde, 0x00, 0x02,
		0xaa, 0xbb, 0xcc, 0xdd, 0x12, 0x34, 0x56, 0x78,
		0xde, 0xad, 0xbe, 0xef,
		0x00, 0x00, 0x00, 0x04,
	};
	struct rtp_packet_view view;

	CHECK(rtp_packet_parse(packet, sizeof(packet), &view) ==
		      RTP_OK,
	      "failed to parse RTP options");
	CHECK(view.header.payload_type == 8 && !view.header.marker,
	      "RTP flags were parsed incorrectly");
	CHECK(view.header.sequence == 0xabcd &&
		      view.header.timestamp == 0x01020304 &&
		      view.header.ssrc == 0x11223344,
	      "RTP integer fields were parsed incorrectly");
	CHECK(view.header_size == 32 && view.padding_size == 4,
	      "RTP option lengths were parsed incorrectly");
	CHECK(view.payload_size == 4 && view.payload[0] == 0xde &&
		      view.payload[3] == 0xef,
	      "RTP options changed the payload view");
	return true;
}

static bool expect_parse_error(uint8_t *packet, size_t size,
			       enum rtp_result expected)
{
	struct rtp_packet_view view;
	struct rtp_packet_view original;

	memset(&view, 0x5a, sizeof(view));
	view.header.sequence = 0x55aa;
	view.payload = packet;
	view.payload_size = 123;
	memcpy(&original, &view, sizeof(original));
	enum rtp_result result =
		rtp_packet_parse(packet, size, &view);

	CHECK(result == expected, "parse returned %s, expected %s",
	      rtp_result_name(result),
	      rtp_result_name(expected));
	CHECK(memcmp(&view, &original, sizeof(view)) == 0,
	      "failed parse partially changed its output");
	return true;
}

static bool test_malformed_packets(void)
{
	uint8_t packet[24] = { 0x80 };
	struct rtp_packet_view view;

	CHECK(rtp_packet_parse(NULL, sizeof(packet), &view) ==
		      RTP_INVALID_ARGUMENT,
	      "accepted a null packet");
	CHECK(rtp_packet_parse(packet, sizeof(packet), NULL) ==
		      RTP_INVALID_ARGUMENT,
	      "accepted a null view");
	CHECK(expect_parse_error(packet, 11,
				 RTP_PACKET_TOO_SHORT),
	      "short packet case failed");

	packet[0] = 0x40;
	CHECK(expect_parse_error(packet, 12,
				 RTP_UNSUPPORTED_VERSION),
	      "wrong version case failed");
	packet[0] = 0x83;
	CHECK(expect_parse_error(packet, 20,
				 RTP_TRUNCATED_CSRC_LIST),
	      "truncated CSRC case failed");
	packet[0] = 0x90;
	CHECK(expect_parse_error(packet, 15,
				 RTP_TRUNCATED_EXTENSION),
	      "missing extension header case failed");
	packet[14] = 0;
	packet[15] = 2;
	CHECK(expect_parse_error(packet, 20,
				 RTP_TRUNCATED_EXTENSION),
	      "truncated extension data case failed");

	memset(packet, 0, sizeof(packet));
	packet[0] = 0xa0;
	CHECK(expect_parse_error(packet, 13,
				 RTP_INVALID_PADDING),
	      "zero padding count case failed");
	packet[12] = 2;
	CHECK(expect_parse_error(packet, 13,
				 RTP_INVALID_PADDING),
	      "oversized padding case failed");
	return true;
}

static bool test_writer_validation_and_overlap(void)
{
	static const uint8_t payload[] = { 1, 2, 3, 4 };
	struct rtp_header header = example_header;
	uint8_t packet[16] = { 1, 2, 3, 4 };
	size_t packet_size = 999;

	CHECK(rtp_packet_write(packet, sizeof(packet), &header,
					   packet, 4, &packet_size) ==
		      RTP_OK,
	      "writer did not support overlapping payload");
	CHECK(packet_size == sizeof(packet) && packet[12] == 1 &&
		      packet[15] == 4,
	      "writer corrupted overlapping payload");

	packet_size = 999;
	CHECK(rtp_packet_write(packet, 15, &header, payload,
					   sizeof(payload), &packet_size) ==
		      RTP_BUFFER_TOO_SMALL,
	      "writer accepted a short buffer");
	CHECK(packet_size == 999, "failed write changed the output size");
	header.payload_type = 128;
	CHECK(rtp_packet_write(packet, sizeof(packet), &header,
					   payload, sizeof(payload),
					   &packet_size) ==
		      RTP_INVALID_ARGUMENT,
	      "writer accepted an invalid payload type");
	CHECK(rtp_packet_write(packet, sizeof(packet), &example_header,
					   NULL, 1, &packet_size) ==
		      RTP_INVALID_ARGUMENT,
	      "writer accepted a null payload");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "write and parse", test_write_and_parse },
		{ "optional headers and padding",
		  test_optional_headers_and_padding },
		{ "malformed packets", test_malformed_packets },
		{ "writer validation and overlap",
		  test_writer_validation_and_overlap },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
