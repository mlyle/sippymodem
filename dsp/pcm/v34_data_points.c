// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_points.h"

#include <math.h>
#include <stddef.h>

static const double V34_DATA_POINT_POWER_EPSILON = 1e-12;

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static bool point_fits(
	const struct qam_point *point, double maximum_power)
{
	double power;

	if (!isfinite(point->in_phase) || !isfinite(point->quadrature))
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) &&
	       (power <= maximum_power ||
		power / maximum_power <=
			1.0 + V34_DATA_POINT_POWER_EPSILON);
}

bool v34_data_points_is_valid(
	const struct v34_data_points *points)
{
	if (points == NULL || !role_is_valid(points->local_role) ||
	    !v34_frame_schedule_is_valid(&points->schedule) ||
	    !v34_transmit_scrambler_is_valid(
		    &points->scrambler, points->local_role) ||
	    !v34_4d_encoder_is_valid(&points->encoder) ||
	    !v34_power_normalizer_is_valid(&points->normalizer) ||
	    points->schedule.geometry.high_mapping_frame_bits !=
		    points->encoder.parameters.mapping_frame_bits ||
	    !v34_mapping_parameters_are_equal(
		    &points->encoder.parameters,
		    &points->normalizer.parameters) ||
	    points->next_point > V34_SYMBOLS_PER_MAPPING_FRAME)
		return false;
	for (size_t index = 0; index < V34_SYMBOLS_PER_MAPPING_FRAME;
	     ++index) {
		if (!point_fits(
			    &points->point[index],
			    points->normalizer.maximum_normalized_point_power))
			return false;
	}
	return true;
}

enum v34_data_points_result v34_data_points_init(
	struct v34_data_points *points,
	const struct v34_frame_schedule *schedule,
	const struct v34_scrambler *scrambler,
	const struct v34_4d_encoder *encoder,
	const struct v34_power_normalizer *normalizer,
	enum v34_station_role local_role)
{
	struct v34_data_points initialized = { 0 };

	if (points == NULL || schedule == NULL || scrambler == NULL ||
	    encoder == NULL || normalizer == NULL ||
	    !role_is_valid(local_role))
		return V34_DATA_POINTS_INVALID_ARGUMENT;
	initialized.schedule = *schedule;
	initialized.scrambler = *scrambler;
	initialized.encoder = *encoder;
	initialized.normalizer = *normalizer;
	initialized.local_role = local_role;
	initialized.next_point = V34_SYMBOLS_PER_MAPPING_FRAME;
	if (!v34_data_points_is_valid(&initialized))
		return V34_DATA_POINTS_INVALID_ARGUMENT;
	*points = initialized;
	return V34_DATA_POINTS_OK;
}

bool v34_data_points_needs_mapping_frame(
	const struct v34_data_points *points)
{
	return v34_data_points_is_valid(points) &&
	       points->next_point == V34_SYMBOLS_PER_MAPPING_FRAME;
}

static bool prepare_next_schedule(struct v34_frame_schedule *schedule)
{
	return !v34_frame_schedule_is_complete(schedule) ||
	       v34_frame_schedule_advance_data_frame(schedule) ==
		       V34_FRAME_SCHEDULE_OK;
}

enum v34_data_points_result v34_data_points_preview_mapping_frame(
	const struct v34_data_points *points,
	struct v34_mapping_frame *mapping_frame)
{
	struct v34_frame_schedule schedule;
	struct v34_mapping_frame preview;

	if (!v34_data_points_is_valid(points) || mapping_frame == NULL)
		return V34_DATA_POINTS_INVALID_ARGUMENT;
	if (!v34_data_points_needs_mapping_frame(points))
		return V34_DATA_POINTS_MAPPING_FRAME_PENDING;
	schedule = points->schedule;
	if (!prepare_next_schedule(&schedule) ||
	    v34_frame_schedule_next(&schedule, &preview) !=
		    V34_FRAME_SCHEDULE_OK)
		return V34_DATA_POINTS_NUMERIC_FAILURE;
	*mapping_frame = preview;
	return V34_DATA_POINTS_OK;
}

static enum v34_data_points_result mapping_result(
	enum v34_mapping_frame_transmitter_result result)
{
	switch (result) {
	case V34_MAPPING_FRAME_TRANSMITTER_OK:
		return V34_DATA_POINTS_OK;
	case V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT:
		return V34_DATA_POINTS_INVALID_ARGUMENT;
	case V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE:
		return V34_DATA_POINTS_OUT_OF_RANGE;
	case V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE:
		return V34_DATA_POINTS_NUMERIC_FAILURE;
	case V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE:
		break;
	}
	return V34_DATA_POINTS_NUMERIC_FAILURE;
}

static enum v34_data_points_result normalization_result(
	enum v34_symbol_power_result result)
{
	switch (result) {
	case V34_SYMBOL_POWER_OK:
		return V34_DATA_POINTS_OK;
	case V34_SYMBOL_POWER_OUT_OF_RANGE:
		return V34_DATA_POINTS_OUT_OF_RANGE;
	case V34_SYMBOL_POWER_NUMERIC_FAILURE:
	case V34_SYMBOL_POWER_INVALID_ARGUMENT:
		break;
	}
	return V34_DATA_POINTS_NUMERIC_FAILURE;
}

enum v34_data_points_result v34_data_points_queue_mapping_frame(
	struct v34_data_points *points,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit)
{
	struct v34_data_points next;
	struct v34_transmitted_mapping_frame transmitted;
	enum v34_data_points_result result;

	if (!v34_data_points_is_valid(points))
		return V34_DATA_POINTS_INVALID_ARGUMENT;
	if (!v34_data_points_needs_mapping_frame(points))
		return V34_DATA_POINTS_MAPPING_FRAME_PENDING;
	next = *points;
	if (!prepare_next_schedule(&next.schedule))
		return V34_DATA_POINTS_NUMERIC_FAILURE;
	result = mapping_result(v34_transmit_mapping_frame(
		&next.schedule, &next.scrambler, &next.encoder,
		primary_bits, primary_bit_count, auxiliary_bit,
		next.normalizer.nonlinear_parameter,
		next.normalizer.precoded_mean_squared_magnitude,
		&transmitted));
	if (result != V34_DATA_POINTS_OK)
		return result;
	for (size_t index = 0; index < V34_SYMBOLS_PER_MAPPING_FRAME;
	     ++index) {
		result = normalization_result(v34_normalize_power(
			&next.normalizer, &transmitted.projected_symbol[index],
			&next.point[index]));
		if (result != V34_DATA_POINTS_OK)
			return result;
	}
	next.next_point = 0;
	if (!v34_data_points_is_valid(&next))
		return V34_DATA_POINTS_NUMERIC_FAILURE;
	*points = next;
	return V34_DATA_POINTS_OK;
}

enum v34_data_points_result v34_data_points_next(
	struct v34_data_points *points, struct qam_point *point)
{
	struct v34_data_points next;
	struct qam_point generated;

	if (!v34_data_points_is_valid(points) || point == NULL)
		return V34_DATA_POINTS_INVALID_ARGUMENT;
	if (v34_data_points_needs_mapping_frame(points))
		return V34_DATA_POINTS_MAPPING_FRAME_REQUIRED;
	next = *points;
	generated = next.point[next.next_point++];
	if (!v34_data_points_is_valid(&next))
		return V34_DATA_POINTS_NUMERIC_FAILURE;
	*points = next;
	*point = generated;
	return V34_DATA_POINTS_OK;
}

const char *v34_data_points_result_name(
	enum v34_data_points_result result)
{
	switch (result) {
	case V34_DATA_POINTS_OK:
		return "ok";
	case V34_DATA_POINTS_MAPPING_FRAME_REQUIRED:
		return "mapping frame required";
	case V34_DATA_POINTS_MAPPING_FRAME_PENDING:
		return "mapping frame pending";
	case V34_DATA_POINTS_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_DATA_POINTS_OUT_OF_RANGE:
		return "out of range";
	case V34_DATA_POINTS_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
