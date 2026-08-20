// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONDITIONING_PATH_H
#define V32_CONDITIONING_PATH_H

#include "conditioning_receiver.h"
#include "receive_path.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_conditioning_path_result {
	V32_CONDITIONING_PATH_OK,
	V32_CONDITIONING_PATH_COMPLETE,
	V32_CONDITIONING_PATH_INVALID_ARGUMENT,
	V32_CONDITIONING_PATH_INVALID_STATE,
};

/* Own the continuous physical receive lane and conditioning protocol until
 * the exact committed observation which makes TRN sufficient. */
struct v32_conditioning_path {
	struct v32_receive_path receive;
	struct v32_conditioning_receiver conditioning;
	uint64_t observation_epoch;
};

/* The next rate/E owner must retain the physical lane unchanged.  A timing
 * epoch may already have exposed its first observation while the final TRN
 * decision was being committed, so that observation crosses the boundary too. */
struct v32_conditioning_path_handoff {
	struct v32_receive_path receive;
	struct v32_symbol_decoder decoder;
	struct v32_equalizer_observation observation;
	bool observation_ready;
};

enum v32_conditioning_path_result v32_conditioning_path_init(
	struct v32_conditioning_path *path,
	enum v32_transmitter_role source_role,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols);

/* Start fresh peer conditioning on a retained physical lane. The source must
 * still be paused at an open post-echo boundary and is never modified. */
enum v32_conditioning_path_result v32_conditioning_path_init_from_receive(
	struct v32_conditioning_path *path,
	enum v32_transmitter_role source_role,
	const struct v32_receive_path *receive);
bool v32_conditioning_path_is_valid(
	const struct v32_conditioning_path *path);
bool v32_conditioning_path_is_complete(
	const struct v32_conditioning_path *path);

/* Consume one 8 kHz receive sample after the corresponding transmit sample.
 * Every recovered observation is decided and committed before more line input
 * is accepted.  COMPLETE consumes the final conditioning sample. */
enum v32_conditioning_path_result v32_conditioning_path_sample(
	struct v32_conditioning_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference);

bool v32_conditioning_path_handoff_is_valid(
	const struct v32_conditioning_path_handoff *handoff);

/* Copy the exact post-conditioning physical and framing boundary.  A failed
 * call leaves the destination unchanged. */
enum v32_conditioning_path_result v32_conditioning_path_handoff(
	const struct v32_conditioning_path *path,
	struct v32_conditioning_path_handoff *handoff);

#endif
