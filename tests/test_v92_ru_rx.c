// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_phase3_ru.h>
#include <v92_ru_rx.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

static double ru_level(unsigned phase, double lu)
{
	return phase % 6u < 3u ? lu : -lu;
}

static bool test_transmitted_sequences(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (unsigned ucode = 1; ucode <= 127; ++ucode) {
			/* Include the latest possible acquisition, every frame
			 * phase, and both sides of the half-frame ambiguity. */
			for (unsigned skip = 0; skip <= 144; ++skip) {
				struct v92_pcm_phase3_ru tx;
				struct v92_ru_rx rx, completed;
				struct v90_pcm_symbol symbol;
				CHECK(v90_pcm_symbol_make(&symbol, law, ucode, true),
				      "invalid test LU");
				CHECK(v92_ru_rx_init(&rx, symbol.sample, skip % 12u),
				      "Ru receiver initialization failed");
				CHECK(v92_pcm_phase3_ru_init(&tx, law, ucode,
					V92_PCM_PHASE3_RU) == V92_PCM_PHASE3_RU_OK,
				      "Ru transmitter initialization failed");
				for (unsigned i = 0; i < 384; ++i) {
					CHECK(v92_pcm_phase3_ru_next(&tx, &symbol) ==
						V92_PCM_PHASE3_RU_OK, "missing Ru symbol");
					if (i < skip)
						continue;
					/* Bounded gain error and alternating level noise. */
					double scale = 0.9 + (i % 3u) * 0.1;
					enum v92_ru_rx_result expected = i - skip == 239 ?
						V92_RU_RX_DETECTED : V92_RU_RX_BUFFERING;
					CHECK(v92_ru_rx_sample(&rx, symbol.sample * scale) ==
						expected && v92_ru_rx_is_valid(&rx),
					      "Ru acquisition failed: law=%u LU=%u skip=%u i=%u",
					      law, ucode, skip, i);
				}
				CHECK(v92_pcm_phase3_ru_init(&tx, law, ucode,
					V92_PCM_PHASE3_RU_BAR) == V92_PCM_PHASE3_RU_OK,
				      "Ru-bar transmitter initialization failed");
				for (unsigned i = 0; i < 24; ++i) {
					enum v92_ru_rx_result expected = i == 0 ?
						V92_RU_RX_BAR_STARTED : i == 23 ?
						V92_RU_RX_COMPLETE : V92_RU_RX_BUFFERING;
					CHECK(v92_pcm_phase3_ru_next(&tx, &symbol) ==
						V92_PCM_PHASE3_RU_OK &&
					      v92_ru_rx_sample(&rx, symbol.sample) == expected &&
					      v92_ru_rx_is_valid(&rx), "Ru-bar boundary lost");
				}
				completed = rx;
				CHECK(v92_ru_rx_sample(&rx, 1234) == V92_RU_RX_COMPLETE &&
				      memcmp(&rx, &completed, sizeof(rx)) == 0,
				      "completed Ru receiver consumed the TRN2u tail");
			}
		}
	}
	return true;
}

static bool acquire(struct v92_ru_rx *rx)
{
	CHECK(v92_ru_rx_init(rx, 1000, 0), "init failed");
	for (unsigned i = 0; i < 240; ++i)
		CHECK(v92_ru_rx_sample(rx, ru_level(i, 1000)) ==
		      (i == 239 ? V92_RU_RX_DETECTED : V92_RU_RX_BUFFERING),
		      "test acquisition failed");
	return true;
}

static bool test_rejection_and_recovery(void)
{
	struct v92_ru_rx rx;
	for (unsigned pattern = 0; pattern < 6; ++pattern) {
		CHECK(v92_ru_rx_init(&rx, 1000, 0), "init failed");
		for (unsigned i = 0; i < 800; ++i) {
			double level = pattern == 0 ? 0 : pattern == 1 ? 1000 :
				pattern == 2 ? ru_level(i, 500) :
				pattern == 3 ? ru_level(i, 1500) :
				pattern == 4 ? ru_level(i + 3, 1000) :
				(i % 4u < 2u ? 1000 : -1000);
			CHECK(v92_ru_rx_sample(&rx, level) == V92_RU_RX_BUFFERING &&
			      rx.run < V92_RU_RX_MIN_SYMBOLS,
			      "false Ru lock on silence/mark/wrong level/polarity/Rf");
		}
	}
	/* A six-symbol shift has identical Ru signs but the wrong 12-symbol
	 * frame boundary. Do not let a polarity reversal move the frame. */
	for (unsigned shift = 1; shift < 12; ++shift) {
		CHECK(acquire(&rx), "acquisition failed");
		for (unsigned i = 0; i < shift; ++i)
			CHECK(v92_ru_rx_sample(&rx, ru_level(i, 1000)) ==
			      V92_RU_RX_BUFFERING, "lost Ru");
		CHECK(v92_ru_rx_sample(&rx, -ru_level(shift, 1000)) ==
		      V92_RU_RX_MISMATCH && rx.phase == (shift + 1u) % 12u,
		      "accepted off-frame reversal or moved alignment");
	}
	for (unsigned corrupt = 0; corrupt < 24; ++corrupt) {
		CHECK(acquire(&rx), "acquisition failed");
		for (unsigned i = 0; i < corrupt; ++i)
			CHECK(v92_ru_rx_sample(&rx, -ru_level(i, 1000)) ==
			      (i == 0 ? V92_RU_RX_BAR_STARTED : V92_RU_RX_BUFFERING),
			      "lost bar");
		CHECK(v92_ru_rx_sample(&rx, 0) == V92_RU_RX_MISMATCH &&
		      rx.run == 0 && rx.bar_symbols == 0 &&
		      rx.phase == (corrupt + 1u) % 12u, "accepted damaged bar");
		for (unsigned i = 0; i < 240; ++i)
			CHECK(v92_ru_rx_sample(&rx, ru_level(rx.phase, 1000)) ==
			      (i == 239 ? V92_RU_RX_DETECTED : V92_RU_RX_BUFFERING),
			      "failed fresh acquisition after damaged bar");
	}
	return true;
}

static bool test_rippled_acquisition(void)
{
	/* A coherent tone need not have a flat per-symbol envelope after
	 * channel equalization. Retain its signs and original frame phase. */
	for (unsigned skip = 0; skip < 12; ++skip) {
		struct v92_ru_rx rx;
		unsigned detected = 0;
		CHECK(v92_ru_rx_init(&rx, 1000, skip), "init failed");
		for (unsigned i = skip; i < 384; ++i) {
			double scale = i % 3u == 1u ? 1.3 : 0.7;
			scale += i % 2u ? 0.04 : -0.04;
			enum v92_ru_rx_result result = v92_ru_rx_sample(&rx, ru_level(i, 1000 * scale));
			detected += result == V92_RU_RX_DETECTED;
			CHECK(result == V92_RU_RX_BUFFERING || result == V92_RU_RX_DETECTED,
			      "lost coherent rippled Ru");
			CHECK(i - skip >= 239 || !detected, "ripple shortened qualification");
			CHECK(v92_ru_rx_is_valid(&rx), "invalid rippled receiver");
		}
		CHECK(detected == 1 && rx.phase == 0, "rippled Ru not acquired");
		for (unsigned i = 0; i < 24; ++i)
			CHECK(v92_ru_rx_sample(&rx, -ru_level(i, 1000)) ==
			      (i == 0 ? V92_RU_RX_BAR_STARTED : i == 23 ? V92_RU_RX_COMPLETE : V92_RU_RX_BUFFERING),
			      "ripple moved reversal/training boundary");
	}
	/* Amplitude smoothing must not hide even one wrong-polarity symbol
	 * or let previous matched samples shorten fresh qualification. */
	for (unsigned corrupt = 0; corrupt < 240; ++corrupt) {
		struct v92_ru_rx rx;
		CHECK(v92_ru_rx_init(&rx, 1000, 0), "init failed");
		for (unsigned i = 0; i < corrupt; ++i)
			CHECK(v92_ru_rx_sample(&rx, ru_level(rx.phase, 1000)) ==
			      V92_RU_RX_BUFFERING, "early acquisition");
		CHECK(v92_ru_rx_sample(&rx, -ru_level(rx.phase, 1000)) ==
		      V92_RU_RX_BUFFERING && rx.run == 0 && rx.mean_level == 0,
		      "smoothed over a polarity error");
		for (unsigned i = 0; i < 240; ++i)
			CHECK(v92_ru_rx_sample(&rx, ru_level(rx.phase, 1000)) ==
			      (i == 239 ? V92_RU_RX_DETECTED : V92_RU_RX_BUFFERING),
			      "polarity error reused acquisition history");
	}
	return true;
}

static bool test_invalid_inputs(void)
{
	struct v92_ru_rx rx, before;
	CHECK(acquire(&rx), "acquisition failed");
	before = rx;
	CHECK(!v92_ru_rx_init(&rx, 0, 0) && !v92_ru_rx_init(&rx, -1, 0) &&
	      !v92_ru_rx_init(&rx, 1000, 12) && !v92_ru_rx_init(NULL, 1000, 0) &&
	      v92_ru_rx_sample(NULL, 0) == V92_RU_RX_INVALID_ARGUMENT &&
	      v92_ru_rx_sample(&rx, NAN) == V92_RU_RX_INVALID_ARGUMENT &&
	      v92_ru_rx_sample(&rx, INFINITY) == V92_RU_RX_INVALID_ARGUMENT &&
	      v92_ru_rx_sample(&rx, -INFINITY) == V92_RU_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &before, sizeof(rx)) == 0, "invalid input mutated state");
	rx.bar_symbols = 1; /* Contradicts the established frame phase. */
	before = rx;
	CHECK(!v92_ru_rx_is_valid(&rx) &&
	      v92_ru_rx_sample(&rx, 1000) == V92_RU_RX_INVALID_STATE &&
	      memcmp(&rx, &before, sizeof(rx)) == 0, "invalid state mutated");
	const double invalid_levels[] = { -1, NAN, INFINITY };
	for (unsigned i = 0; i < ARRAY_LENGTH(invalid_levels); ++i) {
		CHECK(v92_ru_rx_init(&rx, 1000, 0), "init failed");
		rx.mean_level = invalid_levels[i];
		before = rx;
		CHECK(!v92_ru_rx_is_valid(&rx) &&
		      v92_ru_rx_sample(&rx, 1000) == V92_RU_RX_INVALID_STATE &&
		      memcmp(&rx, &before, sizeof(rx)) == 0, "invalid amplitude state mutated");
	}
	CHECK(v92_ru_rx_init(&rx, 1000, 0), "init failed");
	for (unsigned i = 0; i < 800; ++i)
		CHECK(v92_ru_rx_sample(&rx, ru_level(i, DBL_MAX)) == V92_RU_RX_BUFFERING &&
		      v92_ru_rx_is_valid(&rx), "finite overload acquired or overflowed");
	return true;
}

static bool test_acquisition_limits(void)
{
	struct v92_ru_rx rx;
	CHECK(v92_ru_rx_init(&rx, 1000, 1), "init failed");
	/* Ending at the right boundary is insufficient without a full run. */
	for (unsigned i = 0; i < 239; ++i)
		CHECK(v92_ru_rx_sample(&rx, ru_level(rx.phase, 1000)) ==
		      V92_RU_RX_BUFFERING, "short Ru acquired");
	CHECK(rx.phase == 0, "test frame phase lost");
	for (unsigned i = 0; i < 24; ++i)
		CHECK(v92_ru_rx_sample(&rx, -ru_level(i, 1000)) ==
		      V92_RU_RX_BUFFERING && rx.bar_symbols == 0,
		      "short Ru accepted before bar");
	CHECK(v92_ru_rx_init(&rx, 1000, 0), "init failed");
	for (unsigned i = 0; i < 240; ++i)
		CHECK(v92_ru_rx_sample(&rx, ru_level(i, i % 2 ? 750 : 1250)) ==
		      (i == 239 ? V92_RU_RX_DETECTED : V92_RU_RX_BUFFERING),
		      "inclusive LU tolerance failed");
	CHECK(v92_ru_rx_sample(&rx, -749) == V92_RU_RX_MISMATCH,
	      "accepted Ru-bar outside the LU tolerance");
	CHECK(v92_ru_rx_init(&rx, 1000, 0), "init failed");
	uint32_t state = 1;
	for (unsigned i = 0; i < 8000; ++i) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		CHECK(v92_ru_rx_sample(&rx, state & 1u ? 1000 : -1000) ==
		      V92_RU_RX_BUFFERING, "false lock on unrelated two-point data");
	}
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] frame-aligned V.92 Ru reception\n");
	if (!test_transmitted_sequences() || !test_rejection_and_recovery() ||
	    !test_invalid_inputs() || !test_acquisition_limits() || !test_rippled_acquisition()) {
		fprintf(stderr, "[  FAILED  ] frame-aligned V.92 Ru reception\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] frame-aligned V.92 Ru reception\n");
	return 0;
}
