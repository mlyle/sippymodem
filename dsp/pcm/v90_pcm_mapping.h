// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PCM_MAPPING_H
#define V90_PCM_MAPPING_H

#include "v90_cp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct v92_cpt_parameters;
struct v92_cpu_parameters;

enum {
	V90_PCM_FRAME_MAX_BITS = 42,
	PCM_FRAME_MAX_BITS = 48,
};

enum pcm_mapping_format {
	PCM_MAPPING_V90,
	PCM_MAPPING_V91,
};

enum v90_pcm_mapping_plane {
	V90_PCM_MAPPING_TRANSMIT,
	V90_PCM_MAPPING_CODEC,
};

enum v90_pcm_mapping_result {
	V90_PCM_MAPPING_OK,
	V90_PCM_MAPPING_INVALID_ARGUMENT,
	V90_PCM_MAPPING_INVALID_PARAMETERS,
	V90_PCM_MAPPING_OUT_OF_RANGE,
};

/* Owned six-interval PCM mapping derived from one accepted CPt or CP. Masks
 * remain packed because label lookup is infrequent during six-symbol frame
 * construction and this avoids a 768-byte expanded level table per plane. */
struct v90_pcm_mapping {
	struct pcm_constellation
		transmit[V90_CP_FRAME_INTERVALS];
	struct pcm_constellation
		codec[V90_CP_FRAME_INTERVALS];
	enum v90_cp_kind kind;
	enum pcm_mapping_format format;
	enum g711_law law;
	uint8_t modulus[V90_CP_FRAME_INTERVALS];
	uint8_t frame_bits;
	uint8_t modulus_bits;
	uint8_t sign_bits;
	uint8_t sign_redundancy;
	uint8_t spectral_lookahead;
	int8_t shaping_a1_q1_6;
	int8_t shaping_a2_q1_6;
	int8_t shaping_b1_q1_6;
	int8_t shaping_b2_q1_6;
	bool codec_plane_differs;
};

/* V.91 6.3: six differential sign bits and 15..42 modulus bits, with no
 * spectral shaping. Each interval supplies the peer-selected transmit set.
 * V.90/V.92 constructors retain their narrower rate limits. */
enum v90_pcm_mapping_result v90_pcm_mapping_init_v91(
	struct v90_pcm_mapping *mapping, enum g711_law law, uint8_t frame_bits,
	const struct pcm_constellation constellations[PCM_CONSTELLATION_FRAME_INTERVALS]);

enum v90_pcm_mapping_result v90_pcm_mapping_init(
	struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters);

/* The digital transmitter encodes the requested Ucodes in the network law
 * advertised in INFO0d. CP's codec-type field describes the peer codec; it
 * does not renegotiate that network law. Keep the received record unchanged
 * and derive the same mapping with an explicitly owned transmit encoding. */
enum v90_pcm_mapping_result v90_pcm_mapping_init_transmit(
	struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters, enum g711_law law);

/* Compare an accepted CP with the owned network transmit encoding. The
 * peer codec law in the CP does not change that encoding. */
bool v90_pcm_mapping_matches_transmit(
	const struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters, enum g711_law law);

/* Build the Phase 4 control mapping used after online rate renegotiation.
 * V.90 8.6 retains the accepted CPt constellation set and K while taking all
 * sign and spectral-shaping parameters from the preceding data mode.
 * Rejected calls leave mapping unchanged. */
enum v90_pcm_mapping_result
v90_pcm_mapping_init_rate_renegotiation(
	struct v90_pcm_mapping *mapping,
	const struct v90_pcm_mapping *training,
	const struct v90_pcm_mapping *preceding_data);

/* Derive V.92 maps with an explicitly owned network encoding, independent
 * of the peer codec field in CPt/CPu. Preserve the received parameters.
 * Rejected calls leave mapping unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_init_from_v92_cpt(
	struct v90_pcm_mapping *mapping,
	const struct v92_cpt_parameters *parameters, enum g711_law law);

/* Derive the shared downstream data map from an accepted V.92 CPu. A rate
 * number of zero requests cleardown and is rejected. Rejected calls leave
 * mapping unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_init_from_v92_cpu(
	struct v90_pcm_mapping *mapping,
	const struct v92_cpu_parameters *parameters, enum g711_law law);
bool v90_pcm_mapping_is_valid(const struct v90_pcm_mapping *mapping);
bool v90_pcm_mapping_equal(
	const struct v90_pcm_mapping *left,
	const struct v90_pcm_mapping *right);

/* Exact integer bounds on V.90 8.5.2 mean-square power over all 2^K
 * modulus words, not an unweighted mean of constellation levels. Select
 * the measurement plane and its G.711 law explicitly: a codec law need
 * not match the transmitter law. Bounds coincide for integral powers;
 * otherwise ceiling is floor + 1. Rejected calls leave both unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_power(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, enum g711_law law,
	uint32_t *floor_power, uint32_t *ceiling_power);

/* Map K LSB-first modulus bits into labels K0..K5. Rejected calls leave the
 * destination unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_encode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t *bits, size_t bit_count,
	uint8_t labels[V90_CP_FRAME_INTERVALS]);

/* Recover K LSB-first modulus bits from received labels K0..K5. Noise can
 * select unused tuples above 2^K: retain their low K bits rather than losing
 * frame synchronization. Out-of-constellation labels remain invalid, and
 * rejected calls leave the destination unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_decode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t labels[V90_CP_FRAME_INTERVALS],
	uint8_t *bits, size_t bit_count);

/* Resolve a label using the descending-Ucode ordering required by the PCM
 * mapper. Codec masks with incompatible cardinality fall back atomically to
 * the transmitter plane during initialization. */
enum v90_pcm_mapping_result v90_pcm_mapping_ucode(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t label, uint8_t *ucode);

/* Reverse a Ucode to its descending-order constellation label. Ucodes absent
 * from the selected interval and plane are rejected without changing label. */
enum v90_pcm_mapping_result v90_pcm_mapping_label(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t ucode, uint8_t *label);

const char *v90_pcm_mapping_result_name(
	enum v90_pcm_mapping_result result);

#endif /* V90_PCM_MAPPING_H */
