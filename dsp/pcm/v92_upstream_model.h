// SPDX-License-Identifier: BSD-3-Clause
#ifndef V92_UPSTREAM_MODEL_H
#define V92_UPSTREAM_MODEL_H

#include "v92_observation.h"
#include "v92_observation_fit.h"
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

/* The selected CPd fixes the causal observation order for the whole epoch. */
static inline unsigned v92_upstream_model_taps(const struct v92_pcm_mapping *mapping)
{
	const struct v92_cpd_filter *feedback = &mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK];
	return feedback->count >= 3 && feedback->coefficient[2] ? 4 : 3;
}

/* Native fitting cadence, in accepted 8 kHz inputs. More expensive retained
 * channel refinements get a longer interval than one gain-sweep slice. */
enum { V92_MODEL_SWEEP_SAMPLES = 3, V92_MODEL_REFINE_SAMPLES = 12 };

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
	bool acquiring, residual_acquiring, fitting;
	/* Acquisition reuses the inactive competing decoder for pointer-free fit
	 * scratch. All fitted hypotheses finish before independent B1u decoding. */
	union {
		struct v92_pcm_frame_rx alternate;
		struct {
			struct v92_observation_fit_work work;
			int32_t input[V92_OBSERVATION_TRAINING];
			int16_t samples[V92_OBSERVATION_TRAINING];
			double offsets[V92_OBSERVATION_TRAINING];
			unsigned candidate, clock;
		} fit;
	};
};

struct v92_upstream_data_rx;
struct v92_upstream_data_line;
/* Freshly initialized, exclusively owned receiver; incompatible seeds are
 * declined without mutation. Missing acquisition media still closes fitting. */
bool v92_upstream_model_seed(struct v92_upstream_data_rx *receiver,
	const struct v92_upstream_seed *seed);
/* Exclusively owned receiver. Call only after the complete independent B1u
 * bit/quality checks pass. A strongly separated model can then release the
 * competing decoder; uncertain models remain paired. Fits stay immutable. */
bool v92_upstream_model_finish_b1u(struct v92_upstream_data_rx *receiver);
void v92_upstream_model_progress(struct v92_upstream_data_rx *receiver);
void v92_upstream_model_init(struct v92_upstream_data_rx *receiver);
bool v92_upstream_model_is_valid(const struct v92_upstream_data_rx *receiver);
bool v92_upstream_model_has_alternate(const struct v92_upstream_data_rx *receiver);
/* Internal, exclusively owned state. The enclosing frame transaction commits
 * these mutations only after decoding, quality evaluation and fitting succeed.
 * Optional geometry is epoch-owned scratch; reset on new fit publication. */
void v92_upstream_model_observe(struct v92_upstream_model *model,
	const struct v92_pcm_frame_decision *decision, const struct v92_upstream_data_line *line);
void v92_upstream_model_fit(struct v92_upstream_model *model,
	struct v92_upstream_data_line *line, struct v92_pcm_frame_rx *decoder);
enum v92_pcm_frame_result v92_upstream_model_decode(struct v92_upstream_model *model,
	struct v92_pcm_frame_rx *decoder, const struct v92_upstream_data_line *line,
	uint8_t *bits, struct v92_pcm_frame_decision *decision,
	double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS],
	struct v92_observation_geometry geometry[2]);

#endif /* V92_UPSTREAM_MODEL_H */
