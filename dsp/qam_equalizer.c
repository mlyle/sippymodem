// SPDX-License-Identifier: BSD-3-Clause

#include "qam_equalizer.h"

#include <math.h>
#include <stddef.h>

static const double QAM_EQUALIZER_MINIMUM_POWER = 1e-12;

static double point_power(const struct qam_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
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
	    !point_is_finite(&observation->output))
		return false;
	for (size_t index = 0; index < QAM_EQUALIZER_MAX_TAP_COUNT;
	     ++index) {
		if (!point_is_finite(&observation->history[index]))
			return false;
	}
	return true;
}

static struct qam_point multiply(
	const struct qam_point *left,
	const struct qam_point *right)
{
	return (struct qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static bool inverse_channel(
	const struct qam_point *received,
	const struct qam_point *transmitted,
	struct qam_point *inverse)
{
	double power;

	if (!point_is_finite(received) || !point_is_finite(transmitted) ||
	    inverse == NULL)
		return false;
	power = point_power(received);
	if (!isfinite(power) || power <= QAM_EQUALIZER_MINIMUM_POWER)
		return false;
	*inverse = (struct qam_point){
		.in_phase = (transmitted->in_phase * received->in_phase +
			     transmitted->quadrature * received->quadrature) /
			    power,
		.quadrature =
			(transmitted->quadrature * received->in_phase -
			 transmitted->in_phase * received->quadrature) /
			power,
	};
	return point_is_finite(inverse) &&
	       point_power(transmitted) > QAM_EQUALIZER_MINIMUM_POWER;
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
	    !point_is_finite(&equalizer->last_output))
		return false;
	for (size_t index = 0; index < QAM_EQUALIZER_MAX_TAP_COUNT;
	     ++index) {
		if (!point_is_finite(&equalizer->coefficients[index]) ||
		    !point_is_finite(&equalizer->history[index]))
			return false;
		if (index < equalizer->tap_count)
			coefficient_power +=
				point_power(&equalizer->coefficients[index]);
	}
	return isfinite(coefficient_power) &&
	       coefficient_power > QAM_EQUALIZER_MINIMUM_POWER;
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
	    !inverse_channel(received_reference, transmitted_reference,
			     &initialized.coefficients[
				     decision_delay]) ||
	    !qam_equalizer_is_valid(&initialized))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = initialized;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_extend_span(
	struct qam_equalizer *equalizer, unsigned int tap_count)
{
	struct qam_equalizer extended;

	if (!qam_equalizer_is_valid(equalizer) ||
	    tap_count < equalizer->tap_count ||
	    tap_count > QAM_EQUALIZER_MAX_TAP_COUNT)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (equalizer->can_adapt)
		return QAM_EQUALIZER_INVALID_STATE;
	if (tap_count == equalizer->tap_count)
		return QAM_EQUALIZER_OK;
	extended = *equalizer;
	for (size_t index = extended.tap_count; index < tap_count; ++index) {
		extended.coefficients[index] = (struct qam_point){ 0 };
		extended.history[index] = (struct qam_point){ 0 };
	}
	extended.tap_count = tap_count;
	if (!qam_equalizer_is_valid(&extended))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = extended;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_push(
	struct qam_equalizer *equalizer,
	const struct qam_point *received,
	struct qam_point *output, bool *ready)
{
	struct qam_equalizer next;
	struct qam_point filtered = { 0 };
	bool next_ready;

	if (!qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(received) || output == NULL || ready == NULL)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	for (size_t index = next.tap_count - 1u;
	     index != 0; --index)
		next.history[index] = next.history[index - 1];
	next.history[0] = *received;
	for (size_t index = 0; index < next.tap_count;
	     ++index) {
		struct qam_point term = multiply(
			&next.coefficients[index], &next.history[index]);

		filtered.in_phase += term.in_phase;
		filtered.quadrature += term.quadrature;
	}
	if (!point_is_finite(&filtered))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (next.fill <= next.decision_delay)
		next.fill++;
	next_ready = next.fill > next.decision_delay;
	next.last_output = filtered;
	next.can_adapt = next_ready;
	if (!qam_equalizer_is_valid(&next))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
	*ready = next_ready;
	if (next_ready)
		*output = filtered;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result qam_equalizer_observe(
	struct qam_equalizer *equalizer,
	const struct qam_point *received,
	struct qam_equalizer_observation *observation, bool *ready)
{
	struct qam_equalizer next;
	struct qam_point output = { 0 };
	bool generated_ready;
	enum qam_equalizer_result result;

	if (!qam_equalizer_is_valid(equalizer) ||
	    observation == NULL || ready == NULL)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (observation->pending)
		return QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	result = qam_equalizer_push(
		&next, received, &output, &generated_ready);
	if (result != QAM_EQUALIZER_OK)
		return result;
	/* A retained observation is the sole adaptation path for this output. */
	next.can_adapt = false;
	if (!qam_equalizer_is_valid(&next))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
	*ready = generated_ready;
	if (generated_ready) {
		struct qam_equalizer_observation generated = {
			.output = output,
			.tap_count = next.tap_count,
			.decision_delay = next.decision_delay,
			.pending = true,
		};
		for (size_t index = 0;
		     index < next.tap_count; ++index)
			generated.history[index] = next.history[index];
		*observation = generated;
	}
	return QAM_EQUALIZER_OK;
}

static bool apply_adaptation(
	struct qam_equalizer *equalizer,
	const struct qam_point history[
		QAM_EQUALIZER_MAX_TAP_COUNT],
	const struct qam_point *output,
	const struct qam_point *target, double adaptation,
	unsigned int tap_count)
{
	struct qam_point error = {
		.in_phase = target->in_phase - output->in_phase,
		.quadrature = target->quadrature - output->quadrature,
	};
	double power = QAM_EQUALIZER_MINIMUM_POWER;

	for (size_t index = 0; index < tap_count;
	     ++index)
		power += point_power(&history[index]);
	if (!isfinite(power))
		return false;
	for (size_t index = 0; index < tap_count;
	     ++index) {
		struct qam_point conjugate = {
			.in_phase = history[index].in_phase,
			.quadrature = -history[index].quadrature,
		};
		struct qam_point update = multiply(&error, &conjugate);

		equalizer->coefficients[index].in_phase +=
			adaptation * update.in_phase / power;
		equalizer->coefficients[index].quadrature +=
			adaptation * update.quadrature / power;
	}
	return qam_equalizer_is_valid(equalizer);
}

enum qam_equalizer_result qam_equalizer_adapt(
	struct qam_equalizer *equalizer,
	const struct qam_point *target, double adaptation)
{
	struct qam_equalizer next;

	if (!qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(target) || !isfinite(adaptation) ||
	    adaptation < 0.0 || adaptation > 1.0)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!equalizer->can_adapt)
		return QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	if (!apply_adaptation(
		    &next, next.history, &next.last_output, target, adaptation,
		    next.tap_count))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	next.can_adapt = false;
	if (!qam_equalizer_is_valid(&next))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
	return QAM_EQUALIZER_OK;
}

enum qam_equalizer_result
qam_equalizer_adapt_observation(
	struct qam_equalizer *equalizer,
	struct qam_equalizer_observation *observation,
	const struct qam_point *target, double adaptation)
{
	struct qam_equalizer next;
	struct qam_equalizer_observation consumed;

	if (!qam_equalizer_is_valid(equalizer) ||
	    observation == NULL || !point_is_finite(target) ||
	    !isfinite(adaptation) || adaptation < 0.0 || adaptation > 1.0)
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!qam_equalizer_observation_is_pending(observation))
		return QAM_EQUALIZER_INVALID_STATE;
	if (observation->tap_count != equalizer->tap_count ||
	    observation->decision_delay != equalizer->decision_delay)
		return QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	consumed = *observation;
	if (!apply_adaptation(
		    &next, consumed.history, &consumed.output, target,
		    adaptation, consumed.tap_count))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	consumed.pending = false;
	*equalizer = next;
	*observation = consumed;
	return QAM_EQUALIZER_OK;
}

const char *qam_equalizer_result_name(
	enum qam_equalizer_result result)
{
	switch (result) {
	case QAM_EQUALIZER_OK:
		return "ok";
	case QAM_EQUALIZER_INVALID_ARGUMENT:
		return "invalid argument";
	case QAM_EQUALIZER_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
