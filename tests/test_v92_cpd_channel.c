// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpd_channel.h>
#include <v92_cpd_direct.h>

#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
	LEVEL = 4096,
	PAYLOAD_FRAMES = 2000,
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

static bool test_channel(enum g711_law law, double main, double post, double second, uint8_t rate)
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
	int16_t previous = 0, older = 0;
	unsigned payload_frames = rate >= 16u ? PAYLOAD_FRAMES : 120u;
	unsigned frames = V92_B1U_FRAMES + payload_frames + V92_PCM_RX_DELAY_FRAMES;

	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
		int16_t expected = (int16_t)((int)(random_word(&random) % 8192u) - 4096);

		CHECK(v92_upstream_channel_sample(&channel, expected,
			quantize(law, main * expected + post * previous + second * older)), "measurement failed");
		older = previous;
		previous = expected;
	}
	CHECK(v92_cpd_direct_select(&cpd, law, rate, LEVEL, false) == V92_CPD_OK, "initial CPd failed");
	if (rate == 13u)
		CHECK(cpd.constellation[0].count == 32u, "direct 40k did not omit outer points");
	if (rate == 16u)
		CHECK(cpd.constellation[0].count == 27u, "direct 44k did not omit outer points");
	original = cpd;
	CHECK(v92_cpd_channel_design(&cpd, &channel, law, LEVEL, &plan),
		"channel design failed: law=%u rate=%u H=(%g,%g,%g)", law, rate, main, post, second);
	CHECK(plan.observation == V92_PCM_FRAME_CONSTELLATION && plan.delay == 60u &&
		v92_upstream_plan_is_valid(&plan), "invalid receive plan");
	original.four_times_gain_q0_16 = cpd.four_times_gain_q0_16;
	memcpy(original.filter, cpd.filter, sizeof(cpd.filter));
	if (rate >= 16u) {
		static const uint8_t dense_modulus[] = { 27u, 31u, 34u, 39u };
		unsigned int modulus = dense_modulus[rate - 16u];
		__uint128_t capacity = 1u, smaller = 1u;
		__uint128_t required = (__uint128_t)1u << v92_pcm_mapping_data_bits(rate);

		for (size_t symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol) {
			original.modulus[symbol] = symbol % 4u == 3u ? modulus : 2u * modulus;
			CHECK(cpd.modulus[symbol] == original.modulus[symbol],
				"dense rate %u did not balance interval %zu", rate, symbol);
			capacity *= original.modulus[symbol];
			smaller *= symbol % 4u == 3u ? modulus - 1u : 2u * (modulus - 1u);
		}
		CHECK(capacity >= required && smaller < required,
			"dense rate %u did not use minimum balanced capacity", rate);
	}
	if ((original.trellis == V92_CPD_TRELLIS_32_STATE && original.constellation[0].count == 32u) ||
	    (rate == 16u && original.constellation[0].count == 27u))
		original.constellation[0].count = 64u;
	if (original.trellis == V92_CPD_TRELLIS_16_STATE && original.constellation[0].count == 64u) {
		/* Representative values of the generated sinh grid, independently
		 * pinned without repeating the production formula. */
		const struct v92_cpd_constellation *set = &cpd.constellation[0];

		CHECK(set->count == 64u && set->positive_linear[0] == 35 &&
			set->positive_linear[1] == 104 && set->positive_linear[15] == 1252 &&
			set->positive_linear[31] == 3922 && set->positive_linear[47] == 10926 &&
			set->positive_linear[63] == 30000, "dense channel spacing changed");
		original.constellation[0] = *set;
	} else if (original.constellation[0].count == 32u || original.constellation[0].count == 64u) {
		for (unsigned point = 0; point < original.constellation[0].count; ++point)
			CHECK(g711_ucode_sample(law, (uint8_t)(2u * point + 1u), true,
				&original.constellation[0].positive_linear[point]), "expected spacing failed");
	}
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
	previous = older = 0;
	for (unsigned frame = 0; frame < frames + 6; ++frame) {
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
			CHECK(transmitted != INT16_MIN && transmitted != INT16_MAX,
				"transmit clipped: law=%u rate=%u H=(%g,%g) frame=%u symbol=%u",
				law, rate, main, post, frame, symbol);
			line[frame * V92_PCM_MAPPING_FRAME_SYMBOLS + symbol] =
				quantize(law, main * transmitted + post * previous + second * older);
			older = previous;
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
	for (unsigned frame = 0; frame < frames; ++frame) {
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
		if (decision.bits_ready && frame < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES)
			for (unsigned bit = 0; bit < mapping.data_bits_per_frame; ++bit)
				CHECK(bits[bit] == 1u, "B1u failed: law=%u rate=%u frame=%u", law, rate, frame);
		if (frame >= V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			CHECK(decision.bits_ready && !decision.ambiguous &&
				memcmp(bits, payload[frame - V92_PCM_RX_DELAY_FRAMES], mapping.data_bits_per_frame) == 0,
				"payload failed: law=%u rate=%u H=(%g,%g,%g) frame=%u", law, rate, main, post, second, frame);
			++delivered;
		}
	}
	CHECK(delivered == payload_frames, "wrong payload count");
	return true;
}

static bool test_short_channel_selection(void)
{
	static const struct {
		double main, post, second, third, noise;
		unsigned order;
	} cases[] = {
		{ 0.55, 0.43, 0.0, 0.0, 0.0, 1 },
		{ 0.55, 0.43, 0.0, 0.0, 0.003, 1 },
		{ 0.55, 0.43, 0.0, 0.0, 0.01, 1 },
		{ 0.55, 0.43, 0.0, 0.0, 0.02, 0 },
		{ 0.55, 0.43, 0.0, 0.0, 0.04, 0 },
		{ 0.55, 0.43, 0.05, 0.0, 0.0, 2 },
		{ 0.43, 0.55, 0.0, 0.0, 0.0, 0 },
		{ 0.50, 0.50, 0.0, 0.0, 0.0, 0 },
		{ 0.80, -0.25, 0.0, 0.0, 0.0, 1 },
		{ 0.90, 0.08, 0.02, 0.0, 0.0, 2 },
		{ 0.80, -0.15, 0.05, 0.0, 0.003, 2 },
		{ 0.40, 0.60, 0.30, 0.0, 0.0, 2 },
		{ 0.40, 0.60, -0.30, 0.0, 0.0, 0 },
		{ 0.90, 0.08, 0.05, 0.05, 0.0, 0 },
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct v92_upstream_channel channel = { 0 };
		struct v92_cpd_parameters cpd;
		struct v92_upstream_plan plan;
		uint32_t random = 0x12345678u, noise_random = 0x87654321u;
		int16_t previous = 0, second = 0, third = 0;

		for (unsigned n = 0; n < V92_UPSTREAM_CHANNEL_SYMBOLS; ++n) {
			int16_t target = (int16_t)((int)(random_word(&random) % 8192u) - 4096);
			int noise = (int)(random_word(&noise_random) % 8192u) - 4096;
			int32_t input = (int32_t)lround(cases[index].main * target +
				cases[index].post * previous + cases[index].second * second +
				cases[index].third * third + cases[index].noise * noise);

			CHECK(v92_upstream_channel_sample(&channel, target, input), "fit measurement failed");
			third = second;
			second = previous;
			previous = target;
		}
		CHECK(v92_cpd_direct_select(&cpd, G711_LAW_PCMU, 1u, LEVEL, false) == V92_CPD_OK &&
			v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, &plan),
			"fit selection %zu failed to design", index);
		bool simple = cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD]
			.coefficient[V92_UPSTREAM_CHANNEL_DECISION_DELAY] == 24576;

		for (unsigned n = 0; n < V92_UPSTREAM_CHANNEL_Z2_TAPS; ++n)
			if (n != V92_UPSTREAM_CHANNEL_DECISION_DELAY &&
			    cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[n] != 0)
				simple = false;
		for (unsigned n = 2; n < V92_UPSTREAM_CHANNEL_P1_TAPS; ++n)
			if (cpd.filter[V92_CPD_PRECODER_FEEDBACK].coefficient[n] != 0)
				simple = false;
		int first = cpd.filter[V92_CPD_PRECODER_FEEDBACK].coefficient[0];
		int next = cpd.filter[V92_CPD_PRECODER_FEEDBACK].coefficient[1];
		unsigned order = simple ? (next == 0 ? 1u : 2u) : 0u;

		CHECK(order == cases[index].order, "fit selection %zu chose order %u, expected %u",
			index, order, cases[index].order);
		if (simple) {
			CHECK(abs(next) < 16384 && first + next < 16384 && next - first < 16384,
				"quantized short inverse is unstable");
			CHECK(fabs(first / 16384.0 + cases[index].post / cases[index].main) < 0.003 &&
				fabs(next / 16384.0 + cases[index].second / cases[index].main) < 0.003,
				"short inverse did not retain the measured channel coefficients");
		}
	}
	return true;
}

static bool test_quantized_stability(void)
{
	/* Exact moments of white unit-variance input, without sample-rounding
	 * bias. Each real inverse is stable but rounds onto a unit-circle pole. */
	const double channels[][2] = {
		{ 0.0, 65535.0 / 65536.0 },
		{ -0.5, -32767.0 / 65536.0 },
		{ 0.5, -32767.0 / 65536.0 },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(channels); ++i) {
		struct v92_upstream_channel channel = { .symbols = V92_UPSTREAM_CHANNEL_SYMBOLS };
		struct v92_cpd_parameters cpd;
		struct v92_upstream_plan plan;
		double energy = 4294967296.0, b = channels[i][0], c = channels[i][1];

		channel.expected_autocorrelation[0] = (int64_t)energy;
		channel.previous_expected_energy = channel.second_expected_energy = (int64_t)energy;
		channel.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS] = (int64_t)energy;
		channel.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS + 1u] = (int64_t)(energy * b);
		channel.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS + 2u] = (int64_t)(energy * c);
		channel.observed_autocorrelation[0] = (int64_t)(energy * (1.0 + b * b + c * c));
		channel.observed_autocorrelation[1] = (int64_t)(energy * b * (1.0 + c));
		channel.observed_autocorrelation[2] = (int64_t)(energy * c);
		CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMU, 1u, false) == V92_CPD_OK &&
			v92_cpd_channel_design(&cpd, &channel, G711_LAW_PCMU, LEVEL, &plan),
			"stability boundary %zu did not use the general designer", i);
		bool general = false;

		for (unsigned tap = 0; tap < V92_UPSTREAM_CHANNEL_Z2_TAPS; ++tap)
			general |= tap != V92_UPSTREAM_CHANNEL_DECISION_DELAY &&
				cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[tap] != 0;
		CHECK(general, "quantization made a short inverse unstable at boundary %zu", i);
	}
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
	if (!test_rejections() || !test_receive_plan() || !test_short_channel_selection() ||
	    !test_quantized_stability())
		return 1;
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		/* Both laws and trellis families: 24, 28, 32, 36 and 40 kbit/s. */
		for (uint8_t rate = 1u; rate <= 13u; rate += 3u)
			if (!test_channel((enum g711_law)law, 1.0, 0.0, 0.0, rate) ||
				!test_channel((enum g711_law)law, 0.55, 0.43, 0.0, rate) ||
				!test_channel((enum g711_law)law, 0.8, -0.25, 0.0, rate))
				return 1;
		/* Keep the full 48 kbit/s payload through a nontrivial channel,
		 * including wire CPd, B1u normalization and delayed bit delivery. */
		if (!test_channel((enum g711_law)law, 0.9, 0.1, 0.0, 19u) ||
		    !test_channel((enum g711_law)law, 0.55, 0.43, 0.0, 19u))
			return 1;
		/* The old 44k map clipped 95 samples in the unity-channel fixture;
		 * A-law B1u still passed, but the subsequent payload was corrupt.
		 * Also exercise real memory with small, strong and negative echoes. */
		if (!test_channel((enum g711_law)law, 1.0, 0.0, 0.0, 16u) ||
		    !test_channel((enum g711_law)law, 0.98, 0.02, 0.0, 16u) ||
		    !test_channel((enum g711_law)law, 0.55, 0.43, 0.0, 16u) ||
		    !test_channel((enum g711_law)law, 0.8, -0.25, 0.0, 16u))
			return 1;
		/* A real second echo belongs in the negotiated feedback filter,
		 * not in a long, noisy prefilter approximation. Verify delivered data. */
		for (uint8_t rate = 16u; rate <= 19u; ++rate)
			if (!test_channel((enum g711_law)law, 0.9, 0.08, 0.02, rate))
				return 1;
		if (!test_channel((enum g711_law)law, 0.8, -0.15, 0.05, 13u))
			return 1;
	}
	return 0;
}
