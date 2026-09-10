// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_UPSTREAM_MODEL_H
#define V92_UPSTREAM_MODEL_H

#include "v92_observation.h"
#include "v92_pcm_frame.h"

/* One acquisition per mapping epoch, followed by immutable channel fits.
 * Fractional and rounded prefilter hypotheses share the line input, but not
 * coding histories. The data receiver owns the primary decoder; only the
 * alternate lives here. Neither model is copied into traceback paths. */
struct v92_upstream_model {
	struct v92_observation model[2];
	struct v92_pcm_frame_rx alternate;
	struct v92_pcm_frame_tx_state known;
	int32_t x[V92_OBSERVATION_TRAINING], raw[V92_OBSERVATION_TRAINING];
	int16_t u[V92_OBSERVATION_TRAINING], wire[V92_OBSERVATION_TRAINING];
	uint64_t cost[2];
	int16_t coefficient;
	uint8_t valid, primary;
	bool acquiring;
};

struct v92_upstream_data_rx;
struct v92_upstream_data_line;
void v92_upstream_model_init(struct v92_upstream_data_rx *receiver);
bool v92_upstream_model_is_valid(const struct v92_upstream_data_rx *receiver);
/* Internal, exclusively owned state. The enclosing frame transaction commits
 * these mutations only after decoding, quality evaluation and fitting succeed. */
bool v92_upstream_model_observe(struct v92_upstream_model *model,
	const struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line);
void v92_upstream_model_fit(struct v92_upstream_model *model,
	const struct v92_upstream_data_line *line, const struct v92_pcm_frame_rx *decoder);
enum v92_pcm_frame_result v92_upstream_model_decode(struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision,
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS]);

#endif /* V92_UPSTREAM_MODEL_H */
