// SPDX-License-Identifier: BSD-3-Clause

#include "echo_canceller.h"

#include "memory_region.h"

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

static bool state_header_is_valid(
	const struct v32_echo_canceller *canceller)
{
	return canceller != NULL &&
	       geometry_is_valid(
		       canceller->near_delay_symbols,
		       canceller->round_trip_symbols) &&
	       canceller->reference_write < V32_ECHO_REFERENCE_CAPACITY &&
	       canceller->reference_fill <= V32_ECHO_REFERENCE_CAPACITY &&
	       (canceller->reference_fill == V32_ECHO_REFERENCE_CAPACITY ||
		canceller->reference_write == canceller->reference_fill) &&
	       canceller->phase < V32_ECHO_PHASE_COUNT &&
	       (canceller->phase == 0 || canceller->reference_pending) &&
	       qam_adaptation_is_valid(canceller->adaptation) &&
	       (!canceller->adapting || canceller->adaptation > 0.0);
}

bool v32_echo_canceller_is_valid(
	const struct v32_echo_canceller *canceller)
{
	/* Keep the complete scan as a full-width reduction so it can vectorize. */
	unsigned int finite = 1;

	if (!state_header_is_valid(canceller))
		return false;
	for (size_t index = 0; index < V32_ECHO_REFERENCE_CAPACITY;
	     ++index) {
		finite &= !!isfinite(canceller->reference[index].in_phase) &
			  !!isfinite(canceller->reference[index].quadrature);
	}
	for (size_t phase = 0; phase < V32_ECHO_PHASE_COUNT; ++phase) {
		for (size_t window = 0;
		     window < V32_ECHO_WINDOW_COUNT; ++window) {
			for (size_t tap = 0;
			     tap < V32_ECHO_WINDOW_SYMBOLS; ++tap) {
				const struct qam_point *point =
					&canceller->coefficients[phase][window][tap];

				finite &= !!isfinite(point->in_phase) &
					  !!isfinite(point->quadrature);
			}
		}
	}
	return finite;
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
	if (canceller->reference_pending)
		return V32_ECHO_INVALID_STATE;
	canceller->adaptation = adaptation;
	canceller->adapting = true;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_canceller_freeze(
	struct v32_echo_canceller *canceller)
{
	if (!v32_echo_canceller_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (canceller->reference_pending)
		return V32_ECHO_INVALID_STATE;
	canceller->adapting = false;
	return V32_ECHO_OK;
}

enum v32_echo_result v32_echo_canceller_push_reference(
	struct v32_echo_canceller *canceller,
	const struct qam_point *reference)
{
	double power;

	if (!state_header_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (!point_is_finite(reference) ||
	    memory_regions_overlap(
		    reference, sizeof(*reference),
		    canceller, sizeof(*canceller)))
		return V32_ECHO_INVALID_ARGUMENT;
	if (canceller->phase != 0 || canceller->reference_pending)
		return V32_ECHO_INVALID_STATE;
	power = reference->in_phase * reference->in_phase +
		reference->quadrature * reference->quadrature;
	if (!isfinite(power))
		return V32_ECHO_INVALID_ARGUMENT;
	canceller->reference[canceller->reference_write] = *reference;
	canceller->reference_write++;
	if (canceller->reference_write == V32_ECHO_REFERENCE_CAPACITY)
		canceller->reference_write = 0;
	if (canceller->reference_fill < V32_ECHO_REFERENCE_CAPACITY)
		canceller->reference_fill++;
	canceller->reference_pending = true;
	return V32_ECHO_OK;
}

static struct qam_point reference_at_age(
	const struct v32_echo_canceller *canceller, size_t age)
{
	size_t index;

	if (age >= canceller->reference_fill)
		return (struct qam_point){ 0 };
	index = canceller->reference_write +
		V32_ECHO_REFERENCE_CAPACITY - 1u - age;
	if (index >= V32_ECHO_REFERENCE_CAPACITY)
		index -= V32_ECHO_REFERENCE_CAPACITY;
	return canceller->reference[index];
}

static struct qam_point tap_reference(
	const struct v32_echo_canceller *canceller,
	size_t window, size_t tap)
{
	size_t age = canceller->near_delay_symbols + tap;

	if (window == V32_ECHO_DELAYED_WINDOW)
		age += canceller->round_trip_symbols;
	return reference_at_age(canceller, age);
}

static bool prediction_and_energy(
	const struct v32_echo_canceller *canceller, size_t phase,
	double *prediction, double *energy)
{
	double predicted = 0.0;
	double reference_energy = 0.0;

	for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window) {
		for (size_t tap = 0;
		     tap < V32_ECHO_WINDOW_SYMBOLS; ++tap) {
			struct qam_point point =
				tap_reference(canceller, window, tap);
			const struct qam_point *coefficient =
				&canceller->coefficients[phase][window][tap];

			if (!point_is_finite(&point) ||
			    !point_is_finite(coefficient))
				return false;
			predicted += coefficient->in_phase * point.in_phase +
				     coefficient->quadrature * point.quadrature;
			reference_energy +=
				point.in_phase * point.in_phase +
				point.quadrature * point.quadrature;
		}
	}
	*prediction = predicted;
	*energy = reference_energy;
	return isfinite(predicted) && isfinite(reference_energy);
}

static bool update_is_finite(
	const struct v32_echo_canceller *canceller, size_t phase,
	double scale)
{
	for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window) {
		for (size_t tap = 0;
		     tap < V32_ECHO_WINDOW_SYMBOLS; ++tap) {
			struct qam_point point =
				tap_reference(canceller, window, tap);
			const struct qam_point *coefficient =
				&canceller->coefficients[phase][window][tap];

			if (!isfinite(
				    coefficient->in_phase + scale * point.in_phase) ||
			    !isfinite(
				    coefficient->quadrature +
					    scale * point.quadrature))
				return false;
		}
	}
	return true;
}

static void apply_update(
	struct v32_echo_canceller *canceller, size_t phase, double scale)
{
	for (size_t window = 0; window < V32_ECHO_WINDOW_COUNT; ++window) {
		for (size_t tap = 0;
		     tap < V32_ECHO_WINDOW_SYMBOLS; ++tap) {
			struct qam_point point =
				tap_reference(canceller, window, tap);
			struct qam_point *coefficient =
				&canceller->coefficients[phase][window][tap];

			coefficient->in_phase += scale * point.in_phase;
			coefficient->quadrature += scale * point.quadrature;
		}
	}
}

enum v32_echo_result v32_echo_canceller_process(
	struct v32_echo_canceller *canceller,
	const double *received, double *cleaned, size_t count)
{
	double error[V32_ECHO_PHASE_COUNT];
	double scale[V32_ECHO_PHASE_COUNT] = { 0 };
	size_t bytes;

	if (!state_header_is_valid(canceller))
		return V32_ECHO_INVALID_STATE;
	if (count == 0)
		return V32_ECHO_OK;
	if (received == NULL || cleaned == NULL ||
	    count > (size_t)(V32_ECHO_PHASE_COUNT - canceller->phase))
		return V32_ECHO_INVALID_ARGUMENT;
	bytes = count * sizeof(*received);
	if (memory_regions_overlap(
		    received, bytes, canceller, sizeof(*canceller)) ||
	    memory_regions_overlap(
		    cleaned, bytes, canceller, sizeof(*canceller)) ||
	    (received != cleaned &&
	     memory_regions_overlap(received, bytes, cleaned, bytes)))
		return V32_ECHO_INVALID_ARGUMENT;
	if (!canceller->reference_pending)
		return V32_ECHO_INVALID_STATE;
	for (size_t index = 0; index < count; ++index) {
		double energy;
		double prediction;
		size_t phase = canceller->phase + index;

		if (!isfinite(received[index]))
			return V32_ECHO_INVALID_ARGUMENT;
		if (!prediction_and_energy(
			    canceller, phase, &prediction, &energy))
			return V32_ECHO_INVALID_STATE;
		error[index] = received[index] - prediction;
		if (!isfinite(error[index]))
			return V32_ECHO_INVALID_STATE;
		if (!canceller->adapting)
			continue;
		scale[index] = canceller->adaptation * error[index] /
			       (energy + V32_ECHO_ENERGY_FLOOR);
		if (!isfinite(scale[index]) ||
		    !update_is_finite(canceller, phase, scale[index]))
			return V32_ECHO_INVALID_STATE;
	}
	for (size_t index = 0; index < count; ++index) {
		size_t phase = canceller->phase + index;

		if (canceller->adapting)
			apply_update(canceller, phase, scale[index]);
		cleaned[index] = error[index];
	}
	canceller->phase = (uint8_t)(canceller->phase + count);
	if (canceller->phase == V32_ECHO_PHASE_COUNT) {
		canceller->phase = 0;
		canceller->reference_pending = false;
	}
	return V32_ECHO_OK;
}
