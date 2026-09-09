// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_frame.h>

#include <integer_math.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	B1U_TEST_LEVEL = 4096,
	B1U_ORACLE_FRAMES = 8,
};

static struct v92_cpd_parameters parameters(uint8_t rate)
{
	struct v92_cpd_parameters cpd = {
		.four_times_gain_q0_16 = rate == 1u ? 165u : 108u,
		.data_rate_number = rate,
		.trellis = rate == 13u ? V92_CPD_TRELLIS_32_STATE :
					    V92_CPD_TRELLIS_16_STATE,
		.modulus_present = true,
		.filters_present = true,
		.constellations_present = true,
	};
	unsigned int bits = v92_pcm_mapping_data_bits(rate);
	uint8_t point_count = 0u;

	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 4u;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[0] =
		INT16_MAX;
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		unsigned int exponent = bits / V92_PCM_MAPPING_FRAME_SYMBOLS;
		uint8_t required;

		if (symbol < bits % V92_PCM_MAPPING_FRAME_SYMBOLS)
			exponent++;
		cpd.modulus[symbol] = (uint8_t)(1u << exponent);
		required = symbol % 4u == 3u ?
			cpd.modulus[symbol] :
			(uint8_t)((cpd.modulus[symbol] + 1u) / 2u);
		if (point_count < required)
			point_count = required;
	}
	if (rate == 13u)
		point_count = 64u;
	cpd.constellation[0].count = point_count;
	for (uint8_t point = 0u; point < point_count; ++point)
		cpd.constellation[0].positive_linear[point] =
			point_count > 16u ? (int16_t)(8u + point * 16u) :
					    (int16_t)((2u * point + 1u) * 256u);
	return cpd;
}

static bool initialize(
	struct v92_pcm_frame_tx *transmitter, uint8_t rate,
	enum g711_law law)
{
	struct v92_cpd_parameters cpd = parameters(rate);
	struct v92_pcm_mapping mapping;

	return v92_pcm_mapping_init(&mapping, law, &cpd) ==
		       V92_PCM_MAPPING_OK &&
	       v92_pcm_frame_tx_init(transmitter, &mapping) ==
		       V92_PCM_FRAME_OK &&
	       v92_pcm_frame_tx_is_valid(transmitter);
}

static void generated_bits(
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS], size_t count,
	unsigned int seed)
{
	uint32_t state = UINT32_C(0x9e3779b9) ^ seed;

	for (size_t bit = 0u; bit < count; ++bit) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[bit] = (uint8_t)(state >> 31);
	}
}

static bool test_b1u_oracle(void)
{
	/* Eight frames cover both differential signs and several complete
	 * 16-state convolutional cycles without retaining an audio vector. */
	static const uint8_t expected[] = {
		0x57, 0xad, 0xd7, 0x2d, 0x57, 0xd7,
		0x2d, 0x9e, 0x3f, 0x57, 0xd7, 0x9c,
		0xd7, 0x3f, 0xbf, 0xb5, 0x2d, 0x2d,
		0xbf, 0xbf, 0x57, 0x2d, 0xd7, 0x23,
		0x3f, 0xd7, 0x2d, 0x9c, 0xb5, 0x2d,
		0xb5, 0x9e, 0x3f, 0xbf, 0xbf, 0x23,
		0x35, 0x2d, 0x57, 0x57, 0xd7, 0x57,
		0xd7, 0x23, 0x3f, 0x2d, 0x2d, 0xad,
		0x35, 0x2d, 0x3f, 0xd7, 0xd7, 0xb5,
		0x3f, 0xad, 0xb5, 0x35, 0xbf, 0xad,
		0xad, 0xb5, 0xbf, 0xa3, 0xb5, 0xd7,
		0xb5, 0x9e, 0xad, 0x57, 0xb5, 0x23,
		0x2d, 0x3f, 0xd7, 0xb5, 0x3f, 0x2d,
		0xbf, 0x3f, 0x3f, 0xd7, 0x57, 0x1c,
		0xd7, 0x2d, 0x2d, 0xad, 0x35, 0xd7,
		0xbf, 0x35, 0xd7, 0xbf, 0xb5, 0x1c,
	};
	struct v92_pcm_frame_tx transmitter;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t actual[ARRAY_LENGTH(expected)];
	size_t cursor = 0u;

	CHECK(initialize(&transmitter, 1u, G711_LAW_PCMU),
	      "B1u transmitter did not initialize");
	memset(bits, 1, transmitter.mapping.data_bits_per_frame);
	for (unsigned int frame_index = 0u;
	     frame_index < B1U_ORACLE_FRAMES; ++frame_index) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode(
			      &transmitter, bits,
			      transmitter.mapping.data_bits_per_frame,
			      &frame) == V92_PCM_FRAME_OK,
		      "B1u frame %u did not encode", frame_index);
		for (size_t symbol = 0u;
		     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int16_t sample;

			CHECK(v92_pcm_frame_scale(
				      &transmitter.mapping, B1U_TEST_LEVEL,
				      frame.v[symbol], &sample) ==
				      V92_PCM_FRAME_OK,
			      "B1u symbol %zu did not scale", symbol);
			actual[cursor++] = g711_encode_pcmu(sample);
		}
	}
	CHECK(cursor == ARRAY_LENGTH(expected) &&
	      memcmp(actual, expected, sizeof(expected)) == 0,
	      "deterministic B1u wire prefix changed");
	return true;
}

static bool test_rate_matrix_and_reset(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u;
		     rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			struct v92_pcm_frame_tx transmitter;
			struct v92_pcm_frame first;
			struct v92_pcm_frame reset;
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

			CHECK(initialize(
				      &transmitter, rate,
				      (enum g711_law)law),
			      "rate %u law %u did not initialize", rate, law);
			generated_bits(
				bits, transmitter.mapping.data_bits_per_frame,
				rate * 31u + law);
			CHECK(v92_pcm_frame_encode(
				      &transmitter, bits,
				      transmitter.mapping.data_bits_per_frame,
				      &first) == V92_PCM_FRAME_OK &&
			      v92_pcm_frame_tx_reset_b1u(&transmitter) ==
				      V92_PCM_FRAME_OK &&
			      v92_pcm_frame_encode(
				      &transmitter, bits,
				      transmitter.mapping.data_bits_per_frame,
				      &reset) == V92_PCM_FRAME_OK &&
			      memcmp(&first, &reset, sizeof(first)) == 0,
			      "rate %u law %u reset changed its first frame",
			      rate, law);
			for (size_t symbol = 0u;
			     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
				int eta = first.signed_index[symbol];
				unsigned int modulus =
					transmitter.mapping.cpd.modulus[symbol];

				if (symbol % 4u != 3u) {
					int digit = eta % (int)modulus;

					if (digit < 0)
						digit += modulus;
					CHECK((uint8_t)digit == first.digit[symbol],
					      "rate %u symbol %zu changed equivalence",
					      rate, symbol);
				}
			}
		}
	}
	return true;
}

static bool test_filter_history(int32_t prior_u, int32_t prior_x, int32_t prior_v,
	int16_t precoder_feedback, int16_t prefilter_feedback)
{
	struct v92_cpd_parameters cpd = parameters(4u);
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame frame;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

	memset(cpd.filter, 0, sizeof(cpd.filter));
	cpd.filter[V92_CPD_PRECODER_FEED_FORWARD].count = 1u;
	cpd.filter[V92_CPD_PRECODER_FEED_FORWARD].coefficient[0] = 16384;
	cpd.filter[V92_CPD_PRECODER_FEEDBACK].count = 1u;
	cpd.filter[V92_CPD_PRECODER_FEEDBACK].coefficient[0] = precoder_feedback;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 2u;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[0] = 24576;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[1] = 8192;
	cpd.filter[V92_CPD_PREFILTER_FEEDBACK].count = 1u;
	cpd.filter[V92_CPD_PREFILTER_FEEDBACK].coefficient[0] = prefilter_feedback;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) ==
		      V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) ==
		      V92_PCM_FRAME_OK,
	      "filtered mapping did not initialize");
	transmitter.state.filter.history[0] = prior_u;
	transmitter.state.filter.history[1] = prior_x;
	transmitter.state.filter.history[2] = prior_v;
	generated_bits(bits, mapping.data_bits_per_frame, 917u);
	CHECK(v92_pcm_frame_encode(
		      &transmitter, bits, mapping.data_bits_per_frame,
		      &frame) == V92_PCM_FRAME_OK,
	      "filtered frame did not encode");
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int32_t x = round_shift_s64_to_s32_saturating(
			(int64_t)frame.u[symbol] * 32768 +
			(int64_t)prior_u * 16384 +
			(int64_t)prior_x * precoder_feedback * 2,
			15u);
		int32_t v = round_shift_s64_to_s32_saturating(
			(int64_t)x * 24576 + (int64_t)prior_x * 8192 +
			(int64_t)prior_v * prefilter_feedback * 2,
			15u);

		CHECK(frame.x[symbol] == x && frame.v[symbol] == v,
		      "filter recurrence changed at symbol %zu", symbol);
		prior_u = frame.u[symbol];
		prior_x = x;
		prior_v = v;
	}
	return true;
}

static bool test_filters(void)
{
	/* The history contribution must not be rounded or saturated before
	 * adding the current point: exercise half ties of either sign and
	 * cancellation near both signed limits, not just a zero history. */
	return test_filter_history(0, 0, 0, -4096, 4096) &&
		test_filter_history(1, 0, 0, -4096, 4096) &&
		test_filter_history(-1, 0, 0, -4096, 4096) &&
		test_filter_history(4, INT32_MAX, 0, 16384, 4096) &&
		test_filter_history(-4, INT32_MIN, 0, 16384, 4096) &&
		test_filter_history(0, 0, INT32_MAX, -4096, 16384) &&
		test_filter_history(0, 0, INT32_MIN, -4096, 16384);
}

static bool test_level_refinement(void)
{
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	struct v92_pcm_frame frame;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

	memset(bits, 1, sizeof(bits));
	CHECK(initialize(&tx, 13u, G711_LAW_PCMU) &&
	      v92_pcm_frame_rx_init(&rx, &tx.mapping, B1U_TEST_LEVEL) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_encode(&tx, bits, tx.mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "level-refinement fixture failed");
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;

		CHECK(v92_pcm_frame_scale(&tx.mapping, B1U_TEST_LEVEL, frame.v[symbol],
			      &sample) == V92_PCM_FRAME_OK, "level-refinement scaling failed");
		samples[symbol] = g711_decode_pcmu(g711_encode_pcmu(sample));
	}
	struct v92_pcm_frame_rx saved = rx;

	CHECK(v92_pcm_frame_rx_refine_level(&rx, &frame, samples, 4000, 4200) ==
		      V92_PCM_FRAME_OK && rx.upstream_level == B1U_TEST_LEVEL &&
	      rx.minimum_level >= 4000 && rx.maximum_level <= 4200,
	      "equal-error refinement changed LU or exceeded the interval");
	saved.minimum_level = rx.minimum_level;
	saved.maximum_level = rx.maximum_level;
	CHECK(memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "equal-error refinement changed coding history");
	CHECK(v92_pcm_frame_rx_refine_level(NULL, &frame, samples, 4000, 4200) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, NULL, samples, 4000, 4200) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &frame, NULL, 4000, 4200) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, (const struct v92_pcm_frame *)&rx,
		      samples, 4000, 4200) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &frame, (const int16_t *)&rx,
		      4000, 4200) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &frame, samples, 0, 4200) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      v92_pcm_frame_rx_refine_level(&rx, &frame, samples, 4100, 4200) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      v92_pcm_frame_rx_refine_level(&rx, &frame, samples, 4000, 4090) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "invalid refinement changed LU or coding history");
	return true;
}

static bool test_rejection_and_names(void)
{
	static const char *const names[] = {
		"ok",
		"invalid argument",
		"invalid parameters",
		"out of range",
	};
	struct v92_pcm_frame_tx transmitter;
	struct v92_pcm_frame_tx saved;
	struct v92_pcm_frame output;
	struct v92_pcm_frame output_saved;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS] = { 0 };
	int16_t sample = 1234;

	CHECK(initialize(&transmitter, 1u, G711_LAW_PCMU),
	      "contract transmitter did not initialize");
	saved = transmitter;
	memset(&output, 0x5a, sizeof(output));
	output_saved = output;
	CHECK(v92_pcm_frame_encode(
		      &transmitter, bits,
		      transmitter.mapping.data_bits_per_frame - 1u,
		      &output) == V92_PCM_FRAME_OUT_OF_RANGE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &output_saved, sizeof(output)) == 0,
	      "short frame changed state or output");
	bits[0] = 2u;
	CHECK(v92_pcm_frame_encode(
		      &transmitter, bits,
		      transmitter.mapping.data_bits_per_frame,
		      &output) == V92_PCM_FRAME_OUT_OF_RANGE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
	      "non-bit input changed transmitter state");
	transmitter.state.differential = 2u;
	CHECK(v92_pcm_frame_encode(
		      &transmitter, bits,
		      transmitter.mapping.data_bits_per_frame,
		      &output) == V92_PCM_FRAME_INVALID_PARAMETERS,
	      "invalid state encoded a frame");
	CHECK(v92_pcm_frame_scale(
		      &saved.mapping, 0, 123, &sample) ==
		      V92_PCM_FRAME_OUT_OF_RANGE && sample == 1234,
	      "invalid gain changed its output");
	struct v92_pcm_frame_tx invalid = transmitter;

	CHECK(v92_pcm_frame_tx_reset_control(NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_tx_reset_control(&transmitter) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&transmitter, &invalid, sizeof(transmitter)) == 0,
	      "invalid control reset changed state");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_pcm_frame_result_name(
				      (enum v92_pcm_frame_result)index),
			      names[index]) == 0,
		      "result name %zu changed", index);
	CHECK(strcmp(
		      v92_pcm_frame_result_name(
			      (enum v92_pcm_frame_result)-1),
		      "unknown") == 0,
	      "unknown result name changed");
	return true;
}

int main(void)
{
	if (!test_b1u_oracle() || !test_rate_matrix_and_reset() ||
	    !test_filters() || !test_level_refinement() || !test_rejection_and_names())
		return 1;
	return 0;
}
