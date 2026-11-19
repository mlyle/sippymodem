// SPDX-License-Identifier: BSD-3-Clause

#include "echo_canceller.h"

#include "memory_region.h"
#include "ring_history.h"
#include "sample_math.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* A silent, not-yet-filled reference must produce a zero update, not 0/0. */
static const double V32_ECHO_ENERGY_FLOOR = 1e-12;

static bool geometry_is_valid(
	uint16_t near_delay_symbols, uint16_t round_trip_symbols)
{
	size_t oldest = (size_t)near_delay_symbols +
			(size_t)round_trip_symbols +
			V32_ECHO_WINDOW_SYMBOLS - 1u;

	return near_delay_symbols <= V32_ECHO_MAX_NEAR_DELAY_SYMBOLS &&
	       round_trip_symbols >= V32_ECHO_MIN_ROUND_TRIP_SYMBOLS &&
	       round_trip_symbols <= V32_ECHO_MAX_ROUND_TRIP_SYMBOLS &&
	       oldest < V32_ECHO_REFERENCE_CAPACITY;
}

bool v32_echo_canceller_header_is_valid(
	const struct v32_echo_canceller *canceller)
{
	return canceller != NULL &&
	       geometry_is_valid(
		       canceller->near_delay_symbols,
		       canceller->round_trip_symbols) &&
	       ring_history_state_is_valid(
		canceller->reference_write, canceller->reference_fill,
		V32_ECHO_REFERENCE_CAPACITY) &&
	       echo_timing_is_valid(&canceller->far_timing) &&
	       (!canceller->pending_far_delay_symbols ||
		(!canceller->adapting && geometry_is_valid(
			canceller->near_delay_symbols,
			canceller->pending_far_delay_symbols))) &&
	       canceller->pending_samples <= V32_ECHO_PHASE_COUNT &&
	       isfinite(canceller->reference_energy) &&
	       canceller->reference_energy >= 0.0 &&
	       ((canceller->pending_samples != 0 &&
	         canceller->pending_samples != V32_ECHO_PHASE_COUNT) ||
	        canceller->reference_energy == 0.0) &&
	       qam_adaptation_is_valid(canceller->adaptation) &&
	       (!canceller->adapting || canceller->adaptation > 0.0);
}

struct echo_reference_windows {
	struct qam_point points[V32_ECHO_WINDOW_COUNT][V32_ECHO_WINDOW_SYMBOLS];
};

/* The near and far references are fixed across all phases of a symbol. Read
 * each available span once in tap order, then pad only the unfilled tail. */
static void read_reference_windows(const struct v32_echo_canceller *canceller,
	struct echo_reference_windows *windows)
{
	for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window) {
		size_t age = canceller->near_delay_symbols +
			(window == V32_ECHO_DELAYED_WINDOW ? canceller->round_trip_symbols : 0u);
		size_t count = age < canceller->reference_fill ?
			canceller->reference_fill - age : 0u;
		size_t newest = ring_index_at_age(canceller->reference_write,
			V32_ECHO_REFERENCE_CAPACITY, age + 1u);
		struct ring_reverse_span spans[2];
		struct qam_point *destination = windows->points[window];

		if (count > V32_ECHO_WINDOW_SYMBOLS)
			count = V32_ECHO_WINDOW_SYMBOLS;
		ring_reverse_spans(newest, count, V32_ECHO_REFERENCE_CAPACITY, spans);
		for (size_t span = 0; span < 2; ++span)
			for (size_t index = spans[span].end; index != spans[span].begin;)
				*destination++ = canceller->reference[--index];
		while (destination != windows->points[window] + V32_ECHO_WINDOW_SYMBOLS)
			*destination++ = (struct qam_point){ 0 };
	}
}

bool v32_echo_canceller_is_valid(
	const struct v32_echo_canceller *canceller)
{
	if (!v32_echo_canceller_header_is_valid(canceller))
		return false;
	unsigned finite = qam_points_finite_reduction(
		canceller->reference, V32_ECHO_REFERENCE_CAPACITY);
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase)
		for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window)
			finite &= qam_points_finite_reduction(
				canceller->coefficients[phase][window], V32_ECHO_WINDOW_SYMBOLS);
	if (!finite)
		return false;
	if (canceller->pending_samples != 0 &&
	    canceller->pending_samples != V32_ECHO_PHASE_COUNT) {
		struct echo_reference_windows windows;
		double energy = 0.0;

		read_reference_windows(canceller, &windows);
		for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window)
			for (size_t tap = 0; tap < V32_ECHO_WINDOW_SYMBOLS; ++tap)
				energy += qam_point_power(&windows.points[window][tap]);
		return energy == canceller->reference_energy;
	}
	return true;
}

enum v32_echo_result v32_echo_canceller_init(
	struct v32_echo_canceller *canceller,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols)
{
	if (canceller == NULL ||
	    !geometry_is_valid(near_delay_symbols, round_trip_symbols))
		return V32_ECHO_INVALID_ARGUMENT;
	memset(canceller, 0, sizeof(*canceller));
	canceller->near_delay_symbols = near_delay_symbols;
	canceller->round_trip_symbols = round_trip_symbols;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_canceller_start_adaptation(
	struct v32_echo_canceller *canceller, double adaptation)
{
	if (!v32_echo_canceller_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (!qam_adaptation_is_valid(adaptation) || adaptation == 0.0)
		return V32_ECHO_INVALID_ARGUMENT;
	if (canceller->pending_samples)
		return V32_ECHO_INVALID_STATE;
	echo_timing_reset(&canceller->far_timing);
	canceller->pending_far_delay_symbols = 0;
	canceller->far_suspended = false;
	canceller->adaptation = adaptation;
	canceller->adapting = true;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_canceller_freeze(
	struct v32_echo_canceller *canceller)
{
	if (!v32_echo_canceller_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (canceller->pending_samples)
		return V32_ECHO_INVALID_STATE;
	canceller->adapting = false;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_canceller_push_reference(
	struct v32_echo_canceller *canceller,
	const struct qam_point *reference)
{
	double power;

	if (!v32_echo_canceller_header_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (!point_is_finite(reference) ||
	    memory_regions_overlap(
		    reference, sizeof(*reference),
		    canceller, sizeof(*canceller)))
		return V32_ECHO_INVALID_ARGUMENT;
	if (canceller->pending_samples)
		return V32_ECHO_INVALID_STATE;
	power = qam_point_power(reference);
	if (!isfinite(power))
		return V32_ECHO_INVALID_ARGUMENT;
	canceller->reference[canceller->reference_write] = *reference;
	ring_history_advance(&canceller->reference_write,
		&canceller->reference_fill, V32_ECHO_REFERENCE_CAPACITY);
	canceller->pending_samples = V32_ECHO_PHASE_COUNT;
	return V32_ECHO_OK;
}

static inline bool prediction_and_energy(
	const struct v32_echo_canceller *canceller,
	const struct echo_reference_windows *windows, size_t phase,
	double *prediction, double *energy, double *far_prediction)
{
	double predicted = 0.0;
	double far = 0.0;
	double reference_energy = 0.0;

	for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window) {
		for (size_t tap = 0;
		     tap < V32_ECHO_WINDOW_SYMBOLS; ++tap) {
			const struct qam_point *point = &windows->points[window][tap];
			const struct qam_point *coefficient =
				&canceller->coefficients[phase][window][tap];

			double value = qam_point_correlation(coefficient, point).in_phase;
			predicted += value;
			if (window == V32_ECHO_DELAYED_WINDOW)
				far += value;
			if (energy != NULL)
				reference_energy += qam_point_power(point);
		}
	}
	*far_prediction = far;
	*prediction = predicted;
	if (energy != NULL)
		*energy = reference_energy;
	/* Every real correlation includes both reference coordinates. A
	 * nonfinite coordinate or coefficient propagates, including infinity
	 * times zero. Energy is measured only on the first phase; the owner
	 * keeps that reference window fixed through all three phases. */
	return isfinite(predicted) && isfinite(reference_energy) && isfinite(far);
}

static bool prepare_update(
	const struct v32_echo_canceller *canceller,
	const struct echo_reference_windows *windows, size_t phase,
	double scale, struct qam_point updated[V32_ECHO_WINDOW_COUNT]
					      [V32_ECHO_WINDOW_SYMBOLS])
{
	for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window) {
		for (size_t tap = 0;
		     tap < V32_ECHO_WINDOW_SYMBOLS; ++tap) {
			const struct qam_point *point = &windows->points[window][tap];
			const struct qam_point *coefficient =
				&canceller->coefficients[phase][window][tap];

			updated[window][tap] = (struct qam_point){
				.in_phase = coefficient->in_phase + scale * point->in_phase,
				.quadrature = coefficient->quadrature + scale * point->quadrature,
			};
			if (!point_is_finite(&updated[window][tap]))
				return false;
		}
	}
	return true;
}

/* The shared monitor measures 7.2 kHz samples. Preserve all three phase
 * filters by accepting integral 2.4 kHz symbol changes, and apply the accepted
 * delay at the next symbol boundary without resetting TX history. */
static bool retime_far(void *owner, int change_samples)
{
	struct v32_echo_canceller *canceller = owner;
	int64_t delay = (int64_t)canceller->round_trip_symbols +
		change_samples / V32_ECHO_PHASE_COUNT;
	if (change_samples % V32_ECHO_PHASE_COUNT != 0 ||
	    delay < V32_ECHO_MIN_ROUND_TRIP_SYMBOLS ||
	    delay > V32_ECHO_MAX_ROUND_TRIP_SYMBOLS ||
	    (uint64_t)delay + canceller->near_delay_symbols +
		V32_ECHO_WINDOW_SYMBOLS > canceller->reference_fill)
		return false;
	canceller->pending_far_delay_symbols = (uint16_t)delay;
	return true;
}

enum v32_echo_result v32_echo_canceller_process(
	struct v32_echo_canceller *canceller,
	const double *received, double *cleaned, size_t count)
{
	double error[V32_ECHO_PHASE_COUNT];
	double far[V32_ECHO_PHASE_COUNT];
	struct echo_reference_windows windows;
	struct qam_point updated[V32_ECHO_PHASE_COUNT]
				[V32_ECHO_WINDOW_COUNT][V32_ECHO_WINDOW_SYMBOLS];
	size_t bytes;
	double energy;

	if (!v32_echo_canceller_header_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (count == 0)
		return V32_ECHO_OK;
	if (received == NULL || cleaned == NULL ||
	    count > (size_t)(canceller->pending_samples ?
			    canceller->pending_samples : V32_ECHO_PHASE_COUNT))
		return V32_ECHO_INVALID_ARGUMENT;
	bytes = count * sizeof(*received);
	if (memory_regions_overlap(
		    received, bytes, canceller, sizeof(*canceller)) ||
	    memory_regions_overlap(
		    cleaned, bytes, canceller, sizeof(*canceller)) ||
	    (received != cleaned &&
	     memory_regions_overlap(received, bytes, cleaned, bytes)))
		return V32_ECHO_INVALID_ARGUMENT;
	if (!canceller->pending_samples)
		return V32_ECHO_INVALID_STATE;
	read_reference_windows(canceller, &windows);
	energy = canceller->reference_energy;
	for (size_t index = 0; index < count; ++index) {
		double prediction;
		size_t phase = V32_ECHO_PHASE_COUNT - canceller->pending_samples + index;

		if (!isfinite(received[index]))
			return V32_ECHO_INVALID_ARGUMENT;
		bool predicted = phase == 0 ?
			prediction_and_energy(canceller, &windows, phase,
				&prediction, &energy, &far[index]) :
			prediction_and_energy(canceller, &windows, phase,
				&prediction, NULL, &far[index]);
		if (!predicted)
			return V32_ECHO_INVALID_STATE;
		error[index] = received[index] - prediction;
		if (!isfinite(error[index]) || !isfinite(error[index] + far[index]))
			return V32_ECHO_INVALID_STATE;
		if (!canceller->adapting)
			continue;
		double scale = canceller->adaptation * error[index] /
			       (energy + V32_ECHO_ENERGY_FLOOR);
		if (!isfinite(scale) ||
		    !prepare_update(canceller, &windows, phase, scale, updated[index]))
			return V32_ECHO_INVALID_STATE;
	}
	canceller->reference_energy = energy;
	/* Commit only after every requested phase passes preflight. Retain the
	 * checked values rather than walking the reference and computing twice. */
	for (size_t index = 0; index < count; ++index) {
		size_t phase = V32_ECHO_PHASE_COUNT - canceller->pending_samples;

		if (canceller->adapting)
			memcpy(canceller->coefficients[phase], updated[index],
			       sizeof(updated[index]));
		cleaned[index] = error[index];
		if (!canceller->adapting && canceller->adaptation > 0.0) {
			struct echo_timing_model model = {
				.suspended = canceller->far_suspended,
				.retime = retime_far,
				.owner = canceller,
			};
			if (canceller->far_suspended)
				cleaned[index] += far[index];
			/* Exclude the near prediction from far-delay evidence. */
			echo_timing_observe(&canceller->far_timing, &model,
				sample_round_s16(error[index] + far[index]),
				sample_round_s16(error[index]));
			canceller->far_suspended = model.suspended;
		}
		if (--canceller->pending_samples == 0) {
			canceller->reference_energy = 0.0;
			if (canceller->pending_far_delay_symbols) {
				canceller->round_trip_symbols =
					canceller->pending_far_delay_symbols;
				canceller->pending_far_delay_symbols = 0;
			}
		}
	}
	return V32_ECHO_OK;
}
