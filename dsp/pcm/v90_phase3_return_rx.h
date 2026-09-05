// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_RETURN_RX_H
#define V90_PHASE3_RETURN_RX_H

#include "v34_equalizer_handoff.h"
#include "v34_precoder_trainer.h"
#include "v90_phase3_return_marker.h"

#include <stdbool.h>
#include <stdint.h>

/* Continue the digital modem's trained equalizer after Ja. A bounded
 * alternating-point correlation reports S early enough to finish Jd. The
 * same equalizer and hard decisions then locate the complete S-bar boundary.
 * A carrier-tracked handoff receives a short additional confirmation window;
 * decision-directed adaptation begins only after S is qualified, avoiding
 * adaptation to the intervening signal. */
struct v90_phase3_return_rx {
	struct qam_equalizer equalizer;
	/* Second-return reacquisition trains both spans from the same S targets.
	 * The compact candidate remains isolated from the decision path until the
	 * learned long tail is accepted or rejected after the complete E
	 * training interval. */
	struct qam_equalizer compact_equalizer;
	struct v34_precoder_recommendation precoder;
	struct v90_phase3_return_marker semantic;
	double adaptation;
	bool equalizer_reacquiring;
	bool compact_equalizer_active;
};

/* The equalizer has already accepted its decision-delay points following the
 * final S-bar decision. Its next input therefore yields the first undelivered
 * Phase 4 decision without replay. */
struct v90_phase3_return_handoff {
	struct v34_phase3_s_detection marker;
	struct v34_equalizer_handoff equalizer;
	struct qam_equalizer compact_equalizer;
	struct v34_precoder_recommendation precoder;
	bool compact_equalizer_active;
};

enum v90_phase3_return_rx_result v90_phase3_return_rx_init(
	struct v90_phase3_return_rx *rx,
	const struct v34_equalizer_handoff *equalizer,
	double adaptation);
enum v90_phase3_return_rx_result v90_phase3_return_rx_init_with_precoder(
	struct v90_phase3_return_rx *rx,
	const struct v34_equalizer_handoff *equalizer,
	const struct v34_precoder_recommendation *precoder,
	double adaptation);
/* Permit a single interoperability fallback for the first V.90 return. The
 * fallback requires a qualified long S followed by a 16-symbol departure and
 * is distinct from a standards-visible S-bar. Restarting clears permission. */
enum v90_phase3_return_rx_result
v90_phase3_return_rx_allow_implicit_s_end(
	struct v90_phase3_return_rx *rx);
enum v90_phase3_return_rx_result v90_phase3_return_rx_restart(
	struct v90_phase3_return_rx *rx);
/* A qualified raw S prefix may expose a changed symbol boundary that the
 * equalizer inherited from the first return cannot slice. Train bounded long
 * and conventional spans from that known S point, retaining the carrier-loop
 * gains and requiring the ordinary complete marker before handing both to
 * Phase 4 training. */
enum v90_phase3_return_rx_result v90_phase3_return_rx_reacquire_s(
	struct v90_phase3_return_rx *rx,
	const struct qam_point *received);
bool v90_phase3_return_rx_is_valid(
	const struct v90_phase3_return_rx *rx);
bool v90_phase3_return_rx_is_complete(
	const struct v90_phase3_return_rx *rx);

/* Consume one finite carrier- and timing-recovered point. Complete and
 * invalid calls consume nothing. */
enum v90_phase3_return_rx_result v90_phase3_return_rx_point(
	struct v90_phase3_return_rx *rx,
	const struct qam_point *received);

bool v90_phase3_return_handoff_is_valid(
	const struct v90_phase3_return_handoff *handoff);
enum v90_phase3_return_rx_result v90_phase3_return_rx_handoff(
	const struct v90_phase3_return_rx *rx,
	struct v90_phase3_return_handoff *handoff);

const char *v90_phase3_return_rx_result_name(
	enum v90_phase3_return_rx_result result);

#endif /* V90_PHASE3_RETURN_RX_H */
