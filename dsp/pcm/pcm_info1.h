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
};

/* Self-contained handoff from common Phase 2 into the selected Phase 3
 * child. The conventional record is shared by V.90 and V.92; PCM upstream is
 * possible only for V.92 when the digital INFO1d advertised support. */
struct pcm_info1_selection {
	struct pcm_startup_selection startup;
	struct pcm_info1d digital;
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

/* Decode the analog INFO1 payload and select the Phase 3 child. Malformed or
 * unsupported input leaves selection unchanged. */
enum pcm_info1_result pcm_info1_select(
	const struct pcm_startup_selection *startup,
	const struct pcm_info1d *digital, const uint8_t *analog_payload,
	size_t payload_capacity, size_t payload_bit_count,
	struct pcm_info1_selection *selection);

const char *pcm_info1_result_name(enum pcm_info1_result result);

#endif /* PCM_INFO1_H */
