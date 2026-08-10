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

/* Emit a repeating CM/JM sequence. Information octets are sent LSB first
 * between one start and one stop bit; sequence_complete marks each repeat. */
enum sippymodem_v8_framing_result sippymodem_v8_control_generator_init(
	struct sippymodem_v8_control_generator *generator,
	const uint8_t *information, size_t information_count);
enum sippymodem_v8_framing_result sippymodem_v8_control_next_bit(
	struct sippymodem_v8_control_generator *generator, uint8_t *bit,
	bool *sequence_complete);

/* CJ consists of three framed zero octets and is emitted only once. */
enum sippymodem_v8_framing_result sippymodem_v8_cj_bit(
	size_t bit_index, uint8_t *bit);

const char *sippymodem_v8_framing_result_name(
	enum sippymodem_v8_framing_result result);

#endif
