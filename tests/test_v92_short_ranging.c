// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v92_short_ranging.h>

#include "test.h"

#include <limits.h>
#include <string.h>

enum { MAX_DELAY = 257, SAMPLE_LIMIT = 6000 };

struct endpoint {
	struct v92_short_ranging control;
	struct v34_phase2_tone_transmitter tx;
	struct v34_phase2_tone_detector rx;
	struct v92_short_ranging_step last;
	unsigned reversal;
	unsigned complete;
};

static bool init_endpoint(struct endpoint *end, enum phase2_signal_role role)
{
	*end = (struct endpoint){ .reversal = UINT_MAX, .complete = UINT_MAX };
	return v92_short_ranging_init(&end->control, role, 0, 0) &&
	       v34_phase2_tone_transmitter_init(&end->tx,
		       v34_phase2_station_for_signal(role), 12000) == V34_PHASE2_TONE_OK &&
	       v34_phase2_tone_detector_init(&end->rx,
		       v34_phase2_station_for_signal(role)) == V34_PHASE2_TONE_OK;
}

static bool tick(struct endpoint *end, int16_t sample, unsigned index, int16_t *output)
{
	struct v34_phase2_tone_event event;
	struct v92_short_ranging_step step;
	uint32_t guard;

	*output = 0;
	if (end->complete != UINT_MAX)
		return true;
	CHECK(v34_phase2_tone_detector_sample(&end->rx, sample, &event) == V34_PHASE2_TONE_OK,
	      "tone detector failed");
	CHECK(v92_short_ranging_sample(&end->control, end->rx.carrier.tone_present, &event, &step),
	      "ranging failed at %u", index);
	if (step.reverse_carrier) {
		CHECK(end->reversal == UINT_MAX, "short ranging reversed twice");
		end->reversal = index;
		guard = end->tx.guard_phase;
		CHECK(v34_phase2_tone_transmitter_reverse(&end->tx) == V34_PHASE2_TONE_OK &&
		      end->tx.guard_phase == guard, "carrier reversal disturbed answer guard");
	}
	if (step.transmit_tone)
		CHECK(v34_phase2_tone_transmitter_sample(&end->tx, output) == V34_PHASE2_TONE_OK,
		      "tone generator failed");
	else if (end->control.signal_role == PHASE2_SIGNAL_B)
		CHECK(index >= end->reversal + PHASE2_RANGING_TONE_TAIL_SAMPLES,
		      "digital modem became silent before ten-ms tail");
	if (step.event != V92_SHORT_RANGING_NO_EVENT) {
		end->complete = index;
		end->last = step;
	}
	return true;
}

static unsigned distance(unsigned a, unsigned b) { return a > b ? a - b : b - a; }

static bool paired_waveforms(void)
{
	const unsigned delays[] = { 1, 13, MAX_DELAY };
	for (unsigned law = 0; law < 2; ++law) {
		for (unsigned trial = 0; trial < ARRAY_LENGTH(delays); ++trial) {
			unsigned delay = delays[trial];
			int16_t a_to_d[MAX_DELAY] = { 0 }, d_to_a[MAX_DELAY] = { 0 };
			struct endpoint analog, digital;

			CHECK(init_endpoint(&analog, PHASE2_SIGNAL_A) &&
			      init_endpoint(&digital, PHASE2_SIGNAL_B), "initialization failed");
			for (unsigned i = 0; i < SAMPLE_LIMIT; ++i) {
				int16_t a, d;
				unsigned cursor = i % delay;
				CHECK(tick(&analog, d_to_a[cursor], i, &a) &&
				      tick(&digital, a_to_d[cursor], i, &d), "paired tick failed");
				a_to_d[cursor] = law ? g711_decode_pcma(g711_encode_pcma(a)) :
					g711_decode_pcmu(g711_encode_pcmu(a));
				d_to_a[cursor] = law ? g711_decode_pcma(g711_encode_pcma(d)) :
					g711_decode_pcmu(g711_encode_pcmu(d));
				if (analog.complete != UINT_MAX && digital.complete != UINT_MAX)
					break;
			}
			CHECK(analog.last.event == V92_SHORT_RANGING_TRANSMIT_INFO1A &&
			      digital.last.event == V92_SHORT_RANGING_RECEIVE_INFO1A,
			      "pair failed law=%u delay=%u phases=%d/%d", law, delay,
			      analog.control.phase, digital.control.phase);
			CHECK(digital.reversal >= PHASE2_RANGING_INITIAL_TONE_SAMPLES &&
			      distance(analog.reversal - digital.reversal - delay,
				       PHASE2_RANGING_TURNAROUND_SAMPLES) <= 4 &&
			      analog.complete + 1 == analog.reversal + PHASE2_RANGING_TONE_TAIL_SAMPLES,
			      "incorrect physical response/tail: %u/%u/%u", digital.reversal,
			      analog.reversal, analog.complete);
			CHECK(distance(digital.last.round_trip_delay_samples, 2 * delay) <= 4 &&
			      digital.last.info1a_remaining_samples == V92_SHORT_RANGING_TIMEOUT_SAMPLES -
				      (digital.complete - digital.reversal),
			      "incorrect digital RTD/deadline: law=%u delay=%u measured=%u", law,
			      delay, digital.last.round_trip_delay_samples);
			CHECK(analog.last.round_trip_delay_samples == 0 &&
			      analog.last.info1a_remaining_samples == 0,
			      "analogue modem invented a measured delay");
		}
	}
	return true;
}

static bool deadlines_and_guards(void)
{
	struct v92_short_ranging state;
	struct v92_short_ranging_step step;
	struct v34_phase2_tone_event none = { 0 };
	struct v34_phase2_tone_event early = {
		.kind = V34_PHASE2_TONE_PHASE_REVERSED, .reversal_delay_samples = 40,
	};

	CHECK(v92_short_ranging_init(&state, PHASE2_SIGNAL_B, 399, 900), "init failed");
	CHECK(v92_short_ranging_sample(&state, true, &none, &step) && !step.reverse_carrier,
	      "reversed before fifty milliseconds");
	for (unsigned i = 0; i < 100; ++i)
		CHECK(v92_short_ranging_sample(&state, false, &none, &step) && !step.reverse_carrier,
		      "reversed without Tone A");
	CHECK(v92_short_ranging_sample(&state, true, &none, &step) && step.reverse_carrier,
	      "digital guard did not reverse");
	for (unsigned i = 1; i <= V92_SHORT_RANGING_TIMEOUT_SAMPLES; ++i) {
		CHECK(v92_short_ranging_sample(&state, true, i == 100 ? &early : &none, &step),
		      "digital wait failed");
		CHECK(step.transmit_tone == (i < PHASE2_RANGING_TONE_TAIL_SAMPLES),
		      "digital ten-ms tail changed at %u", i);
		CHECK(step.event == (i == V92_SHORT_RANGING_TIMEOUT_SAMPLES ?
			V92_SHORT_RANGING_RECOVER_FULL_PHASE2 : V92_SHORT_RANGING_NO_EVENT),
		      "digital timeout/early reversal misclassified at %u", i);
	}
	CHECK(step.round_trip_delay_samples == 0 && step.info1a_remaining_samples == 0,
	      "timeout invented a usable handoff");
	CHECK(v92_short_ranging_init(&state, PHASE2_SIGNAL_A, 100, 17000), "init failed");
	for (unsigned i = 1; i <= 3000; ++i) {
		CHECK(v92_short_ranging_sample(&state, false, &none, &step) && step.transmit_tone,
		      "analogue timeout changed Tone A");
		CHECK(step.event == (i == 3000 ? V92_SHORT_RANGING_REQUEST_FULL_RETRAIN :
			V92_SHORT_RANGING_NO_EVENT), "analogue deadline restarted at handoff");
	}
	return true;
}

static bool dated_responses(void)
{
	const uint16_t latencies[] = { 1, 40, 120, 319 };
	struct v34_phase2_tone_event edge = { .kind = V34_PHASE2_TONE_PHASE_REVERSED };
	struct v34_phase2_tone_event none = { 0 };
	struct v92_short_ranging state;
	struct v92_short_ranging_step step;

	for (unsigned n = 0; n < ARRAY_LENGTH(latencies); ++n) {
		unsigned response = PHASE2_RANGING_TURNAROUND_SAMPLES - latencies[n];
		/* A valid late B edge ends the analogue wait deadline. Its required
		 * response and tail may extend past 2500 ms from INFO0a. */
		CHECK(v92_short_ranging_init(&state, PHASE2_SIGNAL_A, 0, 19990), "init failed");
		edge.reversal_delay_samples = latencies[n];
		CHECK(v92_short_ranging_sample(&state, true, &edge, &step) &&
		      !step.reverse_carrier && step.event == V92_SHORT_RANGING_NO_EVENT,
		      "dated edge did not schedule the response");
		for (unsigned i = 1; i < response + PHASE2_RANGING_TONE_TAIL_SAMPLES; ++i) {
			CHECK(v92_short_ranging_sample(&state, true, &none, &step) && step.transmit_tone &&
			      step.reverse_carrier == (i == response), "turnaround gained detector latency");
			CHECK(step.event == (i + 1 == response + PHASE2_RANGING_TONE_TAIL_SAMPLES ?
				V92_SHORT_RANGING_TRANSMIT_INFO1A : V92_SHORT_RANGING_NO_EVENT),
			      "late valid response timed out or lost its ten-ms tail");
		}
	}
	CHECK(v92_short_ranging_init(&state, PHASE2_SIGNAL_A, 0, 0), "init failed");
	edge.reversal_delay_samples = 320;
	CHECK(v92_short_ranging_sample(&state, true, &edge, &step) &&
	      state.phase == V92_SHORT_RANGING_ANALOG_WAIT, "unusable late edge accepted");
	edge.reversal_delay_samples = 40;
	CHECK(v92_short_ranging_sample(&state, true, &edge, &step) &&
	      state.phase == V92_SHORT_RANGING_ANALOG_DELAY, "edge ignored");
	CHECK(v92_short_ranging_sample(&state, true, &edge, &step) &&
	      state.phase == V92_SHORT_RANGING_ANALOG_WAIT && !step.reverse_carrier,
	      "restored phase did not cancel the false turn");
	for (unsigned i = 0; i < 400; ++i)
		CHECK(v92_short_ranging_sample(&state, true, &none, &step) && !step.reverse_carrier,
		      "cancelled reversal escaped later");
	return true;
}

static bool transactionality(void)
{
	struct v92_short_ranging state, before;
	struct v92_short_ranging_step step = { .info1a_remaining_samples = 123 }, unchanged = step;
	struct v34_phase2_tone_event bad = { .kind = (enum v34_phase2_tone_event_kind)-1 };

	CHECK(v92_short_ranging_init(&state, PHASE2_SIGNAL_A, 0, 0), "init failed");
	before = state;
	CHECK(!v92_short_ranging_init(&state, (enum phase2_signal_role)-1, 0, 0) &&
	      !v92_short_ranging_init(&state, PHASE2_SIGNAL_A, 0, UINT16_MAX) &&
	      !v92_short_ranging_sample(&state, true, &bad, &step) &&
	      !v92_short_ranging_sample(&state, true, NULL, &step) &&
	      memcmp(&state, &before, sizeof(state)) == 0 &&
	      memcmp(&step, &unchanged, sizeof(step)) == 0, "invalid call mutated state/output");
	bad.kind = V34_PHASE2_TONE_NO_EVENT;
	CHECK(!v92_short_ranging_sample(&state, true, &bad,
		      (struct v92_short_ranging_step *)&state) &&
	      memcmp(&state, &before, sizeof(state)) == 0, "overlapping output accepted");
	state.tail_samples = PHASE2_RANGING_TONE_TAIL_SAMPLES + 1;
	CHECK(!v92_short_ranging_is_valid(&state), "invalid tail accepted");
	state = before;
	state.signal_role = PHASE2_SIGNAL_B;
	CHECK(!v92_short_ranging_is_valid(&state), "digital analogue-wait state accepted");
	state = before;
	state.phase = V92_SHORT_RANGING_COMPLETE;
	before = state;
	bad.kind = V34_PHASE2_TONE_NO_EVENT;
	CHECK(!v92_short_ranging_sample(&state, true, &bad, &step) &&
	      memcmp(&state, &before, sizeof(state)) == 0 &&
	      memcmp(&step, &unchanged, sizeof(step)) == 0, "terminal event was re-emitted");
	return true;
}

int main(void)
{
	return paired_waveforms() && deadlines_and_guards() && dated_responses() &&
	       transactionality() ? 0 : 1;
}
