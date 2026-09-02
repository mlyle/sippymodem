// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_RECEIVE_H
#define V34_DATA_RECEIVE_H

#include <stdbool.h>

/* Shared status for the continuous V.34 channel-to-data receive path. */
enum v34_data_receive_result {
	/* Input was accepted without a publishable interval or mapping frame. */
	V34_DATA_RECEIVE_OK,
	/* A present-time interval was decided without a delayed mapping frame. */
	V34_DATA_RECEIVE_INTERVAL_READY,
	V34_DATA_RECEIVE_MAPPING_FRAME_READY,
	V34_DATA_RECEIVE_B1_IN_PROGRESS,
	V34_DATA_RECEIVE_B1_COMPLETE,
	/* A complete online S followed by S-bar was qualified. */
	V34_DATA_RECEIVE_RATE_RENEGOTIATION,
	/* An earlier line sample reached an unrecoverable decoding boundary. */
	V34_DATA_RECEIVE_FAILED,
	V34_DATA_RECEIVE_INVALID_ARGUMENT,
	V34_DATA_RECEIVE_INVALID_STATE,
	V34_DATA_RECEIVE_NUMERIC_FAILURE,
	V34_DATA_RECEIVE_OUT_OF_RANGE,
};

static inline bool v34_data_receive_emits_mapping_frame(
	enum v34_data_receive_result result)
{
	return result == V34_DATA_RECEIVE_MAPPING_FRAME_READY ||
	       result == V34_DATA_RECEIVE_B1_IN_PROGRESS ||
	       result == V34_DATA_RECEIVE_B1_COMPLETE;
}

#endif /* V34_DATA_RECEIVE_H */
