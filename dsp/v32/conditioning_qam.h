// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONDITIONING_QAM_H
#define V32_CONDITIONING_QAM_H

#include "conditioning_receiver.h"
#include "qam_gain.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_conditioning_qam_result {
	V32_CONDITIONING_QAM_OK,
	V32_CONDITIONING_QAM_COMPLETE,
	V32_CONDITIONING_QAM_INVALID_ARGUMENT,
};

/* Temporary bridge from the floating-point QAM demodulator into the fixed
 * conditioning receiver.  The datapump owns this state; the receiver remains
 * independent of the legacy flat-channel estimate. */
struct v32_conditioning_qam {
	struct qam_gain gain;
	struct qam_point previous_point;
	uint64_t epoch;
	uint8_t quadrant;
	bool has_previous_point;
	bool gain_initialized;
};

enum v32_conditioning_qam_result v32_conditioning_qam_init(
	struct v32_conditioning_qam *adapter);
bool v32_conditioning_qam_is_valid(
	const struct v32_conditioning_qam *adapter);

/* Convert one nonzero demodulator point into one consecutive fixed-domain
 * observation.  Receiver and adapter are committed together only after every
 * fixed decision and gain update succeeds. */
enum v32_conditioning_qam_result v32_conditioning_qam_observe(
	struct v32_conditioning_qam *adapter,
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point);

/* Export the offset-preserving decoder seed required by the temporary QAM
 * receive path.  The fixed receive path uses the receiver's nominal seed. */
enum v32_conditioning_qam_result v32_conditioning_qam_framing_state(
	const struct v32_conditioning_qam *adapter,
	const struct v32_conditioning_receiver *receiver,
	struct v32_symbol_decoder *decoder);

/* Export the final legacy point/gain pair after conditioning completes. */
enum v32_conditioning_qam_result v32_conditioning_qam_handoff(
	const struct v32_conditioning_qam *adapter,
	const struct v32_conditioning_receiver *receiver,
	struct qam_point *point, struct qam_gain *gain);

#endif
