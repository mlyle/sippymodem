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

enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_adapt(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *target, double adaptation)
{
	struct sippymodem_qam_equalizer next;
	struct sippymodem_qam_point error;
	double power = QAM_EQUALIZER_MINIMUM_POWER;

	if (!sippymodem_qam_equalizer_is_valid(equalizer) ||
	    !point_is_finite(target) || !isfinite(adaptation) ||
	    adaptation < 0.0 || adaptation > 1.0)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	if (!equalizer->can_adapt)
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE;
	next = *equalizer;
	error = (struct sippymodem_qam_point){
		.in_phase = target->in_phase - next.last_output.in_phase,
		.quadrature = target->quadrature - next.last_output.quadrature,
	};
	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index)
		power += point_power(&next.history[index]);
	if (!isfinite(power))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	for (size_t index = 0; index < SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT;
	     ++index) {
		struct sippymodem_qam_point conjugate = {
			.in_phase = next.history[index].in_phase,
			.quadrature = -next.history[index].quadrature,
		};
		struct sippymodem_qam_point update = multiply(&error, &conjugate);

		next.coefficients[index].in_phase +=
			adaptation * update.in_phase / power;
		next.coefficients[index].quadrature +=
			adaptation * update.quadrature / power;
	}
	next.can_adapt = false;
	if (!sippymodem_qam_equalizer_is_valid(&next))
		return SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT;
	*equalizer = next;
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
