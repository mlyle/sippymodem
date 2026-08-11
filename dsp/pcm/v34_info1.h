// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_INFO1_H
#define V34_INFO1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_INFO1C_PAYLOAD_BITS = 77,
	V34_INFO1C_PAYLOAD_BYTES =
		(V34_INFO1C_PAYLOAD_BITS + 7) / 8,
	V34_INFO1A_PAYLOAD_BITS = 38,
	V34_INFO1A_PAYLOAD_BYTES =
		(V34_INFO1A_PAYLOAD_BITS + 7) / 8,
	V34_INFO1_MAX_POWER_REDUCTION_DB = 7,
	V34_INFO1_MAX_MD_LENGTH_35_MS_UNITS = 127,
	V34_INFO1_MAX_PREEMPHASIS_FILTER_INDEX = 10,
	V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS = 14,
	V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE = -512,
	V34_INFO1_MAX_FREQUENCY_OFFSET_1_50_HZ_UNITS = 511,
};

enum v34_symbol_rate {
	V34_SYMBOL_RATE_2400,
	V34_SYMBOL_RATE_2743,
	V34_SYMBOL_RATE_2800,
	V34_SYMBOL_RATE_3000,
	V34_SYMBOL_RATE_3200,
	V34_SYMBOL_RATE_3429,
	V34_SYMBOL_RATE_COUNT,
};

enum v34_info1_result {
	V34_INFO1_OK,
	V34_INFO1_INVALID_ARGUMENT,
	V34_INFO1_INVALID_PARAMETERS,
	V34_INFO1_INVALID_PAYLOAD,
};

/* Fields common to INFO1c and INFO1a are named from the sender's point of
 * view. Frequency offset is signed in exact 0.02 Hz units; -512 means that a
 * sufficiently accurate measurement was unavailable. */
struct v34_info1_common {
	uint8_t minimum_peer_power_reduction_db;
	uint8_t additional_peer_power_reduction_db;
	uint8_t local_md_length_35_ms_units;
	int16_t frequency_offset_1_50_hz_units;
};

struct v34_info1_probe_result {
	bool high_carrier;
	uint8_t preemphasis_filter_index;
	uint8_t projected_data_rate_2400_bps_units;
};

struct v34_info1c {
	struct v34_info1_common common;
	/* Indexed by enum v34_symbol_rate. */
	struct v34_info1_probe_result
		answer_transmit[V34_SYMBOL_RATE_COUNT];
};

struct v34_info1a {
	struct v34_info1_common common;
	struct v34_info1_probe_result call_transmit;
	enum v34_symbol_rate answer_to_call_symbol_rate;
	enum v34_symbol_rate call_to_answer_symbol_rate;
};

bool v34_info1c_is_valid(
	const struct v34_info1c *info1c);
bool v34_info1a_is_valid(
	const struct v34_info1a *info1a);

/* Payload fields are packed in transmission order, most-significant bit first
 * within each octet. Multi-bit V.34 fields themselves are sent least-
 * significant bit first. Cross-field consistency with INFO0 is selected and
 * checked by the owning Phase 2 controller. */
enum v34_info1_result v34_info1c_encode(
	const struct v34_info1c *info1c, uint8_t *payload,
	size_t payload_capacity);
enum v34_info1_result v34_info1c_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info1c *info1c);

enum v34_info1_result v34_info1a_encode(
	const struct v34_info1a *info1a, uint8_t *payload,
	size_t payload_capacity);
enum v34_info1_result v34_info1a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info1a *info1a);

const char *v34_info1_result_name(
	enum v34_info1_result result);

#endif
