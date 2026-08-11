// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V8_FRAMING_H
#define SIPPYMODEM_V8_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V8_SYNC_MARK_BITS = 10,
	SIPPYMODEM_V8_SYNC_PATTERN_BITS = 10,
	SIPPYMODEM_V8_PREAMBLE_BITS =
		SIPPYMODEM_V8_SYNC_MARK_BITS +
		SIPPYMODEM_V8_SYNC_PATTERN_BITS,
	SIPPYMODEM_V8_FRAMED_OCTET_BITS = 10,
	/* Bound copied menu data while the semantic menu owner is rebuilt. */
	SIPPYMODEM_V8_MAX_INFORMATION_OCTETS = 16,
	SIPPYMODEM_V8_CJ_OCTETS = 3,
	SIPPYMODEM_V8_CJ_BITS =
		SIPPYMODEM_V8_CJ_OCTETS * SIPPYMODEM_V8_FRAMED_OCTET_BITS,
};

enum sippymodem_v8_framing_result {
	SIPPYMODEM_V8_FRAMING_OK,
	SIPPYMODEM_V8_FRAMING_INVALID_ARGUMENT,
};

struct sippymodem_v8_control_generator {
	uint8_t information[SIPPYMODEM_V8_MAX_INFORMATION_OCTETS];
	size_t information_count;
	size_t bit_index;
};

enum sippymodem_v8_control_event_type {
	SIPPYMODEM_V8_CONTROL_NO_EVENT,
	SIPPYMODEM_V8_CONTROL_SEQUENCE_START,
	SIPPYMODEM_V8_CONTROL_INFORMATION_OCTET,
};

/* A sequence-start event closes the preceding sequence and starts the next;
 * the policy layer can therefore compare complete repeated menus. */
struct sippymodem_v8_control_event {
	enum sippymodem_v8_control_event_type type;
	uint8_t octet;
};

struct sippymodem_v8_control_receiver {
	uint32_t sync_window;
	uint8_t sync_bit_count;
	uint8_t frame_bit_index;
	uint8_t octet;
	bool receiving;
};

struct sippymodem_v8_cj_detector {
	uint16_t window;
	uint8_t window_bit_count;
	uint8_t matched_octets;
	uint8_t continuation_bit_index;
	bool detected;
};

/* Emit a repeating CM/JM sequence. Information octets are sent LSB first
 * between one start and one stop bit; sequence_complete marks each repeat. */
enum sippymodem_v8_framing_result sippymodem_v8_control_generator_init(
	struct sippymodem_v8_control_generator *generator,
	const uint8_t *information, size_t information_count);
enum sippymodem_v8_framing_result sippymodem_v8_control_next_bit(
	struct sippymodem_v8_control_generator *generator, uint8_t *bit,
	bool *sequence_complete);

enum sippymodem_v8_framing_result sippymodem_v8_control_receiver_init(
	struct sippymodem_v8_control_receiver *receiver);
void sippymodem_v8_control_receiver_reset(
	struct sippymodem_v8_control_receiver *receiver);
enum sippymodem_v8_framing_result sippymodem_v8_control_receive_bit(
	struct sippymodem_v8_control_receiver *receiver, uint8_t bit,
	struct sippymodem_v8_control_event *event);

/* CJ consists of three framed zero octets and is emitted only once. */
enum sippymodem_v8_framing_result sippymodem_v8_cj_bit(
	size_t bit_index, uint8_t *bit);
enum sippymodem_v8_framing_result sippymodem_v8_cj_detector_init(
	struct sippymodem_v8_cj_detector *detector);
void sippymodem_v8_cj_detector_reset(
	struct sippymodem_v8_cj_detector *detector);
enum sippymodem_v8_framing_result sippymodem_v8_cj_detect_bit(
	struct sippymodem_v8_cj_detector *detector, uint8_t bit,
	bool *detected);

const char *sippymodem_v8_framing_result_name(
	enum sippymodem_v8_framing_result result);

#endif
