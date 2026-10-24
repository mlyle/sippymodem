// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_echo_reference.h>

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
	DEFAULT_RETURN_DELAY = 2576,
	FILTER_PRE_SAMPLES = 64,
	SHORT_FILTER_DELAY = 337,
	LONG_RETURN_DELAY = 5600,
	LONG_FILTER_DELAY = LONG_RETURN_DELAY - FILTER_PRE_SAMPLES,
};

static int16_t reference_sample(size_t index)
{
	uint32_t value = (uint32_t)index + UINT32_C(0x6d2b79f5);

	value ^= value >> 15;
	value *= UINT32_C(0x2c1b3c6d);
	value ^= value >> 12;
	value *= UINT32_C(0x297a2d39);
	value ^= value >> 15;
	return (int16_t)((int32_t)(value % 16001u) - 8000);
}

static int16_t returned_sample(size_t index)
{
	int32_t echo;
	int32_t remote;
	int32_t mixed;

	if (index < LONG_RETURN_DELAY)
		return 0;
	echo = reference_sample(index - LONG_RETURN_DELAY) / 2;
	remote = reference_sample(index + 17011u) / 32;
	mixed = echo + remote;
	return mixed > INT16_MAX ? INT16_MAX :
	       mixed < INT16_MIN ? INT16_MIN : (int16_t)mixed;
}

static bool test_ring_and_contracts(void)
{
	struct pcm_echo_reference reference = { 0 };
	struct pcm_echo_reference saved;
	struct pcm_echo_reference_scan_result scan = {
		.best_window_delay_samples = 1234,
	};
	struct pcm_echo_reference_scan_result saved_scan = scan;
	int16_t window[3] = { 11, 22, 33 };
	int16_t output = 1234;

	CHECK(pcm_echo_reference_is_valid(&reference),
	      "zero-initialized owner is not valid");
	CHECK(pcm_echo_reference_push(NULL, 0) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE &&
	      pcm_echo_reference_read_at_age(NULL, 1, &output) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE &&
	      pcm_echo_reference_start_scan(
		      NULL, DEFAULT_RETURN_DELAY, FILTER_PRE_SAMPLES) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE &&
	      pcm_echo_reference_scan_observe(NULL, 0, &scan) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE,
	      "null live owner was accepted");
	CHECK(pcm_echo_reference_scan_observe(&reference, 0, &scan) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE &&
	      memcmp(&scan, &saved_scan, sizeof(scan)) == 0,
	      "unconfigured scan was accepted or changed output");
	CHECK(pcm_echo_reference_read_at_age(&reference, 1, &output) ==
		      PCM_ECHO_REFERENCE_NOT_READY && output == 1234,
	      "empty history changed output");
	CHECK(pcm_echo_reference_read_at_age(&reference, 0, &output) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT && output == 1234,
	      "zero age was accepted");
	CHECK(pcm_echo_reference_read_at_age(
		      &reference, PCM_ECHO_REFERENCE_HISTORY_SAMPLES + 1u,
		      &output) == PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
		      output == 1234,
	      "out-of-range age was accepted");
	CHECK(pcm_echo_reference_read_at_age(
		      &reference, 1, (int16_t *)&reference) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT,
	      "owner/output overlap was accepted");

	for (size_t index = 0;
	     index < PCM_ECHO_REFERENCE_HISTORY_SAMPLES + 7u; ++index)
		CHECK(pcm_echo_reference_push(
			      &reference, reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK,
		      "history push failed at %zu", index);
	CHECK(pcm_echo_reference_read_at_age(&reference, 1, &output) ==
		      PCM_ECHO_REFERENCE_OK &&
		      output == reference_sample(
			      PCM_ECHO_REFERENCE_HISTORY_SAMPLES + 6u),
	      "newest wrapped sample changed");
	CHECK(pcm_echo_reference_read_at_age(
		      &reference, PCM_ECHO_REFERENCE_HISTORY_SAMPLES,
		      &output) == PCM_ECHO_REFERENCE_OK &&
		      output == reference_sample(7),
	      "oldest wrapped sample changed");
	CHECK(pcm_echo_reference_read_window(
		      &reference, 1, window, 3) == PCM_ECHO_REFERENCE_OK &&
	      window[0] == reference_sample(
		      PCM_ECHO_REFERENCE_HISTORY_SAMPLES + 6u) &&
	      window[1] == reference_sample(
		      PCM_ECHO_REFERENCE_HISTORY_SAMPLES + 5u) &&
	      window[2] == reference_sample(
		      PCM_ECHO_REFERENCE_HISTORY_SAMPLES + 4u),
	      "wrapped reference window changed");
	CHECK(pcm_echo_reference_read_window(
		      &reference, 1, NULL, 3) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
	      pcm_echo_reference_read_window(
		      &reference, 1, window, 0) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
	      pcm_echo_reference_read_window(
		      &reference, 1, (int16_t *)&reference, 3) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT,
	      "invalid reference window was accepted");

	saved = reference;
	CHECK(pcm_echo_reference_start_scan(&reference, 0,
					    FILTER_PRE_SAMPLES) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
		      memcmp(&reference, &saved, sizeof(saved)) == 0,
	      "zero-delay scan changed owner");
	CHECK(pcm_echo_reference_start_scan(
		      &reference,
		      PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES + 1u,
		      FILTER_PRE_SAMPLES) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
		      memcmp(&reference, &saved, sizeof(saved)) == 0,
	      "oversized-delay scan changed owner");
	CHECK(pcm_echo_reference_start_scan(
		      &reference, DEFAULT_RETURN_DELAY,
		      PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
		      memcmp(&reference, &saved, sizeof(saved)) == 0,
	      "oversized pre-window scan changed owner");
	CHECK(pcm_echo_reference_start_scan(
		      &reference, DEFAULT_RETURN_DELAY,
		      FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK &&
		      pcm_echo_reference_is_valid(&reference),
	      "ordinary scan did not start");
	CHECK(pcm_echo_reference_scan_observe(&reference, 0, NULL) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT,
	      "null scan result was accepted");
	CHECK(pcm_echo_reference_scan_observe(
		      &reference, 0,
		      (struct pcm_echo_reference_scan_result *)&reference) ==
		      PCM_ECHO_REFERENCE_INVALID_ARGUMENT,
	      "owner/scan-result overlap was accepted");

	reference.write = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	CHECK(!pcm_echo_reference_is_valid(&reference) &&
	      pcm_echo_reference_push(&reference, 0) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE &&
	      pcm_echo_reference_scan_observe(&reference, 0, &scan) ==
		      PCM_ECHO_REFERENCE_INVALID_STATE &&
	      memcmp(&scan, &saved_scan, sizeof(scan)) == 0,
	      "corrupt owner was accepted or changed scan output");
	return true;
}

static bool test_long_delay_scan(void)
{
	struct pcm_echo_reference reference = { 0 };
	struct pcm_echo_reference_scan_result result = { 0 };
	struct pcm_echo_reference_scan_result unchanged = {
		.best_window_delay_samples = 77,
		.current_window_delay_samples = 88,
		.sample_count = 99,
	};
	size_t index = 0;
	bool acquired = false;

	CHECK(pcm_echo_reference_start_scan(
		      &reference, SHORT_FILTER_DELAY,
		      FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK,
	      "long-delay scan did not initialize");
	CHECK(reference.scan_count + reference.scan_pre_samples >= LONG_RETURN_DELAY &&
	      reference.scan_count + reference.scan_pre_samples <=
		      PCM_ECHO_REFERENCE_HISTORY_SAMPLES,
	      "scan fill boundary does not cover long return: %u",
	      reference.scan_count + reference.scan_pre_samples);

	while (!acquired &&
	       index < PCM_ECHO_REFERENCE_HISTORY_SAMPLES) {
		enum pcm_echo_reference_result scan_result;
		struct pcm_echo_reference_scan_result report = unchanged;

		scan_result = pcm_echo_reference_scan_observe(
			&reference, returned_sample(index), &report);
		if (reference.fill < reference.scan_count + reference.scan_pre_samples)
			CHECK(scan_result == PCM_ECHO_REFERENCE_NOT_READY &&
			      memcmp(&report, &unchanged, sizeof(report)) == 0,
			      "prefill scan changed output at %zu", index);
		else if (scan_result == PCM_ECHO_REFERENCE_SCAN_READY) {
			result = report;
			acquired = true;
		} else {
			CHECK(scan_result == PCM_ECHO_REFERENCE_OK &&
			      memcmp(&report, &unchanged, sizeof(report)) == 0,
			      "ordinary scan changed output at %zu", index);
		}
		CHECK(pcm_echo_reference_push(
			      &reference, reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK,
		      "scan reference push failed at %zu", index);
		index++;
	}
	CHECK(acquired, "700 ms return delay was not acquired");
	CHECK(result.best_window_delay_samples == LONG_FILTER_DELAY,
	      "long-delay scan selected %u instead of %u",
	      result.best_window_delay_samples, LONG_FILTER_DELAY);
	CHECK(result.current_window_delay_samples ==
		      SHORT_FILTER_DELAY,
	      "scan lost current delay: %u",
	      result.current_window_delay_samples);
	CHECK(result.sample_count == PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES,
	      "first scan report used %u samples", result.sample_count);
	CHECK(result.best_correlation_ppm > 400000u &&
	      result.current_correlation_ppm < 100000u,
	      "scan separation is weak: best=%u current=%u",
	      result.best_correlation_ppm,
	      result.current_correlation_ppm);

	while (reference.scan_samples < PCM_ECHO_REFERENCE_SCAN_CADENCE * 4u) {
		struct pcm_echo_reference_scan_result report = unchanged;
		struct pcm_echo_reference saved = reference;

		/* A rejected observation cannot consume the next report boundary.
		 * Retry it immediately with valid output on the same physical input. */
		CHECK(pcm_echo_reference_scan_observe(
			      &reference, returned_sample(index), NULL) ==
			      PCM_ECHO_REFERENCE_INVALID_ARGUMENT &&
		      memcmp(&reference, &saved, sizeof(reference)) == 0,
		      "rejected scan observation changed its clock or evidence");
		enum pcm_echo_reference_result scan_result =
			pcm_echo_reference_scan_observe(
				&reference, returned_sample(index), &report);

		CHECK(pcm_echo_reference_push(
			      &reference, reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK,
		      "continued scan push failed at %zu", index);
		index++;
		if (reference.scan_samples % PCM_ECHO_REFERENCE_SCAN_CADENCE == 0) {
			CHECK(scan_result == PCM_ECHO_REFERENCE_SCAN_READY &&
			      report.sample_count ==
				      reference.scan_samples &&
			      report.best_window_delay_samples ==
				      LONG_FILTER_DELAY,
			      "cadenced scan report changed: result=%d samples=%u "
			      "best=%u scan=%u",
			      scan_result, report.sample_count,
			      report.best_window_delay_samples,
			      reference.scan_samples);
		} else {
			CHECK(scan_result == PCM_ECHO_REFERENCE_OK &&
			      memcmp(&report, &unchanged, sizeof(report)) == 0,
			      "intermediate scan published output");
		}
	}
	return true;
}

static bool test_cluster_scan(uint32_t first_return, uint32_t pre_samples)
{
	struct pcm_echo_reference reference = { 0 };
	struct pcm_echo_reference_scan_result scan;
	const uint32_t last_return = first_return + 254u;
	const uint32_t current = last_return > pre_samples ? last_return - pre_samples : 1u;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;

	CHECK(pcm_echo_reference_start_scan(&reference, current, pre_samples) == PCM_ECHO_REFERENCE_OK,
	      "cluster scan did not initialize");
	for (size_t index = 0; index < history; ++index)
		CHECK(pcm_echo_reference_push(&reference, reference_sample(index)) == PCM_ECHO_REFERENCE_OK,
		      "cluster history did not fill");
	for (size_t index = history; index < history + PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES; ++index) {
		/* A later, stronger return must not hide earlier comparable paths. */
		int16_t received = reference_sample(index - first_return) / 4 +
			reference_sample(index - first_return - 126u) / 4 +
			3 * reference_sample(index - last_return) / 8;
		enum pcm_echo_reference_result result =
			pcm_echo_reference_scan_observe(&reference, received, &scan);
		CHECK(result == (index + 1u == history + PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES ?
			PCM_ECHO_REFERENCE_SCAN_READY : PCM_ECHO_REFERENCE_OK) &&
		      pcm_echo_reference_push(&reference, reference_sample(index)) == PCM_ECHO_REFERENCE_OK,
		      "cluster scan failed at %zu", index);
	}
	CHECK(scan.best_window_delay_samples <= first_return &&
	      scan.best_window_delay_samples + PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES > last_return &&
	      scan.current_window_delay_samples == current && scan.best_correlation_ppm > 100000u,
	      "cluster window missed returns %u..%u: pre=%u window=%u strength=%u",
	      first_return, last_return, pre_samples, scan.best_window_delay_samples, scan.best_correlation_ppm);
	return true;
}

static bool test_opposite_adjacent_returns(void)
{
	struct pcm_echo_reference reference = { 0 };
	struct pcm_echo_reference_scan_result scan;
	const uint32_t delay = 2679u;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;

	CHECK(pcm_echo_reference_start_scan(&reference, 1u, FILTER_PRE_SAMPLES) ==
		      PCM_ECHO_REFERENCE_OK,
	      "opposite return scan did not initialize");
	for (size_t index = 0; index < history; ++index)
		CHECK(pcm_echo_reference_push(&reference, reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK, "history did not fill");
	for (size_t index = history;
	     index < history + PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES; ++index) {
		/* A two-tap high-pass return cancels against the average of the
		 * same adjacent TX samples, despite having a strong linear echo. */
		int16_t received = (int16_t)(
			(reference_sample(index - delay) -
			 reference_sample(index - delay - 1u)) / 4);
		enum pcm_echo_reference_result result =
			pcm_echo_reference_scan_observe(&reference, received, &scan);

		CHECK(result == (index + 1u == history +
			      PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES ?
			      PCM_ECHO_REFERENCE_SCAN_READY : PCM_ECHO_REFERENCE_OK) &&
		      pcm_echo_reference_push(&reference, reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK,
		      "opposite return scan failed at %zu", index);
	}
	CHECK(scan.best_window_delay_samples <= delay &&
	      scan.best_window_delay_samples + PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES >
		      delay + 1u && scan.best_correlation_ppm > 400000u,
	      "adjacent returns were hidden: window=%u strength=%u",
	      scan.best_window_delay_samples, scan.best_correlation_ppm);
	return true;
}

static bool test_full_history_scan_boundary(void)
{
	struct pcm_echo_reference reference = { 0 }, saved;
	struct pcm_echo_reference_scan_result report = { .sample_count = 123 };
	const struct pcm_echo_reference_scan_result unchanged = report;
	const unsigned maximum = PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES;
	const unsigned pre = PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES - 1u;
	const unsigned history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	const int16_t received = -901;

	CHECK(pcm_echo_reference_start_scan(&reference, maximum, pre) ==
		      PCM_ECHO_REFERENCE_OK &&
	      pcm_echo_reference_current_delay(&reference) == maximum,
	      "maximum scan did not initialize");
	for (unsigned i = 0; i + 1u < history; ++i)
		CHECK(pcm_echo_reference_push(&reference, reference_sample(i)) ==
			      PCM_ECHO_REFERENCE_OK, "history stopped at %u", i);
	saved = reference;
	CHECK(pcm_echo_reference_scan_observe(&reference, received, &report) ==
		      PCM_ECHO_REFERENCE_NOT_READY &&
	      memcmp(&reference, &saved, sizeof(reference)) == 0 &&
	      memcmp(&report, &unchanged, sizeof(report)) == 0,
	      "maximum scan consumed an unavailable oldest sample");
	CHECK(pcm_echo_reference_push(&reference, reference_sample(history - 1u)) ==
		      PCM_ECHO_REFERENCE_OK &&
	      pcm_echo_reference_scan_observe(&reference, received, &report) ==
		      PCM_ECHO_REFERENCE_OK && reference.scan_samples == 1u &&
	      reference.scan_reference_energy[maximum - 1u] ==
		      (uint64_t)((int64_t)reference_sample(0) * reference_sample(0)) &&
	      reference.scan_cross_energy[maximum - 1u] ==
		      (int64_t)reference_sample(0) * received &&
	      memcmp(&report, &unchanged, sizeof(report)) == 0,
	      "maximum scan did not use the oldest retained sample exactly");
	reference.scan_current_index = maximum;
	CHECK(!pcm_echo_reference_is_valid(&reference),
	      "out-of-range current candidate was accepted");
	return true;
}

static bool test_dc_only_scan(bool constant_reference, int16_t constant)
{
	struct pcm_echo_reference reference = { 0 };
	const struct pcm_echo_reference_scan_result unchanged = { .sample_count = 123 };
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;

	CHECK(pcm_echo_reference_start_scan(&reference, DEFAULT_RETURN_DELAY,
		      FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK,
	      "DC scan did not initialize");
	for (size_t index = 0; index < history; ++index)
		CHECK(pcm_echo_reference_push(&reference,
			      constant_reference ? constant : reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK, "DC history did not fill");
	for (size_t index = history; index < history + 2048u; ++index) {
		struct pcm_echo_reference_scan_result report = unchanged;
		int16_t received = constant_reference ?
			(int16_t)(1200 + reference_sample(index) / 8) : constant;

		CHECK(pcm_echo_reference_scan_observe(&reference, received, &report) ==
			      PCM_ECHO_REFERENCE_OK &&
		      memcmp(&report, &unchanged, sizeof(report)) == 0,
		      "constant %s %d invented delay evidence at %zu",
		      constant_reference ? "reference" : "input", constant, index);
		CHECK(pcm_echo_reference_push(&reference,
			      constant_reference ? constant : reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK, "DC history stopped");
	}
	return true;
}

static bool test_biased_return_scan(int sign)
{
	struct pcm_echo_reference reference = { 0 };
	struct pcm_echo_reference_scan_result scan;
	const size_t history = PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	const unsigned delay = 2405;

	CHECK(pcm_echo_reference_start_scan(&reference, DEFAULT_RETURN_DELAY,
		      FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK,
	      "biased scan did not initialize");
	for (size_t index = 0; index < history; ++index)
		CHECK(pcm_echo_reference_push(&reference, index < 6000u ? 8 :
			      (int16_t)(3000 + reference_sample(index))) == PCM_ECHO_REFERENCE_OK,
		      "biased history did not fill");
	for (size_t index = history;
	     index < history + PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES; ++index) {
		/* A weak return lies just before the current filter window. Old
		 * A-law silence (+8) has a stronger uncentered score because the
		 * received signal also has DC; it supplies no delay evidence. */
		int16_t received = (int16_t)(sign * 1200 +
			reference_sample(index - delay) / 16 +
			reference_sample(index + 17011u) / 8);
		enum pcm_echo_reference_result result =
			pcm_echo_reference_scan_observe(&reference, received, &scan);

		CHECK(result == (index + 1u == history + PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES ?
			      PCM_ECHO_REFERENCE_SCAN_READY : PCM_ECHO_REFERENCE_OK) &&
		      pcm_echo_reference_push(&reference,
			      (int16_t)(3000 + reference_sample(index))) == PCM_ECHO_REFERENCE_OK,
		      "biased scan failed at %zu", index);
	}
	CHECK(scan.best_window_delay_samples == delay - FILTER_PRE_SAMPLES &&
	      scan.best_correlation_ppm > 100000u,
	      "DC hid the earlier return: sign=%d delay=%u strength=%u", sign,
	      scan.best_window_delay_samples, scan.best_correlation_ppm);
	CHECK(pcm_echo_reference_start_scan(&reference, DEFAULT_RETURN_DELAY,
		      FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK &&
	      reference.scan_input_sum == 0 && reference.scan_samples == 0,
	      "new scan retained input first moments");
	for (unsigned index = 0; index < reference.scan_count; ++index)
		CHECK(reference.scan_reference_sum[index] == 0,
		      "new scan retained reference first moments at %u", index);
	return true;
}

static bool test_large_centered_moments(bool varying_reference)
{
	struct pcm_echo_reference reference = { 0 }, saved;
	struct pcm_echo_reference_scan_result scan = { .sample_count = 123 };
	const struct pcm_echo_reference_scan_result unchanged = scan;
	/* Reach a reporting boundary with legitimate moments for a long run
	 * of full-scale DC. One changed sample has tiny positive variance;
	 * constant reference bins must retain exactly zero variance even when
	 * long double has only binary64 precision. */
	const uint32_t count = UINT32_MAX - 512u;
	const int64_t sample = INT16_MAX;

	CHECK(pcm_echo_reference_start_scan(&reference, 1u, 0u) == PCM_ECHO_REFERENCE_OK,
	      "large-moment scan did not initialize");
	for (unsigned index = 0; index < PCM_ECHO_REFERENCE_HISTORY_SAMPLES; ++index)
		CHECK(pcm_echo_reference_push(&reference, (int16_t)sample) == PCM_ECHO_REFERENCE_OK,
		      "large-moment history did not fill");
	if (varying_reference)
		reference.samples[(reference.write + PCM_ECHO_REFERENCE_HISTORY_SAMPLES - 1u) %
			PCM_ECHO_REFERENCE_HISTORY_SAMPLES]--;
	reference.scan_samples = count;
	reference.scan_input_energy = count * (uint64_t)(sample * sample);
	reference.scan_input_sum = -(int64_t)count * sample;
	for (unsigned index = 0; index < reference.scan_count; ++index) {
		reference.scan_reference_energy[index] = reference.scan_input_energy;
		reference.scan_cross_energy[index] = -(int64_t)reference.scan_input_energy;
		reference.scan_reference_sum[index] = (int64_t)count * sample;
	}
	enum pcm_echo_reference_result result =
		pcm_echo_reference_scan_observe(&reference, (int16_t)(1 - sample), &scan);
	if (varying_reference)
		CHECK(result == PCM_ECHO_REFERENCE_SCAN_READY &&
		      scan.best_window_delay_samples == 1u &&
		      scan.best_correlation_ppm == 1000000u,
		      "wide centering lost a genuine return: result=%d delay=%u strength=%u",
		      result, scan.best_window_delay_samples, scan.best_correlation_ppm);
	else
		CHECK(result == PCM_ECHO_REFERENCE_OK &&
		      memcmp(&scan, &unchanged, sizeof(scan)) == 0,
		      "large constant reference invented variance");
	/* Once the bounded observation count is exhausted, preserve all
	 * accumulated evidence rather than overflowing first moments. */
	reference.scan_samples = UINT32_MAX;
	saved = reference;
	scan = unchanged;
	CHECK(pcm_echo_reference_scan_observe(&reference, INT16_MIN, &scan) ==
		      PCM_ECHO_REFERENCE_OK &&
	      memcmp(&reference, &saved, sizeof(reference)) == 0 &&
	      memcmp(&scan, &unchanged, sizeof(scan)) == 0,
	      "exhausted scan changed evidence or output");
	return true;
}

int main(void)
{
	if (!test_ring_and_contracts() || !test_long_delay_scan() ||
	    !test_cluster_scan(80u, FILTER_PRE_SAMPLES) ||
	    !test_cluster_scan(2967u, FILTER_PRE_SAMPLES) ||
	    !test_cluster_scan(11424u, FILTER_PRE_SAMPLES) ||
	    !test_cluster_scan(2u, 0u) ||
	    !test_cluster_scan(2967u, PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES - 1u) ||
	    !test_opposite_adjacent_returns() || !test_full_history_scan_boundary() ||
	    !test_dc_only_scan(true, 8) || !test_dc_only_scan(true, -8) ||
	    !test_dc_only_scan(true, INT16_MIN) || !test_dc_only_scan(false, 8) ||
	    !test_dc_only_scan(false, INT16_MAX) ||
	    !test_biased_return_scan(1) || !test_biased_return_scan(-1) ||
	    !test_large_centered_moments(false) || !test_large_centered_moments(true))
		return 1;
	puts("PCM echo reference tests passed");
	return 0;
}
