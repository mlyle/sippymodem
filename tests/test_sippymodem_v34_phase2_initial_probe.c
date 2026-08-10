// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_g711.h>
#include <sippymodem_v34_phase2_initial_probe.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 7000,
	TEST_MAXIMUM_LINE_DELAY = 48,
	TEST_SAMPLE_LIMIT = 12000,
	TEST_STABLE_TONE_SAMPLES = 160,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

enum test_endpoint_stage {
	TEST_ENDPOINT_RANGING,
	TEST_ENDPOINT_INITIAL_PROBE,
};

struct test_delay_line {
	int16_t samples[TEST_MAXIMUM_LINE_DELAY];
	size_t delay_samples;
	size_t cursor;
};

struct test_endpoint {
	struct sippymodem_v34_phase2_ranging ranging;
	struct sippymodem_v34_phase2_initial_probe initial_probe;
	enum test_endpoint_stage stage;
};

struct test_endpoint_event {
	struct sippymodem_v34_phase2_ranging_event ranging;
	struct sippymodem_v34_phase2_initial_probe_event initial_probe;
	bool entered_initial_probe;
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

struct turn_observation {
	size_t caller_ready;
	size_t answerer_ready;
	size_t caller_first_probe_sample;
	size_t answerer_first_probe_sample;
	size_t answerer_l2_started;
	size_t caller_measurement_ready;
	size_t caller_probe_complete;
	size_t answerer_probe_complete;
	size_t caller_tone_pair_active;
	uint32_t answerer_l2_samples;
	uint64_t caller_tone_energy;
	uint64_t answerer_tone_energy;
	bool caller_acquired;
	bool caller_l2_started;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	}
	return 0;
}

static int64_t sample_magnitude(int16_t sample)
{
	return sample < 0 ? -(int64_t)sample : sample;
}

static int16_t delay_line_receive(const struct test_delay_line *line)
{
	return line->samples[line->cursor];
}

static void delay_line_transmit(
	struct test_delay_line *line, int16_t sample)
{
	line->samples[line->cursor] = sample;
	line->cursor = (line->cursor + 1u) % line->delay_samples;
}

static struct sippymodem_v34_info0 capabilities(
	enum sippymodem_v34_phase2_station_role role)
{
	if (role == SIPPYMODEM_V34_PHASE2_CALLING_STATION)
		return (struct sippymodem_v34_info0){
			.symbol_rate_2800_supported = true,
			.low_carrier_3000_supported = true,
			.maximum_symbol_rate_difference_steps = 2,
			.transmit_clock_source =
				SIPPYMODEM_V34_INFO0_CLOCK_INTERNAL,
		};
	return (struct sippymodem_v34_info0){
		.symbol_rate_3429_supported = true,
		.high_carrier_3200_supported = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 4,
		.transmit_clock_source =
			SIPPYMODEM_V34_INFO0_CLOCK_RECEIVE_TIMING,
	};
}

static bool endpoint_init(
	struct test_endpoint *endpoint,
	enum sippymodem_v34_phase2_station_role role)
{
	struct sippymodem_v34_info0 local_capabilities = capabilities(role);
	struct test_endpoint initialized = {
		.stage = TEST_ENDPOINT_RANGING,
	};

	if (sippymodem_v34_phase2_ranging_init(
		    &initialized.ranging, role, TEST_AMPLITUDE,
		    &local_capabilities) != SIPPYMODEM_V34_PHASE2_RANGING_OK)
		return false;
	*endpoint = initialized;
	return true;
}

static bool endpoint_sample(
	struct test_endpoint *endpoint, int16_t received_sample,
	int16_t *transmit_sample, struct test_endpoint_event *event)
{
	struct test_endpoint_event emitted = { 0 };

	if (endpoint->stage == TEST_ENDPOINT_RANGING) {
		enum sippymodem_v34_phase2_ranging_event_kind expected_ready =
			endpoint->ranging.info0_exchange.local_role ==
					SIPPYMODEM_V34_PHASE2_CALLING_STATION ?
				SIPPYMODEM_V34_PHASE2_PROBE_RECEIVE_READY :
				SIPPYMODEM_V34_PHASE2_PROBE_TRANSMIT_READY;

		if (sippymodem_v34_phase2_ranging_sample(
			    &endpoint->ranging, received_sample, transmit_sample,
			    &emitted.ranging) != SIPPYMODEM_V34_PHASE2_RANGING_OK)
			return false;
		if (emitted.ranging.kind == expected_ready) {
			if (sippymodem_v34_phase2_initial_probe_init(
				    &endpoint->initial_probe, &endpoint->ranging) !=
			    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK)
				return false;
			endpoint->stage = TEST_ENDPOINT_INITIAL_PROBE;
			emitted.entered_initial_probe = true;
		}
	} else if (sippymodem_v34_phase2_initial_probe_sample(
			   &endpoint->initial_probe, received_sample,
			   transmit_sample, &emitted.initial_probe) !=
		   SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK) {
		return false;
	}
	*event = emitted;
	return true;
}

static bool pair_init(
	struct test_pair *pair, enum test_codec codec, size_t delay_samples)
{
	struct test_pair initialized = {
		.caller_to_answerer.delay_samples = delay_samples,
		.answerer_to_caller.delay_samples = delay_samples,
		.codec = codec,
	};

	if (pair == NULL || delay_samples == 0 ||
	    delay_samples > TEST_MAXIMUM_LINE_DELAY ||
	    !endpoint_init(
		    &initialized.caller,
		    SIPPYMODEM_V34_PHASE2_CALLING_STATION) ||
	    !endpoint_init(
		    &initialized.answerer,
		    SIPPYMODEM_V34_PHASE2_ANSWERING_STATION))
		return false;
	*pair = initialized;
	return true;
}

static bool pair_sample(
	struct test_pair *pair, struct test_pair_event *event)
{
	struct test_pair_event emitted = { 0 };

	if (!endpoint_sample(
		    &pair->caller,
		    delay_line_receive(&pair->answerer_to_caller),
		    &emitted.caller_transmit, &emitted.caller) ||
	    !endpoint_sample(
		    &pair->answerer,
		    delay_line_receive(&pair->caller_to_answerer),
		    &emitted.answerer_transmit, &emitted.answerer))
		return false;
	delay_line_transmit(
		&pair->caller_to_answerer,
		codec_round_trip(emitted.caller_transmit, pair->codec));
	delay_line_transmit(
		&pair->answerer_to_caller,
		codec_round_trip(emitted.answerer_transmit, pair->codec));
	*event = emitted;
	return true;
}

static void initialize_observation(struct turn_observation *observation)
{
	*observation = (struct turn_observation){
		.caller_ready = SIZE_MAX,
		.answerer_ready = SIZE_MAX,
		.caller_first_probe_sample = SIZE_MAX,
		.answerer_first_probe_sample = SIZE_MAX,
		.answerer_l2_started = SIZE_MAX,
		.caller_measurement_ready = SIZE_MAX,
		.caller_probe_complete = SIZE_MAX,
		.answerer_probe_complete = SIZE_MAX,
		.caller_tone_pair_active = SIZE_MAX,
	};
}

static void observe_turn(
	const struct test_pair *pair, const struct test_pair_event *event,
	bool caller_was_probing, bool answerer_was_probing,
	size_t sample_index, struct turn_observation *observation)
{
	if (event->caller.entered_initial_probe)
		observation->caller_ready = sample_index;
	if (event->answerer.entered_initial_probe)
		observation->answerer_ready = sample_index;
	if (caller_was_probing &&
	    observation->caller_first_probe_sample == SIZE_MAX)
		observation->caller_first_probe_sample = sample_index;
	if (answerer_was_probing &&
	    observation->answerer_first_probe_sample == SIZE_MAX)
		observation->answerer_first_probe_sample = sample_index;
	if (event->caller.initial_probe.receive_event.kind ==
	    SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED)
		observation->caller_acquired = true;
	if (event->caller.initial_probe.receive_event.kind ==
	    SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED)
		observation->caller_l2_started = true;
	if (event->caller.initial_probe.receive_event.kind ==
	    SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY)
		observation->caller_measurement_ready = sample_index;
	if (event->answerer.initial_probe.transmit_event.kind ==
	    SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_L2_STARTED)
		observation->answerer_l2_started = sample_index;
	if (event->caller.initial_probe.kind ==
	    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE)
		observation->caller_probe_complete = sample_index;
	if (event->answerer.initial_probe.kind ==
	    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE) {
		observation->answerer_probe_complete = sample_index;
		observation->answerer_l2_samples =
			event->answerer.initial_probe.transmit_event.l2_samples;
	}
	if (event->caller.initial_probe.kind ==
	    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE)
		observation->caller_tone_pair_active = sample_index;
	if (pair->caller.stage == TEST_ENDPOINT_INITIAL_PROBE &&
	    pair->caller.initial_probe.phase !=
		    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING)
		observation->caller_tone_energy +=
			(uint64_t)sample_magnitude(event->caller_transmit);
	if (pair->answerer.stage == TEST_ENDPOINT_INITIAL_PROBE &&
	    pair->answerer.initial_probe.phase ==
		    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A)
		observation->answerer_tone_energy +=
			(uint64_t)sample_magnitude(event->answerer_transmit);
}

static double measurement_power(
	const struct sippymodem_v34_phase2_probe_spectrum_measurement *spectrum)
{
	double power = 0.0;

	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index)
		power += spectrum->harmonics[index].mean_square;
	return power;
}

static bool check_initial_probe_turn(
	enum test_codec codec, size_t delay_samples)
{
	struct sippymodem_v34_phase2_probe_receiver_measurement measurement;
	struct test_pair pair;
	struct turn_observation observation;
	size_t stable_samples = 0;

	CHECK(pair_init(&pair, codec, delay_samples),
	      "probe-turn endpoints did not initialize");
	initialize_observation(&observation);
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;
		bool caller_was_probing =
			pair.caller.stage == TEST_ENDPOINT_INITIAL_PROBE;
		bool answerer_was_probing =
			pair.answerer.stage == TEST_ENDPOINT_INITIAL_PROBE;

		CHECK(pair_sample(&pair, &event),
		      "probe turn failed for codec %d delay %zu at sample %zu",
		      codec, delay_samples, index);
		observe_turn(
			&pair, &event, caller_was_probing, answerer_was_probing,
			index, &observation);
		if (pair.caller.stage == TEST_ENDPOINT_INITIAL_PROBE &&
		    pair.answerer.stage == TEST_ENDPOINT_INITIAL_PROBE &&
		    pair.caller.initial_probe.phase ==
			    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE &&
		    pair.answerer.initial_probe.phase ==
			    SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A) {
			if (++stable_samples == TEST_STABLE_TONE_SAMPLES)
				break;
		} else {
			stable_samples = 0;
		}
	}
	CHECK(stable_samples == TEST_STABLE_TONE_SAMPLES &&
	      sippymodem_v34_phase2_initial_probe_is_valid(
		      &pair.caller.initial_probe) &&
	      sippymodem_v34_phase2_initial_probe_is_valid(
		      &pair.answerer.initial_probe),
	      "probe turn did not establish the tone pair for codec %d delay %zu",
	      codec, delay_samples);
	CHECK(observation.caller_ready != SIZE_MAX &&
	      observation.answerer_ready != SIZE_MAX &&
	      observation.caller_first_probe_sample ==
		      observation.caller_ready + 1u &&
	      observation.answerer_first_probe_sample ==
		      observation.answerer_ready + 1u,
	      "ranging did not hand off the following sample");
	CHECK(observation.answerer_l2_started ==
		      observation.answerer_first_probe_sample +
			      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES - 1u,
	      "answerer changed the exact 160 ms L1 boundary");
	CHECK(observation.caller_acquired && observation.caller_l2_started &&
	      observation.caller_measurement_ready != SIZE_MAX &&
	      observation.caller_probe_complete ==
		      observation.caller_measurement_ready + 1u,
	      "caller did not complete the measured probe causally");
	CHECK(observation.answerer_probe_complete >
		      observation.caller_probe_complete + delay_samples &&
	      observation.caller_tone_pair_active >
		      observation.answerer_probe_complete + delay_samples &&
	      observation.answerer_l2_samples >=
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
	      observation.answerer_l2_samples <
		      (uint32_t)
			      SIPPYMODEM_V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES +
		      pair.answerer.initial_probe.ranging.round_trip_delay_samples,
	      "tone response did not end answerer L2 within its bound");
	CHECK(sippymodem_v34_phase2_initial_probe_measure(
		      &pair.caller.initial_probe, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK &&
	      measurement.l1.sample_count ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES,
	      "caller did not retain exact probe measurement windows");
	{
		double l1_power = measurement_power(&measurement.l1);
		double l2_power = measurement_power(&measurement.l2);

		CHECK(l2_power > 0.0 && l1_power / l2_power > 3.9 &&
		      l1_power / l2_power < 4.1,
		      "probe power ratio changed for codec %d: %.6f", codec,
		      l1_power / l2_power);
	}
	CHECK(observation.caller_tone_energy != 0 &&
	      observation.answerer_tone_energy != 0,
	      "established tones remained silent");
	return true;
}

static bool test_bidirectional_turn(void)
{
	static const struct {
		enum test_codec codec;
		size_t delay_samples;
	} cases[] = {
		{ TEST_CODEC_LINEAR, 1 },
		{ TEST_CODEC_PCMU, 13 },
		{ TEST_CODEC_PCMA, 47 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(check_initial_probe_turn(
			      cases[index].codec, cases[index].delay_samples),
		      "initial probe turn failed for case %zu", index);
	return true;
}

static bool completed_ranging_pair(
	struct sippymodem_v34_phase2_ranging *caller,
	struct sippymodem_v34_phase2_ranging *answerer)
{
	struct test_pair pair;

	if (!pair_init(&pair, TEST_CODEC_LINEAR, 1))
		return false;
	for (size_t index = 0; index < TEST_SAMPLE_LIMIT; ++index) {
		struct test_pair_event event;

		if (!pair_sample(&pair, &event))
			return false;
		if (pair.caller.stage == TEST_ENDPOINT_INITIAL_PROBE &&
		    pair.answerer.stage == TEST_ENDPOINT_INITIAL_PROBE) {
			*caller = pair.caller.ranging;
			*answerer = pair.answerer.ranging;
			return true;
		}
	}
	return false;
}

static bool test_bounded_failures(void)
{
	struct sippymodem_v34_phase2_ranging caller_ranging;
	struct sippymodem_v34_phase2_ranging answerer_ranging;
	struct sippymodem_v34_phase2_initial_probe caller;
	struct sippymodem_v34_phase2_initial_probe answerer;
	struct sippymodem_v34_phase2_initial_probe failed;
	struct sippymodem_v34_phase2_probe_sequence remote_probe;
	struct sippymodem_v34_phase2_initial_probe_event event;
	int16_t transmit;
	size_t samples;

	CHECK(completed_ranging_pair(&caller_ranging, &answerer_ranging),
	      "failure fixtures did not complete ranging");
	CHECK(sippymodem_v34_phase2_initial_probe_init(
		      &caller, &caller_ranging) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
	      "missing-probe fixture did not initialize");
	for (samples = 0; samples < 1000 &&
			  caller.phase !=
				  SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED;
	     ++samples)
		CHECK(sippymodem_v34_phase2_initial_probe_sample(
			      &caller, 0, &transmit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
		      "missing-probe wait failed at sample %zu", samples);
	CHECK(caller.phase == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED &&
	      caller.failure ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_FAILED,
	      "missing remote probe did not fail explicitly");
	failed = caller;
	transmit = 1234;
	CHECK(sippymodem_v34_phase2_initial_probe_sample(
		      &caller, INT16_MIN, &transmit, &event) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK &&
	      transmit == 0 &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_NONE &&
	      memcmp(&caller, &failed, sizeof(caller)) == 0,
	      "failed controller did not remain silent and stable");

	CHECK(sippymodem_v34_phase2_initial_probe_init(
		      &answerer, &answerer_ranging) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
	      "transmit-limit fixture did not initialize");
	for (samples = 0; samples < 7000 &&
			  answerer.phase !=
				  SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED;
	     ++samples)
		CHECK(sippymodem_v34_phase2_initial_probe_sample(
			      &answerer, 0, &transmit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
		      "probe transmit limit failed at sample %zu", samples);
	CHECK(answerer.phase == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED &&
	      answerer.failure ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_FAILED &&
	      event.transmit_event.kind ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED,
	      "unanswered local probe did not reach its RTD-adjusted limit");

	CHECK(sippymodem_v34_phase2_initial_probe_init(
		      &caller, &caller_ranging) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK &&
	      sippymodem_v34_phase2_probe_sequence_init(
		      &remote_probe, TEST_AMPLITUDE,
		      caller_ranging.round_trip_delay_samples) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK,
	      "tone-timeout fixture did not initialize");
	for (samples = 0; samples < 3000 &&
			  caller.phase ==
				  SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING;
	     ++samples) {
		struct sippymodem_v34_phase2_probe_sequence_event probe_event;
		int16_t received;

		CHECK(sippymodem_v34_phase2_probe_sequence_sample(
			      &remote_probe, false, &received, &probe_event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_SEQUENCE_OK &&
		      sippymodem_v34_phase2_initial_probe_sample(
			      &caller, received, &transmit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
		      "tone-timeout probe failed at sample %zu", samples);
	}
	CHECK(caller.phase ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B,
	      "caller did not begin Tone B after measuring the remote probe");
	for (samples = 0;
	     samples < SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
	     caller.phase != SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED;
	     ++samples)
		CHECK(sippymodem_v34_phase2_initial_probe_sample(
			      &caller, 0, &transmit, &event) ==
			      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
		      "remote-tone timeout failed at sample %zu", samples);
	CHECK(samples + 1u ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
	      caller.phase == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED &&
	      caller.failure ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT &&
	      event.kind == SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_FAILED,
	      "missing Tone A did not reach the exact bounded timeout");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_info0 local_capabilities =
		capabilities(SIPPYMODEM_V34_PHASE2_CALLING_STATION);
	struct sippymodem_v34_phase2_ranging unready;
	struct sippymodem_v34_phase2_ranging caller_ranging;
	struct sippymodem_v34_phase2_ranging answerer_ranging;
	struct sippymodem_v34_phase2_initial_probe probe;
	struct sippymodem_v34_phase2_initial_probe before;
	struct sippymodem_v34_phase2_initial_probe invalid;
	struct sippymodem_v34_phase2_initial_probe_event event = {
		.kind = SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_FAILED,
	};
	struct sippymodem_v34_phase2_initial_probe_event event_before = event;
	int16_t transmit = 1234;

	CHECK(sippymodem_v34_phase2_ranging_init(
		      &unready, SIPPYMODEM_V34_PHASE2_CALLING_STATION,
		      TEST_AMPLITUDE, &local_capabilities) ==
		      SIPPYMODEM_V34_PHASE2_RANGING_OK &&
	      completed_ranging_pair(&caller_ranging, &answerer_ranging),
	      "validation fixtures did not initialize");
	memset(&probe, 0xa5, sizeof(probe));
	before = probe;
	CHECK(sippymodem_v34_phase2_initial_probe_init(&probe, &unready) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      sippymodem_v34_phase2_initial_probe_init(NULL, &caller_ranging) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
	      "invalid initialization changed caller-owned state");
	CHECK(sippymodem_v34_phase2_initial_probe_init(
		      &probe, &caller_ranging) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
	      "validation controller did not initialize");
	before = probe;
	CHECK(sippymodem_v34_phase2_initial_probe_sample(
		      &probe, 0, NULL, &event) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_initial_probe_sample(
		      &probe, 0, &transmit, NULL) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      memcmp(&probe, &before, sizeof(probe)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      transmit == 1234,
	      "invalid sample output was not transactional");
	invalid = probe;
	invalid.phase =
		SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING;
	CHECK(!sippymodem_v34_phase2_initial_probe_is_valid(&invalid) &&
	      sippymodem_v34_phase2_initial_probe_measure(&probe, NULL) ==
		      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT &&
	      strcmp(
		      sippymodem_v34_phase2_initial_probe_result_name(
			      SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK),
		      "ok") == 0 &&
	      strcmp(
		      sippymodem_v34_phase2_initial_probe_result_name(
			      (enum sippymodem_v34_phase2_initial_probe_result)99),
		      "unknown") == 0,
	      "validation or result-name contract changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "bidirectional initial probe", test_bidirectional_turn },
		{ "bounded failures", test_bounded_failures },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
