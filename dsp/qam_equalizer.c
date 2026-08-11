// SPDX-License-Identifier: BSD-3-Clause

#include "qam_equalizer.h"

#include <math.h>
#include <stddef.h>

static const double QAM_EQUALIZER_MINIMUM_POWER = 1e-12;

static bool point_is_finite(const struct qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static double point_power(const struct qam_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
}

bool qam_equalizer_observation_is_pending(
	const struct qam_equalizer_observation *observation)
{
	if (observation == NULL || !observation->pending ||
	    !point_is_finite(&observation->output))
		return false;
	for (size_t index = 0; index < QAM_EQUALIZER_TAP_COUNT;
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
	    equalizer->fill > QAM_EQUALIZER_DELAY + 1u ||
	    (equalizer->can_adapt &&
	     equalizer->fill <= QAM_EQUALIZER_DELAY) ||
	    !point_is_finite(&equalizer->last_output))
		return false;
	for (size_t index = 0; index < QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		if (!point_is_finite(&equalizer->coefficients[index]) ||
		    !point_is_finite(&equalizer->history[index]))
			return false;
		coefficient_power += point_power(&equalizer->coefficients[index]);
	}
	return isfinite(coefficient_power) &&
	       coefficient_power > QAM_EQUALIZER_MINIMUM_POWER;
}

enum qam_equalizer_result qam_equalizer_init(
	struct qam_equalizer *equalizer,
	const struct qam_point *received_reference,
	const struct qam_point *transmitted_reference)
{
	struct qam_equalizer initialized = { 0 };

	if (equalizer == NULL ||
	    !inverse_channel(received_reference, transmitted_reference,
			     &initialized.coefficients[
				     QAM_EQUALIZER_DELAY]) ||
	    !qam_equalizer_is_valid(&initialized))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = initialized;
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
	for (size_t index = QAM_EQUALIZER_TAP_COUNT - 1;
	     index != 0; --index)
		next.history[index] = next.history[index - 1];
	next.history[0] = *received;
	for (size_t index = 0; index < QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		struct qam_point term = multiply(
			&next.coefficients[index], &next.history[index]);

		filtered.in_phase += term.in_phase;
		filtered.quadrature += term.quadrature;
	}
	if (!point_is_finite(&filtered))
		return QAM_EQUALIZER_INVALID_ARGUMENT;
	if (next.fill <= QAM_EQUALIZER_DELAY)
		next.fill++;
	next_ready = next.fill > QAM_EQUALIZER_DELAY;
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
		*observation = (struct qam_equalizer_observation){
			.output = output,
			.pending = true,
		};
		for (size_t index = 0;
		     index < QAM_EQUALIZER_TAP_COUNT; ++index)
			observation->history[index] = next.history[index];
	}
	return QAM_EQUALIZER_OK;
}

static bool apply_adaptation(
	struct qam_equalizer *equalizer,
	const struct qam_point history[
		QAM_EQUALIZER_TAP_COUNT],
	const struct qam_point *output,
	const struct qam_point *target, double adaptation)
{
	struct qam_point error = {
		.in_phase = target->in_phase - output->in_phase,
		.quadrature = target->quadrature - output->quadrature,
	};
	double power = QAM_EQUALIZER_MINIMUM_POWER;

	for (size_t index = 0; index < QAM_EQUALIZER_TAP_COUNT;
	     ++index)
		power += point_power(&history[index]);
	if (!isfinite(power))
		return false;
	for (size_t index = 0; index < QAM_EQUALIZER_TAP_COUNT;
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
		    &next, next.history, &next.last_output, target, adaptation))
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
	next = *equalizer;
	consumed = *observation;
	if (!apply_adaptation(
		    &next, consumed.history, &consumed.output, target,
		    adaptation))
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
