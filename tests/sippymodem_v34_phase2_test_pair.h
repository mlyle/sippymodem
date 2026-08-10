// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_TEST_PAIR_H
#define SIPPYMODEM_V34_PHASE2_TEST_PAIR_H

#include <sippymodem_v34_phase2_reciprocal_probe.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	TEST_AMPLITUDE = 7000,
	TEST_MAXIMUM_LINE_DELAY = 48,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

enum test_endpoint_stage {
	TEST_ENDPOINT_RANGING,
	TEST_ENDPOINT_INITIAL_PROBE,
	TEST_ENDPOINT_PROBE_TURNAROUND,
	TEST_ENDPOINT_RECIPROCAL_PROBE,
};

struct test_delay_line {
	int16_t samples[TEST_MAXIMUM_LINE_DELAY];
	size_t delay_samples;
	size_t cursor;
};

struct test_endpoint {
	struct sippymodem_v34_phase2_ranging ranging;
	struct sippymodem_v34_phase2_initial_probe initial_probe;
	struct sippymodem_v34_phase2_probe_turnaround turnaround;
	struct sippymodem_v34_phase2_reciprocal_probe reciprocal_probe;
	enum test_endpoint_stage stage;
	bool continue_to_turnaround;
	bool continue_to_reciprocal_probe;
};

struct test_endpoint_event {
	struct sippymodem_v34_phase2_ranging_event ranging;
	struct sippymodem_v34_phase2_initial_probe_event initial_probe;
	struct sippymodem_v34_phase2_probe_turnaround_event turnaround;
	struct sippymodem_v34_phase2_reciprocal_probe_event reciprocal_probe;
	bool entered_initial_probe;
	bool entered_turnaround;
	bool entered_reciprocal_probe;
};

struct test_pair {
	struct test_endpoint caller;
	struct test_endpoint answerer;
	struct test_delay_line caller_to_answerer;
	struct test_delay_line answerer_to_caller;
	enum test_codec codec;
};

struct test_pair_event {
	struct test_endpoint_event caller;
	struct test_endpoint_event answerer;
	int16_t caller_transmit;
	int16_t answerer_transmit;
};

struct sippymodem_v34_info0 test_v34_phase2_capabilities(
	enum sippymodem_v34_station_role role);
bool test_v34_phase2_pair_init(
	struct test_pair *pair, enum test_codec codec, size_t delay_samples);
bool test_v34_phase2_pair_sample(
	struct test_pair *pair, struct test_pair_event *event);

#endif
