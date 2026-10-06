// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_UPSTREAM_DATA_H
#define V90_UPSTREAM_DATA_H

#include "v34_data_stream.h"
#include "v90_mp.h"
#include "v90_upstream_mode.h"
#include "v90_phase4_e_line_tx.h"

enum v90_upstream_data_result {
	V90_UPSTREAM_DATA_OK,
	V90_UPSTREAM_DATA_INVALID_ARGUMENT,
	V90_UPSTREAM_DATA_INVALID_STATE,
	V90_UPSTREAM_DATA_INVALID_PARAMETERS,
	V90_UPSTREAM_DATA_NO_COMMON_RATE,
	V90_UPSTREAM_DATA_OUT_OF_RANGE,
	V90_UPSTREAM_DATA_CLIPPING_RISK,
	V90_UPSTREAM_DATA_NUMERIC_FAILURE,
};

/* Preserve the shared mode-selection error categories in both directions. */
enum v90_upstream_data_result v90_upstream_data_mode_result(
	enum v90_upstream_mode_result result);

/* Start the conventional V.34 upstream B1/data path after the analogue
 * modem's E sequence. The selected rate is the highest capability common to
 * CP, MP, the MP maximum, and the trained Phase 3 profile. */
enum v90_upstream_data_result v90_upstream_data_start_after_e(
	struct v34_data_stream *stream,
	const struct v90_phase4_e_line_tx *completed_e,
	const struct v90_mp_record *peer_mp);

const char *v90_upstream_data_result_name(
	enum v90_upstream_data_result result);

#endif /* V90_UPSTREAM_DATA_H */
