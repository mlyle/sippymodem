// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_RATE_DECISION_H
#define V32_RATE_DECISION_H

#include "rate_signal.h"
#include "uncoded_receiver.h"

/* Tentative four-point framing and rate/E detection for one equalizer
 * observation. The path owns event admission and physical commit; publish
 * these semantic owners only after committing the physical decision. */
struct v32_rate_decision {
	struct v32_uncoded_receiver four_point;
	struct v32_rate_signal_detector detector;
	struct v32_uncoded_symbol symbol;
	struct v32_rate_offer offer;
	enum v32_rate_signal_event event;
};

/* All storage is separate. Never change the active decoder/detector, and
 * leave the candidate unchanged on failure. No pointers are retained. */
static inline bool v32_rate_decision_prepare(
	const struct v32_uncoded_receiver *four_point,
	const struct v32_rate_signal_detector *detector,
	const struct v32_equalizer_observation *observation,
	struct v32_rate_decision *decision)
{
	if (four_point == NULL || detector == NULL ||
	    observation == NULL || decision == NULL)
		return false;
	struct v32_rate_decision next = {
		.four_point = *four_point,
		.detector = *detector,
	};

	if (v32_uncoded_receiver_observe(
		    &next.four_point, V32_4800_BITS_PER_SYMBOL,
		    observation, true, &next.symbol) != V32_UNCODED_RECEIVER_OK ||
	    v32_rate_signal_detector_observe(
		    &next.detector, next.symbol.group, &next.event,
		    &next.offer) != V32_RATE_SIGNAL_OK)
		return false;
	*decision = next;
	return true;
}

#endif /* V32_RATE_DECISION_H */
