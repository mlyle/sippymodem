// SPDX-License-Identifier: BSD-3-Clause

#include <v34_timing_control.h>

#include "test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint64_t hash_u8(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
	hash = hash_u8(hash, (uint8_t)value);
	return hash_u8(hash, (uint8_t)(value >> 8));
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
	hash = hash_u16(hash, (uint16_t)value);
	return hash_u16(hash, (uint16_t)(value >> 16));
}

static uint64_t hash_state(
	uint64_t hash, const struct v34_timing_control *control)
{
	hash = hash_u16(hash, control->phase);
	hash = hash_u16(hash, control->phase_increment);
	hash = hash_u16(hash, control->phase_limit);
	hash = hash_u16(hash, control->nominal_increment);
	hash = hash_u32(hash, (uint32_t)control->phase_residue);
	hash = hash_u32(hash, (uint32_t)control->integrator);
	hash = hash_u16(hash, (uint16_t)control->accumulated_delta);
	hash = hash_u16(hash, control->accumulated_count);
	hash = hash_u16(hash, (uint16_t)control->offset_ppm);
	hash = hash_u16(hash, control->average_limit);
	hash = hash_u16(hash, control->state_count);
	hash = hash_u16(hash, (uint16_t)control->timeout);
	hash = hash_u16(hash, (uint16_t)control->proportional_gain);
	hash = hash_u16(hash, (uint16_t)control->integral_gain);
	hash = hash_u8(hash, control->early_index);
	hash = hash_u8(hash, control->late_index);
	hash = hash_u16(hash, control->nominal_symbol_rate);
	hash = hash_u8(hash, (uint8_t)control->local_role);
	hash = hash_u8(hash, (uint8_t)control->mode);
	return hash_u8(hash, control->full_pull_in);
}

static uint32_t next_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static void generate_observations(
	uint32_t *random, unsigned int step,
	int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS])
{
	for (unsigned int index = 0;
	     index < V34_TIMING_CONTROL_OBSERVATIONS; ++index) {
		uint32_t word = next_word(random);
		int32_t centered = (int32_t)((word >> 20) & 0x7ffu) - 1024;

		observations[index] = (int16_t)(
			centered * 9 +
			(int32_t)((step + index * 17u) % 127u) - 63);
	}
	if (step == 0) {
		static const int16_t initial[] = {
			-900, 1200, -2100, 3300, -4700, 6200, -7900,
		};

		memcpy(observations, initial, sizeof(initial));
	}
}

static bool test_exact_control(void)
{
	static const struct {
		uint32_t numerator;
		uint32_t denominator;
		uint16_t increment;
		uint16_t limit;
	} rates[] = {
		{ 2400, 1, 13335, 16002 },
		{ 19200, 7, 11655, 15984 },
		{ 2800, 1, 11430, 16002 },
		{ 3000, 1, 10666, 15999 },
		{ 3200, 1, 10000, 16000 },
		{ 24000, 7, 9331, 15996 },
	};
	uint64_t state_hash = UINT64_C(1469598103934665603);
	uint64_t output_hash = UINT64_C(1469598103934665603);
	uint32_t random = UINT32_C(0x9254a13d);

	for (unsigned int role = 0; role < 2; ++role) {
		for (size_t rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
			for (unsigned int full = 0; full < 2; ++full) {
				struct v34_timing_control control;

				CHECK(v34_timing_control_init(
					      &control, 8000, rates[rate].numerator,
					      rates[rate].denominator,
					      (enum v34_station_role)role) ==
					      V34_TIMING_CONTROL_OK &&
				      control.phase_increment ==
					      rates[rate].increment &&
				      control.phase_limit == rates[rate].limit &&
				      control.phase == rates[rate].limit / 2u,
				      "V.34 timing geometry changed at %u/%zu", role,
				      rate);
				CHECK(v34_timing_control_begin_acquisition(
					      &control, full != 0) ==
					      V34_TIMING_CONTROL_OK,
				      "V.34 timing acquisition did not start");
				for (unsigned int step = 0; step < 10000; ++step) {
					int16_t observations[
						V34_TIMING_CONTROL_OBSERVATIONS];
					uint16_t increment;

					generate_observations(
						&random, step, observations);
					CHECK(v34_timing_control_process(
						      &control, observations,
						      &increment) ==
						      V34_TIMING_CONTROL_OK &&
					      increment == control.phase_increment,
					      "V.34 timing update failed at "
					      "%u/%zu/%u/%u",
					      role, rate, full, step);
					output_hash = hash_u16(output_hash, increment);
					state_hash = hash_state(state_hash, &control);
				}
				CHECK(control.mode ==
					      ((role == V34_CALLING_STATION && full != 0) ?
						       V34_TIMING_CONTROL_MEDIUM_TRACK :
						       V34_TIMING_CONTROL_HOLD),
				      "V.34 timing schedule changed at %u/%zu/%u",
				      role, rate, full);
			}
		}
	}
	CHECK(random == UINT32_C(0x92819fbd) &&
		      state_hash == UINT64_C(0x7c8e3f7419dcc66e) &&
		      output_hash == UINT64_C(0x22de953785b43016),
	      "V.34 timing oracle changed: %#x/%#llx/%#llx", random,
	      (unsigned long long)state_hash,
	      (unsigned long long)output_hash);
	return true;
}

static bool test_ownership(void)
{
	struct v34_timing_control control;
	struct v34_timing_control copy;
	struct v34_timing_control saved;
	struct v34_timing_control uninitialized = { 0 };
	int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS] = {
		0, 500, 700, -200, 300, 100, -50,
	};
	uint16_t increment = 1234;
	uint16_t saved_increment = increment;

	CHECK(!v34_timing_control_is_valid(NULL) &&
		      !v34_timing_control_is_valid(&uninitialized) &&
		      v34_timing_control_init(
			      NULL, 8000, 3000, 1,
			      V34_ANSWERING_STATION) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_init(
			      &control, 0, 3000, 1,
			      V34_ANSWERING_STATION) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_init(
			      &control, 8000, 0, 1,
			      V34_ANSWERING_STATION) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_init(
			      &control, 8000, 3000, 0,
			      V34_ANSWERING_STATION) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_init(
			      &control, 8000, 2000, 1,
			      V34_ANSWERING_STATION) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_init(
			      &control, 8000, 3000, 1,
			      (enum v34_station_role)2) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT,
	      "V.34 timing initialization validation changed");
	CHECK(v34_timing_control_init(
		      &control, 8000, 3000, 1,
		      V34_ANSWERING_STATION) == V34_TIMING_CONTROL_OK,
	      "V.34 timing owner did not initialize");
	saved = control;
	CHECK(v34_timing_control_init(
		      &control, UINT32_MAX, UINT32_MAX, UINT32_MAX,
		      V34_ANSWERING_STATION) ==
		      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      memcmp(&control, &saved, sizeof(control)) == 0,
	      "V.34 timing rejected initialization changed its owner");
	CHECK(v34_timing_control_begin_acquisition(NULL, true) ==
		      V34_TIMING_CONTROL_INVALID_STATE &&
		      v34_timing_control_begin_acquisition(
			      &uninitialized, true) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
		      v34_timing_control_process(
			      NULL, observations, &increment) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
		      v34_timing_control_process(
			      &uninitialized, observations, &increment) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
		      v34_timing_control_process(&control, NULL, &increment) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_process(
			      &control, observations, NULL) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_process(
			      &control, (const int16_t *)(const void *)&control,
			      &increment) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_process(
			      &control, observations,
			      (uint16_t *)(void *)&control) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      v34_timing_control_process(
			      &control, observations,
			      (uint16_t *)(void *)observations) ==
			      V34_TIMING_CONTROL_INVALID_ARGUMENT &&
		      memcmp(&control, &saved, sizeof(control)) == 0 &&
		      increment == saved_increment,
	      "V.34 timing rejection was not transactional");
	copy = control;
	for (unsigned int index = 0; index < 512; ++index) {
		uint16_t left;
		uint16_t right;

		observations[index % V34_TIMING_CONTROL_OBSERVATIONS] =
			(int16_t)(index * 97u);
		CHECK(v34_timing_control_process(
			      &control, observations, &left) ==
			      V34_TIMING_CONTROL_OK &&
		      v34_timing_control_process(
			      &copy, observations, &right) ==
			      V34_TIMING_CONTROL_OK &&
		      left == right &&
		      memcmp(&control, &copy, sizeof(control)) == 0,
		      "V.34 timing owners diverged at %u", index);
	}
	saved = control;
	control.phase = control.phase_limit;
	CHECK(!v34_timing_control_is_valid(&control) &&
		      v34_timing_control_process(
			      &control, observations, &increment) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
		      control.phase == control.phase_limit,
	      "V.34 timing corrupt-state rejection changed");
	control = saved;
	control.phase_increment = (uint16_t)(control.phase_limit * 2u);
	CHECK(!v34_timing_control_is_valid(&control),
	      "V.34 timing accepted an unbounded input cadence");
	control = saved;
	control.phase_residue = 0x4000;
	CHECK(!v34_timing_control_is_valid(&control),
	      "V.34 timing accepted an unnormalized phase residue");
	control = saved;
	control.full_pull_in = true;
	CHECK(!v34_timing_control_is_valid(&control),
	      "V.34 timing accepted a detached pull-in request");
	control = saved;
	return v34_timing_control_is_valid(&control);
}

static bool test_data_freeze(void)
{
	struct v34_timing_control uninitialized = { 0 };
	uint32_t random = UINT32_C(0x2d6414c3);

	CHECK(v34_timing_control_set_data_frozen(NULL, true) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
	      v34_timing_control_set_data_frozen(&uninitialized, true) ==
			      V34_TIMING_CONTROL_INVALID_STATE,
	      "V.34 timing freeze accepted an absent owner");
	for (unsigned int role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role) {
		struct v34_timing_control control;
		struct v34_timing_control saved;
		struct v34_timing_control expected;

		CHECK(v34_timing_control_init(
			      &control, 8000, 3000, 1,
			      (enum v34_station_role)role) ==
			      V34_TIMING_CONTROL_OK,
		      "V.34 timing freeze owner %u did not initialize", role);
		saved = control;
		CHECK(v34_timing_control_set_data_frozen(&control, false) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
		      memcmp(&control, &saved, sizeof(control)) == 0 &&
		      v34_timing_control_begin_acquisition(&control, true) ==
			      V34_TIMING_CONTROL_OK,
		      "V.34 timing freeze changed initial owner %u", role);
		saved = control;
		CHECK(v34_timing_control_set_data_frozen(&control, true) ==
			      V34_TIMING_CONTROL_INVALID_STATE &&
		      memcmp(&control, &saved, sizeof(control)) == 0,
		      "V.34 timing freeze changed pending owner %u", role);
		for (unsigned int step = 0; step < 10000; ++step) {
			int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS];
			uint16_t increment;

			generate_observations(&random, step, observations);
			CHECK(v34_timing_control_process(
				      &control, observations, &increment) ==
				      V34_TIMING_CONTROL_OK,
			      "V.34 timing owner %u did not continue at %u", role,
			      step);
		}
		saved = control;
		if (control.mode != V34_TIMING_CONTROL_HOLD) {
			CHECK(role == V34_CALLING_STATION &&
			      control.mode == V34_TIMING_CONTROL_MEDIUM_TRACK &&
			      v34_timing_control_set_data_frozen(&control, true) ==
				      V34_TIMING_CONTROL_OK &&
			      v34_timing_control_set_data_frozen(&control, false) ==
				      V34_TIMING_CONTROL_OK &&
			      memcmp(&control, &saved, sizeof(control)) == 0,
			      "V.34 timing freeze interrupted active role %u", role);
			continue;
		}
		expected = control;
		expected.mode = V34_TIMING_CONTROL_LOCKED;
		CHECK(v34_timing_control_set_data_frozen(&control, true) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0 &&
		      v34_timing_control_set_data_frozen(&control, true) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0,
		      "V.34 timing freeze changed trained lineage %u", role);
		for (unsigned int step = 0; step < 512; ++step) {
			int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS];
			uint16_t increment;

			generate_observations(&random, step + 2u, observations);
			CHECK(v34_timing_control_process(
				      &control, observations, &increment) ==
				      V34_TIMING_CONTROL_OK &&
			      control.mode == V34_TIMING_CONTROL_LOCKED &&
			      control.integrator == saved.integrator,
			      "V.34 timing freeze escaped at %u/%u", role, step);
		}
		expected = control;
		expected.mode = V34_TIMING_CONTROL_HOLD;
		CHECK(v34_timing_control_set_data_frozen(&control, false) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0 &&
		      v34_timing_control_set_data_frozen(&control, false) ==
			      V34_TIMING_CONTROL_OK,
		      "V.34 timing hold did not resume for role %u", role);
	}
	return true;
}

static bool test_feedback_hold(void)
{
	struct v34_timing_control invalid = { 0 };
	struct v34_timing_control invalid_saved = invalid;
	uint32_t random = UINT32_C(0x031932cd);

	CHECK(v34_timing_control_set_feedback_held(NULL, true) ==
		      V34_TIMING_CONTROL_INVALID_STATE &&
	      v34_timing_control_set_feedback_held(&invalid, true) ==
		      V34_TIMING_CONTROL_INVALID_STATE &&
	      memcmp(&invalid, &invalid_saved, sizeof(invalid)) == 0,
	      "timing feedback hold accepted an absent owner");
	for (unsigned int mode = V34_TIMING_CONTROL_LOCKED;
	     mode <= V34_TIMING_CONTROL_HOLD; ++mode) {
		struct v34_timing_control control;
		struct v34_timing_control saved;
		struct v34_timing_control expected;
		uint16_t minimum = UINT16_MAX;
		uint16_t maximum = 0;

		CHECK(v34_timing_control_init(&control, 8000, 3000, 1,
			      V34_ANSWERING_STATION) == V34_TIMING_CONTROL_OK,
		      "timing feedback owner did not initialize");
		if (mode != V34_TIMING_CONTROL_LOCKED)
			CHECK(v34_timing_control_begin_acquisition(&control, true) ==
				      V34_TIMING_CONTROL_OK,
			      "timing feedback owner did not start acquisition");
		for (unsigned int step = 0; (unsigned int)control.mode < mode &&
		     step < 12000u; ++step) {
			int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS];
			uint16_t increment;

			generate_observations(&random, step, observations);
			CHECK(v34_timing_control_process(&control, observations,
				      &increment) == V34_TIMING_CONTROL_OK,
			      "timing feedback owner did not reach mode %u", mode);
		}
		CHECK((unsigned int)control.mode == mode,
		      "timing feedback fixture missed mode %u", mode);
		/* A nonintegral learned frequency must keep accumulating while
		 * feedback is held, not become a constant rounded increment. */
		control.integrator = 0x1c001;
		saved = control;
		expected = saved;
		expected.feedback_held = true;
		CHECK(v34_timing_control_set_feedback_held(&control, true) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0 &&
		      v34_timing_control_set_feedback_held(&control, true) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0,
		      "timing feedback hold changed the acquired state %u", mode);
		for (unsigned int step = 0; step < 512u; ++step) {
			int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS];
			int32_t accumulator = control.integrator + control.phase_residue;
			int32_t delta = (accumulator + 0x4000) >> 15;
			uint16_t increment;

			generate_observations(&random, step + 1u, observations);
			expected = control;
			CHECK(v34_timing_control_process(&control, observations,
				      &increment) == V34_TIMING_CONTROL_OK &&
			      increment == control.nominal_increment + delta &&
			      control.phase_residue == accumulator - delta * 0x8000,
			      "held clock lost its fractional frequency %u/%u", mode, step);
			/* Offset reporting advances with the oscillator; acquisition,
			 * learned frequency, gains and physical phase do not change. */
			expected.phase_increment = control.phase_increment;
			expected.phase_residue = control.phase_residue;
			expected.accumulated_delta = control.accumulated_delta;
			expected.accumulated_count = control.accumulated_count;
			expected.offset_ppm = control.offset_ppm;
			CHECK(memcmp(&control, &expected, sizeof(control)) == 0,
			      "quiet observations changed acquisition %u/%u", mode, step);
			if (increment < minimum)
				minimum = increment;
			if (increment > maximum)
				maximum = increment;
		}
		CHECK(minimum < maximum && control.integrator == saved.integrator,
		      "held clock did not retain fractional frequency %u", mode);
		expected = control;
		expected.feedback_held = false;
		CHECK(v34_timing_control_set_feedback_held(&control, false) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0,
		      "timing feedback release changed its retained state %u", mode);
		{
			int16_t observations[V34_TIMING_CONTROL_OBSERVATIONS];
			uint16_t increment;
			uint16_t expected_increment;

			generate_observations(&random, 0, observations);
			CHECK(v34_timing_control_process(&control, observations,
				      &increment) == V34_TIMING_CONTROL_OK &&
			      v34_timing_control_process(&expected, observations,
				      &expected_increment) == V34_TIMING_CONTROL_OK &&
			      increment == expected_increment &&
			      memcmp(&control, &expected, sizeof(control)) == 0,
		      "timing feedback did not resume its retained epoch %u", mode);
		}
		CHECK(v34_timing_control_set_feedback_held(&control, true) ==
			      V34_TIMING_CONTROL_OK,
		      "timing feedback fixture could not hold before restart");
		expected = control;
		expected.mode = V34_TIMING_CONTROL_INITIAL_PHASE;
		expected.full_pull_in = false;
		expected.feedback_held = false;
		CHECK(v34_timing_control_begin_acquisition(&control, false) ==
			      V34_TIMING_CONTROL_OK &&
		      memcmp(&control, &expected, sizeof(control)) == 0,
		      "new acquisition retained an old quiet hold %u", mode);
		CHECK(v34_timing_control_set_feedback_held(&control, true) ==
			      V34_TIMING_CONTROL_OK &&
		      v34_timing_control_init(&control, 8000, 3000, 1,
			      V34_ANSWERING_STATION) == V34_TIMING_CONTROL_OK &&
		      !control.feedback_held && control.integrator == 0,
		      "new timing owner retained the old epoch %u", mode);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact control", test_exact_control },
		{ "ownership", test_ownership },
		{ "data freeze", test_data_freeze },
		{ "feedback hold", test_feedback_hold },
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
