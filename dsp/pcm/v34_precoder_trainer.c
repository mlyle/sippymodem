// SPDX-License-Identifier: BSD-3-Clause

#include "v34_precoder_trainer.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/* The maintained fixed receiver updates a Q14 coefficient by one Q16-scaled
 * product of two decisions whose unit-power scale is sqrt(2) * 2289. */
static const double V34_PRECODER_ADAPTATION =
	(2.0 * 2289.0 * 2289.0) / (16384.0 * 65536.0);
static const double V34_PRECODER_MAXIMUM_COMPONENT =
	(double)INT16_MAX / V34_Q14_SCALE;
static const double V34_PRECODER_MAXIMUM_MAGNITUDE_SUM = 4.0;
static const double V34_PRECODER_MAXIMUM_DECISION_ERROR_POWER = 0.5;
static const double V34_PRECODER_MINIMUM_DECISION_ERROR_POWER = 1e-8;

static bool coefficients_are_zero(
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap)
		if (coefficients[tap].real != 0 ||
		    coefficients[tap].imaginary != 0)
			return false;
	return true;
}

bool v34_precoder_recommendation_is_valid(
	const struct v34_precoder_recommendation *recommendation)
{
	if (recommendation == NULL)
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap)
		if (recommendation->coefficients[tap].real == INT16_MIN ||
		    recommendation->coefficients[tap].imaginary == INT16_MIN)
			return false;
	return recommendation->enabled ?
		       !coefficients_are_zero(recommendation->coefficients) :
		       coefficients_are_zero(recommendation->coefficients);
}

bool v34_precoder_trainer_is_valid(
	const struct v34_precoder_trainer *trainer)
{
	double magnitude_sum = 0.0;

	if (trainer == NULL || !trainer->initialized ||
	    !isfinite(trainer->decision_error_power) ||
	    trainer->decision_error_power < 0.0 ||
	    !isfinite(trainer->feedback_error_power) ||
	    trainer->feedback_error_power < 0.0 ||
	    trainer->measured_symbols > trainer->observed_symbols ||
	    (trainer->observed_symbols <
		     V34_PRECODER_TRAINER_SETTLING_SYMBOLS &&
	     trainer->measured_symbols != 0) ||
	    (trainer->observed_symbols >=
		     V34_PRECODER_TRAINER_SETTLING_SYMBOLS &&
	     trainer->measured_symbols !=
		     trainer->observed_symbols -
			     V34_PRECODER_TRAINER_SETTLING_SYMBOLS + 1u))
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		if (!point_is_finite(&trainer->coefficients[tap]) ||
		    !point_is_finite(&trainer->error_history[tap]) ||
		    fabs(trainer->coefficients[tap].in_phase) >
			    V34_PRECODER_MAXIMUM_COMPONENT ||
		    fabs(trainer->coefficients[tap].quadrature) >
			    V34_PRECODER_MAXIMUM_COMPONENT)
			return false;
		magnitude_sum += fabs(trainer->coefficients[tap].in_phase) +
			fabs(trainer->coefficients[tap].quadrature);
	}
	return isfinite(magnitude_sum);
}

enum v34_precoder_trainer_result v34_precoder_trainer_init(
	struct v34_precoder_trainer *trainer)
{
	struct v34_precoder_trainer initialized = {
		.initialized = true,
	};

	if (trainer == NULL)
		return V34_PRECODER_TRAINER_INVALID_ARGUMENT;
	if (!v34_precoder_trainer_is_valid(&initialized))
		return V34_PRECODER_TRAINER_INVALID_STATE;
	*trainer = initialized;
	return V34_PRECODER_TRAINER_OK;
}

static struct qam_point multiply_conjugate(
	struct qam_point left, struct qam_point right)
{
	return (struct qam_point){
		.in_phase = left.in_phase * right.in_phase +
			    left.quadrature * right.quadrature,
		.quadrature = left.in_phase * right.quadrature -
			      left.quadrature * right.in_phase,
	};
}

static double bounded_component(double value)
{
	if (value > V34_PRECODER_MAXIMUM_COMPONENT)
		return V34_PRECODER_MAXIMUM_COMPONENT;
	if (value < -V34_PRECODER_MAXIMUM_COMPONENT)
		return -V34_PRECODER_MAXIMUM_COMPONENT;
	return value;
}

enum v34_precoder_trainer_result v34_precoder_trainer_observe(
	struct v34_precoder_trainer *trainer,
	const struct qam_point *decision,
	const struct qam_point *target)
{
	struct v34_precoder_trainer next;
	struct qam_point decision_error;
	struct qam_point feedback_error;

	if (!v34_precoder_trainer_is_valid(trainer) ||
	    !point_is_finite(decision) || !point_is_finite(target))
		return V34_PRECODER_TRAINER_INVALID_ARGUMENT;
	if (trainer->observed_symbols == UINT32_MAX)
		return V34_PRECODER_TRAINER_INVALID_STATE;
	next = *trainer;
	decision_error = (struct qam_point){
		.in_phase = decision->in_phase - target->in_phase,
		.quadrature = decision->quadrature - target->quadrature,
	};
	feedback_error = decision_error;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		struct qam_point prediction = qam_point_multiply(
			&next.coefficients[tap], &next.error_history[tap]);

		feedback_error.in_phase += prediction.in_phase;
		feedback_error.quadrature += prediction.quadrature;
	}
	if (!point_is_finite(&decision_error) ||
	    !point_is_finite(&feedback_error))
		return V34_PRECODER_TRAINER_NUMERIC_FAILURE;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		struct qam_point gradient = multiply_conjugate(
			next.error_history[tap], feedback_error);

		next.coefficients[tap].in_phase = bounded_component(
			next.coefficients[tap].in_phase -
			V34_PRECODER_ADAPTATION * gradient.in_phase);
		next.coefficients[tap].quadrature = bounded_component(
			next.coefficients[tap].quadrature -
			V34_PRECODER_ADAPTATION * gradient.quadrature);
	}
	for (size_t tap = V34_PRECODER_TAP_COUNT - 1u; tap != 0; --tap)
		next.error_history[tap] = next.error_history[tap - 1u];
	next.error_history[0] = decision_error;
	next.observed_symbols++;
	if (next.observed_symbols >=
	    V34_PRECODER_TRAINER_SETTLING_SYMBOLS) {
		next.decision_error_power += qam_point_power(&decision_error);
		next.feedback_error_power += qam_point_power(&feedback_error);
		next.measured_symbols++;
	}
	if (!v34_precoder_trainer_is_valid(&next))
		return V34_PRECODER_TRAINER_NUMERIC_FAILURE;
	*trainer = next;
	return V34_PRECODER_TRAINER_OK;
}

static int16_t q14_component(double component)
{
	long quantized = lround(component * V34_Q14_SCALE);

	if (quantized > INT16_MAX)
		quantized = INT16_MAX;
	if (quantized < -INT16_MAX)
		quantized = -INT16_MAX;
	return (int16_t)quantized;
}

enum v34_precoder_trainer_result v34_precoder_trainer_finish(
	const struct v34_precoder_trainer *trainer,
	struct v34_precoder_recommendation *recommendation)
{
	struct v34_precoder_recommendation generated = { 0 };
	double decision_power;
	double feedback_power;
	double magnitude_sum = 0.0;
	bool coefficients_unclipped = true;

	if (!v34_precoder_trainer_is_valid(trainer) ||
	    recommendation == NULL)
		return V34_PRECODER_TRAINER_INVALID_ARGUMENT;
	if (trainer->measured_symbols <
	    V34_PRECODER_TRAINER_MINIMUM_MEASURED_SYMBOLS) {
		*recommendation = generated;
		return V34_PRECODER_TRAINER_OK;
	}
	decision_power = trainer->decision_error_power /
		(double)trainer->measured_symbols;
	feedback_power = trainer->feedback_error_power /
		(double)trainer->measured_symbols;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		if (fabs(trainer->coefficients[tap].in_phase) >=
			    V34_PRECODER_MAXIMUM_COMPONENT ||
		    fabs(trainer->coefficients[tap].quadrature) >=
			    V34_PRECODER_MAXIMUM_COMPONENT)
			coefficients_unclipped = false;
		magnitude_sum += fabs(trainer->coefficients[tap].in_phase) +
			fabs(trainer->coefficients[tap].quadrature);
		generated.coefficients[tap] =
			(struct v34_q14_coefficient){
				.real = q14_component(
					trainer->coefficients[tap].in_phase),
				.imaginary = q14_component(
					trainer->coefficients[tap].quadrature),
			};
	}
	generated.enabled = decision_power >=
				    V34_PRECODER_MINIMUM_DECISION_ERROR_POWER &&
			    decision_power <
				    V34_PRECODER_MAXIMUM_DECISION_ERROR_POWER &&
			    feedback_power < decision_power &&
			    coefficients_unclipped &&
			    magnitude_sum <
				    V34_PRECODER_MAXIMUM_MAGNITUDE_SUM &&
			    !coefficients_are_zero(generated.coefficients);
	if (!generated.enabled)
		memset(generated.coefficients, 0,
		       sizeof(generated.coefficients));
	if (!v34_precoder_recommendation_is_valid(&generated))
		return V34_PRECODER_TRAINER_NUMERIC_FAILURE;
	*recommendation = generated;
	return V34_PRECODER_TRAINER_OK;
}
