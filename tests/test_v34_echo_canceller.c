// SPDX-License-Identifier: BSD-3-Clause

#include <v34_echo_canceller.h>

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, ...)                                                \
	do {                                                                   \
		if (!(condition)) {                                               \
			fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);            \
			fprintf(stderr, __VA_ARGS__);                              \
			fputc('\n', stderr);                                       \
			return false;                                              \
		}                                                              \
	} while (0)

enum {
	PATH_CAPACITY = 256,
	NEAR_DELAY = 32,
	TRAIN_SAMPLES = 18000,
	VERIFY_SAMPLES = 8000,
};

struct generated_line {
	int16_t history[PATH_CAPACITY];
	size_t write;
	size_t fill;
};

static int16_t line_at_age(const struct generated_line *line, size_t age)
{
	size_t index;

	if (age == 0 || age > line->fill)
		return 0;
	index = line->write + PATH_CAPACITY - age;
	if (index >= PATH_CAPACITY)
		index -= PATH_CAPACITY;
	return line->history[index];
}

static void line_push(struct generated_line *line, int16_t sample)
{
	line->history[line->write] = sample;
	line->write = (line->write + 1u) % PATH_CAPACITY;
	if (line->fill < PATH_CAPACITY)
		line->fill++;
}

static uint32_t prbs_next(uint32_t *state)
{
	uint32_t value = *state;

	value = value * UINT32_C(1664525) + UINT32_C(1013904223);
	*state = value;
	return value;
}

static int16_t generated_sample(uint32_t *state, int16_t amplitude)
{
	uint32_t value = prbs_next(state);
	int32_t level = (int32_t)((value >> 29) & 7u) - 3;

	return (int16_t)(level * amplitude / 4);
}

static int16_t q15_scale(int16_t sample, int16_t scale)
{
	return (int16_t)((int32_t)sample * scale / 32768);
}

static int16_t generated_echo(const struct generated_line *line)
{
	int32_t echo = q15_scale(line_at_age(line, NEAR_DELAY), 11000);

	if (echo > INT16_MAX)
		return INT16_MAX;
	if (echo < INT16_MIN)
		return INT16_MIN;
	return (int16_t)echo;
}

static bool step_canceller(
	struct v34_echo_canceller *canceller,
	struct generated_line *line, int16_t local, int16_t remote,
	int16_t *cleaned)
{
	int32_t received = (int32_t)remote + generated_echo(line);
	int16_t bounded = received > INT16_MAX ? INT16_MAX :
			  received < INT16_MIN ? INT16_MIN : (int16_t)received;

	if (v34_echo_canceller_predict(canceller, bounded, cleaned) !=
		    V34_ECHO_CANCELLER_OK ||
	    v34_echo_canceller_advance(
		    canceller, bounded, *cleaned, local) !=
		    V34_ECHO_CANCELLER_OK)
		return false;
	line_push(line, local);
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_echo_canceller canceller;
	struct v34_echo_canceller saved;
	int16_t output = 1234;

	CHECK(v34_echo_canceller_init(NULL) ==
		      V34_ECHO_CANCELLER_INVALID_ARGUMENT,
	      "null owner initialized");
	memset(&canceller, 0xa5, sizeof(canceller));
	CHECK(v34_echo_canceller_init(&canceller) ==
		      V34_ECHO_CANCELLER_OK &&
		      v34_echo_canceller_is_valid(&canceller) &&
		      v34_echo_canceller_is_adapting(&canceller),
	      "ordinary owner did not initialize");
	memset(&saved, 0, sizeof(saved));
	saved.adapting = true;
	CHECK(memcmp(&canceller, &saved, sizeof(saved)) == 0,
	      "initialization did not clear the complete owner");
	CHECK(v34_echo_canceller_predict(&canceller, 17, NULL) ==
		      V34_ECHO_CANCELLER_INVALID_ARGUMENT,
	      "null prediction output accepted");
	CHECK(v34_echo_canceller_predict(
		      &canceller, 17, (int16_t *)&canceller) ==
		      V34_ECHO_CANCELLER_INVALID_ARGUMENT,
	      "owner/output overlap accepted");
	CHECK(v34_echo_canceller_predict(&canceller, 17, &output) ==
		      V34_ECHO_CANCELLER_OK && output == 17,
	      "initial prediction changed clean input");
	saved = canceller;
	CHECK(v34_echo_canceller_advance(&canceller, 17, 18, 91) ==
		      V34_ECHO_CANCELLER_SEQUENCE_MISMATCH &&
		      memcmp(&canceller, &saved, sizeof(saved)) == 0,
	      "mismatched prediction changed owner");
	CHECK(v34_echo_canceller_advance(&canceller, 17, output, 91) ==
		      V34_ECHO_CANCELLER_OK &&
		      canceller.reference_fill == 1,
	      "matching prediction did not advance");
	CHECK(v34_echo_canceller_freeze(&canceller) ==
		      V34_ECHO_CANCELLER_OK &&
		      !v34_echo_canceller_is_adapting(&canceller) &&
		      !canceller.filtering,
	      "owner did not freeze");
	canceller.reference_write = V34_ECHO_TAP_COUNT;
	CHECK(!v34_echo_canceller_is_valid(&canceller) &&
	      v34_echo_canceller_predict(&canceller, 0, &output) ==
		      V34_ECHO_CANCELLER_INVALID_STATE &&
	      output == 17,
	      "corrupt owner was accepted or changed output");
	CHECK(v34_echo_canceller_init(&canceller) ==
		      V34_ECHO_CANCELLER_OK &&
	      v34_echo_canceller_is_valid(&canceller) &&
	      memcmp(&canceller, &saved, sizeof(saved)) == 0,
	      "reset retained echo history or frozen state");
	return true;
}

static bool test_prediction_purity(void)
{
	static const uint32_t clocks[] = { 0, 143, 2024, 17999, UINT32_MAX };
	static const struct {
		int16_t received, residual;
	} cases[] = {
		{ INT16_MIN, 31744 }, /* Residual wraps rather than saturating. */
		{ -1000, -2024 },
		{ 0, -1024 },
		{ INT16_MAX, 31743 },
	};

	for (size_t clock = 0; clock < sizeof(clocks) / sizeof(clocks[0]); ++clock) {
		for (unsigned flags = 0; flags < 4; ++flags) {
			for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
				struct v34_echo_canceller canceller, saved;
				int16_t output;

				CHECK(v34_echo_canceller_init(&canceller) == V34_ECHO_CANCELLER_OK,
				      "prediction owner did not initialize");
				canceller.reference_write = canceller.reference_fill = 1;
				canceller.references[0] = 8192;
				canceller.coefficients[0] = -2048;
				canceller.residues[0] = 123;
				canceller.training_started = true;
				canceller.filtering = (flags & 1u) != 0;
				canceller.adapting = (flags & 2u) != 0;
				canceller.sample_count = clocks[clock];
				canceller.alpha = 512;
				saved = canceller;
				int16_t expected = canceller.filtering ?
					cases[i].residual : cases[i].received;
				for (unsigned repeat = 0; repeat < 2; ++repeat)
					CHECK(v34_echo_canceller_predict(&canceller,
						      cases[i].received, &output) == V34_ECHO_CANCELLER_OK &&
					      output == expected && memcmp(&canceller, &saved, sizeof(saved)) == 0,
					      "prediction changed output or owner at %zu/%u/%zu", clock, flags, i);
				CHECK(v34_echo_canceller_advance(&canceller, cases[i].received,
					      expected == INT16_MAX ? INT16_MIN : expected + 1, 16384) ==
					      V34_ECHO_CANCELLER_SEQUENCE_MISMATCH &&
				      memcmp(&canceller, &saved, sizeof(saved)) == 0,
				      "rejected advance changed prediction owner");
				CHECK(v34_echo_canceller_advance(&canceller,
					      cases[i].received, output, 16384) == V34_ECHO_CANCELLER_OK &&
				      canceller.sample_count == clocks[clock] + (saved.adapting ? 1u : 0u) &&
				      canceller.reference_write == 2 && canceller.references[1] == 8192,
				      "matching advance changed the sample or reference clock");
				if (clock == 0 && saved.adapting && cases[i].received == 0)
					CHECK(canceller.coefficients[0] == -2056 && canceller.residues[0] == 123,
					      "adaptation failed to use the residual while filtering was %u", flags & 1u);
			}
		}
	}
	return true;
}

static bool test_near_cancellation(void)
{
	struct v34_echo_canceller canceller;
	struct generated_line line = { 0 };
	uint32_t local_state = UINT32_C(0x42519554);
	uint32_t remote_state = UINT32_C(0x63014994);
	uint64_t echo_energy = 0;
	uint64_t residual_energy = 0;

	CHECK(v34_echo_canceller_init(&canceller) ==
		      V34_ECHO_CANCELLER_OK,
	      "training owner did not initialize");
	for (size_t sample = 0; sample < TRAIN_SAMPLES; ++sample) {
		int16_t local = generated_sample(&local_state, 7000);
		int16_t cleaned;

		CHECK(step_canceller(
			      &canceller, &line, local, 0, &cleaned),
		      "training failed at sample %zu", sample);
	}
	CHECK(canceller.alpha != 0,
	      "adaptive scales were not established");
	CHECK(v34_echo_canceller_freeze(&canceller) ==
		      V34_ECHO_CANCELLER_OK && canceller.filtering,
	      "trained owner did not freeze");
	for (size_t sample = 0; sample < VERIFY_SAMPLES; ++sample) {
		int16_t local = generated_sample(&local_state, 7000);
		int16_t remote = generated_sample(&remote_state, 900);
		int16_t echo = generated_echo(&line);
		int16_t cleaned;
		int32_t error;

		CHECK(step_canceller(
			      &canceller, &line, local, remote, &cleaned),
		      "frozen cancellation failed at sample %zu", sample);
		error = (int32_t)cleaned - remote;
		echo_energy += (uint64_t)((int32_t)echo * echo);
		residual_energy += (uint64_t)((int64_t)error * error);
	}
	CHECK(residual_energy * 100u < echo_energy,
	      "echo suppression too small: residual=%" PRIu64
	      " source=%" PRIu64,
	      residual_energy, echo_energy);
	return true;
}

static bool test_rounding_boundaries(void)
{
	static const struct {
		int16_t alpha, received;
		uint16_t coefficient, residue;
	} cases[] = {
		{ 1, 4096, 0, 1 },       /* Positive half rounds upward. */
		{ 1, -4096, 0, 0 },      /* Negative half rounds upward. */
		{ 16384, 16384, UINT16_MAX, 0x8000 }, /* Wrap, not clamp. */
		{ INT16_MIN, INT16_MIN, 0, 0 },      /* Unsigned doubling. */
	};
	struct v34_echo_canceller canceller;
	int16_t output;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		CHECK(v34_echo_canceller_init(&canceller) == V34_ECHO_CANCELLER_OK,
		      "rounding owner did not initialize");
		canceller.references[0] = 1;
		canceller.reference_write = canceller.reference_fill = 1;
		canceller.training_started = true;
		canceller.alpha = cases[i].alpha;
		CHECK(v34_echo_canceller_predict(
			      &canceller, cases[i].received, &output) == V34_ECHO_CANCELLER_OK &&
		      output == cases[i].received &&
		      v34_echo_canceller_advance(&canceller, cases[i].received, output, 0) ==
			      V34_ECHO_CANCELLER_OK &&
		      (uint16_t)canceller.coefficients[0] == cases[i].coefficient &&
		      canceller.residues[0] == cases[i].residue,
		      "adaptation rounding case %zu changed", i);
	}
	for (int sign = -1; sign <= 1; sign += 2) {
		CHECK(v34_echo_canceller_init(&canceller) == V34_ECHO_CANCELLER_OK,
		      "smoothing owner did not initialize");
		canceller.training_started = true;
		canceller.sample_count = 2024;
		canceller.alpha = (int16_t)(sign * 4096);
		CHECK(v34_echo_canceller_advance(&canceller, 0, 0, 0) ==
			      V34_ECHO_CANCELLER_OK &&
		      canceller.alpha == (sign > 0 ? 4076 : -4075),
		      "smoothing half tie changed for sign %d", sign);
	}
	return true;
}

static bool test_energy_normalization(void)
{
	static const struct {
		size_t count;
		int16_t reference, tail, alpha;
	} cases[] = {
		{ 144, 0, 0, 123 },       /* Silence retains the previous scale. */
		{ 1, 8, 0, -13 },
		{ 1, 16, 0, -3 },
		{ 1, 32, 0, -1 },
		{ 144, 3500, 0, -1994 },
		{ 144, -16384, 0, -90 }, /* Maximum ordinary half-amplitude energy. */
		/* Full-range stored references are also accepted owner states. */
		{ 63, INT16_MIN, 0, -52 },
		{ 65, INT16_MIN, 0, -3277 },
		{ 128, INT16_MIN, 0, 123 }, /* Wrapped zero retains the scale. */
		{ 144, INT16_MIN, 0, -205 },
		{ 127, INT16_MIN, INT16_MAX, 0 }, /* Rounded divisor wraps to zero. */
		{ 64, INT16_MIN, 0, 0 }, /* 0x80000000 has no normalizable bits. */
	};
	struct v34_echo_canceller canceller, saved;
	int16_t output;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		CHECK(v34_echo_canceller_init(&canceller) == V34_ECHO_CANCELLER_OK,
		      "energy owner did not initialize");
		canceller.reference_fill = V34_ECHO_TAP_COUNT;
		canceller.training_started = true;
		canceller.sample_count = V34_ECHO_TAP_COUNT - 1u;
		canceller.alpha = 123;
		for (size_t tap = 0; tap < cases[i].count; ++tap)
			canceller.references[tap] = cases[i].reference;
		if (cases[i].tail != 0)
			canceller.references[cases[i].count] = cases[i].tail;
		for (size_t tap = 0; tap < V34_ECHO_TAP_COUNT; ++tap)
			canceller.residues[tap] = 0x1234;
		saved = canceller;
		CHECK(v34_echo_canceller_predict(&canceller, 345, &output) ==
			      V34_ECHO_CANCELLER_OK && output == 345 &&
		      memcmp(&canceller, &saved, sizeof(saved)) == 0,
		      "energy case %zu changed pure prediction", i);
		CHECK(v34_echo_canceller_advance(&canceller, 345, 346, -2048) ==
			      V34_ECHO_CANCELLER_SEQUENCE_MISMATCH &&
		      memcmp(&canceller, &saved, sizeof(saved)) == 0,
		      "energy case %zu changed rejected advance", i);
		CHECK(v34_echo_canceller_advance(&canceller, 345, output, -2048) ==
			      V34_ECHO_CANCELLER_OK &&
		      canceller.alpha == cases[i].alpha &&
		      canceller.sample_count == V34_ECHO_TAP_COUNT &&
		      canceller.reference_write == 1 &&
		      canceller.reference_fill == V34_ECHO_TAP_COUNT &&
		      canceller.references[0] == -1024,
		      "energy case %zu did not normalize and advance", i);
		if (cases[i].alpha == 0)
			CHECK(memcmp(canceller.coefficients, saved.coefficients,
				     sizeof(saved.coefficients)) == 0 &&
			      memcmp(canceller.residues, saved.residues,
				     sizeof(saved.residues)) == 0,
			      "unnormalizable energy updated coefficients");
	}
	return true;
}

int main(void)
{
	if (!test_validation_and_transactionality() || !test_prediction_purity() ||
	    !test_near_cancellation() ||
	    !test_rounding_boundaries() || !test_energy_normalization())
		return 1;
	puts("V.34 echo canceller tests passed");
	return 0;
}
