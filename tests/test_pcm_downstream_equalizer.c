// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_equalizer.h>

#include "test.h"

#include <math.h>
#include <string.h>

static int16_t decode(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMU ? g711_decode_pcmu(octet) : g711_decode_pcma(octet);
}

static uint8_t encode(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ? g711_encode_pcmu(sample) : g711_encode_pcma(sample);
}

static bool test_identity(void)
{
	struct pcm_downstream_equalizer equalizer = { 0 };
	uint8_t decision;

	for (unsigned code = 0; code < 256; ++code)
		CHECK(pcm_downstream_equalizer_receive(&equalizer, (uint8_t)code, &decision) &&
		      decision == code, "inactive equalizer changed code %u", code);
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		int16_t level;
		CHECK(g711_ucode_sample(law, 95, true, &level) &&
		      pcm_downstream_equalizer_init(&equalizer, law, level), "identity init failed");
		for (unsigned n = 0; n < 300; ++n)
			CHECK(pcm_downstream_equalizer_train(&equalizer,
				encode(law, n % 6 < 3 ? level : -level)), "identity training failed");
		struct pcm_downstream_equalizer trained = equalizer;
		for (unsigned code = 0; code < 256; ++code)
			CHECK(pcm_downstream_equalizer_receive(&equalizer, (uint8_t)code, &decision) &&
			      decision == code, "law %u identity changed code %u to %u", law, code, decision);
		CHECK(memcmp(equalizer.filter.coefficients, trained.filter.coefficients,
			sizeof(equalizer.filter.coefficients)) == 0, "receive adapted held taps");
	}
	return true;
}

struct channel {
	int16_t previous, second;
};

static uint8_t channel_receive(struct channel *channel, enum g711_law law, int16_t sample)
{
	double shaped = .90 * sample + .08 * channel->previous + .02 * channel->second;
	channel->second = channel->previous;
	channel->previous = sample;
	return encode(law, (int16_t)lround(shaped));
}

static bool test_filtered_channel(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_downstream_equalizer equalizer;
		struct channel channel = { 0 };
		int16_t level, levels[8];
		uint32_t random = 123;
		double before_error = 0, after_error = 0;
		unsigned before_wrong = 0, after_wrong = 0;

		CHECK(g711_ucode_sample(law, 95, true, &level) &&
		      pcm_downstream_equalizer_init(&equalizer, law, level), "filtered init failed");
		for (unsigned n = 0; n < 512; ++n) {
			/* Ri/Ri-bar sign reversal, not a known data payload. */
			bool positive = (n % 6 < 3) != (n >= 480);
			CHECK(pcm_downstream_equalizer_train(&equalizer,
				channel_receive(&channel, law, positive ? level : -level)),
			      "law %u filtered training stopped at %u", law, n);
		}
		for (unsigned k = 0; k < 8; ++k)
			CHECK(g711_ucode_sample(law, (uint8_t)(40 + 8 * k), true, &levels[k]),
			      "test level failed");
		struct pcm_downstream_equalizer trained = equalizer;
		for (unsigned n = 0; n < 4096; ++n) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int16_t target = levels[(random >> 24) & 7u];
			if (random & UINT32_C(0x80000000))
				target = -target;
			uint8_t received = channel_receive(&channel, law, target), decision;
			CHECK(pcm_downstream_equalizer_receive(&equalizer, received, &decision),
			      "law %u filtered receive failed", law);
			double before = decode(law, received), after = decode(law, decision);
			before_error += (before - target) * (before - target);
			after_error += (after - target) * (after - target);
			/* Slice a fixed sparse constellation, independent of training. */
			double before_best = INFINITY, after_best = INFINITY;
			int16_t before_decision = 0, after_decision = 0;
			for (unsigned k = 0; k < 16; ++k) {
				int16_t candidate = k < 8 ? levels[k] : -levels[k - 8];
				if (fabs(before - candidate) < before_best) {
					before_best = fabs(before - candidate);
					before_decision = candidate;
				}
				if (fabs(after - candidate) < after_best) {
					after_best = fabs(after - candidate);
					after_decision = candidate;
				}
			}
			before_wrong += before_decision != target;
			after_wrong += after_decision != target;
		}
		CHECK(after_error < before_error / 16 && before_wrong > 100 && after_wrong == 0,
		      "law %u residual %.1f/%.1f wrong %u/%u", law, after_error, before_error,
		      after_wrong, before_wrong);
		CHECK(memcmp(equalizer.filter.coefficients, trained.filter.coefficients,
			sizeof(equalizer.filter.coefficients)) == 0, "data changed trained taps");
	}
	return true;
}

static bool test_validation(void)
{
	struct pcm_downstream_equalizer equalizer = { 0 }, before = equalizer;
	uint8_t decision = 0xa5;

	CHECK(!pcm_downstream_equalizer_train(&equalizer, 0) &&
	      !pcm_downstream_equalizer_init(&equalizer, (enum g711_law)99, 100) &&
	      !pcm_downstream_equalizer_init(&equalizer, G711_LAW_PCMU, 0) &&
	      !pcm_downstream_equalizer_init(&equalizer, G711_LAW_PCMU, -1) &&
	      memcmp(&equalizer, &before, sizeof(equalizer)) == 0, "invalid init changed state");
	CHECK(!pcm_downstream_equalizer_is_valid(NULL) &&
	      !pcm_downstream_equalizer_init(NULL, G711_LAW_PCMU, 100) &&
	      !pcm_downstream_equalizer_train(NULL, 0) &&
	      !pcm_downstream_equalizer_receive(NULL, 0, &decision) && decision == 0xa5,
	      "null instance accepted or changed output");
	CHECK(pcm_downstream_equalizer_init(&equalizer, G711_LAW_PCMU, 7932), "valid init failed");
	before = equalizer;
	CHECK(!pcm_downstream_equalizer_receive(&equalizer, 0, NULL) &&
	      !pcm_downstream_equalizer_receive(&equalizer, 0, (uint8_t *)&equalizer) &&
	      !pcm_downstream_equalizer_train(&equalizer, 0xff) &&
	      memcmp(&equalizer, &before, sizeof(equalizer)) == 0, "invalid input changed state");
	equalizer.filter.coefficients[0].in_phase = NAN;
	before = equalizer;
	CHECK(!pcm_downstream_equalizer_receive(&equalizer, 0, &decision) &&
	      !pcm_downstream_equalizer_train(&equalizer, 0) && decision == 0xa5 &&
	      memcmp(&equalizer, &before, sizeof(equalizer)) == 0, "nonfinite state accepted");
	return true;
}

int main(void)
{
	return test_identity() && test_filtered_channel() && test_validation() ? 0 : 1;
}
