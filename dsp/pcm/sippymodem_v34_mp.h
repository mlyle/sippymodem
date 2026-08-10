// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_MP_H
#define SIPPYMODEM_V34_MP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_MP_TYPE0_FRAME_SYNC_BITS = 17,
	SIPPYMODEM_V34_MP_TYPE0_INFORMATION_BITS = 48,
	SIPPYMODEM_V34_MP_CRC_BITS = 16,
	SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS = 88,
	SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES =
		SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS / 8,
	SIPPYMODEM_V34_MP_RATE_CAPABILITY_BITS = 14,
	SIPPYMODEM_V34_MP_RATE_CAPABILITY_MASK =
		(1u << SIPPYMODEM_V34_MP_RATE_CAPABILITY_BITS) - 1u,
	SIPPYMODEM_V34_MP_MINIMUM_RATE_2400_BPS_UNITS = 1,
	SIPPYMODEM_V34_MP_MAXIMUM_RATE_2400_BPS_UNITS = 14,
};

enum sippymodem_v34_mp_trellis {
	SIPPYMODEM_V34_MP_TRELLIS_16_STATE,
	SIPPYMODEM_V34_MP_TRELLIS_32_STATE,
	SIPPYMODEM_V34_MP_TRELLIS_64_STATE,
};

enum sippymodem_v34_mp_result {
	SIPPYMODEM_V34_MP_OK,
	SIPPYMODEM_V34_MP_INVALID_ARGUMENT,
	SIPPYMODEM_V34_MP_INVALID_PARAMETERS,
	SIPPYMODEM_V34_MP_INVALID_FRAME,
	SIPPYMODEM_V34_MP_CRC_MISMATCH,
};

/* Rate fields are multiples of 2400 bit/s. Rates 13 and 14 require separate
 * knowledge that the peer supports the 1664-point constellation. */
struct sippymodem_v34_mp_parameters {
	uint8_t maximum_call_to_answer_rate_2400_bps_units;
	uint8_t maximum_answer_to_call_rate_2400_bps_units;
	bool auxiliary_channel_enabled;
	enum sippymodem_v34_mp_trellis remote_transmitter_trellis;
	bool remote_transmitter_nonlinear_encoding;
	bool remote_transmitter_expanded_shaping;
	bool acknowledges_peer_mp;
	/* Bit zero represents 2400 bit/s, through bit 13 for 33 600 bit/s. */
	uint16_t data_rate_capability_mask;
	bool asymmetric_rates_enabled;
};

bool sippymodem_v34_mp_parameters_are_valid(
	const struct sippymodem_v34_mp_parameters *parameters);

/* Frames are packed in transmission order, most-significant bit first within
 * each octet. Reserved information bits are transmitted as zero and ignored
 * when decoding, but remain covered by the CRC. */
enum sippymodem_v34_mp_result sippymodem_v34_mp_type0_encode(
	const struct sippymodem_v34_mp_parameters *parameters,
	uint8_t *frame, size_t frame_capacity);
enum sippymodem_v34_mp_result sippymodem_v34_mp_type0_decode(
	const uint8_t *frame, size_t frame_capacity,
	struct sippymodem_v34_mp_parameters *parameters);

const char *sippymodem_v34_mp_result_name(
	enum sippymodem_v34_mp_result result);

#endif
