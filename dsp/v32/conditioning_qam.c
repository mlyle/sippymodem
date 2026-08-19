// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning_qam.h"

#include "constellation.h"
#include "memory_region.h"

#include <math.h>
#include <stddef.h>

static const double CONDITIONING_GAIN_ADAPTATION = 1.0 / 32.0;
static const double MINIMUM_POINT_POWER = 1e-12;
static const uint64_t CONDITIONING_QAM_LINEAGE =
	UINT64_C(0x763332636f6e6401);

static bool point_has_power(const struct qam_point *point)
{
	double power;

	if (!point_is_finite(point))
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) && power > MINIMUM_POINT_POWER;
}

bool v32_conditioning_qam_is_valid(
	const struct v32_conditioning_qam *adapter)
{
	if (adapter == NULL || adapter->quadrant >= QPSK_QUADRANT_COUNT ||
	    adapter->has_previous_point != (adapter->epoch != 0) ||
	    adapter->gain_initialized != qam_gain_is_valid(&adapter->gain) ||
	    (!adapter->gain_initialized &&
	     (adapter->gain.channel_gain.in_phase != 0.0 ||
	      adapter->gain.channel_gain.quadrature != 0.0 ||
	      adapter->gain.adaptation != 0.0)))
		return false;
	if (!adapter->has_previous_point)
		return adapter->quadrant == 0 && !adapter->gain_initialized &&
		       adapter->previous_point.in_phase == 0.0 &&
		       adapter->previous_point.quadrature == 0.0;
	return point_has_power(&adapter->previous_point);
}

enum v32_conditioning_qam_result v32_conditioning_qam_init(
	struct v32_conditioning_qam *adapter)
{
	struct v32_conditioning_qam initialized = { 0 };

	if (adapter == NULL || !v32_conditioning_qam_is_valid(&initialized))
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	*adapter = initialized;
	return V32_CONDITIONING_QAM_OK;
}

static bool owners_are_compatible(
	const struct v32_conditioning_qam *adapter,
	const struct v32_conditioning_receiver *receiver)
{
	return adapter->has_previous_point ==
		       receiver->has_previous_quadrant &&
	       adapter->epoch == receiver->observation_epoch &&
	       (!adapter->has_previous_point ||
		(adapter->quadrant == receiver->previous_quadrant &&
		 receiver->observation_lineage == CONDITIONING_QAM_LINEAGE));
}

enum v32_conditioning_qam_result v32_conditioning_qam_observe(
	struct v32_conditioning_qam *adapter,
	struct v32_conditioning_receiver *receiver,
	const struct qam_point *point)
{
	struct v32_conditioning_qam next_adapter;
	struct v32_conditioning_receiver next_receiver;
	struct v32_conditioning_symbol_decision next_decision;
	struct v32_equalizer_observation observation;
	struct v32_fixed_point fixed_point;
	struct qam_point reference;
	enum v32_conditioning_receiver_result receiver_result;
	uint8_t quadrant;

	if (adapter == NULL || receiver == NULL || point == NULL ||
	    memory_regions_overlap(
		    adapter, sizeof(*adapter), receiver, sizeof(*receiver)) ||
	    memory_regions_overlap(
		    adapter, sizeof(*adapter), point, sizeof(*point)) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), point, sizeof(*point)) ||
	    !v32_conditioning_qam_is_valid(adapter) ||
	    !v32_conditioning_receiver_is_valid(receiver) ||
	    !owners_are_compatible(adapter, receiver) ||
	    !point_has_power(point) ||
	    adapter->epoch == UINT64_MAX)
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	if (v32_conditioning_receiver_is_complete(receiver))
		return V32_CONDITIONING_QAM_COMPLETE;
	next_adapter = *adapter;
	next_receiver = *receiver;
	quadrant = next_adapter.quadrant;
	if (next_adapter.has_previous_point) {
		uint8_t turn;

		if (qam_nearest_quarter_turn(
			    &next_adapter.previous_point, point, &turn) != QAM_OK)
			return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
		quadrant = (uint8_t)((quadrant + turn) & 3u);
	}
	if (v32_4800_fixed_point(quadrant, &fixed_point) !=
		    V32_CONSTELLATION_OK)
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	observation = (struct v32_equalizer_observation){
		.i = fixed_point.i,
		.q = fixed_point.q,
		.phase = 0,
		.magnitude = 1,
		.epoch = next_adapter.epoch + 1u,
		.lineage = CONDITIONING_QAM_LINEAGE,
	};
	receiver_result = v32_conditioning_receiver_observe(
		&next_receiver, &observation, &next_decision);
	if (receiver_result == V32_CONDITIONING_RECEIVER_COMPLETE)
		return V32_CONDITIONING_QAM_COMPLETE;
	if (receiver_result != V32_CONDITIONING_RECEIVER_OK)
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	if (next_decision.reference_known &&
	    next_decision.reference_confirmed) {
		if (v32_4800_constellation_point(
			    next_decision.reference_quadrant,
			    &reference) != V32_CONSTELLATION_OK)
			return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
		if (!next_adapter.gain_initialized) {
			if (qam_gain_init_from_reference(
				    &next_adapter.gain, point, &reference,
				    CONDITIONING_GAIN_ADAPTATION) != QAM_GAIN_OK)
				return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
			next_adapter.gain_initialized = true;
		} else if (qam_gain_track(
				   &next_adapter.gain, point, &reference) != QAM_GAIN_OK) {
			return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
		}
	}
	next_adapter.previous_point = *point;
	next_adapter.epoch = observation.epoch;
	next_adapter.quadrant = quadrant;
	next_adapter.has_previous_point = true;
	if (!v32_conditioning_qam_is_valid(&next_adapter) ||
	    !v32_conditioning_receiver_is_valid(&next_receiver) ||
	    !owners_are_compatible(&next_adapter, &next_receiver))
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	*adapter = next_adapter;
	*receiver = next_receiver;
	return V32_CONDITIONING_QAM_OK;
}

enum v32_conditioning_qam_result v32_conditioning_qam_handoff(
	const struct v32_conditioning_qam *adapter,
	const struct v32_conditioning_receiver *receiver,
	struct qam_point *point, struct qam_gain *gain)
{
	if (adapter == NULL || receiver == NULL || point == NULL ||
	    gain == NULL ||
	    memory_regions_overlap(
		    adapter, sizeof(*adapter), receiver, sizeof(*receiver)) ||
	    memory_regions_overlap(
		    adapter, sizeof(*adapter), point, sizeof(*point)) ||
	    memory_regions_overlap(
		    adapter, sizeof(*adapter), gain, sizeof(*gain)) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), point, sizeof(*point)) ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), gain, sizeof(*gain)) ||
	    memory_regions_overlap(point, sizeof(*point), gain, sizeof(*gain)))
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	if (!v32_conditioning_qam_is_valid(adapter) ||
	    !v32_conditioning_receiver_is_valid(receiver) ||
	    !owners_are_compatible(adapter, receiver) ||
	    !v32_conditioning_receiver_is_complete(receiver) ||
	    !adapter->gain_initialized)
		return V32_CONDITIONING_QAM_INVALID_ARGUMENT;
	*point = adapter->previous_point;
	*gain = adapter->gain;
	return V32_CONDITIONING_QAM_OK;
}
