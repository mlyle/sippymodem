// SPDX-License-Identifier: BSD-3-Clause

#include <conditioning.h>
#include <conditioning_path.h>
#include <constellation.h>
#include <qam.h>
#include <rate_signal.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_LINE_SAMPLE_LIMIT = 8000,
};

static const struct qam_channel V32_CHANNEL = {
	.sample_rate = V32_ECHO_FRONTEND_INPUT_RATE,
	.symbol_rate_numerator = 2400,
	.symbol_rate_denominator = 1,
	.carrier_frequency_numerator = 1800,
	.carrier_frequency_denominator = 1,
	.rolloff = 0.25,
	.pulse_span_symbols = 6,
};

static const struct v32_rate_offer RATE_OFFER = {
	.rates = V32_RATE_CAPABILITY_4800 |
		 V32_RATE_CAPABILITY_9600,
	.trellis_at_highest_rate = false,
};

struct generated_source {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter conditioning;
	struct v32_symbol_encoder rate_encoder;
	struct qam_point point;
	uint16_t rate_word;
	unsigned int rate_dibit;
	unsigned int rate_symbols;
	bool rate_started;
};

static bool source_init(
	struct generated_source *source,
	enum v32_transmitter_role role)
{
	memset(source, 0, sizeof(*source));
	return qam_modulator_init(
		       &source->modulator, &V32_CHANNEL,
		       TEST_AMPLITUDE) == QAM_OK &&
	       v32_conditioning_transmitter_init(
		       &source->conditioning, role,
		       V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		       V32_CONDITIONING_OK &&
	       v32_rate_signal_encode(
		       V32_RATE_SIGNAL, &RATE_OFFER,
		       &source->rate_word) == V32_RATE_SIGNAL_OK;
}

static bool source_next_point(struct generated_source *source)
{
	uint8_t quadrant;

	if (!v32_conditioning_transmitter_is_complete(
		    &source->conditioning)) {
		if (v32_conditioning_transmitter_next(
			    &source->conditioning, &quadrant) !=
		    V32_CONDITIONING_OK)
			return false;
	} else {
		uint8_t dibit;

		if (!source->rate_started) {
			if (v32_conditioning_transmitter_framing_state(
				    &source->conditioning,
				    &source->rate_encoder) !=
			    V32_CONDITIONING_OK)
				return false;
			source->rate_started = true;
		}
		if (v32_rate_signal_dibit(
			    source->rate_word,
			    source->rate_dibit, &dibit) !=
			    V32_RATE_SIGNAL_OK ||
		    v32_4800_symbol_encode(
			    &source->rate_encoder, dibit,
			    &quadrant) != V32_FRAMING_OK)
			return false;
		source->rate_dibit =
			(source->rate_dibit + 1u) % V32_RATE_SIGNAL_DIBITS;
		source->rate_symbols++;
	}
	return v32_4800_constellation_point(
		       quadrant, &source->point) == V32_CONSTELLATION_OK;
}

static bool source_sample(
	struct generated_source *source, int16_t *sample,
	struct v32_echo_reference *reference, bool *accepted)
{
	bool can_accept = qam_modulator_can_accept_symbol(
		&source->modulator);

	if (can_accept &&
	    (!source_next_point(source) ||
	     v32_echo_reference_init(
		     reference, &source->point,
		     source->modulator.carrier_phase) != V32_ECHO_OK))
		return false;
	return qam_modulator_sample(
		       &source->modulator, &source->point,
		       sample, accepted) == QAM_OK &&
	       *accepted == can_accept;
}

static bool retained_receive_init(
	struct v32_receive_path *receive, unsigned int cursor)
{
	struct v32_equalizer_observation observation;
	struct v32_equalizer_decision decision;
	int16_t tail[2] = { 0 };
	int16_t sample = 0;
	size_t consumed;
	size_t produced;

	if (v32_receive_path_init(
		    receive, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) !=
		    V32_RECEIVE_PATH_OK ||
	    v32_equalizer_set_carrier_tracking(
		    &receive->equalizer, false) != V32_EQUALIZER_OK ||
	    v32_equalizer_process(
		    &receive->equalizer, &sample, 1, &consumed,
		    &observation) != V32_EQUALIZER_OBSERVATION_READY ||
	    consumed != 1)
		return false;
	decision = (struct v32_equalizer_decision){
		.observation = observation,
		.ideal_phase = observation.phase,
		.ideal_magnitude = observation.magnitude,
	};
	if (v32_equalizer_commit(&receive->equalizer, &decision) !=
		    V32_EQUALIZER_OK ||
	    v32_equalizer_process(
		    &receive->equalizer, tail, 3u - cursor, &consumed,
		    &observation) != V32_EQUALIZER_OK ||
	    consumed != 3u - cursor ||
	    v32_equalizer_set_adaptation(
		    &receive->equalizer, V32_EQUALIZER_ADAPTATION_FINE) !=
		    V32_EQUALIZER_OK ||
	    v32_line_receiver_process(
		    &receive->line, NULL, 0, true, NULL, 0, &produced) !=
		    V32_LINE_RECEIVER_OK)
		return false;
	receive->equalizer.carrier_training_symbols = 77;
	return v32_receive_path_is_valid(receive) &&
	       receive->equalizer.epoch != 0 && receive->line.input_paused &&
	       receive->equalizer.samples_until_observation == cursor;
}

static bool run_generated_role(
	enum v32_transmitter_role role,
	unsigned int retained_cursor, unsigned int carrier_octant,
	struct v32_conditioning_path *completed)
{
	struct generated_source source;
	struct v32_conditioning_path path;
	struct v32_conditioning_path rejected;
	struct v32_conditioning_path saved_rejected;
	struct v32_conditioning_path_handoff handoff;
	struct v32_receive_path receive;
	struct v32_receive_path saved_receive;
	struct v32_receive_path expected_receive;
	struct v32_symbol_decoder expected_decoder;
	bool complete = false;
	bool retained = retained_cursor != 0;

	CHECK(completed != NULL && source_init(&source, role),
	      "generated conditioning source did not initialize for role %d",
	      role);
	source.modulator.carrier_phase = carrier_octant * acos(-1.0) / 4.0;
	if (!retained) {
		CHECK(v32_conditioning_path_init(
			      &path, role, 0,
			      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
			      V32_CONDITIONING_PATH_OK,
		      "fresh conditioning path did not initialize for role %d",
		      role);
	} else {
		memset(&rejected, 0x5a, sizeof(rejected));
		saved_rejected = rejected;
		CHECK(retained_receive_init(&receive, retained_cursor),
		      "retained receive path did not initialize for role %d",
		      role);
		saved_receive = receive;
		expected_receive = receive;
		CHECK(v32_receive_path_reacquire_conditioning(
			      &expected_receive) == V32_RECEIVE_PATH_OK &&
		      v32_conditioning_path_init_from_receive(
			      &rejected, (enum v32_transmitter_role)2,
			      &receive) == V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
		      memcmp(&rejected, &saved_rejected,
			     sizeof(rejected)) == 0 &&
		      v32_conditioning_path_init_from_receive(
			      &path, role, &receive) ==
			      V32_CONDITIONING_PATH_OK &&
		      memcmp(&receive, &saved_receive, sizeof(receive)) == 0 &&
		      memcmp(&path.receive, &expected_receive,
			     sizeof(path.receive)) == 0 &&
		      !path.conditioning.has_previous_quadrant &&
		      path.conditioning.observation_epoch == 0 &&
		      path.observation_epoch == path.receive.equalizer.epoch,
		      "retained conditioning path changed its boundary for role %d",
		      role);
	}
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !complete; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;
		enum v32_conditioning_path_result result;

		CHECK(source_sample(
			      &source, &sample, &reference, &accepted),
		      "generated conditioning source stopped at %u", index);
		result = v32_conditioning_path_sample(
			&path, sample, accepted ? &reference : NULL);
		CHECK(result == V32_CONDITIONING_PATH_OK ||
		      result == V32_CONDITIONING_PATH_COMPLETE,
		      "generated conditioning path stopped at %u: %d",
		      index, result);
		complete = result == V32_CONDITIONING_PATH_COMPLETE;
	}
	CHECK(complete,
	      "generated conditioning reached its line limit for role %d/%d "
	      "phase=%d trn=%u epoch=%llu signal=%d cursor=%u carrier=%u",
	      role, retained, path.conditioning.phase,
	      path.conditioning.trn_symbols,
	      (unsigned long long)path.conditioning.observation_epoch,
	      path.receive.line.timing.signal_detected,
	      retained_cursor, carrier_octant);
	CHECK(path.receive.line.timing.tracking_enabled &&
	      v32_conditioning_path_is_complete(&path) &&
	      path.receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      path.conditioning.trn_symbols >=
		      V32_CONDITIONING_TRN_MIN_SYMBOLS,
	      "generated conditioning state was incomplete for role %d/%d",
	      role, retained);
	CHECK(source.rate_started,
	      "generated source had not crossed rate for role %d/%d",
	      role, retained);
	CHECK(v32_conditioning_receiver_framing_state(
		      &path.conditioning, &expected_decoder) ==
		      V32_CONDITIONING_RECEIVER_OK &&
	      v32_conditioning_path_handoff(&path, &handoff) ==
		      V32_CONDITIONING_PATH_OK &&
	      v32_conditioning_path_handoff_is_valid(&handoff),
	      "generated conditioning handoff was invalid for role %d/%d",
	      role, retained);
	CHECK(handoff.decoder.phase_quadrant ==
		      expected_decoder.phase_quadrant &&
	      handoff.decoder.descrambler.history ==
		      expected_decoder.descrambler.history &&
	      handoff.decoder.descrambler.transmitter_role ==
		      expected_decoder.descrambler.transmitter_role,
	      "generated conditioning framing changed for role %d/%d",
	      role, retained);
	CHECK(handoff.observation_ready &&
	      handoff.receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      handoff.observation.epoch ==
		      path.conditioning.observation_epoch + 1u,
	      "generated conditioning lost its buffered rate observation");
	*completed = path;
	return true;
}

static bool test_s_timing_lifecycle(void)
{
	struct generated_source source;
	struct v32_conditioning_path path;
	bool qualified = false;
	unsigned held_samples = 0;
	int16_t held_phase = 0, held_fraction = 0;

	CHECK(source_init(&source, V32_CALLING_TRANSMITTER) &&
	      v32_conditioning_path_init(&path, V32_CALLING_TRANSMITTER,
		      0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) == V32_CONDITIONING_PATH_OK,
	      "S timing fixture did not initialize");
	for (unsigned index = 0; index < TEST_LINE_SAMPLE_LIMIT; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;

		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "S timing source stopped");
		/* Keep S briefly, then interrupt it before a genuine S-bar edge. */
		if (qualified && held_samples >= 100)
			sample = 0;
		CHECK(v32_conditioning_path_sample(&path, sample,
			      accepted ? &reference : NULL) == V32_CONDITIONING_PATH_OK,
		      "interrupted S falsely completed conditioning");
		if (!qualified && path.conditioning.phase == V32_CONDITIONING_MATCH_TRN) {
			qualified = true;
			held_phase = path.receive.line.timing.polyphase_index;
			held_fraction = path.receive.line.timing.fractional_phase_q11;
		}
		if (!qualified)
			continue;
		if (path.conditioning.phase == V32_CONDITIONING_SEARCH_S_BAR) {
			CHECK(held_samples >= 100 && path.receive.line.timing.tracking_enabled,
			      "lost S did not resume timing acquisition");
			return true;
		}
		CHECK(path.conditioning.phase == V32_CONDITIONING_MATCH_TRN &&
		      !path.receive.line.timing.tracking_enabled &&
		      path.receive.line.timing.polyphase_index == held_phase &&
		      path.receive.line.timing.fractional_phase_q11 == held_fraction,
		      "qualified S timing moved at sample %u", index);
		held_samples++;
	}
	CHECK(false, "interrupted S did not release timing hold");
}

static bool test_contracts(
	const struct v32_conditioning_path *completed)
{
	struct v32_conditioning_path path;
	struct v32_conditioning_path saved;
	struct v32_conditioning_path_handoff handoff;
	struct v32_conditioning_path_handoff saved_handoff;
	struct v32_echo_reference invalid_reference = {
		.point = { NAN, 0.0 },
	};

	CHECK(v32_conditioning_path_init(
		      NULL, V32_CALLING_TRANSMITTER, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_init(
		      &path, (enum v32_transmitter_role)2, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_init(
		      &path, V32_CALLING_TRANSMITTER, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_CONDITIONING_PATH_OK,
	      "conditioning-path contract fixture did not initialize");
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	saved = path;
	CHECK(v32_conditioning_path_sample(NULL, 0, NULL) ==
		      V32_CONDITIONING_PATH_INVALID_STATE &&
	      v32_conditioning_path_sample(&path, 0, NULL) ==
		      V32_CONDITIONING_PATH_INVALID_STATE &&
	      v32_conditioning_path_sample(
		      &path, 0,
		      (const struct v32_echo_reference *)&path) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_sample(
		      &path, 0, &invalid_reference) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_handoff(&path, &handoff) ==
		      V32_CONDITIONING_PATH_INVALID_STATE &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "conditioning-path rejection changed state or output");
	CHECK(v32_conditioning_path_init_from_receive(
		      NULL, V32_CALLING_TRANSMITTER, &path.receive) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_init_from_receive(
		      &path, V32_CALLING_TRANSMITTER, NULL) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_init_from_receive(
		      &path, V32_CALLING_TRANSMITTER, &path.receive) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "conditioning path accepted an invalid retained owner");

	path.receive.equalizer.epoch++;
	CHECK(!v32_conditioning_path_is_valid(&path) &&
	      v32_conditioning_path_sample(&path, 0, NULL) ==
		      V32_CONDITIONING_PATH_INVALID_STATE,
	      "conditioning path accepted mismatched epochs");

	path = *completed;
	saved = path;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v32_conditioning_path_sample(&path, 0, NULL) ==
		      V32_CONDITIONING_PATH_COMPLETE &&
	      v32_conditioning_path_handoff(&path, NULL) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      v32_conditioning_path_handoff(
		      &path,
		      (struct v32_conditioning_path_handoff *)&path) ==
		      V32_CONDITIONING_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "complete conditioning path changed on rejected calls");
	CHECK(v32_conditioning_path_handoff(&path, &handoff) ==
		      V32_CONDITIONING_PATH_OK &&
	      v32_conditioning_path_handoff_is_valid(&handoff),
	      "complete conditioning handoff was not valid");
	saved_handoff = handoff;
	for (unsigned int pending = 0; pending < 2; ++pending) {
		static const size_t fields[] = {
			offsetof(struct v32_equalizer_observation, i),
			offsetof(struct v32_equalizer_observation, q),
			offsetof(struct v32_equalizer_observation, phase),
			offsetof(struct v32_equalizer_observation, magnitude),
			offsetof(struct v32_equalizer_observation, epoch),
			offsetof(struct v32_equalizer_observation, lineage),
		};
		unsigned char *bytes = (unsigned char *)&handoff.observation;

		handoff = saved_handoff;
		if (!pending) {
			CHECK(v32_receive_path_init(&handoff.receive, 0,
				      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) == V32_RECEIVE_PATH_OK,
			      "inactive handoff fixture failed");
			handoff.observation = (struct v32_equalizer_observation){ 0 };
			handoff.observation_ready = false;
		}
		CHECK(v32_conditioning_path_handoff_is_valid(&handoff),
		      "handoff comparison fixture %u invalid", pending);
		for (size_t field = 0; field < ARRAY_LENGTH(fields); ++field) {
			bytes[fields[field]] ^= 1u;
			CHECK(!v32_conditioning_path_handoff_is_valid(&handoff),
			      "handoff %u ignored observation field %zu", pending, field);
			bytes[fields[field]] ^= 1u;
		}
	}
	handoff = saved_handoff;
	handoff.observation_ready = !handoff.observation_ready;
	CHECK(!v32_conditioning_path_handoff_is_valid(&handoff),
	      "conditioning handoff accepted mismatched readiness");
	return true;
}

int main(void)
{
	struct v32_conditioning_path completed;

	for (unsigned int cursor = 0; cursor <= 3; ++cursor)
		for (unsigned int octant = 0; octant < 8; ++octant)
			for (unsigned int role = 0; role < 2; ++role)
				if (!run_generated_role(
					    (enum v32_transmitter_role)role,
					    cursor, octant, &completed))
					return 1;
	if (!test_s_timing_lifecycle() || !test_contracts(&completed))
		return 1;
	puts("V.32 conditioning path tests passed");
	return 0;
}
