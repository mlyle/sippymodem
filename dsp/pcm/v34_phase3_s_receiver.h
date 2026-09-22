// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_S_RECEIVER_H
#define V34_PHASE3_S_RECEIVER_H

#include "v34_analytic_filter.h"
#include "v34_phase3_s_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Five points provide four alternating quarter-turn transitions. */
	V34_PHASE3_S_PREFIX_TURNS = 4,
	/* If timing lands between alternating S decisions, points two symbols
	 * apart still recur while adjacent points remain distinct. */
	V34_PHASE3_S_PREFIX_RECURRENCE_LAG = 2,
	V34_PHASE3_S_PREFIX_RECURRENCE_HITS = 8,
	V34_PHASE3_S_RECEIVER_MAX_S_ERRORS = 8,
	/* A companded line can smear the S-to-S-bar reversal across several
	 * equalized decisions.  Keep this bound small relative to the 16T marker;
	 * the retained S suffix still anchors the phase. */
	V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS = 4,
	/* A sample-domain receiver may lock after S begins. Retaining at least
	 * 32T before the 16T S-bar still gives a bounded correlation. */
	V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS = 32,
	/* Four lag-two recurrence hits plus the retained points qualify an
	 * online S candidate without assuming absolute carrier phase. */
	V34_PHASE3_S_HUNT_CANDIDATE_RECURRENCE_HITS = 4,
	V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS =
		V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
		V34_PHASE3_S_HUNT_CANDIDATE_RECURRENCE_HITS,
	V34_PHASE3_S_HUNT_CANDIDATE_SYMBOLS =
		V34_PHASE3_S_SEQUENCE_SYMBOLS -
		V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS +
		V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	/* Preserve a complete suffix before the S-bar boundary from adaptive
	 * feedback. A fractionally spaced equalizer is non-causal at its decision
	 * point, so the visible transition can precede the hard marker. */
	V34_PHASE3_S_HUNT_ADAPTATION_SYMBOLS =
		V34_PHASE3_S_SYMBOLS -
		V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS,
};

enum v34_phase3_s_prefix_result {
	V34_PHASE3_S_PREFIX_OK,
	V34_PHASE3_S_PREFIX_DETECTED,
	V34_PHASE3_S_PREFIX_REBASE_TIMING,
	V34_PHASE3_S_PREFIX_COMPLETE,
	V34_PHASE3_S_PREFIX_INVALID_ARGUMENT,
};

/* Qualify the alternating relative phase at the start of S without assuming
 * the channel's absolute carrier phase. This early prefix gate lets a line
 * receiver reject unrelated pre-S energy while retaining enough of the
 * finite 128T S interval for timing and equalizer acquisition. */
struct v34_phase3_s_prefix {
	struct qam_point previous;
	struct qam_point recurrence[V34_PHASE3_S_PREFIX_RECURRENCE_LAG];
	uint8_t previous_turn;
	uint8_t alternating_turns;
	uint8_t recurrence_points;
	uint8_t recurrence_hits;
	bool previous_valid;
	bool recurrence_qualified;
	bool require_recurrence;
	bool complete;
};

bool v34_phase3_s_prefix_is_valid(
	const struct v34_phase3_s_prefix *prefix);
enum v34_phase3_s_prefix_result v34_phase3_s_prefix_init(
	struct v34_phase3_s_prefix *prefix);
/* Require both the ordinary alternating turns and continuing lag-two
 * recurrence evidence. This stronger gate is for a receiver that searches
 * through structured data before using S to rebuild its equalizer. */
enum v34_phase3_s_prefix_result v34_phase3_s_prefix_init_recurrent(
	struct v34_phase3_s_prefix *prefix);
enum v34_phase3_s_prefix_result v34_phase3_s_prefix_receive(
	struct v34_phase3_s_prefix *prefix,
	const struct qam_point *point, double minimum_point_power);

enum v34_phase3_s_receiver_result {
	V34_PHASE3_S_RECEIVER_OK,
	/* This symbol completed a qualified S/S-bar marker. */
	V34_PHASE3_S_RECEIVER_DETECTED,
	/* A marker was already detected; no symbol was consumed. */
	V34_PHASE3_S_RECEIVER_COMPLETE,
	V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT,
};

struct v34_phase3_s_detection {
	/* Received rotation of the first point in S, including any fixed
	 * quarter-turn ambiguity left by the carrier/equalizer front end. */
	uint8_t clockwise_quarter_turn_offset;
	uint8_t s_errors;
	uint8_t s_bar_errors;
};

/* One known S target for the equalizer owner. The target is meaningful only
 * when `adapt` is true; S-bar and a pending boundary remain detector evidence
 * and must not train the equalizer. */
struct v34_phase3_s_adaptation {
	struct v34_analytic_sample target;
	bool adapt;
};

bool v34_phase3_s_detection_is_valid(
	const struct v34_phase3_s_detection *detection);

/* Bounded hard-decision history for locating S followed by the complete 16T
 * S-bar marker at an arbitrary input boundary. The ordinary initializer
 * requires all 128T of S; a suffix initializer supports a receiver that locks
 * during S. The complete retained S suffix supplies the phase anchor and keeps
 * the full bounded S error budget; S and S-bar remain separate so a smeared
 * transition cannot veto an otherwise qualified marker. Fixed-distance
 * evidence for a complete S-bar can publish the same hard alignment
 * immediately; an otherwise non-exact four-symbol boundary is confirmed by
 * later decisions. Expected rotations are generated during correlation. */
struct v34_phase3_s_receiver {
	/* Low nibble: hard training decision. High nibble: fixed-distance
	 * evidence for rotations 0..3 when metric_enabled is set. */
	uint8_t decisions[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_detection detection;
	struct v34_phase3_s_detection pending_detection;
	uint8_t next_decision;
	uint8_t buffered_decisions;
	uint8_t s_symbols;
	uint8_t pending_symbols;
	/* Bounded decisions consumed after the retained marker while a
	 * transition-error candidate awaited confirmation. */
	uint8_t post_marker_symbols;
	/* Next and alternate S targets, cleared before any decision and after
	 * detection. Shared by every semantic consumer, independent of the
	 * decision-history cursor or the phase at which reception started. */
	uint8_t target_rotation[2];
	bool detection_pending;
	bool pending_metric;
	bool metric_enabled;
	bool complete;
};

/* Locate S/S-bar in an arbitrary online symbol stream. The recurrence gate
 * prevents ordinary data from becoming equalizer feedback; once qualified,
 * the bounded suffix receiver supplies known S targets and stops adapting at
 * S-bar. */
struct v34_phase3_s_hunt {
	/* One absolute decision history covers every fixed carrier rotation. */
	struct v34_phase3_s_receiver marker;
	struct v34_phase3_s_prefix prefix;
	/* Keep the detector's bounded confirmation tail so the next protocol
	 * owner can replay decisions consumed beyond the exact S-bar boundary. */
	struct v34_analytic_sample
		recent_observation[V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS];
	uint16_t candidate_symbols;
	uint8_t candidate_rotation[2];
	uint8_t next_target_rotation;
	uint8_t recent_observation_count;
	/* Training may acquire phase across hard-decision quadrant boundaries;
	 * online data keeps the stricter adjacent-quadrant candidate gate. */
	bool acquire_phase;
	bool candidate;
	bool s_bar_seen;
};

bool v34_phase3_s_hunt_is_valid(
	const struct v34_phase3_s_hunt *hunt);
enum v34_phase3_s_receiver_result v34_phase3_s_hunt_init(
	struct v34_phase3_s_hunt *hunt);
/* Use only while waiting for training, never while delivering online data.
 * Retains the measured recurrence gate and complete S/S-bar requirement. */
enum v34_phase3_s_receiver_result v34_phase3_s_hunt_init_training(
	struct v34_phase3_s_hunt *hunt);
bool v34_phase3_s_hunt_is_complete(
	const struct v34_phase3_s_hunt *hunt);
/* `control` is true when this observation belongs to a qualified candidate;
 * `adaptation` then supplies its known S target when adaptation is safe. */
enum v34_phase3_s_receiver_result v34_phase3_s_hunt_receive(
	struct v34_phase3_s_hunt *hunt,
	struct v34_analytic_sample observation,
	struct v34_phase3_s_adaptation *adaptation, bool *control);
bool v34_phase3_s_hunt_get_detection(
	const struct v34_phase3_s_hunt *hunt,
	struct v34_phase3_s_detection *detection);
bool v34_phase3_s_hunt_get_post_marker_symbols(
	const struct v34_phase3_s_hunt *hunt, uint8_t *symbols);
/* Copy one observation consumed after the exact S-bar boundary. `index` is
 * ordered from the first post-marker observation. */
bool v34_phase3_s_hunt_get_post_marker_observation(
	const struct v34_phase3_s_hunt *hunt, uint8_t index,
	struct v34_analytic_sample *observation);

/* Detect a suffix of S followed by the complete S-bar. This is for receivers
 * whose carrier and symbol timing lock after S has already begun. */
enum v34_phase3_s_receiver_result v34_phase3_s_receiver_init_suffix(
	struct v34_phase3_s_receiver *receiver, uint8_t s_symbols);
/* Use hard decisions first, with the fixed-domain relative metric as a
 * bounded fallback when companding collapses the decision quadrants. */
enum v34_phase3_s_receiver_result
v34_phase3_s_receiver_init_fixed_suffix(
	struct v34_phase3_s_receiver *receiver, uint8_t s_symbols);

bool v34_phase3_s_receiver_is_valid(
	const struct v34_phase3_s_receiver *receiver);
bool v34_phase3_s_receiver_is_complete(
	const struct v34_phase3_s_receiver *receiver);

/* Accept one hard decision in the training-constellation convention. S and
 * S-bar use quarter point zero; a different valid point counts as a symbol
 * error. Labels outside 0..3 and already-complete calls consume nothing. An
 * exact transition reports at the end of S-bar. If the physical transition
 * smears either side of the nominal boundary, fixed-distance evidence may
 * confirm the complete S-bar immediately; otherwise the receiver waits a
 * bounded number of decisions for a better alignment. */
enum v34_phase3_s_receiver_result v34_phase3_s_receiver_receive(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns);

/* Receive one fixed-domain decision and generate the alternating S target
 * from the same owned cursor. This is the adaptive path used by V.34 Phase 3
 * and Phase 4. Rejected and completed calls leave both outputs unchanged. */
enum v34_phase3_s_receiver_result
v34_phase3_s_receiver_receive_adaptive(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns,
	struct v34_analytic_sample observation,
	struct v34_phase3_s_adaptation *adaptation);

/* Copy the completed correlation result. A false return leaves the output
 * unchanged. */
bool v34_phase3_s_receiver_get_detection(
	const struct v34_phase3_s_receiver *receiver,
	struct v34_phase3_s_detection *detection);

#endif /* V34_PHASE3_S_RECEIVER_H */
