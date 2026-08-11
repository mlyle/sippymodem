// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_FRAMING_H
#define V8_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V8_SYNC_MARK_BITS = 10,
	V8_SYNC_PATTERN_BITS = 10,
	V8_PREAMBLE_BITS =
		V8_SYNC_MARK_BITS +
		V8_SYNC_PATTERN_BITS,
	V8_FRAMED_OCTET_BITS = 10,
	/* Bound copied menu data while the semantic menu owner is rebuilt. */
	V8_MAX_INFORMATION_OCTETS = 16,
	V8_CJ_OCTETS = 3,
	V8_CJ_BITS =
		V8_CJ_OCTETS * V8_FRAMED_OCTET_BITS,
};

enum v8_framing_result {
	V8_FRAMING_OK,
	V8_FRAMING_INVALID_ARGUMENT,
};

struct v8_control_generator {
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t information_count;
	size_t bit_index;
};

enum v8_control_event_type {
	V8_CONTROL_NO_EVENT,
	V8_CONTROL_SEQUENCE_START,
	V8_CONTROL_INFORMATION_OCTET,
};

/* A sequence-start event closes the preceding sequence and starts the next;
 * the policy layer can therefore compare complete repeated menus. */
struct v8_control_event {
	enum v8_control_event_type type;
	uint8_t octet;
};

struct v8_control_receiver {
	uint32_t sync_window;
	uint8_t sync_bit_count;
	uint8_t frame_bit_index;
	uint8_t octet;
	bool receiving;
};

struct v8_cj_detector {
	uint16_t window;
	uint8_t window_bit_count;
	uint8_t matched_octets;
	uint8_t continuation_bit_index;
	bool detected;
};

/* Emit a repeating CM/JM sequence. Information octets are sent LSB first
 * between one start and one stop bit; sequence_complete marks each repeat. */
enum v8_framing_result v8_control_generator_init(
	struct v8_control_generator *generator,
	const uint8_t *information, size_t information_count);
enum v8_framing_result v8_control_next_bit(
	struct v8_control_generator *generator, uint8_t *bit,
	bool *sequence_complete);

enum v8_framing_result v8_control_receiver_init(
	struct v8_control_receiver *receiver);
void v8_control_receiver_reset(
	struct v8_control_receiver *receiver);
enum v8_framing_result v8_control_receive_bit(
	struct v8_control_receiver *receiver, uint8_t bit,
	struct v8_control_event *event);

/* CJ consists of three framed zero octets and is emitted only once. */
enum v8_framing_result v8_cj_bit(
	size_t bit_index, uint8_t *bit);
enum v8_framing_result v8_cj_detector_init(
	struct v8_cj_detector *detector);
void v8_cj_detector_reset(
	struct v8_cj_detector *detector);
enum v8_framing_result v8_cj_detect_bit(
	struct v8_cj_detector *detector, uint8_t bit,
	bool *detected);

const char *v8_framing_result_name(
	enum v8_framing_result result);

#endif
