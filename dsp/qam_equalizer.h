// SPDX-License-Identifier: BSD-3-Clause

#ifndef QAM_EQUALIZER_H
#define QAM_EQUALIZER_H

#include "qam.h"

#include <stdbool.h>

enum {
	QAM_EQUALIZER_DEFAULT_TAP_COUNT = 7,
	QAM_EQUALIZER_MAX_TAP_COUNT = 61,
	QAM_EQUALIZER_DELAY = 3,
};

enum qam_equalizer_result {
	QAM_EQUALIZER_OK,
	QAM_EQUALIZER_INVALID_ARGUMENT,
	QAM_EQUALIZER_INVALID_STATE,
};

/* A symbol-spaced, complex normalized-LMS equalizer. Storage is bounded at
 * QAM_EQUALIZER_MAX_TAP_COUNT, while each instance owns its active span and
 * decision delay. Ordinary and span-only initialization retain the historic
 * three-symbol delay. */
struct qam_equalizer {
	struct qam_point coefficients[
		QAM_EQUALIZER_MAX_TAP_COUNT];
	struct qam_point history[
		QAM_EQUALIZER_MAX_TAP_COUNT];
	struct qam_point last_output;
	/* Carrier tracking rotates decisions without folding a changing carrier
	 * phase into the learned channel impulse response. */
	struct qam_point last_unrotated_output;
	double carrier_phase;
	double carrier_frequency;
	double carrier_proportional_gain;
	double carrier_integral_gain;
	double last_carrier_phase;
	unsigned int tap_count;
	unsigned int decision_delay;
	unsigned int fill;
	bool can_adapt;
	bool carrier_tracking;
};

/* An explicitly owned gradient snapshot for a decision that becomes known
 * after a later symbol. Pending observations may be copied with their owning
 * receiver state, but must be adapted only along the same equalizer lineage. */
struct qam_equalizer_observation {
	struct qam_point history[
		QAM_EQUALIZER_MAX_TAP_COUNT];
	struct qam_point output;
	struct qam_point unrotated_output;
	double carrier_phase;
	unsigned int tap_count;
	unsigned int decision_delay;
	bool pending;
	bool carrier_tracking;
};

enum qam_equalizer_result qam_equalizer_init(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference);

/* Select a longer bounded span without changing the fixed decision delay.
 * tap_count must exceed QAM_EQUALIZER_DELAY and fit the owned storage. */
enum qam_equalizer_result qam_equalizer_init_span(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference,
	unsigned int tap_count);

/* Select both bounded FIR span and decision delay. decision_delay identifies
 * the tap seeded with the inverse of the initial flat-channel observation and
 * must be less than tap_count. */
enum qam_equalizer_result qam_equalizer_init_geometry(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference,
	unsigned int tap_count, unsigned int decision_delay);

/* Extend an idle equalizer without moving its decision point or disturbing
 * learned coefficients and sample history. Newly exposed taps start at zero.
 * The caller must own any retained observations; their old geometry cannot be
 * applied after a successful extension. */
enum qam_equalizer_result qam_equalizer_extend_span(
	struct qam_equalizer *equalizer, unsigned int tap_count);

/* Add a decision-directed second-order carrier loop. The proportional and
 * integral gains are fractions of the measured phase error applied per
 * equalized symbol. Enabling is allowed only with no unconsumed direct
 * adaptation output; existing channel taps and delay history are retained. */
enum qam_equalizer_result qam_equalizer_enable_carrier_tracking(
	struct qam_equalizer *equalizer, double proportional_gain,
	double integral_gain);
bool qam_equalizer_is_valid(
	const struct qam_equalizer *equalizer);

/* A delay-full equalizer with no unconsumed direct-adaptation output can be
 * transferred without replaying input or losing an adaptation decision. */
bool qam_equalizer_is_handoff_ready(
	const struct qam_equalizer *equalizer);

/* Push one received symbol.  ready is false until the decision delay has
 * filled.  Invalid calls leave the equalizer and outputs unchanged. */
enum qam_equalizer_result qam_equalizer_push(
	struct qam_equalizer *equalizer,
	const struct qam_point *received,
	struct qam_point *output, bool *ready);

/* Push one symbol while retaining the exact output and tap inputs needed for
 * delayed adaptation. observation must not already be pending. It changes
 * only when ready becomes true; the caller then owns it until adaptation or
 * deliberate discard. */
enum qam_equalizer_result qam_equalizer_observe(
	struct qam_equalizer *equalizer,
	const struct qam_point *received,
	struct qam_equalizer_observation *observation, bool *ready);

bool qam_equalizer_observation_is_pending(
	const struct qam_equalizer_observation *observation);

/* Adapt the most recently produced output toward a known or decided symbol.
 * Each output may be adapted at most once. */
enum qam_equalizer_result qam_equalizer_adapt(
	struct qam_equalizer *equalizer,
	const struct qam_point *target, double adaptation);

/* Apply and consume one delayed observation transactionally. The equalizer
 * may have accepted later symbols since the observation was produced. */
enum qam_equalizer_result
qam_equalizer_adapt_observation(
	struct qam_equalizer *equalizer,
	struct qam_equalizer_observation *observation,
	const struct qam_point *target, double adaptation);

const char *qam_equalizer_result_name(
	enum qam_equalizer_result result);

#endif
