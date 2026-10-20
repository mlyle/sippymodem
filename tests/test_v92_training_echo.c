// SPDX-License-Identifier: BSD-3-Clause

#include <v92_training_echo.h>
#include <sample_math.h>

#include "test.h"

#include <stdlib.h>
#include <string.h>

enum stimulus { SHIFT, NO_ECHO, UNRELATED, WRONG_REFERENCE, MISSING, CLIPPED, FAILED };

static int16_t noise(unsigned n, unsigned salt, unsigned span)
{
	uint32_t value = n + salt;
	value ^= value >> 15;
	value *= UINT32_C(0x2c1b3c6d);
	value ^= value >> 12;
	value *= UINT32_C(0x297a2d39);
	value ^= value >> 15;
	return (int16_t)((int32_t)(value % (2u * span + 1u)) - (int32_t)span);
}

static int16_t transmit(unsigned n)
{
	return noise(n, UINT32_C(0x6d2b79f5), 8000);
}

static bool qualify(struct pcm_host_echo *echo)
{
	CHECK(pcm_host_echo_init(echo) == PCM_HOST_ECHO_OK, "echo initialization");
	for (unsigned i = 0; i < 4096; ++i)
		CHECK(pcm_echo_reference_push(&echo->reference, transmit(i)) == PCM_ECHO_REFERENCE_OK,
		      "reference history");
	echo->train_samples = PCM_HOST_ECHO_TRAIN_MIN_SAMPLES;
	echo->train_peak = PCM_HOST_ECHO_READY_MIN_PEAK;
	echo->filter_ready = true;
	echo->coefficients[PCM_HOST_ECHO_FILTER_PRE_SAMPLES] = 0.0625f;
	return pcm_host_echo_is_valid(echo);
}

static bool run(enum stimulus stimulus, int shift, unsigned configuration)
{
	struct pcm_host_echo *echo = calloc(1, sizeof(*echo));
	struct v92_pcm_phase4_trn2u_line_rx *line = calloc(1, sizeof(*line));
	struct v92_training_echo timing = { 0 };
	CHECK(echo && line && qualify(echo), "fixture allocation/qualification");
	line->polarity = configuration & 2u ? -1 : 1;
	CHECK(v92_pcm_phase4_trn2u_init(&line->reference, configuration & 1u ?
		V92_CONSTELLATION_8_POINT : V92_CONSTELLATION_4_POINT, 1, 1100) ==
		V92_PCM_PHASE4_TRN2U_OK, "training initialization");
	struct v92_pcm_phase4_trn2u source = line->reference;
	if (stimulus == WRONG_REFERENCE)
		for (unsigned i = 0; i < 83; ++i) {
			int16_t ignored;
			CHECK(v92_pcm_phase4_trn2u_next(&source, &ignored) == V92_PCM_PHASE4_TRN2U_OK,
			      "source displacement");
		}
	unsigned original_delay = echo->window_delay_samples, events = 0, observed = 0;
	int16_t history[5] = { 0 };
	for (unsigned n = 0; n < 12000; ++n) {
		int16_t generated, cleaned, ignored;
		CHECK(v92_pcm_phase4_trn2u_next(&source, &generated) == V92_PCM_PHASE4_TRN2U_OK,
		      "source sample");
		for (unsigned k = 4; k; --k)
			history[k] = history[k - 1];
		history[0] = generated * line->polarity;
		double peer = .7 * history[0] - .5 * history[2] + .2 * history[4] + 130;
		int change = n >= 8000 && stimulus == SHIFT ? shift : 0;
		unsigned returned_index = (unsigned)((int)(4096 + n - original_delay -
			PCM_HOST_ECHO_FILTER_PRE_SAMPLES) - change);
		int returned = stimulus == NO_ECHO ? 0 : stimulus == UNRELATED ?
			noise(n, 17719, 500) : transmit(returned_index) / 16;
		int16_t raw = sample_round_s16(peer + returned + noise(n, 7919, 15));
		if (n == 7800 && stimulus == CLIPPED)
			raw = INT16_MAX;
		if (n == 7800 && stimulus == FAILED)
			line->failed = true;
		CHECK(pcm_host_echo_predict(echo, PCM_HOST_ECHO_FILTER, raw, &cleaned) ==
			PCM_HOST_ECHO_OK, "echo prediction");
		struct v92_pcm_phase4_trn2u reference = line->reference;
		size_t pushed = echo->reference.write;
		float coefficient = echo->coefficients[PCM_HOST_ECHO_FILTER_PRE_SAMPLES];
		int delay_change = 999;
		enum v92_training_echo_result result = v92_training_echo_observe(&timing,
			echo, line, PCM_HOST_ECHO_FILTER, raw, cleaned,
			!(n == 7800 && stimulus == MISSING), &delay_change);
		CHECK(result != V92_TRAINING_ECHO_ERROR, "monitor failed at %u", n);
		CHECK(memcmp(&reference, &line->reference, sizeof(reference)) == 0 &&
			echo->reference.write == pushed &&
			echo->coefficients[PCM_HOST_ECHO_FILTER_PRE_SAMPLES] == coefficient,
			"observation advanced the receiver, TX history or fitted echo");
		if (result == V92_TRAINING_ECHO_RETIMED) {
			CHECK(stimulus == SHIFT && delay_change == shift && n >= 8000,
			      "unexpected timing event %d at %u", delay_change, n);
			++events;
		} else
			CHECK(delay_change == 0, "unreported delay change");
		if (n == 7800 && stimulus >= MISSING)
			CHECK(!timing.active && !timing.ready && !timing.view, "epoch not cleared");
		bool monitoring = result != V92_TRAINING_ECHO_UNOBSERVED;
		observed += monitoring;
		CHECK((monitoring ? pcm_host_echo_advance_reference(echo, PCM_HOST_ECHO_FILTER,
			raw, transmit(4096 + n)) : pcm_host_echo_advance_predicted(echo,
			PCM_HOST_ECHO_FILTER, raw, cleaned, transmit(4096 + n))) == PCM_HOST_ECHO_OK,
			"reference advance");
		CHECK(echo->reference.write == (pushed + 1) % PCM_ECHO_REFERENCE_HISTORY_SAMPLES, "TX reference count");
		CHECK(v92_pcm_phase4_trn2u_next(&line->reference, &ignored) == V92_PCM_PHASE4_TRN2U_OK,
		      "receiver reference advance");
	}
	unsigned expected = stimulus == SHIFT && shift != 0;
	CHECK(observed > 1000 && events == expected &&
		(int)echo->window_delay_samples - (int)original_delay == (expected ? shift : 0),
		"stimulus %d configuration %u: events=%u expected=%u delay=%d", stimulus,
		configuration, events, expected, (int)echo->window_delay_samples - (int)original_delay);
	free(line);
	free(echo);
	return true;
}

static bool test_boundaries(void)
{
	struct pcm_host_echo *echo = calloc(1, sizeof(*echo));
	struct pcm_host_echo *before = malloc(sizeof(*before));
	struct v92_pcm_phase4_trn2u_line_rx *line = calloc(1, sizeof(*line));
	struct v92_training_echo timing = { 0 };
	CHECK(echo && before && line && qualify(echo), "boundary allocation");
	line->polarity = 1;
	CHECK(v92_pcm_phase4_trn2u_init(&line->reference, V92_CONSTELLATION_4_POINT,
		1, 1100) == V92_PCM_PHASE4_TRN2U_OK, "reference initialization");
	/* Enter the projected view with a completed fit. Clearing it must reset
	 * timing evidence while preserving qualified filters and actual history. */
	timing.active = timing.ready = timing.view = true;
	timing.count = 1024;
	timing.fill = V92_TRAINING_ECHO_LAGS;
	timing.lag[1] = 2;
	timing.lag[2] = 4;
	*before = *echo;
	int change = 99;
	CHECK(v92_training_echo_observe(&timing, echo, NULL, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && change == 0,
		"epoch exit");
	CHECK(!timing.active && !timing.view && !timing.ready &&
		memcmp(before, echo, sizeof(*echo)) == 0, "exit changed qualified echo");
	/* A discontinuity cannot collect a second copy of a symbol. */
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && timing.fill == 1,
		"epoch start");
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && timing.fill == 1,
		"duplicate reference");
	line->reference.emitted_symbols += 2;
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && timing.fill == 1,
		"reference gap");
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, INT16_MIN, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && !timing.active,
		"clipped cleaned sample");
	line->renegotiation = true;
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && !timing.active,
		"short renegotiation");
	line->renegotiation = false;
	for (enum pcm_host_echo_mode mode = PCM_HOST_ECHO_BYPASS;
	     mode <= PCM_HOST_ECHO_FILTER_DATA; ++mode)
		if (mode != PCM_HOST_ECHO_FILTER)
			CHECK(v92_training_echo_observe(&timing, echo, line, mode,
				100, 50, true, &change) == V92_TRAINING_ECHO_UNOBSERVED && !timing.active,
				"unsupported echo mode");
	struct v92_training_echo saved = timing;
	change = 99;
	CHECK(v92_training_echo_observe(NULL, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_ERROR && change == 99,
		"null state");
	CHECK(v92_training_echo_observe(&timing, echo, line, (enum pcm_host_echo_mode)99,
		100, 50, true, &change) == V92_TRAINING_ECHO_ERROR && change == 99 &&
		memcmp(&saved, &timing, sizeof(saved)) == 0, "invalid mode mutation");
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, (int *)&timing.history[0]) == V92_TRAINING_ECHO_ERROR,
		"overlapping output");
	timing.fill = V92_TRAINING_ECHO_LAGS + 1;
	saved = timing;
	CHECK(v92_training_echo_observe(&timing, echo, line, PCM_HOST_ECHO_FILTER,
		100, 50, true, &change) == V92_TRAINING_ECHO_ERROR && change == 99 &&
		memcmp(&saved, &timing, sizeof(saved)) == 0, "invalid history bounds");
	free(line);
	free(before);
	free(echo);
	return true;
}

int main(void)
{
	if (!test_boundaries())
		return 1;
	for (unsigned configuration = 0; configuration < 4; ++configuration) {
		for (int shift = -98; shift <= 98; shift += 98)
			if (!run(SHIFT, shift, configuration))
				return 1;
		for (enum stimulus stimulus = NO_ECHO; stimulus <= FAILED; ++stimulus)
			if (!run(stimulus, 0, configuration))
				return 1;
	}
	return 0;
}
