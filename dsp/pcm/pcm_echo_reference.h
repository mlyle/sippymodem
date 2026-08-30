// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_ECHO_REFERENCE_H
#define PCM_ECHO_REFERENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	PCM_ECHO_REFERENCE_SAMPLE_RATE = 8000,
	PCM_ECHO_REFERENCE_HISTORY_MS = 1500,
	PCM_ECHO_REFERENCE_HISTORY_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE *
		PCM_ECHO_REFERENCE_HISTORY_MS / 1000,
	PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE * 700 / 1000,
	PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES = 384,
	PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES =
		PCM_ECHO_REFERENCE_HISTORY_SAMPLES -
		PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES + 1,
	PCM_ECHO_REFERENCE_SCAN_SPAN = 4096,
	PCM_ECHO_REFERENCE_SCAN_STEP = 2,
	PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE / 8,
	PCM_ECHO_REFERENCE_SCAN_CADENCE = 512,
	PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES = 4097,
};

_Static_assert(PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES +
		       PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES - 1 >=
		       PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES,
	       "PCM echo reference must retain at least 700 ms of delay");

enum pcm_echo_reference_result {
	PCM_ECHO_REFERENCE_OK,
	PCM_ECHO_REFERENCE_NOT_READY,
	PCM_ECHO_REFERENCE_SCAN_READY,
	PCM_ECHO_REFERENCE_INVALID_ARGUMENT,
	PCM_ECHO_REFERENCE_INVALID_STATE,
};

struct pcm_echo_reference_scan_result {
	uint32_t best_window_delay_samples;
	uint32_t current_window_delay_samples;
	uint32_t sample_count;
	uint32_t best_correlation_ppm;
	uint32_t current_correlation_ppm;
};

/*
 * Own the host-rate transmit history and one bounded delay-correlation scan.
 * Delay candidates identify the start of a later adaptive-filter window;
 * pre_samples selects the known reference point within that window.
 */
struct pcm_echo_reference {
	int16_t samples[PCM_ECHO_REFERENCE_HISTORY_SAMPLES];
	uint64_t scan_reference_energy
		[PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES];
	int64_t scan_cross_energy[PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES];
	size_t write;
	size_t fill;
	uint64_t scan_input_energy;
	uint32_t scan_start;
	uint32_t scan_step;
	uint32_t scan_count;
	uint32_t scan_current_index;
	uint32_t scan_pre_samples;
	uint32_t scan_required_fill;
	uint32_t scan_samples;
	uint32_t scan_epoch;
};

enum pcm_echo_reference_result pcm_echo_reference_init(
	struct pcm_echo_reference *reference);
bool pcm_echo_reference_is_valid(
	const struct pcm_echo_reference *reference);

enum pcm_echo_reference_result pcm_echo_reference_push(
	struct pcm_echo_reference *reference, int16_t sample);
enum pcm_echo_reference_result pcm_echo_reference_read_at_age(
	const struct pcm_echo_reference *reference, uint32_t age,
	int16_t *sample);
enum pcm_echo_reference_result pcm_echo_reference_read_window(
	const struct pcm_echo_reference *reference, uint32_t first_age,
	int16_t *samples, size_t count);

enum pcm_echo_reference_result pcm_echo_reference_start_scan(
	struct pcm_echo_reference *reference,
	uint32_t current_window_delay_samples,
	uint32_t pre_samples);
enum pcm_echo_reference_result pcm_echo_reference_scan_observe(
	struct pcm_echo_reference *reference, int16_t received_sample,
	struct pcm_echo_reference_scan_result *result);

#endif /* PCM_ECHO_REFERENCE_H */
