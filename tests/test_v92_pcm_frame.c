// SPDX-License-Identifier: BSD-3-Clause

#include "wide_uint.h"

#include <v92_pcm_frame.h>

#include <integer_math.h>

#include "test.h"
#include "v92_clipping_fixture.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	B1U_TEST_LEVEL = 4096,
	B1U_ORACLE_FRAMES = 8,
	FILTER_HISTORY_TAPS = 32,
	FILTER_HISTORY_ACTIVE_TAP = 15,
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
	unsigned int bits = pcm_upstream_frame_bits(rate);
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

			CHECK(v92_pcm_frame_scale(transmitter.mapping.cpd.four_times_gain_q0_16, B1U_TEST_LEVEL,
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

static bool test_b1u_coding_acquisition(void)
{
	const uint8_t rates[] = { 1u, 7u, 13u, 19u };
	const unsigned histories[] = { 0u, 1u, 6u };
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (size_t rate = 0u; rate < ARRAY_LENGTH(rates); ++rate) {
			/* Include both a 60-bit partial group and a 72-bit full group. */
			unsigned cases = rates[rate] == 7u ? 1u : ARRAY_LENGTH(histories);
			for (unsigned index = 0u; index < cases; ++index) {
				unsigned changed_history = histories[index];
				struct v92_pcm_frame_tx tx;
				struct v92_pcm_frame_rx rx;
				uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
				struct v92_pcm_frame frame;

				CHECK(initialize(&tx, rates[rate], (enum g711_law)law), "coding acquisition TX init");
				CHECK(v92_pcm_frame_rx_init_constellation(&rx, &tx.mapping) == V92_PCM_FRAME_OK,
				      "coding acquisition RX init");
				size_t count = tx.mapping.data_bits_per_frame;

				/* Exercise mismatched scrambler contexts without claiming
				 * that a different seed is normative.
				 * The transmitter's zero-state reset has separate oracle tests.
				 * A small gain error must not lock in the wrong early history. */
				memset(bits, 1, count);
				tx.state.scrambler.history = changed_history;
				unsigned validated = 0u, payload = 0u;
				for (unsigned f = 0u; f < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES + 4u; ++f) {
					uint8_t decoded[V92_PCM_MAPPING_MAX_BITS + 1u];
					struct v92_pcm_frame_decision decision;

					memset(decoded, 0xa5, sizeof(decoded));
					if (f >= V92_B1U_FRAMES)
						generated_bits(bits, count, f);
					CHECK(v92_pcm_frame_encode(&tx, bits, count, &frame) == V92_PCM_FRAME_OK,
					      "coding acquisition encode");
					int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
					for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
						samples[i] = (int16_t)lround(frame.u[i] * (changed_history == 6u ? .99 : 1.0));
					CHECK(v92_pcm_frame_decode(&rx, samples, f < V92_B1U_FRAMES,
						      decoded, count, &decision) == V92_PCM_FRAME_OK,
					      "coding acquisition frame %u law %u rate %u", f, law, rates[rate]);
					for (size_t i = count; i < sizeof(decoded); ++i)
						CHECK(decoded[i] == 0xa5, "coding acquisition wrote beyond payload");
					if (f < V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES)
						continue;
					unsigned delivered = f - V92_PCM_RX_DELAY_FRAMES;

					CHECK(decision.bits_ready && !decision.ambiguous,
					      "unsettled coding: law %u rate %u history %u frame %u",
					      law, rates[rate], changed_history, delivered);
					if (delivered < V92_B1U_FRAMES) {
						for (size_t i = 0u; i < count; ++i)
							CHECK(decoded[i] == 1u,
							      "B1u coding history: law %u rate %u history %u frame %u bit %zu",
							      law, rates[rate], changed_history, delivered, i);
						validated++;
					} else {
						uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

						generated_bits(expected, count, delivered);
						CHECK(memcmp(decoded, expected, count) == 0, "post-B1u coding changed payload");
						payload++;
					}
				}
				CHECK(validated == V92_B1U_FRAMES - V92_B1U_ACQUISITION_FRAMES && payload == 4u,
				      "coding acquisition shortened validation or payload");
			}
		}
	}
	return true;
}

static bool test_model_acquisition_boundary(void)
{
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	struct v92_pcm_frame frame;
	struct v92_pcm_frame_decision decision;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];

	CHECK(initialize(&tx, 1u, G711_LAW_PCMU) &&
	      v92_pcm_frame_rx_init_constellation(&rx, &tx.mapping) == V92_PCM_FRAME_OK,
	      "model acquisition init");
	size_t count = tx.mapping.data_bits_per_frame;

	memset(bits, 1, sizeof(bits));
	memset(decoded, 0x5a, sizeof(decoded));
	for (unsigned f = 0u; f < V92_B1U_ACQUISITION_FRAMES; ++f)
		CHECK(v92_pcm_frame_encode(&tx, bits, count, &frame) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_acquire_model(&rx, frame.u, decoded, count, &decision) == V92_PCM_FRAME_OK,
		      "model acquisition frame %u", f);
	struct v92_pcm_frame_rx saved = rx;
	struct v92_pcm_frame_decision saved_decision = decision;
	uint8_t saved_bits[sizeof(decoded)];

	memcpy(saved_bits, decoded, sizeof(decoded));
	CHECK(v92_pcm_frame_acquire_model(&rx, frame.u, decoded, count, &decision) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0 &&
	      memcmp(decoded, saved_bits, sizeof(decoded)) == 0,
	      "model acquisition consumed a validation frame or changed rejected output");
	CHECK(v92_pcm_frame_encode(&tx, bits, count, &frame) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_decode(&rx, frame.u, true, decoded, count, &decision) == V92_PCM_FRAME_OK &&
	      rx.noise.frames == V92_B1U_ACQUISITION_FRAMES + 1u,
	      "model acquisition did not hand off to ordinary validation");
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

/* V.92 6.3/6.4 and 8.7.1: calculate the transmitted constraints without
 * calling the production scrambler, modulus, inverse-map or trellis helpers.
 * A legal equivalence class need not have one uniquely prescribed point. */
static unsigned modulo(int value, unsigned base)
{
	int remainder = value % (int)base;
	return (unsigned)(remainder < 0 ? remainder + (int)base : remainder);
}

static uint8_t figure9_subset(int first, int second)
{
	/* V.34 Figure 9 in printed row/column order, coordinates 2*eta+1. */
	static const uint8_t labels[4][4] = {
		{ 1, 6, 5, 2 }, { 4, 3, 0, 7 },
		{ 5, 2, 1, 6 }, { 0, 7, 4, 3 },
	};
	return labels[modulo(1 - second, 4)][modulo(first + 2, 4)];
}

static void trellis_step(uint8_t delay[6], unsigned states, const int16_t eta[4])
{
	/* V.34 Table 13 is normative, unlike the production Boolean reduction. */
	static const uint8_t input[8][8] = {
		{ 0, 0, 1, 1, 8, 8, 9, 9 }, { 3, 2, 2, 3, 11, 10, 10, 11 },
		{ 5, 5, 4, 4, 13, 13, 12, 12 }, { 6, 7, 7, 6, 14, 15, 15, 14 },
		{ 8, 8, 9, 9, 0, 0, 1, 1 }, { 11, 10, 10, 11, 3, 2, 2, 3 },
		{ 13, 13, 12, 12, 5, 5, 4, 4 }, { 14, 15, 15, 14, 6, 7, 7, 6 },
	};
	uint8_t word = input[figure9_subset(eta[0], eta[1])][figure9_subset(eta[2], eta[3])];
	uint8_t y1 = word & 1u, y2 = (word >> 1) & 1u;
	uint8_t y3 = (word >> 2) & 1u, y4 = word >> 3;
	uint8_t next[6] = { 0 };

	/* Figures 10-12, delay boxes numbered left to right. V.92 advances
	 * them once per FOUR samples, not at V.34's two-sample boundary. */
	if (states == 16) {
		next[0] = delay[3];
		next[1] = delay[0] ^ y2 ^ delay[3];
		next[2] = delay[1] ^ y2;
		next[3] = delay[2] ^ y1;
	} else if (states == 32) {
		next[0] = delay[4];
		next[1] = delay[0] ^ y2;
		next[2] = delay[1] ^ y1;
		next[3] = delay[2] ^ y4;
		next[4] = delay[3] ^ y2;
	} else {
		uint8_t feedback = y1 ^ delay[1], tail = delay[0] ^ delay[1];

		next[0] = y4 ^ tail ^ (feedback & delay[2]);
		next[1] = y3 ^ tail ^ delay[3] ^ (y2 & delay[2]);
		next[2] = feedback ^ delay[2];
		next[3] = delay[2];
		next[4] = delay[5];
		next[5] = y2 ^ delay[4] ^ delay[2];
	}
	memcpy(delay, next, sizeof(next));
}

static bool test_transmitter_equations(uint8_t rate, unsigned trellis,
	unsigned law, bool uneven)
{
	enum { RM_START = 56, RM_PRIME_START = RM_START + 32, CONTROL_START = RM_PRIME_START + 2,
		FRAMES = CONTROL_START + 4, SYMBOLS = FRAMES * 12 };
	struct v92_cpd_parameters cpd = parameters(rate);
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	int32_t u[SYMBOLS] = { 0 }, x[SYMBOLS] = { 0 }, v[SYMBOLS] = { 0 };
	uint8_t history[23] = { 0 }, delay[6] = { 0 }, differential = 0;
	unsigned cursor = 0, signs = 0, parities = 0;
	struct wide_uint capacity = wide_from_u64(1);

	cpd.trellis = (enum v92_cpd_trellis)trellis;
	/* Six different constellations ensure 6T constellation and 4T trellis
	 * phases cross rather than accidentally repeating together. */
	memset(cpd.constellation, 0, sizeof(cpd.constellation));
	for (unsigned phase = 0; phase < 6; ++phase) {
		cpd.frame_constellation[phase] = (uint8_t)phase;
		cpd.constellation[phase].count = 128;
		for (unsigned point = 0; point < 128; ++point)
			cpd.constellation[phase].positive_linear[point] =
				(int16_t)((2 * point + 1) * (16 + phase));
	}
	if (uneven) {
		cpd.modulus[0]++;
		cpd.modulus[6]++;
		if (rate == 1u) {
			/* Exercise a singleton fourth-symbol class alongside large
			 * odd moduli, retaining more than enough frame capacity. */
			cpd.modulus[0] = 255u;
			cpd.modulus[3] = 1u;
			cpd.modulus[6] = 3u;
		}
	}
	for (unsigned i = 0; i < 12; ++i)
		wide_multiply_u32(capacity, cpd.modulus[i], &capacity);
	memset(cpd.filter, 0, sizeof(cpd.filter));
	cpd.filter[V92_CPD_PRECODER_FEED_FORWARD] =
		(struct v92_cpd_filter){ .count = 3, .coefficient = { 8192, -4096, 2048 } };
	cpd.filter[V92_CPD_PRECODER_FEEDBACK] =
		(struct v92_cpd_filter){ .count = 2, .coefficient = { -4096, 1024 } };
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD] =
		(struct v92_cpd_filter){ .count = 7, .coefficient = { 0, 24576, 0, 0, 0, 0, 8192 } };
	cpd.filter[V92_CPD_PREFILTER_FEEDBACK] =
		(struct v92_cpd_filter){ .count = 2, .coefficient = { 2048, -1024 } };
	CHECK(v92_pcm_mapping_init(&mapping, (enum g711_law)law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK,
	      "equation fixture init: rate %u trellis %u law %u", rate, trellis, law);
	for (unsigned f = 0; f < FRAMES; ++f) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame frame;
		struct wide_uint value = wide_from_u64(0);
		bool rm = f >= RM_START && f < CONTROL_START;
		bool prime = f >= RM_PRIME_START;
		uint8_t sign = 0;

		if (f == CONTROL_START) {
			CHECK(v92_pcm_frame_tx_reset_control(&transmitter) == V92_PCM_FRAME_OK,
				"post-RM control reset failed");
			memset(history, 0, sizeof(history));
			differential = 0;
			cursor = 0;
		}
		generated_bits(bits, mapping.data_bits_per_frame, f + rate * 71u);
		if (f < 48)
			memset(bits, 1, mapping.data_bits_per_frame);
		if (rm) {
			struct v92_pcm_frame_tx_state before = transmitter.state;

			CHECK(v92_pcm_frame_encode_rm(&transmitter.mapping, &transmitter.state,
				prime, &frame) == V92_PCM_FRAME_OK &&
				memcmp(&before.scrambler, &transmitter.state.scrambler,
					sizeof(before.scrambler)) == 0 &&
				before.differential == transmitter.state.differential,
				"RM changed scrambler/differential history");
		} else {
			/* GPA = 1 + x^-5 + x^-23, including across B1u -> data. */
			for (unsigned bit = 0; bit < mapping.data_bits_per_frame; ++bit) {
				uint8_t scrambled = bits[bit] ^ history[(cursor + 18) % 23] ^ history[cursor];

				history[cursor] = scrambled;
				cursor = (cursor + 1) % 23;
				wide_add(value, wide_shift_left(wide_from_u64(scrambled), bit), &value);
			}
			sign = (uint8_t)(differential ^ (wide_compare(value,
				wide_shift_right(wide_subtract(capacity, wide_from_u64(1)), 1)) > 0));
			if (differential)
				value = wide_subtract(wide_subtract(capacity, wide_from_u64(1)), value);
			differential = sign;
			CHECK(v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame,
				&frame) == V92_PCM_FRAME_OK, "equation encoding failed");
		}
		CHECK(frame.frame_sign == sign, "modulus sign differs at frame %u", f);
		signs |= 1u << sign;
		for (unsigned i = 0; i < 12; ++i) {
			unsigned n = f * 12 + i, modulus = cpd.modulus[i];
			unsigned digit = wide_divide_u32(value, modulus, &value), base = i - i % 4;
			if (rm)
				digit = (i % 4u < 2u) != prime ? modulus - 1u : 0u;
			uint8_t y0 = delay[3 + trellis];
			int eta = frame.signed_index[i];
			int residue = eta;
			const struct v92_cpd_constellation *points = &cpd.constellation[i % 6];

			CHECK(frame.digit[i] == digit && frame.y0[i] == y0,
			      "mapping/trellis differs: frame %u symbol %u", f, i);
			parities |= 1u << y0;
			if (i % 4 == 3) {
				residue -= (int)modulo(frame.signed_index[base] +
					frame.signed_index[base + 1] + frame.signed_index[base + 2] + y0, 2);
				CHECK(modulo(residue, 2) == 0, "fourth point has wrong coset parity");
				residue /= 2;
			}
			CHECK(modulo(residue, modulus) == digit && eta >= -128 && eta < 128,
			      "point is outside its equivalence class");
			u[n] = eta < 0 ? -points->positive_linear[-eta - 1] : points->positive_linear[eta];
			CHECK(frame.u[i] == u[n], "six-symbol constellation phase differs");
			/* Evaluate 6.4.2 directly in separate full-sequence histories.
			 * Stage rounding is our fixed-point policy, not a V.92 mandate. */
			long double a = u[n], b = 0;
			for (unsigned tap = 0; tap < 3 && tap < n; ++tap)
				a += (long double)u[n - tap - 1] * cpd.filter[0].coefficient[tap] / 32768.L;
			for (unsigned tap = 0; tap < 2 && tap < n; ++tap)
				a += (long double)x[n - tap - 1] * cpd.filter[1].coefficient[tap] / 16384.L;
			/* Our selection policy minimizes |x|, with ascending eta
			 * breaking ties. Exhaust all indices independently of the
			 * production class iterator, including negative residues. */
			long double best = HUGE_VALL;
			int expected_eta = 0;

			for (int candidate = -(int)points->count; candidate < points->count; ++candidate) {
				int candidate_residue = candidate;

				if (i % 4 == 3) {
					candidate_residue -= (int)modulo(frame.signed_index[base] +
						frame.signed_index[base + 1] + frame.signed_index[base + 2] + y0, 2);
					if (modulo(candidate_residue, 2))
						continue;
					candidate_residue /= 2;
				}
				if (modulo(candidate_residue, modulus) != digit)
					continue;
				int point = candidate < 0 ? -points->positive_linear[-candidate - 1] :
					points->positive_linear[candidate];
				long double score = fabsl(roundl(a - u[n] + point));

				if (score < best) {
					best = score;
					expected_eta = candidate;
				}
			}
			CHECK(isfinite(best) && eta == expected_eta,
			      "equivalence enumeration changed point selection: frame %u symbol %u", f, i);
			x[n] = (int32_t)roundl(a);
			for (unsigned tap = 0; tap < 7 && tap <= n; ++tap)
				b += (long double)x[n - tap] * cpd.filter[2].coefficient[tap] / 32768.L;
			for (unsigned tap = 0; tap < 2 && tap < n; ++tap)
				b += (long double)v[n - tap - 1] * cpd.filter[3].coefficient[tap] / 16384.L;
			v[n] = (int32_t)roundl(b);
			CHECK(frame.x[i] == x[n] && frame.v[i] == v[n],
			      "filter equation differs: frame %u symbol %u", f, i);
			for (unsigned level = 4096; level <= 32767; level += 28671) {
				long double scaled = roundl((long double)v[n] *
					cpd.four_times_gain_q0_16 * level / 262144.L);
				int16_t expected = (int16_t)fmaxl(INT16_MIN, fminl(INT16_MAX, scaled));
				int16_t sample;

				CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, (int16_t)level, frame.v[i],
					&sample) == V92_PCM_FRAME_OK && sample == expected,
				      "prefilter gain or PCM saturation differs");
			}
			if (i % 4 == 3)
				trellis_step(delay, 16u << trellis, frame.signed_index + base);
		}
		CHECK(wide_compare(value, wide_from_u64(0)) == 0, "modulus conversion left high-order data");
	}
	CHECK(signs == 3 && parities == 3, "equation fixture missed sign/trellis transitions");
	return true;
}

static bool test_transmitter_equation_matrix(void)
{
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned trellis = 0; trellis < 3; ++trellis)
			for (uint8_t rate = 1; rate <= 19; ++rate)
				for (unsigned uneven = 0; uneven < 2; ++uneven)
					CHECK(test_transmitter_equations(rate, trellis, law, uneven != 0),
					      "equation matrix: rate %u trellis %u law %u uneven %u",
					      rate, trellis, law, uneven);
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

static struct v92_cpd_parameters history_parameters(unsigned active)
{
	struct v92_cpd_parameters cpd = parameters(9);

	memset(cpd.filter, 0, sizeof(cpd.filter));
	for (unsigned section = 0; section < V92_CPD_FILTER_SECTION_COUNT; ++section)
		cpd.filter[section].count = FILTER_HISTORY_TAPS;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[0] = INT16_MAX;
	if (active)
		cpd.filter[active - 1u].coefficient[FILTER_HISTORY_ACTIVE_TAP] = 512;
	return cpd;
}

static bool test_causal_filter_history(void)
{
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned model = 0; model < 2; ++model)
			for (unsigned active = 0; active <= V92_CPD_FILTER_SECTION_COUNT; ++active) {
				struct v92_cpd_parameters cpd = history_parameters(active);
				struct v92_pcm_mapping mapping;
				struct v92_pcm_frame_tx tx;
				struct v92_pcm_frame_rx rx;
				struct v92_pcm_frame frame;
				struct v92_pcm_frame_decision decision;
				uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
				int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

				CHECK(v92_pcm_mapping_init(&mapping, (enum g711_law)law, &cpd) == V92_PCM_MAPPING_OK &&
				      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
				      (model ? v92_pcm_frame_rx_init_constellation(&rx, &mapping) :
					       v92_pcm_frame_rx_init(&rx, &mapping, B1U_TEST_LEVEL)) == V92_PCM_FRAME_OK,
				      "causal history initialization");
				memset(bits, 1, mapping.data_bits_per_frame);
				CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
				      V92_PCM_FRAME_OK, "causal history encoding");
				for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
					if (model)
						samples[i] = frame.u[i];
					else {
						CHECK(v92_pcm_frame_scale(cpd.four_times_gain_q0_16, B1U_TEST_LEVEL,
							frame.v[i], &samples[i]) == V92_PCM_FRAME_OK,
						      "causal history scaling");
						samples[i] = g711_quantize((enum g711_law)law, samples[i]);
					}
				}
				CHECK((model ? v92_pcm_frame_acquire_model(&rx, samples, decoded,
					mapping.data_bits_per_frame, &decision) :
					v92_pcm_frame_decode(&rx, samples, true, decoded,
					mapping.data_bits_per_frame, &decision)) == V92_PCM_FRAME_OK &&
				      v92_pcm_frame_rx_is_valid(&rx), "causal history decoding");
				CHECK((active || model) ? rx.path_count > 1u : rx.path_count == 1u,
				      "law %u model %u active %u retained %u histories",
				      law, model, active, rx.path_count);
				if (model && !active) {
					bool different = false;

					for (unsigned path = 1; path < rx.path_count; ++path)
						different |= memcmp(rx.paths[0].filter.history + FILTER_HISTORY_TAPS,
							rx.paths[path].filter.history + FILTER_HISTORY_TAPS,
							3u * sizeof(int32_t)) != 0;
					CHECK(different, "future channel-model x history was merged");
				}
			}
	return true;
}

static bool test_delayed_filter_stream(void)
{
	enum { FRAMES = 72 };

	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned active = 1; active <= V92_CPD_FILTER_SECTION_COUNT; ++active) {
			struct v92_cpd_parameters cpd = history_parameters(active);
			struct v92_pcm_mapping mapping;
			struct v92_pcm_frame_tx tx;
			struct v92_pcm_frame_rx rx;
			uint8_t bits[FRAMES][V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
			uint32_t random = 123;
			unsigned payload_bits = 0;

			CHECK(v92_pcm_mapping_init(&mapping, (enum g711_law)law, &cpd) == V92_PCM_MAPPING_OK &&
			      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
			      v92_pcm_frame_rx_init(&rx, &mapping, B1U_TEST_LEVEL) == V92_PCM_FRAME_OK,
			      "delayed stream initialization");
			for (unsigned f = 0; f < FRAMES; ++f) {
				struct v92_pcm_frame frame;
				struct v92_pcm_frame_decision decision;
				int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

				for (unsigned i = 0; i < mapping.data_bits_per_frame; ++i) {
					random ^= random << 13u;
					random ^= random >> 17u;
					random ^= random << 5u;
					bits[f][i] = f < V92_B1U_FRAMES ? 1u : random & 1u;
				}
				CHECK(v92_pcm_frame_encode(&tx, bits[f], mapping.data_bits_per_frame, &frame) ==
				      V92_PCM_FRAME_OK, "delayed stream encoding");
				for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
					CHECK(v92_pcm_frame_scale(cpd.four_times_gain_q0_16, B1U_TEST_LEVEL,
						frame.v[i], &samples[i]) == V92_PCM_FRAME_OK, "delayed stream scaling");
					samples[i] = g711_quantize((enum g711_law)law, samples[i]);
				}
				CHECK(v92_pcm_frame_decode(&rx, samples, f < V92_B1U_FRAMES,
					decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
				      "delayed stream decoding");
				if (decision.bits_ready) {
					CHECK(f >= V92_PCM_RX_DELAY_FRAMES,
					      "delayed stream published bits before lookahead");
					unsigned source = f - V92_PCM_RX_DELAY_FRAMES;

					CHECK(memcmp(decoded, bits[source], mapping.data_bits_per_frame) == 0,
					      "law %u filter %u frame %u changed decoded bits", law, active, f);
					if (source >= V92_B1U_FRAMES)
						payload_bits += mapping.data_bits_per_frame;
				}
			}
			CHECK(payload_bits != 0, "delayed stream never verified payload");
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
	struct v92_pcm_level_fit fit = { 0 };

	memset(bits, 1, sizeof(bits));
	CHECK(initialize(&tx, 13u, G711_LAW_PCMU) &&
	      v92_pcm_frame_rx_init(&rx, &tx.mapping, B1U_TEST_LEVEL) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_encode(&tx, bits, tx.mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "level-refinement fixture failed");
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;

		CHECK(v92_pcm_frame_scale(tx.mapping.cpd.four_times_gain_q0_16, B1U_TEST_LEVEL, frame.v[symbol],
			      &sample) == V92_PCM_FRAME_OK, "level-refinement scaling failed");
		samples[symbol] = g711_decode_pcmu(g711_encode_pcmu(sample));
	}
	struct v92_pcm_frame_rx saved = rx;

	struct g711_interval interval;
	CHECK(g711_quantization_interval(G711_LAW_PCMU, B1U_TEST_LEVEL, &interval),
	      "training level had no cell");
	CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) ==
		      V92_PCM_FRAME_OK && rx.upstream_level == B1U_TEST_LEVEL &&
	      rx.minimum_level >= interval.lower && rx.maximum_level <= interval.upper &&
	      fit.frames == 1u && fit.minimum == interval.lower && fit.maximum == interval.upper,
	      "equal-error refinement changed LU or exceeded the interval");
	saved.minimum_level = rx.minimum_level;
	saved.maximum_level = rx.maximum_level;
	CHECK(memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "equal-error refinement changed coding history");
	struct v92_pcm_level_fit fit_saved = fit;
	CHECK(v92_pcm_frame_rx_refine_level(NULL, &fit, &frame, samples) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, NULL, &frame, samples) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &fit, NULL, samples) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, NULL) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, (struct v92_pcm_level_fit *)&rx,
		      &frame, samples) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &fit, (const struct v92_pcm_frame *)&rx,
		      samples) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, (const int16_t *)&rx) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &fit, (const struct v92_pcm_frame *)&fit,
		      samples) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, (const int16_t *)&fit) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 && memcmp(&fit, &fit_saved, sizeof(fit)) == 0,
	      "invalid refinement changed LU or coding history");
	for (unsigned invalid = 0u; invalid < 4u; ++invalid) {
		fit = fit_saved;
		if (invalid == 0u) fit.minimum = 0;
		if (invalid == 1u) fit.minimum = B1U_TEST_LEVEL + 1;
		if (invalid == 2u) fit.maximum = B1U_TEST_LEVEL - 1;
		if (invalid == 3u) fit.maximum = fit.minimum + V92_PCM_LEVEL_FIT_POINTS;
		struct v92_pcm_level_fit before = fit;
		CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS && memcmp(&fit, &before, sizeof(fit)) == 0 &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0, "invalid fit changed owned state");
	}
	/* A biased first frame may temporarily favor a different level, but
	 * later known training must be allowed to correct it within the original
	 * cell. The old frame-by-frame intersection made that impossible. */
	fit = (struct v92_pcm_level_fit){ 0 };
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		CHECK(v92_pcm_frame_scale(tx.mapping.cpd.four_times_gain_q0_16, interval.upper, frame.v[symbol], &sample) ==
		      V92_PCM_FRAME_OK, "biased-frame scaling failed");
		samples[symbol] = g711_decode_pcmu(g711_encode_pcmu(sample));
	}
	CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) == V92_PCM_FRAME_OK &&
	      rx.minimum_level > B1U_TEST_LEVEL, "bias did not isolate the early-narrowing regression");
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		CHECK(v92_pcm_frame_scale(tx.mapping.cpd.four_times_gain_q0_16, B1U_TEST_LEVEL, frame.v[symbol], &sample) ==
		      V92_PCM_FRAME_OK, "clean-frame scaling failed");
		samples[symbol] = g711_decode_pcmu(g711_encode_pcmu(sample));
	}
	while (fit.frames < V92_B1U_FRAMES)
		CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) == V92_PCM_FRAME_OK,
		      "cumulative refinement failed");
	CHECK(rx.minimum_level <= B1U_TEST_LEVEL && rx.maximum_level >= B1U_TEST_LEVEL,
	      "early biased frame permanently excluded the true level");
	saved = rx;
	fit_saved = fit;
	CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 && memcmp(&fit, &fit_saved, sizeof(fit)) == 0,
	      "post-B1 data changed the fit");
	fit.frames = 1u;
	for (size_t i = 0; i < V92_PCM_LEVEL_FIT_POINTS; ++i) fit.error[i] = UINT64_MAX;
	fit_saved = fit;
	CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 && memcmp(&fit, &fit_saved, sizeof(fit)) == 0,
	      "cost overflow changed the fit");
	/* The final PCMU cell includes clipping headroom and is wider than an
	 * ordinary 1024-unit cell. The bounded scratch must cover both laws. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		CHECK(initialize(&tx, 13u, law) &&
		      v92_pcm_frame_rx_init(&rx, &tx.mapping, INT16_MAX) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_encode(&tx, bits, tx.mapping.data_bits_per_frame, &frame) == V92_PCM_FRAME_OK,
		      "clipped-cell fixture failed");
		for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int16_t sample;
			CHECK(v92_pcm_frame_scale(tx.mapping.cpd.four_times_gain_q0_16, INT16_MAX, frame.v[symbol], &sample) ==
			      V92_PCM_FRAME_OK, "clipped-cell scaling failed");
			samples[symbol] = law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
				g711_decode_pcma(g711_encode_pcma(sample));
		}
		fit = (struct v92_pcm_level_fit){ 0 };
		CHECK(v92_pcm_frame_rx_refine_level(&rx, &fit, &frame, samples) == V92_PCM_FRAME_OK &&
		      fit.maximum == INT16_MAX && fit.maximum - fit.minimum < V92_PCM_LEVEL_FIT_POINTS,
		      "clipped training cell exceeded bounded fit");
	}
	return true;
}

static bool test_pending_bit_validation(void)
{
	static const uint8_t rates[] = { 1u, 2u, 4u, 19u };
	static const uint8_t depths[] = { 0u, 5u, V92_PCM_RX_DELAY_FRAMES };
	static const uint8_t values[] = { 1u, 2u, 128u, 255u };

	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (size_t rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
			struct v92_pcm_frame_tx tx;
			struct v92_pcm_frame_rx rx;

			CHECK(initialize(&tx, rates[rate], (enum g711_law)law) &&
			      v92_pcm_frame_rx_init(&rx, &tx.mapping, B1U_TEST_LEVEL) ==
				      V92_PCM_FRAME_OK, "bit-validation initialization failed");
			/* Unaligned input and nonzero bytes just outside the frame must
			 * not affect validation; every in-frame upper bit must fail. */
			for (size_t offset = 0; offset < sizeof(uint64_t); ++offset) {
				uint8_t storage[V92_PCM_MAPPING_MAX_BITS + sizeof(uint64_t) + 1u];
				uint8_t *bits = storage + offset;
				size_t count = tx.mapping.data_bits_per_frame;
				struct v92_pcm_frame output;
				struct v92_pcm_frame_tx copy = tx;

				memset(storage, 255, sizeof(storage));
				memset(bits, 1, count);
				CHECK(v92_pcm_frame_encode(&copy, bits, count, &output) == V92_PCM_FRAME_OK,
				      "unaligned valid input rejected");
				copy = tx;
				struct v92_pcm_frame saved = output;

				for (size_t bit = 0; bit < count; ++bit) {
					bits[bit] = bit % 2u ? 2u : 128u;
					CHECK(v92_pcm_frame_encode(&copy, bits, count, &output) ==
						      V92_PCM_FRAME_OUT_OF_RANGE &&
					      memcmp(&copy, &tx, sizeof(copy)) == 0 &&
					      memcmp(&output, &saved, sizeof(output)) == 0,
					      "invalid input at offset %zu bit %zu accepted or mutated state", offset, bit);
					bits[bit] = 1u;
				}
			}
			rx.path_count = 2u;
			rx.paths[1] = rx.paths[0];
			for (size_t depth = 0; depth < ARRAY_LENGTH(depths); ++depth) {
				rx.pending_frames = depths[depth];
				CHECK(v92_pcm_frame_rx_is_valid(&rx), "zero history rejected");
				for (size_t path = 0; path < rx.path_count; ++path) {
					for (size_t frame = 0; frame < V92_PCM_RX_DELAY_FRAMES; ++frame) {
						for (size_t bit = 0; bit < V92_PCM_MAPPING_MAX_BITS; ++bit) {
							bool owned = frame < rx.pending_frames &&
								bit < rx.mapping.data_bits_per_frame;
							uint8_t *slot = &rx.paths[path].pending[frame][bit];

							for (size_t value = 0; value < ARRAY_LENGTH(values); ++value) {
								*slot = values[value];
								CHECK(v92_pcm_frame_rx_is_valid(&rx) ==
								      (owned && *slot == 1u),
								      "law %u rate %u depth %u path %zu frame %zu bit %zu value %u",
								      law, rates[rate], rx.pending_frames, path, frame, bit, *slot);
							}
							*slot = 0u;
						}
					}
				}
			}
		}
	}
	return true;
}

static bool test_receiver_initialization(void)
{
	const int16_t levels[] = { 0, 1, B1U_TEST_LEVEL, INT16_MAX };
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			for (unsigned selection = V92_CPD_TRELLIS_16_STATE;
			     selection <= V92_CPD_TRELLIS_64_STATE; ++selection) {
				struct v92_cpd_parameters cpd = parameters(rate);
				struct v92_pcm_mapping mapping;
				cpd.trellis = (enum v92_cpd_trellis)selection;
				CHECK(v92_pcm_mapping_init(&mapping, (enum g711_law)law, &cpd) ==
				      V92_PCM_MAPPING_OK, "receiver initialization mapping");
				for (size_t index = 0u; index < ARRAY_LENGTH(levels); ++index) {
					struct v92_pcm_frame_rx expected = { 0 }, rx, saved;
					expected.mapping = mapping;
					expected.observation = index == 0u ? V92_PCM_FRAME_CONSTELLATION : V92_PCM_FRAME_FILTER_OUTPUT;
					expected.upstream_level = expected.minimum_level = expected.maximum_level = levels[index];
					expected.path_count = 1u;
					CHECK(v34_scrambler_init(&expected.paths[0].descrambler, V34_POLYNOMIAL_GPA) == V34_FRAMING_OK &&
					      v34_convolutional_encoder_init(&expected.paths[0].trellis,
						      (enum v34_trellis_selection)selection) == V34_TRELLIS_OK,
					      "receiver initialization coding seeds");
					expected.b1u_scrambler = expected.paths[0].descrambler;
					for (unsigned alias = 0u; alias < 2u; ++alias) {
						memset(&rx, 0xa5, sizeof(rx));
						rx.mapping = mapping;
						const struct v92_pcm_mapping *input = alias ? &rx.mapping : &mapping;
						enum v92_pcm_frame_result result = index == 0u ?
							v92_pcm_frame_rx_init_constellation(&rx, input) :
							v92_pcm_frame_rx_init(&rx, input, levels[index]);
						CHECK(result == V92_PCM_FRAME_OK && v92_pcm_frame_rx_is_valid(&rx) &&
						      memcmp(&rx, &expected, sizeof(rx)) == 0,
						      "receiver replacement changed law %u rate %u trellis %u domain %zu alias %u",
						      law, rate, selection, index, alias);
						/* A malformed in-owner mapping must also survive rejection. */
						rx.mapping.data_bits_per_frame++;
						saved = rx;
						result = index == 0u ? v92_pcm_frame_rx_init_constellation(&rx, &rx.mapping) :
							v92_pcm_frame_rx_init(&rx, &rx.mapping, levels[index]);
						CHECK(result == V92_PCM_FRAME_INVALID_PARAMETERS &&
						      memcmp(&rx, &saved, sizeof(rx)) == 0,
						      "rejected alias changed receiver");
						CHECK(v92_pcm_frame_rx_init(&rx, &mapping, 0) == V92_PCM_FRAME_INVALID_PARAMETERS &&
						      v92_pcm_frame_rx_init(&rx, &mapping, -1) == V92_PCM_FRAME_INVALID_PARAMETERS &&
						      v92_pcm_frame_rx_init(&rx, NULL, levels[index]) == V92_PCM_FRAME_INVALID_ARGUMENT &&
						      v92_pcm_frame_rx_init_constellation(&rx, NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
						      memcmp(&rx, &saved, sizeof(rx)) == 0,
						      "invalid level or missing mapping changed receiver");
					}
				}
			}
		}
	}
	CHECK(v92_pcm_frame_rx_init(NULL, NULL, 0) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_rx_init_constellation(NULL, NULL) == V92_PCM_FRAME_INVALID_ARGUMENT,
	      "missing receiver changed initialization result");
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
	CHECK(v92_pcm_frame_encode_owned(
		      &transmitter.mapping, &transmitter.state,
		      bits, transmitter.mapping.data_bits_per_frame,
		      &output) == V92_PCM_FRAME_OUT_OF_RANGE &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &output_saved, sizeof(output)) == 0,
	      "owned encode changed state or output after invalid bits");
	CHECK(v92_pcm_frame_encode_owned(NULL, &transmitter.state,
		      bits, saved.mapping.data_bits_per_frame, &output) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_owned(&transmitter.mapping, NULL,
		      bits, saved.mapping.data_bits_per_frame, &output) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(saved)) == 0 &&
	      memcmp(&output, &output_saved, sizeof(output)) == 0,
	      "missing mapping or coding state changed the frame transaction");
	bits[0] = 0u;
	struct v92_pcm_frame_tx expected = saved;
	struct v92_pcm_frame_tx_state staged = saved.state;
	struct v92_pcm_frame expected_frame;

	CHECK(v92_pcm_frame_encode(&expected, bits,
		      saved.mapping.data_bits_per_frame, &expected_frame) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_encode_owned(&saved.mapping, &staged, bits,
		      saved.mapping.data_bits_per_frame, &output) == V92_PCM_FRAME_OK &&
	      memcmp(&staged, &expected.state, sizeof(staged)) == 0 &&
	      memcmp(&output, &expected_frame, sizeof(output)) == 0 &&
	      memcmp(&saved, &transmitter, sizeof(saved)) == 0,
	      "separate coding state changed the mapping owner or encoded frame");
	output = output_saved;
	transmitter.state.differential = 2u;
	CHECK(v92_pcm_frame_encode(
		      &transmitter, bits,
		      transmitter.mapping.data_bits_per_frame,
		      &output) == V92_PCM_FRAME_INVALID_PARAMETERS,
	      "invalid state encoded a frame");
	CHECK(v92_pcm_frame_scale(saved.mapping.cpd.four_times_gain_q0_16, 0, 123, &sample) ==
		      V92_PCM_FRAME_OUT_OF_RANGE && sample == 1234,
	      "invalid gain changed its output");
	CHECK(v92_pcm_frame_scale(0u, 1, 123, &sample) ==
		      V92_PCM_FRAME_INVALID_PARAMETERS && sample == 1234 &&
	      v92_pcm_frame_scale(1u, -1, 123, &sample) ==
		      V92_PCM_FRAME_OUT_OF_RANGE && sample == 1234 &&
	      v92_pcm_frame_scale(0u, 0, 123, NULL) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT,
	      "invalid scalar gain/level/output changed its contract");
	struct v92_pcm_frame_tx invalid = transmitter;
	struct v92_pcm_frame_tx alias_saved = saved;

	CHECK(v92_pcm_frame_encode_rm(NULL, &saved.state, false, &output) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_rm(&saved.mapping, NULL, false, &output) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_rm(&saved.mapping, &saved.state, true, NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_rm(&saved.mapping, &saved.state, false, (struct v92_pcm_frame *)&saved) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_rm(&saved.mapping, &saved.state, false, (struct v92_pcm_frame *)&saved.state) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_rm(&saved.mapping, (struct v92_pcm_frame_tx_state *)&saved.mapping,
		      false, &output) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_encode_rm(&transmitter.mapping, &transmitter.state, true, &output) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&saved, &alias_saved, sizeof(saved)) == 0 &&
	      memcmp(&transmitter, &invalid, sizeof(transmitter)) == 0 &&
	      memcmp(&output, &output_saved, sizeof(output)) == 0,
	      "invalid RM encoding changed state/output");
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

static bool test_b1u_parity_completion(void)
{
	struct v92_cpd_parameters cpd = parameters(10u);
	const uint8_t moduli[] = { 28, 28, 28, 14, 28, 26, 26, 14, 26, 26, 26, 13 };
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx rx;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint32_t random = 11;
	unsigned verified = 0;

	cpd.four_times_gain_q0_16 = 128;
	memcpy(cpd.modulus, moduli, sizeof(moduli));
	for (unsigned i = 0; i < cpd.constellation[0].count; ++i)
		cpd.constellation[0].positive_linear[i] = (int16_t)((2 * i + 1) * 256);
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
	      "parity-completion mapping did not initialize");
	memset(bits, 1, mapping.data_bits_per_frame);
	for (unsigned n = 0; n < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES; ++n) {
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
		int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];

		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "parity-completion source did not encode");
		for (unsigned symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			/* Twelve uniform bytes give centred integer noise of about
			 * 128 RMS against a 512-unit grid. This seed exposes cheap
			 * prefixes pruning the true path before their expensive
			 * parity symbol arrives. */
			int noise = -1530;

			for (unsigned i = 0; i < 12; ++i) {
				random = random * UINT32_C(1664525) + UINT32_C(1013904223);
				noise += (int)(random >> 24);
			}
			int sample = frame.u[symbol] + noise / 2;

			CHECK(sample > INT16_MIN && sample < INT16_MAX,
			      "parity-completion input clipped");
			samples[symbol] = (int16_t)sample;
		}
		CHECK(v92_pcm_frame_decode(&rx, samples, n < V92_B1U_FRAMES,
			decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
		      "parity-completion receive failed at %u", n);
		if (n < V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES)
			continue;
		CHECK(decision.bits_ready && !decision.ambiguous &&
		      memcmp(decoded, bits, mapping.data_bits_per_frame) == 0,
		      "parity completion lost validation B1u at %u", n);
		++verified;
	}
	CHECK(verified == V92_B1U_FRAMES - V92_B1U_ACQUISITION_FRAMES,
	      "parity-completion validation extent changed");
	return true;
}

static bool test_b1u_validation_history(void)
{
	/* Both complete-window explanations have useful, distinct cases. */
	const uint8_t expected[] = { 0, V92_B1U_SOURCE_MISMATCH,
		V92_B1U_BOTH_MISMATCH, V92_B1U_CODE_MISMATCH, 0, 0 };
	for (unsigned mode = 0; mode < ARRAY_LENGTH(expected); mode++) {
		struct v92_cpd_parameters cpd = parameters(10);
		for (unsigned i = 0; i < cpd.constellation[0].count; i++)
			cpd.constellation[0].positive_linear[i] = (int16_t)((2 * i + 1) * 256);
		struct v92_pcm_mapping mapping;
		struct v92_pcm_frame_tx tx;
		struct v92_pcm_frame_rx rx;
		CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
		      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		      v92_pcm_frame_rx_init_constellation(&rx, &mapping) == V92_PCM_FRAME_OK,
		      "B1 validation histories did not initialize");
		uint8_t mismatches = 0;
		for (unsigned n = 0; n < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES; n++) {
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
			memset(bits, 1, mapping.data_bits_per_frame);
			if ((mode == 2 && n == 40) || (mode == 3 && n == 22)) bits[0] = 0;
			if ((mode == 1 || mode == 2) && n == V92_B1U_ACQUISITION_FRAMES)
				for (unsigned i = 0; i < rx.path_count; i++) rx.paths[i].descrambler.history ^= 1;
			struct v92_pcm_frame frame;
			struct v92_pcm_frame_decision decision;
			CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) == V92_PCM_FRAME_OK,
			      "B1 validation source failed");
			if (mode == 4) {
				/* Another allowed representative with identical trellis labels.
				 * Identity filters leave later points independent of this choice. */
				int eta = frame.signed_index[0];
				eta += eta < 0 ? cpd.modulus[0] : -(int)cpd.modulus[0];
				frame.u[0] = eta < 0 ? (int16_t)-cpd.constellation[0].positive_linear[-eta - 1] :
					cpd.constellation[0].positive_linear[eta];
			}
			enum v92_pcm_frame_result result = mode == 5 && n < V92_B1U_ACQUISITION_FRAMES ?
				v92_pcm_frame_acquire_model(&rx, frame.u, decoded, mapping.data_bits_per_frame, &decision) :
				v92_pcm_frame_decode(&rx, frame.u, n < V92_B1U_FRAMES, decoded,
					mapping.data_bits_per_frame, &decision);
			CHECK(result == V92_PCM_FRAME_OK,
			      "B1 validation receive failed");
			if (mode == 5 && decision.bits_ready &&
			    n < V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES)
				CHECK(decision.b1u_mismatches & V92_B1U_CODE_MISMATCH,
				      "forced acquisition certified its own coded word");
			if (n < V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES) continue;
			CHECK(decision.bits_ready && !decision.ambiguous,
			      "B1 validation lost a delayed word");
			mismatches |= decision.b1u_mismatches;
		}
		CHECK(mismatches == expected[mode], "B1 explanation changed: mode=%u got=%u expected=%u",
		      mode, mismatches, expected[mode]);
		uint16_t pending = rx.paths[0].b1u_code_mismatches;
		CHECK(v92_pcm_frame_rx_reset_control(&rx) == V92_PCM_FRAME_OK &&
		      rx.b1u_frames == V92_B1U_FRAMES && rx.paths[0].b1u_code_mismatches == pending,
		      "control reset lost delayed validation evidence");
	}
	return true;
}

static bool test_b1u_output_range(void)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	unsigned clips = 0, first = V92_B1U_SYMBOLS;

	CHECK(v92_clipping_parameters(&cpd) &&
	      v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK,
	      "captured clipping selection failed to initialize");
	struct v92_pcm_mapping saved = mapping;

	CHECK(!v92_pcm_frame_b1u_fits(&mapping, 2620) &&
	      memcmp(&mapping, &saved, sizeof(mapping)) == 0,
	      "unrepresentable B1u accepted or mapping changed");
	memset(bits, 1, mapping.data_bits_per_frame);
	for (unsigned n = 0; n < V92_B1U_FRAMES; ++n) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "captured B1u encoding failed");
		for (unsigned j = 0; j < V92_PCM_MAPPING_FRAME_SYMBOLS; ++j) {
			int16_t sample;

			CHECK(v92_pcm_frame_scale(cpd.four_times_gain_q0_16, 2620, frame.v[j], &sample) ==
			      V92_PCM_FRAME_OK, "legacy saturated scaling changed");
			if (sample == INT16_MIN || sample == INT16_MAX) {
				if (!clips++) first = n * V92_PCM_MAPPING_FRAME_SYMBOLS + j;
			}
		}
	}
	CHECK(clips == 74 && first == 349, "captured clipping boundary changed: %u/%u", clips, first);
	/* Large feedback alone is legal: the finite-alphabet precoder chooses
	 * equivalent points to limit its output. Do not impose an L1 norm cap. */
	cpd = parameters(1u);
	cpd.constellation[0].count = 64u;
	for (unsigned i = 0; i < 64; ++i)
		cpd.constellation[0].positive_linear[i] = (int16_t)(100 * (i + 1));
	cpd.filter[V92_CPD_PRECODER_FEEDBACK] =
		(struct v92_cpd_filter){ .count = 1, .coefficient = { 20000 } };
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_b1u_fits(&mapping, 1024), "usable large-feedback B1u rejected");
	CHECK(!v92_pcm_frame_b1u_fits(NULL, 1024) && !v92_pcm_frame_b1u_fits(&mapping, 0),
	      "invalid B1u check accepted");
	return true;
}

static bool check_b1u_measurement(
	const struct v92_cpd_parameters *cpd, enum g711_law law)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_b1u_measurement actual, expected = { 0 };
	struct v92_cpd_parameters saved = *cpd;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];

	CHECK(v92_pcm_mapping_init(&mapping, law, cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_measure_b1u(cpd, law, &actual) == V92_PCM_FRAME_OK,
	      "B1u measurement did not initialize");
	memset(ones, 1, mapping.data_bits_per_frame);
	/* Compare the owned measurement with the public checked encoder, using
	 * its independent state and all 48 frames, including filter history. */
	for (unsigned index = 0; index < V92_B1U_FRAMES; ++index) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode(&tx, ones, mapping.data_bits_per_frame, &frame) ==
		      V92_PCM_FRAME_OK, "checked B1u encoding failed");
		for (unsigned symbol = 0; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int slot = frame.signed_index[symbol] + V92_CPD_MAX_CONSTELLATION_POINTS;

			CHECK(slot >= 0 && slot < (int)ARRAY_LENGTH(expected.signed_frequencies),
			      "checked B1u index escaped its constellation");
			expected.point_energy += (uint64_t)((int64_t)frame.u[symbol] * frame.u[symbol]);
			expected.prefilter_energy += (uint64_t)((int64_t)frame.v[symbol] * frame.v[symbol]);
			expected.signed_frequencies[slot]++;
		}
	}
	CHECK(actual.point_energy == expected.point_energy &&
	      actual.prefilter_energy == expected.prefilter_energy &&
	      memcmp(actual.signed_frequencies, expected.signed_frequencies,
		     sizeof(actual.signed_frequencies)) == 0 &&
	      memcmp(cpd, &saved, sizeof(saved)) == 0,
	      "owned B1u measurement differs from checked encoding or changes CPd");
	return true;
}

static bool test_b1u_measurement(void)
{
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t rate = 1; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate)
			for (unsigned code = V92_CPD_TRELLIS_16_STATE;
			     code <= V92_CPD_TRELLIS_64_STATE; ++code)
				for (unsigned filtered = 0; filtered < 2; ++filtered) {
					struct v92_cpd_parameters cpd = parameters(rate);

					cpd.trellis = (enum v92_cpd_trellis)code;
					if (filtered) {
						cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD] =
							(struct v92_cpd_filter){ .count = 3,
								.coefficient = { 24576, -8192, 4096 } };
						cpd.filter[V92_CPD_PRECODER_FEEDBACK] =
							(struct v92_cpd_filter){ .count = 2,
								.coefficient = { 8192, -4096 } };
					}
					CHECK(check_b1u_measurement(&cpd, (enum g711_law)law),
					      "B1u measurement rate %u law %u code %u filter %u failed",
					      rate, law, code, filtered);
				}
	return true;
}

static bool test_b1u_measurement_contracts(void)
{
	struct v92_cpd_parameters cpd = parameters(1u);
	struct v92_b1u_measurement actual, saved;

	/* Zero prefilter energy is representable; gain selection owns its use. */
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD] = (struct v92_cpd_filter){ .count = 1 };
	CHECK(v92_pcm_frame_measure_b1u(&cpd, G711_LAW_PCMU, &actual) == V92_PCM_FRAME_OK &&
	      actual.point_energy > 0 && actual.prefilter_energy == 0,
	      "zero-energy B1u measurement changed");
	memset(&actual, 0x5a, sizeof(actual));
	memcpy(&saved, &actual, sizeof(saved));
	cpd = parameters(1u);
	CHECK(v92_pcm_frame_measure_b1u(NULL, G711_LAW_PCMU, &actual) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      memcmp(&actual, &saved, sizeof(actual)) == 0 &&
	      v92_pcm_frame_measure_b1u(&cpd, G711_LAW_PCMU, NULL) == V92_PCM_FRAME_INVALID_ARGUMENT &&
	      v92_pcm_frame_measure_b1u(&cpd, (enum g711_law)99, &actual) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&actual, &saved, sizeof(actual)) == 0,
	      "invalid B1u measurement changed output");
	cpd.data_rate_number = 0;
	CHECK(v92_pcm_frame_measure_b1u(&cpd, G711_LAW_PCMU, &actual) == V92_PCM_FRAME_INVALID_PARAMETERS &&
	      memcmp(&actual, &saved, sizeof(actual)) == 0, "invalid CPd changed measurement");
	cpd = parameters(1u);
	cpd.filter[V92_CPD_PREFILTER_FEEDBACK] = (struct v92_cpd_filter){
		.count = 2, .coefficient = { INT16_MIN, INT16_MIN } };
	CHECK(v92_pcm_frame_measure_b1u(&cpd, G711_LAW_PCMU, &actual) == V92_PCM_FRAME_OUT_OF_RANGE &&
	      memcmp(&actual, &saved, sizeof(actual)) == 0, "unrepresentable energy changed output");
	union {
		struct v92_cpd_parameters parameters;
		struct v92_b1u_measurement measurement;
	} aliased, original;
	memset(&aliased, 0x5a, sizeof(aliased));
	aliased.parameters = parameters(1u);
	memcpy(&original, &aliased, sizeof(original));
	CHECK(v92_pcm_frame_measure_b1u(&aliased.parameters, G711_LAW_PCMU, &aliased.measurement) ==
		      V92_PCM_FRAME_INVALID_ARGUMENT && memcmp(&aliased, &original, sizeof(aliased)) == 0,
	      "overlapping B1u measurement changed CPd");
	return true;
}

int main(void)
{
	if (!test_b1u_measurement() || !test_b1u_measurement_contracts() ||
	    !test_b1u_output_range() || !test_b1u_oracle() || !test_b1u_coding_acquisition() ||
	    !test_b1u_parity_completion() || !test_b1u_validation_history() ||
	    !test_model_acquisition_boundary() || !test_rate_matrix_and_reset() ||
	    !test_transmitter_equation_matrix() ||
	    !test_filters() || !test_causal_filter_history() ||
	    !test_delayed_filter_stream() || !test_level_refinement() ||
	    !test_pending_bit_validation() || !test_receiver_initialization() ||
	    !test_rejection_and_names())
		return 1;
	return 0;
}
