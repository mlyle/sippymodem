// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_INFO_H
#define SIPPYMODEM_V34_PHASE2_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS = 16,
	SIPPYMODEM_V34_PHASE2_INFO_CRC_BITS = 16,
	SIPPYMODEM_V34_PHASE2_INFO_END_BITS = 4,
	SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS = 77,
	SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BYTES =
		(SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS + 7) / 8,
	SIPPYMODEM_V34_PHASE2_INFO_MAX_FRAME_BITS =
		SIPPYMODEM_V34_PHASE2_INFO_SYNC_BITS +
		SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BITS +
		SIPPYMODEM_V34_PHASE2_INFO_CRC_BITS +
		SIPPYMODEM_V34_PHASE2_INFO_END_BITS,
};

enum sippymodem_v34_phase2_info_result {
	SIPPYMODEM_V34_PHASE2_INFO_OK,
	SIPPYMODEM_V34_PHASE2_INFO_INVALID_ARGUMENT,
};

/* Payload octets and every emitted field are packed most-significant bit first. */
struct sippymodem_v34_phase2_info_generator {
	uint8_t payload[SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];
	size_t payload_bit_count;
	size_t bit_index;
	uint16_t crc;
};

enum sippymodem_v34_phase2_info_event_type {
	SIPPYMODEM_V34_PHASE2_INFO_NO_EVENT,
	SIPPYMODEM_V34_PHASE2_INFO_FRAME_RECEIVED,
	SIPPYMODEM_V34_PHASE2_INFO_FRAME_REJECTED,
};

struct sippymodem_v34_phase2_info_event {
	enum sippymodem_v34_phase2_info_event_type type;
	uint8_t payload[SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];
	size_t payload_bit_count;
};

enum sippymodem_v34_phase2_info_receive_stage {
	SIPPYMODEM_V34_PHASE2_INFO_SEARCH_SYNC,
	SIPPYMODEM_V34_PHASE2_INFO_RECEIVE_PAYLOAD,
	SIPPYMODEM_V34_PHASE2_INFO_RECEIVE_CRC,
	SIPPYMODEM_V34_PHASE2_INFO_RECEIVE_END,
};

struct sippymodem_v34_phase2_info_receiver {
	uint8_t payload[SIPPYMODEM_V34_PHASE2_INFO_MAX_PAYLOAD_BYTES];
	size_t expected_payload_bit_count;
	size_t payload_bit_index;
	uint16_t sync_window;
	uint16_t crc;
	uint16_t received_crc;
	uint8_t crc_bit_count;
	uint8_t end_bit_count;
	enum sippymodem_v34_phase2_info_receive_stage stage;
};

/* Phase 2 handshake state selects the INFO payload shape; its length is not
 * carried on the wire. Initialize the receiver with that expected length. */

size_t sippymodem_v34_phase2_info_frame_bit_count(
	size_t payload_bit_count);
enum sippymodem_v34_phase2_info_result sippymodem_v34_phase2_info_crc(
	const uint8_t *payload, size_t payload_bit_count, uint16_t *crc);

enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_generator_init(
	struct sippymodem_v34_phase2_info_generator *generator,
	const uint8_t *payload, size_t payload_bit_count);
bool sippymodem_v34_phase2_info_generator_is_valid(
	const struct sippymodem_v34_phase2_info_generator *generator);
enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_generator_reset(
	struct sippymodem_v34_phase2_info_generator *generator);

/* frame_complete accompanies the final end-marker bit. */
enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_next_bit(
	struct sippymodem_v34_phase2_info_generator *generator,
	uint8_t *bit, bool *frame_complete);

enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_receiver_init(
	struct sippymodem_v34_phase2_info_receiver *receiver,
	size_t expected_payload_bit_count);
bool sippymodem_v34_phase2_info_receiver_is_valid(
	const struct sippymodem_v34_phase2_info_receiver *receiver);
enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_receiver_reset(
	struct sippymodem_v34_phase2_info_receiver *receiver);
enum sippymodem_v34_phase2_info_result
sippymodem_v34_phase2_info_receive_bit(
	struct sippymodem_v34_phase2_info_receiver *receiver, uint8_t bit,
	struct sippymodem_v34_phase2_info_event *event);

const char *sippymodem_v34_phase2_info_result_name(
	enum sippymodem_v34_phase2_info_result result);

#endif
