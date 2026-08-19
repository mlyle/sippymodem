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

static bool run_generated_role(
	enum v32_transmitter_role role,
	struct v32_conditioning_path *completed)
{
	struct generated_source source;
	struct v32_conditioning_path path;
	struct v32_conditioning_path_handoff handoff;
	struct v32_symbol_decoder expected_decoder;
	bool complete = false;

	CHECK(completed != NULL && source_init(&source, role) &&
	      v32_conditioning_path_init(
		      &path, role, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_CONDITIONING_PATH_OK,
	      "generated conditioning path did not initialize for role %d",
	      role);
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
	CHECK(complete && v32_conditioning_path_is_complete(&path) &&
	      path.receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      path.conditioning.trn_symbols >=
		      V32_CONDITIONING_TRN_MIN_SYMBOLS &&
	      source.rate_started &&
	      v32_conditioning_receiver_framing_state(
		      &path.conditioning, &expected_decoder) ==
		      V32_CONDITIONING_RECEIVER_OK &&
	      v32_conditioning_path_handoff(&path, &handoff) ==
		      V32_CONDITIONING_PATH_OK &&
	      v32_conditioning_path_handoff_is_valid(&handoff) &&
	      handoff.decoder.phase_quadrant ==
		      expected_decoder.phase_quadrant &&
	      handoff.decoder.descrambler.history ==
		      expected_decoder.descrambler.history &&
	      handoff.decoder.descrambler.transmitter_role ==
		      expected_decoder.descrambler.transmitter_role,
	      "generated conditioning handoff changed for role %d", role);
	CHECK(handoff.observation_ready &&
	      handoff.receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      handoff.observation.epoch ==
		      path.conditioning.observation_epoch + 1u,
	      "generated conditioning lost its buffered rate observation");
	*completed = path;
	return true;
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
	handoff.observation_ready = !handoff.observation_ready;
	CHECK(!v32_conditioning_path_handoff_is_valid(&handoff),
	      "conditioning handoff accepted mismatched readiness");
	return true;
}

int main(void)
{
	struct v32_conditioning_path completed;

	if (!run_generated_role(V32_CALLING_TRANSMITTER, &completed) ||
	    !run_generated_role(V32_ANSWERING_TRANSMITTER, &completed) ||
	    !test_contracts(&completed))
		return 1;
	puts("V.32 conditioning path tests passed");
	return 0;
}
