// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_NONLINEAR_H
#define V34_NONLINEAR_H

#include "v34_precoder.h"

#include <stdbool.h>

enum v34_nonlinear_parameter {
	V34_NONLINEAR_THETA_ZERO,
	V34_NONLINEAR_THETA_5_OVER_16,
};

enum v34_nonlinear_result {
	V34_NONLINEAR_OK,
	V34_NONLINEAR_INVALID_ARGUMENT,
	V34_NONLINEAR_NUMERIC_FAILURE,
};

bool v34_nonlinear_parameter_is_valid(
	enum v34_nonlinear_parameter parameter);
bool v34_nonlinear_configuration_is_valid(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude);

/* Apply the clause 9.7 radial projection to a squared magnitude. This shares
 * the exact projection calculation used for complex symbols and leaves the
 * destination unchanged on failure. */
enum v34_nonlinear_result
v34_nonlinear_project_power(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude, double squared_magnitude,
	double *projected_squared_magnitude);

/* Apply the clause 9.7 radial projection to one exact Q7 precoder output.
 * mean_squared_magnitude is E[|x|^2] in unscaled constellation-grid units;
 * the returned point uses the same units. Absolute transmit-power
 * normalization remains the responsibility of the modulation owner. The
 * destination is unchanged when an argument or intermediate is rejected. */
enum v34_nonlinear_result
v34_nonlinear_encode(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude,
	const struct v34_q7_point *precoded_symbol,
	struct qam_point *projected_symbol);

/* Invert the clause 9.7 radial projection for one finite equalized symbol.
 * Both points use unscaled constellation-grid units. The returned estimate
 * remains floating point because channel noise need not land on the Q7
 * precoder grid. Input and destination may alias; the destination is unchanged
 * on failure. */
enum v34_nonlinear_result
v34_nonlinear_unproject(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude,
	const struct qam_point *projected_symbol,
	struct qam_point *precoded_estimate);

#endif /* V34_NONLINEAR_H */
