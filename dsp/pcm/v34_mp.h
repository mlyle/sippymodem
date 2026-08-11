// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_MP_H
#define V34_MP_H

#include "v34_precoder.h"
#include "v34_trellis.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_MP_TYPE0_FRAME_SYNC_BITS = 17,
	V34_MP_TYPE0_INFORMATION_BITS = 48,
	V34_MP_CRC_BITS = 16,
	V34_MP_TYPE0_FRAME_BITS = 88,
	V34_MP_TYPE0_FRAME_BYTES =
		V34_MP_TYPE0_FRAME_BITS / 8,
	V34_MP_TYPE1_INFORMATION_BITS = 144,
	V34_MP_TYPE1_FRAME_BITS = 188,
	V34_MP_TYPE1_FRAME_BYTES =
		(V34_MP_TYPE1_FRAME_BITS + 7) / 8,
	V34_MP_RATE_CAPABILITY_BITS = 14,
	V34_MP_RATE_CAPABILITY_MASK =
		(1u << V34_MP_RATE_CAPABILITY_BITS) - 1u,
	V34_MP_MINIMUM_RATE_2400_BPS_UNITS = 1,
	V34_MP_MAXIMUM_RATE_2400_BPS_UNITS = 14,
};

enum v34_mp_result {
	V34_MP_OK,
	V34_MP_INVALID_ARGUMENT,
	V34_MP_INVALID_PARAMETERS,
	V34_MP_INVALID_FRAME,
	V34_MP_CRC_MISMATCH,
};

/* Rate fields are multiples of 2400 bit/s. Rates 13 and 14 require separate
 * knowledge that the peer supports the 1664-point constellation. */
struct v34_mp_parameters {
	uint8_t maximum_call_to_answer_rate_2400_bps_units;
	uint8_t maximum_answer_to_call_rate_2400_bps_units;
	bool auxiliary_channel_enabled;
	enum v34_trellis_selection remote_transmitter_trellis;
	bool remote_transmitter_nonlinear_encoding;
	bool remote_transmitter_expanded_shaping;
	bool acknowledges_peer_mp;
	/* Bit zero represents 2400 bit/s, through bit 13 for 33 600 bit/s. */
	uint16_t data_rate_capability_mask;
	bool asymmetric_rates_enabled;
};

bool v34_mp_parameters_are_valid(
	const struct v34_mp_parameters *parameters);

/* Type 1 extends the common modulation parameters with the three complex
 * Q14 precoder taps h(1) through h(3), in transmission order. */
struct v34_mp_type1_parameters {
	struct v34_mp_parameters common;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
};

bool v34_mp_type1_parameters_are_valid(
	const struct v34_mp_type1_parameters *parameters);

enum v34_mp_receive_event_type {
	V34_MP_RECEIVE_NO_EVENT,
	V34_MP_RECEIVE_TYPE0,
	V34_MP_RECEIVE_TYPE1,
	V34_MP_RECEIVE_REJECTED,
};

union v34_mp_received_parameters {
	struct v34_mp_parameters type0;
	struct v34_mp_type1_parameters type1;
};

struct v34_mp_receive_event {
	enum v34_mp_receive_event_type type;
	/* Meaningful for a rejected frame; accepted frames report OK. */
	enum v34_mp_result frame_result;
	union v34_mp_received_parameters parameters;
};

enum v34_mp_receive_stage {
	V34_MP_SEARCH_SYNC,
	V34_MP_RECEIVE_TYPE,
	V34_MP_RECEIVE_FRAME,
};

struct v34_mp_receiver {
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES];
	size_t frame_bit_count;
	size_t bit_index;
	uint8_t sync_one_count;
	enum v34_mp_receive_stage stage;
};

/* Frames are packed in transmission order, most-significant bit first within
 * each octet. Reserved information bits are transmitted as zero and ignored
 * when decoding, but remain covered by the CRC. */
enum v34_mp_result v34_mp_type0_encode(
	const struct v34_mp_parameters *parameters,
	uint8_t *frame, size_t frame_capacity);
enum v34_mp_result v34_mp_type0_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v34_mp_parameters *parameters);
enum v34_mp_result v34_mp_type1_encode(
	const struct v34_mp_type1_parameters *parameters,
	uint8_t *frame, size_t frame_capacity);
enum v34_mp_result v34_mp_type1_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct v34_mp_type1_parameters *parameters);

/* Search a continuous descrambled bit stream for the 17-one MP prefix, infer
 * the record length from its type bit, and emit only CRC-validated typed
 * parameters. A rejected call leaves state and event unchanged. */
enum v34_mp_result v34_mp_receiver_init(
	struct v34_mp_receiver *receiver);
bool v34_mp_receiver_is_valid(
	const struct v34_mp_receiver *receiver);
enum v34_mp_result v34_mp_receive_bit(
	struct v34_mp_receiver *receiver, uint8_t bit,
	struct v34_mp_receive_event *event);

const char *v34_mp_result_name(
	enum v34_mp_result result);

#endif
