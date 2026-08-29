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
	PATH_CAPACITY = 1024,
	NEAR_DELAY = 32,
	FAR_DELAY = 500,
	FAR_TAP_DELAY = 33,
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
	int32_t echo =
		q15_scale(line_at_age(line, NEAR_DELAY), 11000) +
		q15_scale(
			line_at_age(line, FAR_DELAY + FAR_TAP_DELAY), 6000);

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

	CHECK(v34_echo_canceller_init(NULL, FAR_DELAY) ==
		      V34_ECHO_CANCELLER_INVALID_ARGUMENT,
	      "null owner initialized");
	CHECK(v34_echo_canceller_init(&canceller, FAR_DELAY) ==
		      V34_ECHO_CANCELLER_OK &&
		      v34_echo_canceller_is_valid(&canceller) &&
		      v34_echo_canceller_is_adapting(&canceller) &&
		      canceller.far_delay_samples == FAR_DELAY,
	      "ordinary owner did not initialize");
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
	CHECK(v34_echo_canceller_init(
		      &canceller, V34_ECHO_MAX_FAR_DELAY_SAMPLES) ==
		      V34_ECHO_CANCELLER_OK &&
		      canceller.far_delay_samples ==
			      V34_ECHO_MAX_FAR_DELAY_SAMPLES,
	      "maximum far delay was not retained");
	CHECK(v34_echo_canceller_init(
		      &canceller, V34_ECHO_MAX_FAR_DELAY_SAMPLES + 1u) ==
		      V34_ECHO_CANCELLER_OK &&
		      canceller.far_delay_samples == 0,
	      "long path did not fall back to near cancellation");
	canceller.reference_write = V34_ECHO_REFERENCE_SAMPLES;
	CHECK(!v34_echo_canceller_is_valid(&canceller) &&
	      v34_echo_canceller_predict(&canceller, 0, &output) ==
		      V34_ECHO_CANCELLER_INVALID_STATE &&
	      output == 17,
	      "corrupt owner was accepted or changed output");
	return true;
}

static bool test_near_and_far_cancellation(void)
{
	struct v34_echo_canceller canceller;
	struct generated_line line = { 0 };
	uint32_t local_state = UINT32_C(0x42519554);
	uint32_t remote_state = UINT32_C(0x63014994);
	uint64_t echo_energy = 0;
	uint64_t residual_energy = 0;

	CHECK(v34_echo_canceller_init(&canceller, FAR_DELAY) ==
		      V34_ECHO_CANCELLER_OK,
	      "training owner did not initialize");
	for (size_t sample = 0; sample < TRAIN_SAMPLES; ++sample) {
		int16_t local = generated_sample(&local_state, 7000);
		int16_t cleaned;

		CHECK(step_canceller(
			      &canceller, &line, local, 0, &cleaned),
		      "training failed at sample %zu", sample);
	}
	CHECK(canceller.near_filter.alpha != 0 &&
	      canceller.far_filter.alpha != 0,
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

int main(void)
{
	CHECK(test_validation_and_transactionality(),
	      "validation/transactionality failed");
	CHECK(test_near_and_far_cancellation(),
	      "generated near/far cancellation failed");
	puts("V.34 echo canceller tests passed");
	return 0;
}
