// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_MP_H
#define V90_MP_H

#include "v34_precoder.h"
#include "v34_trellis.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_MP_FRAME_SYNC_BITS = 17,
	V90_MP_CRC_BITS = 16,
	V90_MP_TYPE0_MINIMUM_BITS = 86,
	V90_MP_TYPE1_MINIMUM_BITS = 188,
	V90_MP_TRAINING_FRAME_MINIMUM_BITS = 9,
	V90_MP_TRAINING_FRAME_MAXIMUM_BITS = 30,
	V90_MP_MAXIMUM_SEQUENCE_BITS = 210,
	V90_MP_MAXIMUM_SEQUENCE_BYTES =
		(V90_MP_MAXIMUM_SEQUENCE_BITS + 7) / 8,
	V90_MP_RATE_CAPABILITY_BITS = 13,
	V90_MP_RATE_CAPABILITY_MASK =
		(1u << V90_MP_RATE_CAPABILITY_BITS) - 1u,
	V90_MP_MINIMUM_RATE_2400_BPS_UNITS = 2,
	/* V.90 requires rates through 28.8 kbit/s; 31.2 and 33.6 kbit/s are
	 * optional. */
	V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS = 12,
	V90_MP_MAXIMUM_RATE_2400_BPS_UNITS = 14,
};

enum v90_mp_result {
	V90_MP_OK,
	V90_MP_INVALID_ARGUMENT,
	V90_MP_INVALID_PARAMETERS,
	V90_MP_NEED_OUTPUT,
	V90_MP_INVALID_FRAME,
	V90_MP_CRC_MISMATCH,
};

/* These parameters describe the V.34 upstream transmitter in the analogue
 * modem. A maximum rate of zero requests cleardown; otherwise the rate is in
 * 2400 bit/s units. Bit zero of the capability mask represents 4800 bit/s. */
struct v90_mp_parameters {
	uint8_t maximum_upstream_rate_2400_bps_units;
	enum v34_trellis_selection upstream_trellis;
	bool upstream_nonlinear_encoding;
	bool upstream_expanded_shaping;
	bool acknowledges_cp;
	uint16_t upstream_rate_capability_mask;
};

bool v90_mp_parameters_are_valid(
	const struct v90_mp_parameters *parameters);

struct v90_mp_type1_parameters {
	struct v90_mp_parameters common;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
};

bool v90_mp_type1_parameters_are_valid(
	const struct v90_mp_type1_parameters *parameters);

enum v90_mp_record_type {
	V90_MP_RECORD_TYPE0,
	V90_MP_RECORD_TYPE1,
};

union v90_mp_record_parameters {
	struct v90_mp_parameters type0;
	struct v90_mp_type1_parameters type1;
};

struct v90_mp_record {
	enum v90_mp_record_type type;
	union v90_mp_record_parameters parameters;
};

bool v90_mp_record_is_valid(const struct v90_mp_record *record);

/* Lower the advertised maximum to the highest enabled rate at or below the
 * requested ceiling. A valid record with no such enabled rate is left
 * unchanged, as is an existing lower maximum or cleardown request. Invalid
 * calls leave the record unchanged. */
bool v90_mp_record_limit_maximum_upstream_rate(
	struct v90_mp_record *record,
	uint8_t maximum_rate_2400_bps_units);

/* MP-prime repeats the selected MP record with only the CP acknowledgement
 * changed. These helpers keep that comparison independent of structure
 * padding and make the shared field explicit for both record layouts. Invalid
 * records never compare equal and never report an acknowledgement. */
bool v90_mp_record_acknowledges_cp(
	const struct v90_mp_record *record);
bool v90_mp_record_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right);
bool v90_mp_record_parameters_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right);

/* Return the exact sequence length after the mandatory zero fill extends the
 * record to a whole six-symbol mapping frame. Rejected calls leave bit_count
 * unchanged. */
enum v90_mp_result v90_mp_sequence_size(
	enum v90_mp_record_type type, uint8_t training_frame_bits,
	size_t *bit_count);

/* Sequences are packed in transmission order, most-significant bit first in
 * each octet; multibit MP fields themselves are sent least-significant bit
 * first. Encoding writes only the reported sequence bytes. Rejected calls
 * leave sequence and sequence_bit_count unchanged. */
enum v90_mp_result v90_mp_encode(
	const struct v90_mp_record *record, uint8_t training_frame_bits,
	uint8_t *sequence, size_t sequence_capacity,
	size_t *sequence_bit_count);

/* Decode one exact, padded MP sequence. Reserved information bits are covered
 * by the CRC but otherwise ignored. Rejected calls leave record unchanged. */
enum v90_mp_result v90_mp_decode(
	const uint8_t *sequence, size_t sequence_bit_count,
	uint8_t training_frame_bits, struct v90_mp_record *record);

const char *v90_mp_result_name(enum v90_mp_result result);

#endif /* V90_MP_H */
