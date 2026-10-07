// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_INFO1_H
#define PCM_INFO1_H

#include "pcm_startup.h"
#include "v90_info1.h"
#include "v92_info1d.h"
#include "v92_pcm_info1a.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum pcm_upstream_mode {
	PCM_UPSTREAM_V34,
	PCM_UPSTREAM_PCM,
};

enum pcm_info1_result {
	PCM_INFO1_OK,
	PCM_INFO1_INVALID_ARGUMENT,
	PCM_INFO1_INVALID_RECORD,
	PCM_INFO1_PROTOCOL_MISMATCH,
	PCM_INFO1_INVALID_PAYLOAD,
	PCM_INFO1_PCM_UNAVAILABLE,
};

union pcm_info1d_record {
	struct v34_info1c v90;
	struct v92_info1d v92;
};

/* A protocol-tagged digital INFO1 record. Constructors prevent V.90/V.92
 * records from being confused at the common selection boundary. */
struct pcm_info1d {
	union pcm_info1d_record record;
	enum pcm_protocol protocol;
};

union pcm_info1a_record {
	struct v90_info1a conventional;
	struct v92_pcm_info1a pcm;
	struct v34_info1a v34_fallback;
};

enum pcm_info1a_kind {
	PCM_INFO1A_CONVENTIONAL,
	PCM_INFO1A_PCM,
	/* A conventional V.34 INFO1a declines the offered PCM session. */
	PCM_INFO1A_V34_FALLBACK,
};

/* Self-contained INFO1 decision for the selected Phase 3 child. Full Phase 2
 * retains INFO1d; short Phase 2 has no digital INFO1 record and takes the
 * conventional upstream carrier choice from INFO1a instead. The startup
 * phase2 tag selects the union member. Short PCM upstream uses Table 18
 * unchanged and leaves short_high_carrier false. */
struct pcm_info1_selection {
	struct pcm_startup_selection startup;
	union {
		struct pcm_info1d digital;
		bool short_high_carrier;
	};
	union pcm_info1a_record analog;
	enum pcm_upstream_mode upstream;
};

bool pcm_info1d_is_valid(const struct pcm_info1d *info1d);
bool pcm_info1_selection_is_valid(
	const struct pcm_info1_selection *selection);

enum pcm_info1_result pcm_info1d_init_v90(
	struct pcm_info1d *info1d, const struct v34_info1c *record);
enum pcm_info1_result pcm_info1d_init_v92(
	struct pcm_info1d *info1d, const struct v92_info1d *record);

/* Decode the full-Phase-2 analog INFO1 payload against the digital INFO1d.
 * Malformed or unsupported input leaves selection unchanged. */
enum pcm_info1_result pcm_info1_select(
	const struct pcm_startup_selection *startup,
	const struct pcm_info1d *digital, const uint8_t *analog_payload,
	size_t payload_capacity, size_t payload_bit_count,
	struct pcm_info1_selection *selection);

/* V.92 9.4 and Amendment 1 Tables 18/19: short Phase 2 exchanges only INFO1a.
 * Both INFO0 records must already have selected short Phase 2. The owner
 * supplies retained training policy separately at the Phase 3 boundary;
 * this decision neither invents INFO1d nor enables modem-level Quick Connect.
 * The ignored conventional frequency-offset field is normalized to -512.
 * Rejected calls leave selection/payload unchanged. */
enum pcm_info1_result pcm_info1_select_short(
	const struct pcm_startup_selection *startup,
	const uint8_t *analog_payload, size_t payload_capacity,
	size_t payload_bit_count, struct pcm_info1_selection *selection);
enum pcm_info1_result pcm_info1_encode_short(
	const struct pcm_info1_selection *selection,
	uint8_t *payload, size_t payload_capacity);

#endif /* PCM_INFO1_H */
