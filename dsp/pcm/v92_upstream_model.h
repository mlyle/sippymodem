// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_UPSTREAM_MODEL_H
#define V92_UPSTREAM_MODEL_H

#include "v92_observation.h"
#include "v92_pcm_frame.h"
#include "v92_residual.h"

/* Optional physical correction from the preceding rate epoch. Coding and
 * filter history are never retained. It seeds an acquisition candidate only;
 * that candidate must win a fresh fit before independent B1u validation. */
struct v92_upstream_seed {
	struct v92_residual residual;
	double gain;
};
bool v92_upstream_seed_is_valid(const struct v92_upstream_seed *seed);

/* One acquisition per mapping epoch, followed by immutable channel fits.
 * Fractional and rounded prefilter hypotheses share the line input, but not
 * coding histories. The data receiver owns the primary decoder; only the
 * alternate lives here. Neither model is copied into traceback paths. Direct
 * calls instead reuse the alternate for independent B1u validation, from the
 * end of acquisition until the final delayed B1u decision. Residual acquisition
 * reuses it for a known-B1 or physically seeded candidate, then discards
 * it before validation. */
struct v92_upstream_model {
	struct v92_observation model[2];
	struct v92_upstream_seed seed;
	/* The observation contract owns exactly one training scratch. */
	union {
		struct {
			int32_t x[V92_OBSERVATION_TRAINING], raw[V92_OBSERVATION_TRAINING];
			int16_t u[V92_OBSERVATION_TRAINING], wire[V92_OBSERVATION_TRAINING];
			int16_t host[V92_OBSERVATION_TRAINING];
		};
		struct v92_pcm_level_fit level;
	};
	uint64_t cost[2];
	/* Frozen single-codec quality prediction if neither tandem model fits. */
	double scalar_gain;
	int16_t coefficient;
	uint8_t valid, primary;
	bool acquiring, residual_acquiring;
	/* This large receiver is live only while a second hypothesis is active. */
	struct v92_pcm_frame_rx alternate;
};

struct v92_upstream_data_rx;
struct v92_upstream_data_line;
/* Freshly initialized, exclusively owned receiver; incompatible seeds are
 * declined without mutation. Missing acquisition media still closes fitting. */
bool v92_upstream_model_seed(struct v92_upstream_data_rx *receiver,
	const struct v92_upstream_seed *seed);
void v92_upstream_model_init(struct v92_upstream_data_rx *receiver);
bool v92_upstream_model_is_valid(const struct v92_upstream_data_rx *receiver);
bool v92_upstream_model_has_alternate(const struct v92_upstream_data_rx *receiver);
/* Internal, exclusively owned state. The enclosing frame transaction commits
 * these mutations only after decoding, quality evaluation and fitting succeed. */
void v92_upstream_model_observe(struct v92_upstream_model *model,
	const struct v92_pcm_frame_decision *decision, const struct v92_upstream_data_line *line);
void v92_upstream_model_fit(struct v92_upstream_model *model,
	struct v92_upstream_data_line *line, struct v92_pcm_frame_rx *decoder);
enum v92_pcm_frame_result v92_upstream_model_decode(struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision,
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS]);

#endif /* V92_UPSTREAM_MODEL_H */
