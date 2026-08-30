// SPDX-License-Identifier: BSD-3-Clause

#include <v34_frame_schedule.h>

#include "test.h"

#include <string.h>

struct symbol_rate_fixture {
	enum v34_symbol_rate symbol_rate;
	uint8_t data_frames;
	uint8_t mapping_frames;
	uint8_t minimum_rate;
	uint8_t maximum_rate;
	uint16_t auxiliary_pattern;
};

static const struct symbol_rate_fixture SYMBOL_RATES[] = {
	{ V34_SYMBOL_RATE_2400, 7, 12, 1, 9, 0x06db },
	{ V34_SYMBOL_RATE_2743, 8, 12, 2, 11, 0x056b },
	{ V34_SYMBOL_RATE_2800, 7, 14, 2, 11, 0x15ab },
	{ V34_SYMBOL_RATE_3000, 7, 15, 2, 12, 0x2aab },
	{ V34_SYMBOL_RATE_3200, 7, 16, 2, 13, 0x5555 },
	{ V34_SYMBOL_RATE_3429, 8, 15, 2, 14, 0x1555 },
};

struct collected_frame {
	uint16_t switching_pattern;
	uint16_t auxiliary_pattern;
	uint16_t data_bits;
	uint16_t primary_bits;
	uint8_t high_frames;
	uint8_t auxiliary_frames;
	uint8_t inversion_masks[16];
};

static bool schedules_are_equal(
	const struct v34_frame_schedule *left,
	const struct v34_frame_schedule *right)
{
	return v34_frame_geometry_is_equal(
		       &left->geometry, &right->geometry) &&
	       left->data_frame_in_superframe ==
		       right->data_frame_in_superframe &&
	       left->next_mapping_frame == right->next_mapping_frame;
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
	       left->carries_auxiliary_bit ==
		       right->carries_auxiliary_bit &&
	       left->completes_data_frame == right->completes_data_frame;
}

static bool collect_schedule(
	struct v34_frame_schedule *schedule,
	struct collected_frame *collected)
{
	struct collected_frame generated = { 0 };
	uint8_t mapping_frames =
		schedule->geometry.mapping_frames_per_data_frame;

	for (uint8_t index = 0; index < mapping_frames; ++index) {
		struct v34_mapping_frame mapping_frame;

		CHECK(v34_frame_schedule_next(
			      schedule, &mapping_frame) ==
			      V34_FRAME_SCHEDULE_OK,
		      "frame schedule stopped at mapping frame %u/%u", index,
		      mapping_frames);
		CHECK(mapping_frame.index == index &&
		      mapping_frame.data_bits ==
			      schedule->geometry.high_mapping_frame_bits -
				      !mapping_frame.high_frame &&
		      mapping_frame.primary_bits ==
			      mapping_frame.data_bits -
				      mapping_frame.carries_auxiliary_bit &&
		      mapping_frame.completes_data_frame ==
			      (index + 1u == mapping_frames),
		      "mapping frame %u/%u is internally inconsistent", index,
		      mapping_frames);
		generated.switching_pattern = (uint16_t)(
			(generated.switching_pattern << 1) |
			mapping_frame.high_frame);
		generated.auxiliary_pattern = (uint16_t)(
			(generated.auxiliary_pattern << 1) |
			mapping_frame.carries_auxiliary_bit);
		generated.data_bits = (uint16_t)(
			generated.data_bits + mapping_frame.data_bits);
		generated.primary_bits = (uint16_t)(
			generated.primary_bits + mapping_frame.primary_bits);
		generated.high_frames = (uint8_t)(
			generated.high_frames + mapping_frame.high_frame);
		generated.auxiliary_frames = (uint8_t)(
			generated.auxiliary_frames +
			mapping_frame.carries_auxiliary_bit);
		generated.inversion_masks[index] =
			mapping_frame.inversion_4d_mask;
	}
	CHECK(v34_frame_schedule_is_complete(schedule),
	      "frame schedule did not finish at its mapping-frame boundary");
	{
		struct v34_frame_schedule saved = *schedule;
		struct v34_mapping_frame output = {
			.index = UINT8_MAX,
			.data_bits = UINT8_MAX,
			.primary_bits = UINT8_MAX,
			.inversion_4d_mask = UINT8_MAX,
			.high_frame = true,
			.carries_auxiliary_bit = true,
			.completes_data_frame = false,
		};
		struct v34_mapping_frame expected = output;

		CHECK(v34_frame_schedule_next(schedule, &output) ==
			      V34_FRAME_SCHEDULE_FRAME_COMPLETE &&
		      schedules_are_equal(schedule, &saved) &&
		      mapping_frames_are_equal(&output, &expected),
		      "completed frame schedule emitted another mapping frame");
	}
	*collected = generated;
	return true;
}

static bool collect_frame(
	const struct v34_frame_geometry *geometry,
	uint8_t data_frame_in_superframe, struct collected_frame *collected)
{
	struct v34_frame_schedule schedule;

	CHECK(v34_frame_schedule_init(
		      &schedule, geometry, data_frame_in_superframe) ==
		      V34_FRAME_SCHEDULE_OK,
	      "frame %u schedule did not initialize",
	      data_frame_in_superframe);
	return collect_schedule(&schedule, collected);
}

static bool test_normative_patterns(void)
{
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t primary_rate;
		bool auxiliary;
		uint8_t high_bits;
		uint16_t switching_pattern;
	} switching[] = {
		{ V34_SYMBOL_RATE_2400, 1, false, 8, 0x0fff },
		{ V34_SYMBOL_RATE_2400, 1, true, 9, 0x06db },
		{ V34_SYMBOL_RATE_2743, 2, false, 14, 0x0fff },
		{ V34_SYMBOL_RATE_2743, 2, true, 15, 0x056b },
		{ V34_SYMBOL_RATE_2800, 2, false, 14, 0x1bb7 },
		{ V34_SYMBOL_RATE_2800, 2, true, 15, 0x0489 },
		{ V34_SYMBOL_RATE_3000, 8, false, 52, 0x0421 },
		{ V34_SYMBOL_RATE_3000, 8, true, 52, 0x3777 },
		{ V34_SYMBOL_RATE_3200, 13, false, 78, 0xffff },
		{ V34_SYMBOL_RATE_3200, 13, true, 79, 0x5555 },
		{ V34_SYMBOL_RATE_3429, 14, false, 79, 0x14a5 },
		{ V34_SYMBOL_RATE_3429, 14, true, 79, 0x3f7f },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(switching); ++index) {
		struct v34_frame_geometry geometry;
		struct collected_frame frame;

		CHECK(v34_frame_geometry_init(
			      &geometry, switching[index].symbol_rate,
			      switching[index].primary_rate,
			      switching[index].auxiliary) ==
			      V34_FRAME_SCHEDULE_OK &&
		      collect_frame(&geometry, 0, &frame),
		      "normative switching fixture %zu did not run", index);
		CHECK(geometry.high_mapping_frame_bits ==
			      switching[index].high_bits &&
		      frame.switching_pattern ==
			      switching[index].switching_pattern,
		      "normative switching fixture %zu changed: %u/0x%04x",
		      index, geometry.high_mapping_frame_bits,
		      frame.switching_pattern);
	}

	for (size_t index = 0; index < ARRAY_LENGTH(SYMBOL_RATES); ++index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[index];
		struct v34_frame_geometry geometry;
		struct collected_frame frame;

		CHECK(v34_frame_geometry_init(
			      &geometry, fixture->symbol_rate,
			      fixture->minimum_rate, true) ==
			      V34_FRAME_SCHEDULE_OK &&
		      collect_frame(&geometry, 0, &frame),
		      "auxiliary fixture %zu did not run", index);
		CHECK(geometry.data_frames_per_superframe ==
			      fixture->data_frames &&
		      geometry.mapping_frames_per_data_frame ==
			      fixture->mapping_frames &&
		      frame.auxiliary_pattern == fixture->auxiliary_pattern,
		      "auxiliary fixture %zu changed: 0x%04x", index,
		      frame.auxiliary_pattern);
	}
	return true;
}

static bool test_all_supported_schedules(void)
{
	for (size_t rate_index = 0;
	     rate_index < ARRAY_LENGTH(SYMBOL_RATES); ++rate_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[rate_index];

		for (uint8_t rate = fixture->minimum_rate;
		     rate <= fixture->maximum_rate; ++rate) {
			for (unsigned int auxiliary = 0; auxiliary < 2;
			     ++auxiliary) {
				struct v34_frame_geometry geometry;
				struct collected_frame frame;
				uint16_t primary_bits = (uint16_t)rate *
					(fixture->data_frames == 7u ? 96u : 84u);
				uint8_t auxiliary_bits = auxiliary != 0 ?
					(fixture->data_frames == 7u ? 8u : 7u) :
					0u;
				uint16_t total_bits = primary_bits + auxiliary_bits;
				uint8_t high_bits = (uint8_t)(
					(total_bits + fixture->mapping_frames - 1u) /
					fixture->mapping_frames);
				uint8_t high_frames = (uint8_t)(
					total_bits -
					(uint16_t)(high_bits - 1u) *
						fixture->mapping_frames);

				CHECK(v34_frame_geometry_init(
					      &geometry, fixture->symbol_rate, rate,
					      auxiliary != 0) ==
					      V34_FRAME_SCHEDULE_OK &&
				      v34_frame_geometry_is_valid(
					      &geometry) &&
				      collect_frame(&geometry, 0, &frame),
				      "schedule %zu/%u/%u did not run", rate_index,
				      rate, auxiliary);
				CHECK(geometry.data_bits_per_frame == total_bits &&
				      geometry.high_mapping_frame_bits == high_bits &&
				      geometry.high_mapping_frame_count == high_frames &&
				      geometry.auxiliary_bits_per_frame ==
					      auxiliary_bits &&
				      frame.data_bits == total_bits &&
				      frame.primary_bits == primary_bits &&
				      frame.high_frames == high_frames &&
				      frame.auxiliary_frames == auxiliary_bits,
				      "schedule %zu/%u/%u totals changed", rate_index,
				      rate, auxiliary);
			}
		}
	}
	return true;
}

static uint8_t expected_inversion_mask(
	const struct v34_frame_geometry *geometry,
	uint8_t data_frame, uint8_t mapping_frame)
{
	static const uint8_t j7[] = {
		0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0,
	};
	static const uint8_t j8[] = {
		0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0,
	};
	const uint8_t *pattern =
		geometry->data_frames_per_superframe == 7u ? j7 : j8;
	uint16_t midpoint =
		(uint16_t)geometry->mapping_frames_per_data_frame * 2u;
	uint8_t mask = 0;

	if (mapping_frame == 0 && pattern[data_frame * 2u] != 0)
		mask |= 1u;
	if (mapping_frame == midpoint / 4u &&
	    pattern[data_frame * 2u + 1u] != 0)
		mask |= (uint8_t)(1u << (midpoint % 4u));
	return mask;
}

static bool test_inversion_and_b1(void)
{
	for (size_t rate_index = 0;
	     rate_index < ARRAY_LENGTH(SYMBOL_RATES); ++rate_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[rate_index];
		struct v34_frame_geometry geometry;

		CHECK(v34_frame_geometry_init(
			      &geometry, fixture->symbol_rate,
			      fixture->minimum_rate, false) ==
			      V34_FRAME_SCHEDULE_OK,
		      "inversion fixture %zu did not initialize", rate_index);
		for (uint8_t data_frame = 0;
		     data_frame < geometry.data_frames_per_superframe;
		     ++data_frame) {
			struct collected_frame frame;

			CHECK(collect_frame(&geometry, data_frame, &frame),
			      "inversion frame %zu/%u did not run", rate_index,
			      data_frame);
			for (uint8_t mapping_frame = 0;
			     mapping_frame < geometry.mapping_frames_per_data_frame;
			     ++mapping_frame) {
				CHECK(frame.inversion_masks[mapping_frame] ==
					      expected_inversion_mask(
						      &geometry, data_frame,
						      mapping_frame),
				      "inversion moved at %zu/%u/%u", rate_index,
				      data_frame, mapping_frame);
			}
		}
		{
			struct v34_frame_schedule b1;
			struct collected_frame frame;

			CHECK(v34_b1_frame_schedule_init(
				      &b1, &geometry) ==
				      V34_FRAME_SCHEDULE_OK &&
			      b1.data_frame_in_superframe ==
				      geometry.data_frames_per_superframe - 1u &&
			      collect_schedule(&b1, &frame),
			      "B1 schedule %zu did not run", rate_index);
			for (uint8_t mapping_frame = 0;
			     mapping_frame < geometry.mapping_frames_per_data_frame;
			     ++mapping_frame) {
				CHECK(frame.inversion_masks[mapping_frame] ==
					      (mapping_frame == 0 ? 1u : 0u),
				      "B1 inversion moved at %zu/%u", rate_index,
				      mapping_frame);
			}
		}
	}
	return true;
}

static bool test_data_frame_rollover(void)
{
	for (size_t rate_index = 0;
	     rate_index < ARRAY_LENGTH(SYMBOL_RATES); ++rate_index) {
		const struct symbol_rate_fixture *fixture =
			&SYMBOL_RATES[rate_index];
		struct v34_frame_geometry geometry;

		CHECK(v34_frame_geometry_init(
			      &geometry, fixture->symbol_rate,
			      fixture->minimum_rate, true) ==
			      V34_FRAME_SCHEDULE_OK,
		      "rollover geometry %zu did not initialize", rate_index);
		for (uint8_t data_frame = 0;
		     data_frame < geometry.data_frames_per_superframe;
		     ++data_frame) {
			struct v34_frame_schedule schedule;
			struct v34_frame_schedule advanced;
			struct collected_frame ignored;
			uint8_t expected = (uint8_t)(data_frame + 1u);

			if (expected == geometry.data_frames_per_superframe)
				expected = 0;
			CHECK(v34_frame_schedule_init(
				      &schedule, &geometry, data_frame) ==
				      V34_FRAME_SCHEDULE_OK &&
			      collect_schedule(&schedule, &ignored) &&
			      v34_frame_schedule_prepare_mapping_frame(
				      &schedule) ==
				      V34_FRAME_SCHEDULE_OK &&
			      v34_frame_schedule_is_valid(&schedule) &&
			      !v34_frame_schedule_is_complete(
				      &schedule) &&
			      schedule.data_frame_in_superframe == expected &&
			      schedule.next_mapping_frame == 0,
			      "data frame %zu/%u did not roll over", rate_index,
			      data_frame);
			advanced = schedule;
			CHECK(v34_frame_schedule_prepare_mapping_frame(
				      &schedule) == V34_FRAME_SCHEDULE_OK &&
			      schedules_are_equal(&schedule, &advanced),
			      "incomplete schedule %zu/%u changed", rate_index,
			      data_frame);
		}
	}
	return true;
}

static bool test_validation(void)
{
	struct v34_frame_geometry geometry;
	struct v34_frame_geometry saved_geometry;
	struct v34_frame_schedule schedule;
	struct v34_frame_schedule saved_schedule;
	struct v34_mapping_frame mapping_frame = {
		.index = 3,
		.data_bits = 17,
		.primary_bits = 16,
		.inversion_4d_mask = 5,
		.high_frame = true,
		.carries_auxiliary_bit = true,
		.completes_data_frame = false,
	};
	struct v34_mapping_frame saved_mapping_frame = mapping_frame;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, true) ==
		      V34_FRAME_SCHEDULE_OK,
	      "validation geometry did not initialize");
	saved_geometry = geometry;
	CHECK(v34_frame_geometry_init(
		      NULL, V34_SYMBOL_RATE_3000, 8, true) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_COUNT, 8, true) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 0, true) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3429, 1, true) ==
		      V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION &&
	      v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_2400, 10, true) ==
		      V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION &&
	      v34_frame_geometry_is_equal(&geometry, &saved_geometry),
	      "invalid geometry selection changed its destination");

	geometry = saved_geometry;
	geometry.data_bits_per_frame++;
	CHECK(!v34_frame_geometry_is_valid(&geometry),
	      "geometry accepted a changed bit total");
	geometry = saved_geometry;
	geometry.high_mapping_frame_count++;
	CHECK(!v34_frame_geometry_is_valid(&geometry),
	      "geometry accepted a changed switching count");
	geometry = saved_geometry;
	geometry.auxiliary_channel_enabled = false;
	CHECK(!v34_frame_geometry_is_valid(&geometry) &&
	      !v34_frame_geometry_is_valid(NULL),
	      "geometry accepted inconsistent auxiliary state");

	geometry = saved_geometry;
	CHECK(v34_frame_schedule_init(&schedule, &geometry, 2) ==
		      V34_FRAME_SCHEDULE_OK,
	      "validation schedule did not initialize");
	saved_schedule = schedule;
	CHECK(v34_frame_schedule_init(NULL, &geometry, 2) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_schedule_init(&schedule, NULL, 2) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_schedule_init(
		      &schedule, &geometry,
		      geometry.data_frames_per_superframe) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_b1_frame_schedule_init(NULL, &geometry) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_b1_frame_schedule_init(&schedule, NULL) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      schedules_are_equal(&schedule, &saved_schedule),
	      "invalid schedule initialization changed its destination");

	CHECK(v34_frame_schedule_next(
		      NULL, &mapping_frame) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_schedule_prepare_mapping_frame(NULL) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      v34_frame_schedule_next(&schedule, NULL) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      schedules_are_equal(&schedule, &saved_schedule) &&
	      mapping_frames_are_equal(
		      &mapping_frame, &saved_mapping_frame),
	      "invalid schedule emission changed state or output");
	schedule = saved_schedule;
	schedule.next_mapping_frame =
		(uint8_t)(geometry.mapping_frames_per_data_frame + 1u);
	saved_schedule = schedule;
	CHECK(!v34_frame_schedule_is_valid(&schedule) &&
	      v34_frame_schedule_prepare_mapping_frame(&schedule) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      schedules_are_equal(&schedule, &saved_schedule) &&
	      v34_frame_schedule_next(
		      &schedule, &mapping_frame) ==
		      V34_FRAME_SCHEDULE_INVALID_ARGUMENT &&
	      mapping_frames_are_equal(
		      &mapping_frame, &saved_mapping_frame) &&
	      !v34_frame_schedule_is_valid(NULL) &&
	      !v34_frame_schedule_is_complete(NULL),
	      "schedule accepted corrupt state");

	return true;
}

static bool test_geometry_equality(void)
{
	struct v34_frame_geometry geometry;

	CHECK(v34_frame_geometry_init(
		      &geometry, V34_SYMBOL_RATE_3000, 8, true) ==
		      V34_FRAME_SCHEDULE_OK &&
	      v34_frame_geometry_is_equal(&geometry, &geometry) &&
	      !v34_frame_geometry_is_equal(NULL, &geometry) &&
	      !v34_frame_geometry_is_equal(&geometry, NULL),
	      "frame-geometry equality rejected its base contract");
	for (unsigned int field = 0; field < 9; ++field) {
		struct v34_frame_geometry changed = geometry;

		switch (field) {
		case 0:
			changed.symbol_rate = V34_SYMBOL_RATE_3200;
			break;
		case 1:
			changed.data_bits_per_frame++;
			break;
		case 2:
			changed.primary_data_rate_2400_bps_units++;
			break;
		case 3:
			changed.data_frames_per_superframe++;
			break;
		case 4:
			changed.mapping_frames_per_data_frame++;
			break;
		case 5:
			changed.high_mapping_frame_bits++;
			break;
		case 6:
			changed.high_mapping_frame_count++;
			break;
		case 7:
			changed.auxiliary_bits_per_frame++;
			break;
		case 8:
			changed.auxiliary_channel_enabled =
				!changed.auxiliary_channel_enabled;
			break;
		}
		CHECK(!v34_frame_geometry_is_equal(&geometry, &changed) &&
		      !v34_frame_geometry_is_equal(&changed, &geometry),
		      "frame-geometry field %u did not affect equality", field);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative patterns", test_normative_patterns },
		{ "all supported schedules", test_all_supported_schedules },
		{ "inversion and B1", test_inversion_and_b1 },
		{ "data frame rollover", test_data_frame_rollover },
		{ "validation", test_validation },
		{ "geometry equality", test_geometry_equality },
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
