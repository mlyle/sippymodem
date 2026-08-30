// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INFO_H
#define V34_PHASE2_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_PHASE2_INFO_LEADING_FILL_BITS = 4,
	V34_PHASE2_INFO_SYNC_BITS = 8,
	V34_PHASE2_INFO_PREFIX_BITS =
		V34_PHASE2_INFO_LEADING_FILL_BITS +
		V34_PHASE2_INFO_SYNC_BITS,
	V34_PHASE2_INFO_CRC_BITS = 16,
	V34_PHASE2_INFO_TRAILING_FILL_BITS = 4,
	V34_PHASE2_INFO_MAX_PAYLOAD_BITS = 77,
	V34_PHASE2_INFO_MAX_PAYLOAD_BYTES =
		(V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 7) / 8,
	V34_PHASE2_INFO_MAX_FRAME_BITS =
		V34_PHASE2_INFO_PREFIX_BITS +
		V34_PHASE2_INFO_MAX_PAYLOAD_BITS +
		V34_PHASE2_INFO_CRC_BITS +
		V34_PHASE2_INFO_TRAILING_FILL_BITS,
};

enum v34_phase2_info_result {
	V34_PHASE2_INFO_OK,
	V34_PHASE2_INFO_INVALID_ARGUMENT,
};

/* Payload octets and every emitted field are packed most-significant bit first. */
struct v34_phase2_info_generator {
	uint8_t payload[V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];
	size_t payload_bit_count;
	size_t bit_index;
	uint16_t crc;
};

enum v34_phase2_info_event_type {
	V34_PHASE2_INFO_NO_EVENT,
	V34_PHASE2_INFO_FRAME_RECEIVED,
	V34_PHASE2_INFO_FRAME_REJECTED,
};

struct v34_phase2_info_event {
	enum v34_phase2_info_event_type type;
	uint8_t payload[V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];
	size_t payload_bit_count;
};

enum v34_phase2_info_receive_stage {
	V34_PHASE2_INFO_SEARCH_SYNC,
	V34_PHASE2_INFO_RECEIVE_PAYLOAD,
	V34_PHASE2_INFO_RECEIVE_CRC,
	V34_PHASE2_INFO_RECEIVE_TAIL,
};

struct v34_phase2_info_receiver {
	uint8_t payload[V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];
	size_t expected_payload_bit_count;
	size_t payload_bit_index;
	uint16_t sync_window;
	uint16_t crc;
	uint16_t received_crc;
	uint8_t crc_bit_count;
	uint8_t tail_bit_count;
	enum v34_phase2_info_receive_stage stage;
};

/* Phase 2 handshake state selects the INFO payload shape; its length is not
 * carried on the wire. Initialize the receiver with that expected length. */

size_t v34_phase2_info_frame_bit_count(
	size_t payload_bit_count);

enum v34_phase2_info_result
v34_phase2_info_generator_init(
	struct v34_phase2_info_generator *generator,
	const uint8_t *payload, size_t payload_bit_count);
bool v34_phase2_info_generator_is_valid(
	const struct v34_phase2_info_generator *generator);

/* frame_complete accompanies the final transmitted fill bit. */
enum v34_phase2_info_result
v34_phase2_info_next_bit(
	struct v34_phase2_info_generator *generator,
	uint8_t *bit, bool *frame_complete);

enum v34_phase2_info_result
v34_phase2_info_receiver_init(
	struct v34_phase2_info_receiver *receiver,
	size_t expected_payload_bit_count);
bool v34_phase2_info_receiver_is_valid(
	const struct v34_phase2_info_receiver *receiver);
enum v34_phase2_info_result
v34_phase2_info_receive_bit(
	struct v34_phase2_info_receiver *receiver, uint8_t bit,
	struct v34_phase2_info_event *event);

#endif
