// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_INFO0_H
#define V90_INFO0_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_INFO0A_PAYLOAD_BITS = 17,
	V90_INFO0D_PAYLOAD_BITS = 30,
	V90_INFO0A_PAYLOAD_BYTES =
		(V90_INFO0A_PAYLOAD_BITS + 7) / 8,
	V90_INFO0D_PAYLOAD_BYTES =
		(V90_INFO0D_PAYLOAD_BITS + 7) / 8,
	V90_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS = 5,
};

enum v90_pcm_law {
	V90_PCM_MU_LAW,
	V90_PCM_A_LAW,
};

enum v90_info0_result {
	V90_INFO0_OK,
	V90_INFO0_INVALID_ARGUMENT,
	V90_INFO0_INVALID_CAPABILITIES,
	V90_INFO0_INVALID_PAYLOAD,
};

/* Fields shared by INFO0a and INFO0d, corresponding to wire bits 12:28.
 * Reserved bits 26:27 are emitted as zero and ignored on reception. */
struct v90_info0 {
	bool symbol_rate_2743_supported;
	bool symbol_rate_2800_supported;
	bool symbol_rate_3429_supported;
	bool low_carrier_3000_supported;
	bool high_carrier_3000_supported;
	bool low_carrier_3200_supported;
	bool high_carrier_3200_supported;
	bool symbol_rate_3429_transmit_permitted;
	bool transmit_power_reduction_supported;
	uint8_t maximum_symbol_rate_difference_steps;
	bool cme_modem;
	bool constellation_1664_supported;
	bool acknowledges_peer_info0;
};

struct v90_info0d {
	struct v90_info0 common;
	/* Codes defined by V.90 Table 7, not absolute dBm0 values. */
	uint8_t nominal_transmit_power;
	uint8_t maximum_transmit_power;
	bool measure_power_at_codec_output;
	enum v90_pcm_law pcm_law;
	bool upstream_symbol_rate_3429_supported;
};

bool v90_info0_is_valid(const struct v90_info0 *info0);
bool v90_info0d_is_valid(const struct v90_info0d *info0d);

enum v90_info0_result v90_info0a_encode(
	const struct v90_info0 *info0a, uint8_t *payload,
	size_t payload_capacity);
enum v90_info0_result v90_info0a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info0 *info0a);
enum v90_info0_result v90_info0d_encode(
	const struct v90_info0d *info0d, uint8_t *payload,
	size_t payload_capacity);
enum v90_info0_result v90_info0d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info0d *info0d);

const char *v90_info0_result_name(enum v90_info0_result result);

#endif
