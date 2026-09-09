// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpd_channel.h>
#include <v92_cpd_direct.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	LEVEL = 4096,
	PAYLOAD_FRAMES = 120,
	FRAMES = V92_B1U_FRAMES + PAYLOAD_FRAMES + V92_PCM_RX_DELAY_FRAMES,
	SYMBOLS = (FRAMES + 6) * V92_PCM_MAPPING_FRAME_SYMBOLS,
};

static uint32_t random_word(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return *state;
}

static int16_t quantize(enum g711_law law, double value)
{
	int16_t sample = (int16_t)lround(fmax(INT16_MIN, fmin(INT16_MAX, value)));

	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static __uint128_t distance(__uint128_t left, __uint128_t right)
{
	return left >= right ? left - right : right - left;
}

static bool test_channel(enum g711_law law, double main, double post, uint8_t rate)
{
	struct v92_upstream_channel channel = { 0 };
	struct v92_cpd_parameters cpd, original;
	struct v92_upstream_plan plan;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_rx receiver;
	uint8_t payload[FRAMES + 6][V92_PCM_MAPPING_MAX_BITS];
	int16_t line[SYMBOLS];
	uint32_t random = 0x35791bdu;
	uint64_t energy = 0u;
	int16_t previous = 0;

	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
		int16_t expected = (int16_t)((int)(random_word(&random) % 8192u) - 4096);

		CHECK(v92_upstream_channel_sample(&channel, expected,
			quantize(law, main * expected + post * previous)), "measurement failed");
		previous = expected;
	}
	CHECK(v92_cpd_direct_select(&cpd, law, rate, LEVEL, false) == V92_CPD_OK, "initial CPd failed");
	if (rate == 13u)
		CHECK(cpd.constellation[0].count == 32u, "direct 40k did not omit outer points");
	original = cpd;
	CHECK(v92_cpd_channel_design(&cpd, &channel, law, LEVEL, &plan),
		"channel design failed: law=%u rate=%u H=(%g,%g)", law, rate, main, post);
	CHECK(plan.observation == V92_PCM_FRAME_CONSTELLATION && plan.delay == 60u &&
		v92_upstream_plan_is_valid(&plan), "invalid receive plan");
	original.four_times_gain_q0_16 = cpd.four_times_gain_q0_16;
	memcpy(original.filter, cpd.filter, sizeof(cpd.filter));
	if (original.trellis == V92_CPD_TRELLIS_32_STATE && original.constellation[0].count == 32u)
		original.constellation[0].count = 64u;
	if (original.constellation[0].count == 32u || original.constellation[0].count == 64u)
		for (unsigned point = 0; point < original.constellation[0].count; ++point)
			CHECK(g711_ucode_sample(law, (uint8_t)(2u * point + 1u), true,
				&original.constellation[0].positive_linear[point]), "expected spacing failed");
	CHECK(memcmp(&original, &cpd, sizeof(cpd)) == 0,
		"channel design changed unrelated CPd selection");
	CHECK(cpd.filter[V92_CPD_PRECODER_FEED_FORWARD].count == 0u &&
		cpd.filter[V92_CPD_PRECODER_FEEDBACK].count == 90u &&
		cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count == 85u &&
		cpd.filter[V92_CPD_PREFILTER_FEEDBACK].count == 0u,
		"design changed minimum-capability filter geometry");
	{
		uint8_t wire[V92_CPD_MAX_FRAME_BYTES];
		struct v92_cpd_parameters decoded;
		size_t count;

		CHECK(v92_cpd_encode(&cpd, 17u, wire, sizeof(wire), &count) == V92_CPD_OK &&
			v92_cpd_decode(wire, sizeof(wire), count, 17u, &decoded) == V92_CPD_OK &&
			memcmp(&decoded, &cpd, sizeof(cpd)) == 0, "designed CPd wire round trip failed");
	}
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
		v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
		v92_pcm_frame_rx_init_constellation(&receiver, &mapping) == V92_PCM_FRAME_OK,
		"paired mapping failed");
	previous = 0;
	for (unsigned frame = 0; frame < FRAMES + 6; ++frame) {
		struct v92_pcm_frame encoded;

		for (unsigned bit = 0; bit < mapping.data_bits_per_frame; ++bit)
			payload[frame][bit] = frame < V92_B1U_FRAMES ? 1u : (uint8_t)(random_word(&random) & 1u);
		CHECK(v92_pcm_frame_encode(&transmitter, payload[frame], mapping.data_bits_per_frame,
			&encoded) == V92_PCM_FRAME_OK, "frame %u encode failed", frame);
		for (unsigned symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int16_t transmitted;

			if (frame < V92_B1U_FRAMES)
				energy += (uint64_t)((int64_t)encoded.v[symbol] * encoded.v[symbol]);
			CHECK(v92_pcm_frame_scale(&mapping, LEVEL, encoded.v[symbol], &transmitted) ==
				V92_PCM_FRAME_OK, "frame %u scale failed", frame);
			line[frame * V92_PCM_MAPPING_FRAME_SYMBOLS + symbol] =
				quantize(law, main * transmitted + post * previous);
			previous = transmitted;
		}
	}
	/* Check unit mean-square power independently of the receive gain: a
	 * wrong 4G could otherwise cancel out in the paired coordinate plan. */
	__uint128_t target = (__uint128_t)V92_B1U_SYMBOLS << 36;
	uint32_t gain = cpd.four_times_gain_q0_16;
	__uint128_t selected = (__uint128_t)gain * gain * energy;

	CHECK(gain > 1u && gain < UINT16_MAX &&
		distance(selected, target) <= distance((__uint128_t)(gain - 1u) * (gain - 1u) * energy, target) &&
		distance(selected, target) <= distance((__uint128_t)(gain + 1u) * (gain + 1u) * energy, target),
		"4G did not normalize the actual B1u filter output");
	unsigned delivered = 0;
	for (unsigned frame = 0; frame < FRAMES; ++frame) {
		int16_t observed[V92_PCM_MAPPING_FRAME_SYMBOLS];
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame_decision decision;

		for (unsigned symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			double value = line[plan.delay + frame * V92_PCM_MAPPING_FRAME_SYMBOLS + symbol] * plan.gain;

			CHECK(value >= INT16_MIN && value <= INT16_MAX, "observation overflowed");
			observed[symbol] = (int16_t)lround(value);
		}
		CHECK(v92_pcm_frame_decode(&receiver, observed, frame < V92_B1U_FRAMES, bits,
			mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
			"frame %u decode failed", frame);
		if (frame >= V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			CHECK(decision.bits_ready && !decision.ambiguous &&
				memcmp(bits, payload[frame - V92_PCM_RX_DELAY_FRAMES], mapping.data_bits_per_frame) == 0,
				"payload failed: law=%u rate=%u H=(%g,%g) frame=%u", law, rate, main, post, frame);
			++delivered;
		}
	}
	CHECK(delivered == PAYLOAD_FRAMES, "wrong payload count");
	return true;
}

static bool test_rejections(void)
{
	struct v92_upstream_channel channel = { 0 };
	struct v92_cpd_parameters cpd, saved;
	struct v92_upstream_plan plan = { 0 }, saved_plan = plan;

	CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMU, 1u, false) == V92_CPD_OK, "CPd failed");
	saved = cpd;
	CHECK(!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, &plan),
		"incomplete channel accepted");
	channel.symbols = V92_UPSTREAM_CHANNEL_SYMBOLS;
	CHECK(!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, &plan), "silence accepted");
	memset(&channel, 0, sizeof(channel));
	uint32_t expected_random = 0x293729du, observed_random = 0x794531bu;

	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i)
		CHECK(v92_upstream_channel_sample(&channel,
			(int16_t)((int)(random_word(&expected_random) % 8192u) - 4096),
			(int)(random_word(&observed_random) % 8192u) - 4096), "noise fixture failed");
	CHECK(!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, &plan),
		"uncorrelated full-power observations accepted");
	CHECK(!v92_cpd_channel_design(NULL, &channel, G711_LAW_PCMU, LEVEL, &plan) &&
		!v92_cpd_channel_design(&cpd, NULL, G711_LAW_PCMU, LEVEL, &plan) &&
		!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, NULL) &&
		!v92_cpd_channel_design(&cpd, &channel, (enum g711_law)-1, LEVEL, &plan) &&
		!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, 0, &plan) &&
		!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, -1, &plan) &&
		!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, (void *)&cpd) &&
		!v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, (void *)&channel),
		"invalid arguments accepted");
	CHECK(memcmp(&cpd, &saved, sizeof(cpd)) == 0 && memcmp(&plan, &saved_plan, sizeof(plan)) == 0,
		"rejected design changed outputs");
	return true;
}

static bool test_receive_plan(void)
{
	struct v92_upstream_plan plan = { 0 };

	CHECK(v92_upstream_plan_is_valid(&plan) && !v92_upstream_plan_is_valid(NULL), "default plan failed");
	plan.gain = 1.0;
	CHECK(!v92_upstream_plan_is_valid(&plan), "filter-output plan accepted unused gain");
	plan.observation = V92_PCM_FRAME_CONSTELLATION;
	plan.delay = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	CHECK(v92_upstream_plan_is_valid(&plan), "recovered plan failed");
	--plan.delay;
	CHECK(!v92_upstream_plan_is_valid(&plan), "plan accepted input delay before E2u decision");
	plan.delay = V92_B1U_SYMBOLS;
	CHECK(!v92_upstream_plan_is_valid(&plan), "plan accepted delay beyond B1u");
	plan.delay = V92_UPSTREAM_CHANNEL_DECISION_DELAY;
	plan.gain = NAN;
	CHECK(!v92_upstream_plan_is_valid(&plan), "plan accepted NaN gain");
	plan.gain = INFINITY;
	CHECK(!v92_upstream_plan_is_valid(&plan), "plan accepted infinite gain");
	plan.gain = 0.0;
	CHECK(!v92_upstream_plan_is_valid(&plan), "plan accepted zero gain");
	return true;
}

int main(void)
{
	if (!test_rejections() || !test_receive_plan())
		return 1;
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		/* Both laws and trellis families: 24, 28, 32, 36 and 40 kbit/s. */
		for (uint8_t rate = 1u; rate <= 13u; rate += 3u)
			if (!test_channel((enum g711_law)law, 1.0, 0.0, rate) ||
				!test_channel((enum g711_law)law, 0.55, 0.43, rate) ||
				!test_channel((enum g711_law)law, 0.8, -0.25, rate))
				return 1;
	return 0;
}
