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
	CHECK(reference.scan_required_fill >= LONG_RETURN_DELAY &&
	      reference.scan_required_fill <=
		      PCM_ECHO_REFERENCE_HISTORY_SAMPLES,
	      "scan fill boundary does not cover long return: %u",
	      reference.scan_required_fill);

	while (!acquired &&
	       index < PCM_ECHO_REFERENCE_HISTORY_SAMPLES) {
		enum pcm_echo_reference_result scan_result;
		struct pcm_echo_reference_scan_result report = unchanged;

		scan_result = pcm_echo_reference_scan_observe(
			&reference, returned_sample(index), &report);
		if (reference.fill < reference.scan_required_fill)
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
	CHECK(result.best_window_delay_samples == LONG_FILTER_DELAY - 1u,
	      "long-delay scan selected %u instead of %u",
	      result.best_window_delay_samples, LONG_FILTER_DELAY - 1u);
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

	while (reference.scan_samples < PCM_ECHO_REFERENCE_SCAN_CADENCE * 2u) {
		struct pcm_echo_reference_scan_result report = unchanged;
		enum pcm_echo_reference_result scan_result =
			pcm_echo_reference_scan_observe(
				&reference, returned_sample(index), &report);

		CHECK(pcm_echo_reference_push(
			      &reference, reference_sample(index)) ==
			      PCM_ECHO_REFERENCE_OK,
		      "continued scan push failed at %zu", index);
		index++;
		if (reference.scan_samples ==
		    PCM_ECHO_REFERENCE_SCAN_CADENCE * 2u) {
			CHECK(scan_result == PCM_ECHO_REFERENCE_SCAN_READY &&
			      report.sample_count ==
				      PCM_ECHO_REFERENCE_SCAN_CADENCE * 2u &&
			      report.best_window_delay_samples ==
				      LONG_FILTER_DELAY - 1u,
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

int main(void)
{
	if (!test_ring_and_contracts() || !test_long_delay_scan() ||
	    !test_cluster_scan(80u, FILTER_PRE_SAMPLES) ||
	    !test_cluster_scan(2967u, FILTER_PRE_SAMPLES) ||
	    !test_cluster_scan(11424u, FILTER_PRE_SAMPLES) ||
	    !test_cluster_scan(2u, 0u) ||
	    !test_cluster_scan(2967u, PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES - 1u))
		return 1;
	puts("PCM echo reference tests passed");
	return 0;
}
