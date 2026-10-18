// SPDX-License-Identifier: BSD-3-Clause

#include "qam_equalizer.h"
#include "memory_region.h"

#include <math.h>
#include <stddef.h>

static const double QAM_EQUALIZER_MINIMUM_POWER = 1e-12;
static const double QAM_CARRIER_PI = 3.14159265358979323846;
static const double QAM_CARRIER_TWO_PI = 6.28318530717958647692;
static const double QAM_CARRIER_MAX_FREQUENCY = 0.25;

static bool carrier_gains_are_valid(
	double proportional_gain, double integral_gain)
{
	return isfinite(proportional_gain) && proportional_gain > 0.0 &&
	       proportional_gain <= 1.0 && isfinite(integral_gain) &&
	       integral_gain >= 0.0 && integral_gain <= proportional_gain;
}

static bool carrier_state_is_valid(const struct qam_equalizer *equalizer)
{
	if (!point_is_finite(&equalizer->last_unrotated_output) ||
	    !isfinite(equalizer->carrier_phase) ||
	    equalizer->carrier_phase < -QAM_CARRIER_PI ||
	    equalizer->carrier_phase > QAM_CARRIER_PI ||
	    !isfinite(equalizer->carrier_frequency) ||
	    fabs(equalizer->carrier_frequency) > QAM_CARRIER_MAX_FREQUENCY ||
	    !isfinite(equalizer->carrier_error_limit) ||
	    equalizer->carrier_error_limit < 0.0 ||
	    !isfinite(equalizer->last_carrier_phase) ||
	    equalizer->last_carrier_phase < -QAM_CARRIER_PI ||
	    equalizer->last_carrier_phase > QAM_CARRIER_PI)
		return false;
	if (equalizer->carrier_tracking)
		return carrier_gains_are_valid(
			equalizer->carrier_proportional_gain,
			equalizer->carrier_integral_gain);
	return equalizer->carrier_phase == 0.0 &&
	       equalizer->carrier_frequency == 0.0 &&
	       equalizer->carrier_proportional_gain == 0.0 &&
	       equalizer->carrier_integral_gain == 0.0 &&
	       equalizer->last_carrier_phase == 0.0;
}

static double wrap_carrier_phase(double phase)
{
	phase = remainder(phase, QAM_CARRIER_TWO_PI);
	return phase == -0.0 ? 0.0 : phase;
}

static bool geometry_is_valid(
	unsigned int tap_count, unsigned int decision_delay)
{
	return tap_count != 0 && decision_delay < tap_count &&
	       tap_count <= QAM_EQUALIZER_MAX_TAP_COUNT;
}

bool qam_equalizer_observation_is_pending(
	const struct qam_equalizer_observation *observation)
{
	if (observation == NULL || !observation->pending ||
	    !geometry_is_valid(
		    observation->tap_count, observation->decision_delay) ||
	    !point_is_finite(&observation->output) ||
	    !point_is_finite(&observation->unrotated_output) ||
	    !isfinite(observation->carrier_phase) ||
	    observation->carrier_phase < -QAM_CARRIER_PI ||
	    observation->carrier_phase > QAM_CARRIER_PI ||
	    (!observation->carrier_tracking &&
	     observation->carrier_phase != 0.0))
		return false;
	for (size_t index = 0; index < observation->tap_count;
	     ++index) {
		if (!point_is_finite(&observation->history[index]))
			return false;
	}
	return true;
}

bool qam_equalizer_is_valid(
	const struct qam_equalizer *equalizer)
{
	double coefficient_power = 0.0;

	if (equalizer == NULL ||
	    !geometry_is_valid(
		    equalizer->tap_count, equalizer->decision_delay) ||
	    equalizer->fill > equalizer->decision_delay + 1u ||
	    (equalizer->can_adapt &&
	     equalizer->fill <= equalizer->decision_delay) ||
	    !point_is_finite(&equalizer->last_output) ||
	    !carrier_state_is_valid(equalizer))
		return false;
	for (size_t index = 0; index < equalizer->tap_count;
	     ++index) {
		if (!point_is_finite(&equalizer->coefficients[index]) ||
		    !point_is_finite(&equalizer->history[index]))
			return false;
		coefficient_power +=
			qam_point_power(&equalizer->coefficients[index]);
	}
	return isfinite(coefficient_power) &&
	       coefficient_power > QAM_EQUALIZER_MINIMUM_POWER;
}

bool qam_equalizer_is_handoff_ready(
	const struct qam_equalizer *equalizer)
{
	return qam_equalizer_is_valid(equalizer) &&
	       equalizer->fill == equalizer->decision_delay + 1u &&
	       !equalizer->can_adapt;
}

enum qam_equalizer_result qam_equalizer_init(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference)
{
	return qam_equalizer_init_span(
		equalizer, received_reference, transmitted_reference,
		QAM_EQUALIZER_DEFAULT_TAP_COUNT);
}

enum qam_equalizer_result qam_equalizer_init_span(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference,
	unsigned int tap_count)
{
	return qam_equalizer_init_geometry(
		equalizer, received_reference, transmitted_reference, tap_count,
		QAM_EQUALIZER_DELAY);
}

enum qam_equalizer_result qam_equalizer_init_geometry(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference,
	unsigned int tap_count, unsigned int decision_delay)
{
	struct qam_equalizer initialized = {
		.tap_count = tap_count,
		.decision_delay = decision_delay,
	};

	if (equalizer == NULL ||
	    !geometry_is_valid(tap_count, decision_delay) ||
	    !qam_point_divide(transmitted_reference, received_reference,
			      &initialized.coefficients[decision_delay]) ||
	    !(qam_point_power(transmitted_reference) > QAM_EQUALIZER_MINIMUM_POWER) ||
	    !qam_equalizer_is_valid(&initialized))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = initialized;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_extend_span(
	struct qam_equalizer *equalizer, unsigned int tap_count)
{
	if (!qam_equalizer_is_valid(equalizer) ||
	    tap_count < equalizer->tap_count ||
	    tap_count > QAM_EQUALIZER_MAX_TAP_COUNT)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (equalizer->can_adapt)
		return QAM_EQUALIZER_INVALID_STATE;
	if (tap_count == equalizer->tap_count)
		return QAM_EQUALIZER_OK;
	for (size_t index = equalizer->tap_count; index < tap_count; ++index) {
		equalizer->coefficients[index] = (struct qam_point){ 0 };
		equalizer->history[index] = (struct qam_point){ 0 };
	}
	equalizer->tap_count = tap_count;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_enable_carrier_tracking(
	struct qam_equalizer *equalizer, double proportional_gain,
	double integral_gain)
{
	if (!qam_equalizer_is_valid(equalizer) ||
	    !carrier_gains_are_valid(proportional_gain, integral_gain) ||
	    integral_gain == 0.0)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (equalizer->can_adapt || equalizer->carrier_tracking)
		return QAM_EQUALIZER_INVALID_STATE;
	equalizer->last_unrotated_output = equalizer->last_output;
	equalizer->carrier_proportional_gain = proportional_gain;
	equalizer->carrier_integral_gain = integral_gain;
	equalizer->carrier_tracking = true;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_hold_carrier_frequency(
	struct qam_equalizer *equalizer)
{
	if (!qam_equalizer_is_valid(equalizer))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!equalizer->carrier_tracking)
		return QAM_EQUALIZER_INVALID_STATE;
	equalizer->carrier_integral_gain = 0.0;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_set_carrier_proportional_gain(
	struct qam_equalizer *equalizer, double proportional_gain)
{
	if (!qam_equalizer_is_valid(equalizer) ||
	    !isfinite(proportional_gain) || proportional_gain <= 0.0 ||
	    proportional_gain > 1.0)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!equalizer->carrier_tracking)
		return QAM_EQUALIZER_INVALID_STATE;
	if (proportional_gain < equalizer->carrier_integral_gain)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	equalizer->carrier_proportional_gain = proportional_gain;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_set_carrier_error_limit(
	struct qam_equalizer *equalizer, double maximum_error)
{
	if (!qam_equalizer_is_valid(equalizer) ||
	    !isfinite(maximum_error) || maximum_error <= 0.0)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!equalizer->carrier_tracking)
		return QAM_EQUALIZER_INVALID_STATE;
	equalizer->carrier_error_limit = maximum_error;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_push(
	struct qam_equalizer *equalizer,
	const struct qam_point *received,
	struct qam_point *output, bool *ready)
{
	const struct memory_region outputs[] = {
		{ equalizer, sizeof(*equalizer) },
		{ output, sizeof(*output) },
		{ ready, sizeof(*ready) },
	};
	struct qam_point input;
	struct qam_point filtered = { 0 };
	struct qam_point corrected;
	double correction_phase;
	bool next_ready;

	if (!qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(received) || output == NULL || ready == NULL ||
	    memory_region_set_overlaps(outputs, 3))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	input = *received;
	/* Evaluate the shifted FIR before committing any history or output. */
	for (size_t index = 0; index < equalizer->tap_count;
	     ++index) {
		struct qam_point term = qam_point_multiply(
			&equalizer->coefficients[index],
			index == 0 ? &input : &equalizer->history[index - 1u]);

		filtered.in_phase += term.in_phase;
		filtered.quadrature += term.quadrature;
	}
	if (!point_is_finite(&filtered))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	correction_phase = equalizer->carrier_phase;
	corrected = equalizer->carrier_tracking ?
		    qam_point_rotate(&filtered, correction_phase) : filtered;
	if (!point_is_finite(&corrected))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	memmove(equalizer->history + 1, equalizer->history,
		(equalizer->tap_count - 1u) * sizeof(input));
	equalizer->history[0] = input;
	if (equalizer->fill <= equalizer->decision_delay)
		equalizer->fill++;
	next_ready = equalizer->fill > equalizer->decision_delay;
	equalizer->last_unrotated_output = filtered;
	equalizer->last_output = corrected;
	equalizer->last_carrier_phase = correction_phase;
	if (next_ready && equalizer->carrier_tracking)
		equalizer->carrier_phase = wrap_carrier_phase(
			equalizer->carrier_phase + equalizer->carrier_frequency);
	equalizer->can_adapt = next_ready;
	*ready = next_ready;
	if (next_ready)
		*output = corrected;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_observe(
	struct qam_equalizer *equalizer,
	const struct qam_point *received,
	struct qam_equalizer_observation *observation, bool *ready)
{
	const struct memory_region outputs[] = {
		{ equalizer, sizeof(*equalizer) },
		{ observation, sizeof(*observation) },
		{ ready, sizeof(*ready) },
	};
	struct qam_point output = { 0 };
	enum qam_equalizer_result result;

	if (equalizer == NULL || observation == NULL || ready == NULL ||
	    memory_region_set_overlaps(outputs, 3))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (observation->pending)
		return qam_equalizer_is_valid(equalizer) ?
			       QAM_EQUALIZER_INVALID_STATE :
			       QAM_EQUALIZER_INVALID_ARGUMENT;
	result = qam_equalizer_push(
		equalizer, received, &output, ready);
	if (result != QAM_EQUALIZER_OK)
		return result;
	/* A retained observation is the sole adaptation path for this output. */
	equalizer->can_adapt = false;
	if (*ready) {
		*observation = (struct qam_equalizer_observation){
			.output = output,
			.unrotated_output = equalizer->last_unrotated_output,
			.carrier_phase = equalizer->last_carrier_phase,
			.tap_count = equalizer->tap_count,
			.decision_delay = equalizer->decision_delay,
			.pending = true,
			.carrier_tracking = equalizer->carrier_tracking,
		};
		memcpy(observation->history, equalizer->history,
			equalizer->tap_count * sizeof(equalizer->history[0]));
	}
	return QAM_EQUALIZER_OK;
}

static enum qam_equalizer_result adapt_decision(
	struct qam_equalizer *equalizer,
	struct qam_equalizer_observation *observation,
	const struct qam_point *target, double adaptation)
{
	struct qam_point coefficients[QAM_EQUALIZER_MAX_TAP_COUNT];
	struct qam_point unrotated_target;
	const struct qam_point *output, *unrotated_output, *history;
	double phase, frequency, correction_phase;
	double power = QAM_EQUALIZER_MINIMUM_POWER;
	double coefficient_power = 0.0;

	if (!qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(target) ||
	    !qam_adaptation_is_valid(adaptation) ||
	    (observation != NULL && memory_regions_overlap(equalizer,
		    sizeof(*equalizer), observation, sizeof(*observation))))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (observation != NULL) {
		if (!qam_equalizer_observation_is_pending(observation) ||
		    observation->tap_count != equalizer->tap_count ||
		    observation->decision_delay != equalizer->decision_delay ||
		    observation->carrier_tracking != equalizer->carrier_tracking)
			return QAM_EQUALIZER_INVALID_STATE;
	} else if (!equalizer->can_adapt) {
		return QAM_EQUALIZER_INVALID_STATE;
	}
	output = observation != NULL ? &observation->output : &equalizer->last_output;
	unrotated_output = observation != NULL ? &observation->unrotated_output :
		&equalizer->last_unrotated_output;
	history = observation != NULL ? observation->history : equalizer->history;
	correction_phase = observation != NULL ? observation->carrier_phase :
		equalizer->last_carrier_phase;
	phase = equalizer->carrier_phase;
	frequency = equalizer->carrier_frequency;
	unrotated_target = *target;
	if (equalizer->carrier_tracking) {
		unrotated_target = qam_point_rotate(target, -correction_phase);
		if (!point_is_finite(&unrotated_target))
			return QAM_EQUALIZER_INVALID_ARGUMENT;
		if (qam_point_power(output) > QAM_EQUALIZER_MINIMUM_POWER &&
		    qam_point_power(target) > QAM_EQUALIZER_MINIMUM_POWER) {
			/* The V.34 cross-product detector naturally gives weak
			 * decisions less authority than a unit-angle error. */
			double error = target->quadrature * output->in_phase -
				target->in_phase * output->quadrature;
			if (equalizer->carrier_error_limit > 0.0) {
				if (error > equalizer->carrier_error_limit)
					error = equalizer->carrier_error_limit;
				else if (error < -equalizer->carrier_error_limit)
					error = -equalizer->carrier_error_limit;
			}
			if (!isfinite(error))
				return QAM_EQUALIZER_INVALID_ARGUMENT;
			frequency += equalizer->carrier_integral_gain * error;
			if (frequency > QAM_CARRIER_MAX_FREQUENCY)
				frequency = QAM_CARRIER_MAX_FREQUENCY;
			else if (frequency < -QAM_CARRIER_MAX_FREQUENCY)
				frequency = -QAM_CARRIER_MAX_FREQUENCY;
			phase = wrap_carrier_phase(phase +
				equalizer->carrier_proportional_gain * error);
			if (!isfinite(phase) || !isfinite(frequency))
				return QAM_EQUALIZER_INVALID_ARGUMENT;
		}
	}
	struct qam_point error = {
		.in_phase = unrotated_target.in_phase - unrotated_output->in_phase,
		.quadrature = unrotated_target.quadrature - unrotated_output->quadrature,
	};
	for (size_t index = 0; index < equalizer->tap_count; ++index)
		power += qam_point_power(&history[index]);
	if (!isfinite(power))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	for (size_t index = 0; index < equalizer->tap_count; ++index) {
		struct qam_point update = qam_point_correlation(&history[index], &error);

		coefficients[index] = equalizer->coefficients[index];
		coefficients[index].in_phase += adaptation * update.in_phase / power;
		coefficients[index].quadrature += adaptation * update.quadrature / power;
		coefficient_power += qam_point_power(&coefficients[index]);
	}
	if (!isfinite(coefficient_power) || coefficient_power <= QAM_EQUALIZER_MINIMUM_POWER)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	/* Stage only the taps and carrier state adaptation actually changes.
	 * History, decisions, and inactive taps remain owned by the receiver. */
	memcpy(equalizer->coefficients, coefficients,
		equalizer->tap_count * sizeof(coefficients[0]));
	equalizer->carrier_phase = phase;
	equalizer->carrier_frequency = frequency;
	if (observation == NULL)
		equalizer->can_adapt = false;
	if (observation != NULL)
		observation->pending = false;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_adapt(
	struct qam_equalizer *equalizer,
	const struct qam_point *target, double adaptation)
{
	return adapt_decision(equalizer, NULL, target, adaptation);
}

enum qam_equalizer_result
qam_equalizer_adapt_observation(
	struct qam_equalizer *equalizer,
	struct qam_equalizer_observation *observation,
	const struct qam_point *target, double adaptation)
{
	if (observation == NULL)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	return adapt_decision(equalizer, observation, target, adaptation);
}
