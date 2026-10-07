// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_points.h>
#include <v34_power_calibration.h>

#include "test.h"
#include "v34_state_fixture.h"
#include "v34_encoder_test_helpers.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct points_fixture {
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;
};

static bool initialize_fixture(
	struct points_fixture *fixture, enum v34_station_role role)
{
	struct points_fixture generated = { 0 };

	if (fixture == NULL ||
	    v34_frame_geometry_init(
		    &generated.geometry, V34_SYMBOL_RATE_3000, 8, true) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry,
		    (uint8_t)(generated.geometry.data_frames_per_superframe - 1u)) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_transmit_scrambler_init(&generated.scrambler, role) !=
		    V34_FRAMING_OK ||
	    v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    V34_TRELLIS_32_STATE) != V34_4D_ENCODER_OK ||
	    v34_calibrate_mapping_power(
		    &generated.schedule, &generated.scrambler,
		    &generated.encoder, V34_NONLINEAR_THETA_5_OVER_16,
		    &generated.normalizer) != V34_POWER_CALIBRATION_OK)
		return false;
	generated.schedule.next_mapping_frame =
		generated.geometry.mapping_frames_per_data_frame;
	if (!v34_frame_schedule_is_complete(&generated.schedule))
		return false;
	*fixture = generated;
	return true;
}

static void generate_bits(
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS], size_t count,
	uint32_t seed)
{
	uint32_t state = seed | 1u;

	for (size_t index = 0; index < count; ++index) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[index] = (uint8_t)(state >> 31);
	}
}

static bool mapping_frames_are_equal(
	const struct v34_mapping_frame *left,
	const struct v34_mapping_frame *right)
{
	return left->index == right->index &&
	       left->data_bits == right->data_bits &&
	       left->primary_bits == right->primary_bits &&
	       left->inversion_4d_mask == right->inversion_4d_mask &&
	       left->high_frame == right->high_frame &&
	       left->carries_auxiliary_bit == right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame;
}

static bool queue_and_compare_frame(
	struct v34_data_points *points,
	struct v34_frame_schedule *schedule,
	struct v34_scrambler *scrambler,
	struct v34_4d_encoder *encoder, size_t frame_index)
{
	struct v34_frame_schedule preview_schedule;
	struct v34_mapping_frame expected_request;
	struct v34_mapping_frame actual_request;
	struct v34_transmitted_mapping_frame transmitted;
	struct qam_point expected[V34_SYMBOLS_PER_MAPPING_FRAME];
	uint8_t primary_bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit = (uint8_t)(frame_index & 1u);
	const uint8_t *auxiliary;

	if (v34_frame_schedule_prepare_mapping_frame(schedule) !=
		    V34_FRAME_SCHEDULE_OK)
		return false;
	preview_schedule = *schedule;
	if (v34_frame_schedule_next(
		    &preview_schedule, &expected_request) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_data_points_preview_mapping_frame(
		    points, &actual_request) != V34_DATA_POINTS_OK ||
	    !mapping_frames_are_equal(&actual_request, &expected_request))
		return false;
	generate_bits(
		primary_bits, expected_request.primary_bits,
		UINT32_C(0x71d4935b) ^ (uint32_t)frame_index);
	auxiliary = expected_request.carries_auxiliary_bit ?
		&auxiliary_bit : NULL;
	if (v34_transmit_mapping_frame(
		    schedule, scrambler, encoder, primary_bits,
		    expected_request.primary_bits, auxiliary,
		    points->normalizer.nonlinear_parameter,
		    points->normalizer.precoded_mean_squared_magnitude,
		    &transmitted) != V34_MAPPING_FRAME_TRANSMITTER_OK)
		return false;
	for (size_t index = 0; index < V34_SYMBOLS_PER_MAPPING_FRAME;
	     ++index) {
		if (v34_normalize_power(
			    &points->normalizer,
			    &transmitted.projected_symbol[index],
			    &expected[index]) != V34_SYMBOL_POWER_OK)
			return false;
	}
	if (v34_data_points_queue_mapping_frame(
		    points, primary_bits, expected_request.primary_bits,
		    auxiliary) != V34_DATA_POINTS_OK ||
	    !v34_frame_geometry_is_equal(&points->schedule.geometry, &schedule->geometry) ||
	    points->schedule.data_frame_in_superframe != schedule->data_frame_in_superframe ||
	    points->schedule.next_mapping_frame != schedule->next_mapping_frame ||
	    points->scrambler.history != scrambler->history ||
	    points->scrambler.polynomial != scrambler->polynomial ||
	    !v34_test_encoders_are_equal(&points->encoder, encoder))
		return false;
	for (size_t index = 0; index < V34_SYMBOLS_PER_MAPPING_FRAME;
	     ++index) {
		struct qam_point actual;

		if (v34_data_points_next(points, &actual) !=
			    V34_DATA_POINTS_OK ||
		    actual.in_phase != expected[index].in_phase ||
		    actual.quadrature != expected[index].quadrature)
			return false;
	}
	return v34_data_points_needs_mapping_frame(points);
}

static bool test_mapping_stream(void)
{
	for (unsigned int role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role) {
		struct points_fixture fixture;
		struct v34_data_points points;
		struct v34_frame_schedule schedule;
		struct v34_scrambler scrambler;
		struct v34_4d_encoder encoder;
		struct qam_point untouched = { 123.0, -456.0 };

		CHECK(initialize_fixture(
			      &fixture, (enum v34_station_role)role) &&
		      v34_data_points_init(
			      &points, &fixture.schedule, &fixture.scrambler,
			      &fixture.encoder, &fixture.normalizer,
			      (enum v34_station_role)role) ==
			      V34_DATA_POINTS_OK &&
		      v34_data_points_needs_mapping_frame(&points) &&
		      v34_data_points_next(&points, &untouched) ==
			      V34_DATA_POINTS_MAPPING_FRAME_REQUIRED &&
		      untouched.in_phase == 123.0 &&
		      untouched.quadrature == -456.0,
		      "role %u stream did not initialize transactionally", role);
		schedule = fixture.schedule;
		scrambler = fixture.scrambler;
		encoder = fixture.encoder;
		CHECK(queue_and_compare_frame(
			      &points, &schedule, &scrambler, &encoder, 0) &&
		      schedule.data_frame_in_superframe == 0 &&
		      schedule.next_mapping_frame == 1 &&
		      queue_and_compare_frame(
			      &points, &schedule, &scrambler, &encoder, 1) &&
		      schedule.next_mapping_frame == 2,
		      "role %u mapping stream diverged", role);
	}
	return true;
}

static bool bytes_are_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != value)
			return false;
	}
	return true;
}

static bool test_transactional_validation(void)
{
	struct points_fixture fixture;
	struct v34_data_points points;
	struct v34_data_points before;
	struct v34_data_points rejected;
	struct v34_scrambler wrong_scrambler;
	struct v34_frame_geometry other_geometry;
	struct v34_frame_schedule other_schedule;
	struct v34_mapping_frame request = { 0 };
	struct v34_mapping_frame saved_request;
	struct qam_point output = { 123.0, -456.0 };
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t malformed[V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit = 1;
	uint8_t invalid_auxiliary = 2;
	const uint8_t *auxiliary;
	const uint8_t *wrong_auxiliary;

	CHECK(initialize_fixture(&fixture, V34_CALLING_STATION) &&
	      v34_transmit_scrambler_init(
		      &wrong_scrambler, V34_ANSWERING_STATION) ==
		      V34_FRAMING_OK &&
	      v34_frame_geometry_init(
		      &other_geometry, V34_SYMBOL_RATE_3000, 7, true) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_frame_schedule_init(
		      &other_schedule, &other_geometry, 0) ==
		      V34_FRAME_SCHEDULE_OK,
	      "validation fixture did not initialize");
	memset(&rejected, 0xa5, sizeof(rejected));
	CHECK(v34_data_points_init(
		      NULL, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer,
		      V34_CALLING_STATION) == V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_init(
		      &rejected, NULL, &fixture.scrambler, &fixture.encoder,
		      &fixture.normalizer, V34_CALLING_STATION) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_init(
		      &rejected, &fixture.schedule, &wrong_scrambler,
		      &fixture.encoder, &fixture.normalizer,
		      V34_CALLING_STATION) == V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_init(
		      &rejected, &other_schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer,
		      V34_CALLING_STATION) == V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_init(
		      &rejected, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer,
		      (enum v34_station_role)99) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      bytes_are_filled(&rejected, sizeof(rejected), 0xa5),
	      "rejected initialization changed its destination");
	CHECK(v34_data_points_init(
		      &points, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer,
		      V34_CALLING_STATION) == V34_DATA_POINTS_OK,
	      "valid points did not initialize");
	before = points;
	saved_request = request;
	CHECK(!v34_data_points_is_valid(NULL) &&
	      !v34_data_points_needs_mapping_frame(NULL) &&
	      v34_data_points_preview_mapping_frame(NULL, &request) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_preview_mapping_frame(&points, NULL) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_queue_mapping_frame(
		      NULL, NULL, 0, NULL) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_next(NULL, &output) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_next(&points, NULL) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_equal(&points, &before) &&
	      mapping_frames_are_equal(&request, &saved_request) &&
	      output.in_phase == 123.0 && output.quadrature == -456.0,
	      "invalid operation changed state or output");
	CHECK(v34_data_points_preview_mapping_frame(&points, &request) ==
		      V34_DATA_POINTS_OK,
	      "validation frame preview failed");
	generate_bits(bits, request.primary_bits, UINT32_C(0x5eab1703));
	memcpy(malformed, bits, request.primary_bits);
	malformed[request.primary_bits / 2u] = 2;
	auxiliary = request.carries_auxiliary_bit ? &auxiliary_bit : NULL;
	wrong_auxiliary = request.carries_auxiliary_bit ?
		&invalid_auxiliary : &auxiliary_bit;
	CHECK(v34_data_points_queue_mapping_frame(
		      &points, bits, (size_t)request.primary_bits - 1u,
		      auxiliary) == V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_queue_mapping_frame(
		      &points, malformed, request.primary_bits, auxiliary) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_queue_mapping_frame(
		      &points, bits, request.primary_bits, wrong_auxiliary) ==
		      V34_DATA_POINTS_INVALID_ARGUMENT &&
	      v34_data_points_equal(&points, &before),
	      "rejected frame changed mapping state");
	CHECK(v34_data_points_queue_mapping_frame(
		      &points, bits, request.primary_bits, auxiliary) ==
		      V34_DATA_POINTS_OK,
	      "valid frame did not queue");
	before = points;
	saved_request = request;
	CHECK(v34_data_points_preview_mapping_frame(&points, &request) ==
		      V34_DATA_POINTS_MAPPING_FRAME_PENDING &&
	      v34_data_points_queue_mapping_frame(
		      &points, bits, request.primary_bits, auxiliary) ==
		      V34_DATA_POINTS_MAPPING_FRAME_PENDING &&
	      v34_data_points_equal(&points, &before) &&
	      mapping_frames_are_equal(&request, &saved_request),
	      "pending frame was replaced");
	for (unsigned int corruption = 0; corruption < 3; ++corruption) {
		points = before;
		if (corruption == 0)
			points.point[V34_SYMBOLS_PER_MAPPING_FRAME - 1u].in_phase = NAN;
		else if (corruption == 1)
			points.next_point = V34_SYMBOLS_PER_MAPPING_FRAME + 1u;
		else
			points.normalizer.normalization_gain = NAN;
		rejected = points;
		CHECK(!v34_data_points_is_valid(&points) &&
		      !v34_data_points_needs_mapping_frame(&points) &&
		      v34_data_points_preview_mapping_frame(&points, &request) ==
			      V34_DATA_POINTS_INVALID_ARGUMENT &&
		      v34_data_points_queue_mapping_frame(
			      &points, bits, request.primary_bits, auxiliary) ==
			      V34_DATA_POINTS_INVALID_ARGUMENT &&
		      v34_data_points_next(&points, &output) ==
			      V34_DATA_POINTS_INVALID_ARGUMENT &&
		      v34_data_points_equal(&points, &rejected) &&
		      mapping_frames_are_equal(&request, &saved_request) &&
		      output.in_phase == 123.0 && output.quadrature == -456.0,
		      "corrupt point state %u was accepted or changed outputs",
		      corruption);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "mapping stream", test_mapping_stream },
		{ "transactional validation", test_transactional_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
