// SPDX-License-Identifier: BSD-3-Clause

#include "qam_equalizer.h"

#include <math.h>
#include <stddef.h>

static const double QAM_EQUALIZER_MINIMUM_POWER = 1e-12;

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static double point_power(const struct sippymodem_qam_point *point)
{
	return point->in_phase * point->in_phase +
	       point->quadrature * point->quadrature;
}

bool sippymodem_qam_equalizer_observation_is_pending(
	const struct sippymodem_qam_equalizer_observation *observation)
{
	if (observation == NULL || !observation->pending ||
	    !point_is_finite(&observation->output))
		return false;
	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		if (!point_is_finite(&observation->history[index]))
			return false;
	}
	return true;
}

static struct sippymodem_qam_point multiply(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return (struct sippymodem_qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static bool inverse_channel(
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *transmitted,
	struct sippymodem_qam_point *inverse)
{
	double power;

	if (!point_is_finite(received) || !point_is_finite(transmitted) ||
	    inverse == NULL)
		return false;
	power = point_power(received);
	if (!isfinite(power) || power <= QAM_EQUALIZER_MINIMUM_POWER)
		return false;
	*inverse = (struct sippymodem_qam_point){
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

bool sippymodem_qam_equalizer_is_valid(
	const struct sippymodem_qam_equalizer *equalizer)
{
	double coefficient_power = 0.0;

	if (equalizer == NULL ||
	    equalizer->fill > SIPPYMODEM_QAM_EQUALIZER_DELAY + 1u ||
	    (equalizer->can_adapt &&
	     equalizer->fill <= SIPPYMODEM_QAM_EQUALIZER_DELAY) ||
	    !point_is_finite(&equalizer->last_output))
		return false;
	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		if (!point_is_finite(&equalizer->coefficients[index]) ||
		    !point_is_finite(&equalizer->history[index]))
			return false;
		coefficient_power += point_power(&equalizer->coefficients[index]);
	}
	return isfinite(coefficient_power) &&
	       coefficient_power > QAM_EQUALIZER_MINIMUM_POWER;
}

enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_init(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *received_reference,
	const struct sippymodem_qam_point *transmitted_reference)
{
	struct sippymodem_qam_equalizer initialized = { 0 };

	if (equalizer == NULL ||
	    !inverse_channel(received_reference, transmitted_reference,
			     &initialized.coefficients[
				     SIPPYMODEM_QAM_EQUALIZER_DELAY]) ||
	    !sippymodem_qam_equalizer_is_valid(&initialized))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = initialized;
	return SIPPYMODEM_QAM_EQUALIZER_OK;
}

enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_push(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *received,
	struct sippymodem_qam_point *output, bool *ready)
{
	struct sippymodem_qam_equalizer next;
	struct sippymodem_qam_point filtered = { 0 };
	bool next_ready;

	if (!sippymodem_qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(received) || output == NULL || ready == NULL)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	next = *equalizer;
	for (size_t index = SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT - 1;
	     index != 0; --index)
		next.history[index] = next.history[index - 1];
	next.history[0] = *received;
	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		struct sippymodem_qam_point term = multiply(
			&next.coefficients[index], &next.history[index]);

		filtered.in_phase += term.in_phase;
		filtered.quadrature += term.quadrature;
	}
	if (!point_is_finite(&filtered))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	if (next.fill <= SIPPYMODEM_QAM_EQUALIZER_DELAY)
		next.fill++;
	next_ready = next.fill > SIPPYMODEM_QAM_EQUALIZER_DELAY;
	next.last_output = filtered;
	next.can_adapt = next_ready;
	if (!sippymodem_qam_equalizer_is_valid(&next))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
	*ready = next_ready;
	if (next_ready)
		*output = filtered;
	return SIPPYMODEM_QAM_EQUALIZER_OK;
}

enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_observe(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *received,
	struct sippymodem_qam_equalizer_observation *observation, bool *ready)
{
	struct sippymodem_qam_equalizer next;
	struct sippymodem_qam_point output = { 0 };
	bool generated_ready;
	enum sippymodem_qam_equalizer_result result;

	if (!sippymodem_qam_equalizer_is_valid(equalizer) ||
	    observation == NULL || ready == NULL)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	if (observation->pending)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	result = sippymodem_qam_equalizer_push(
		&next, received, &output, &generated_ready);
	if (result != SIPPYMODEM_QAM_EQUALIZER_OK)
		return result;
	/* A retained observation is the sole adaptation path for this output. */
	next.can_adapt = false;
	if (!sippymodem_qam_equalizer_is_valid(&next))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
	*ready = generated_ready;
	if (generated_ready) {
		*observation = (struct sippymodem_qam_equalizer_observation){
			.output = output,
			.pending = true,
		};
		for (size_t index = 0;
		     index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT; ++index)
			observation->history[index] = next.history[index];
	}
	return SIPPYMODEM_QAM_EQUALIZER_OK;
}

static bool apply_adaptation(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point history[
		SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT],
	const struct sippymodem_qam_point *output,
	const struct sippymodem_qam_point *target, double adaptation)
{
	struct sippymodem_qam_point error = {
		.in_phase = target->in_phase - output->in_phase,
		.quadrature = target->quadrature - output->quadrature,
	};
	double power = QAM_EQUALIZER_MINIMUM_POWER;

	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index)
		power += point_power(&history[index]);
	if (!isfinite(power))
		return false;
	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		struct sippymodem_qam_point conjugate = {
			.in_phase = history[index].in_phase,
			.quadrature = -history[index].quadrature,
		};
		struct sippymodem_qam_point update = multiply(&error, &conjugate);

		equalizer->coefficients[index].in_phase +=
			adaptation * update.in_phase / power;
		equalizer->coefficients[index].quadrature +=
			adaptation * update.quadrature / power;
	}
	return sippymodem_qam_equalizer_is_valid(equalizer);
}

enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_adapt(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *target, double adaptation)
{
	struct sippymodem_qam_equalizer next;

	if (!sippymodem_qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(target) || !isfinite(adaptation) ||
	    adaptation < 0.0 || adaptation > 1.0)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!equalizer->can_adapt)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	if (!apply_adaptation(
		    &next, next.history, &next.last_output, target, adaptation))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	next.can_adapt = false;
	if (!sippymodem_qam_equalizer_is_valid(&next))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
	return SIPPYMODEM_QAM_EQUALIZER_OK;
}

enum sippymodem_qam_equalizer_result
sippymodem_qam_equalizer_adapt_observation(
	struct sippymodem_qam_equalizer *equalizer,
	struct sippymodem_qam_equalizer_observation *observation,
	const struct sippymodem_qam_point *target, double adaptation)
{
	struct sippymodem_qam_equalizer next;
	struct sippymodem_qam_equalizer_observation consumed;

	if (!sippymodem_qam_equalizer_is_valid(equalizer) ||
	    observation == NULL || !point_is_finite(target) ||
	    !isfinite(adaptation) || adaptation < 0.0 || adaptation > 1.0)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!sippymodem_qam_equalizer_observation_is_pending(observation))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	consumed = *observation;
	if (!apply_adaptation(
		    &next, consumed.history, &consumed.output, target,
		    adaptation))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	consumed.pending = false;
	*equalizer = next;
	*observation = consumed;
	return SIPPYMODEM_QAM_EQUALIZER_OK;
}

const char *sippymodem_qam_equalizer_result_name(
	enum sippymodem_qam_equalizer_result result)
{
	switch (result) {
	case SIPPYMODEM_QAM_EQUALIZER_OK:
		return "ok";
	case SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
