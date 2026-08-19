// SPDX-License-Identifier: BSD-3-Clause

#include <conditioning.h>
#include <conditioning_path.h>
#include <constellation.h>
#include <qam.h>
#include <rate_path.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_LINE_SAMPLE_LIMIT = 8000,
	TEST_CALLING_RATE_OBSERVATIONS = 2 * V32_RATE_SIGNAL_DIBITS,
	/* The generated answering transition exposes two nonidentical candidate
	 * words before the first pair which the standard permits us to accept. */
	TEST_ANSWERING_RATE_OBSERVATIONS = 4 * V32_RATE_SIGNAL_DIBITS,
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

struct generated_source {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter conditioning;
	struct v32_symbol_encoder rate_encoder;
	struct qam_point point;
	struct v32_rate_offer offer;
	uint16_t rate_word;
	unsigned int rate_dibit;
	unsigned int rate_symbols;
	bool rate_started;
};

static struct v32_rate_offer role_offer(enum v32_transmitter_role role)
{
	if (role == V32_CALLING_TRANSMITTER)
		return (struct v32_rate_offer){
			.rates = V32_RATE_CAPABILITY_4800 |
				 V32_RATE_CAPABILITY_7200 |
				 V32_RATE_CAPABILITY_9600 |
				 V32_RATE_CAPABILITY_12000 |
				 V32_RATE_CAPABILITY_14400,
			.trellis_at_highest_rate = true,
		};
	return (struct v32_rate_offer){
		.rates = V32_RATE_CAPABILITY_4800 |
			 V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = false,
	};
}

static bool source_init(
	struct generated_source *source,
	enum v32_transmitter_role role)
{
	memset(source, 0, sizeof(*source));
	source->offer = role_offer(role);
	return qam_modulator_init(
		       &source->modulator, &V32_CHANNEL,
		       TEST_AMPLITUDE) == QAM_OK &&
	       v32_conditioning_transmitter_init(
		       &source->conditioning, role,
		       V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		       V32_CONDITIONING_OK &&
	       v32_rate_signal_encode(
		       V32_RATE_SIGNAL, &source->offer,
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
			    source->rate_word, source->rate_dibit,
			    &dibit) != V32_RATE_SIGNAL_OK ||
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

static bool complete_conditioning(
	struct generated_source *source,
	enum v32_transmitter_role role,
	struct v32_conditioning_path *conditioning,
	struct v32_conditioning_path_handoff *handoff)
{
	bool complete = false;

	CHECK(source_init(source, role) &&
	      v32_conditioning_path_init(
		      conditioning, role, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_CONDITIONING_PATH_OK,
	      "rate-path conditioning did not initialize for role %d", role);
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !complete; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;
		enum v32_conditioning_path_result result;

		CHECK(source_sample(source, &sample, &reference, &accepted),
		      "conditioning source stopped at %u", index);
		result = v32_conditioning_path_sample(
			conditioning, sample, accepted ? &reference : NULL);
		CHECK(result == V32_CONDITIONING_PATH_OK ||
		      result == V32_CONDITIONING_PATH_COMPLETE,
		      "conditioning path stopped at %u: %d", index, result);
		complete = result == V32_CONDITIONING_PATH_COMPLETE;
	}
	CHECK(complete && source->rate_started &&
	      v32_conditioning_path_handoff(conditioning, handoff) ==
		      V32_CONDITIONING_PATH_OK &&
	      handoff->observation_ready &&
	      handoff->receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE,
	      "conditioning did not expose the first rate observation");
	return true;
}

static bool run_role(
	enum v32_transmitter_role role,
	struct v32_conditioning_path_handoff *conditioning_handoff,
	struct v32_rate_path *started,
	struct v32_rate_path *completed)
{
	struct generated_source source;
	struct v32_conditioning_path conditioning;
	struct v32_conditioning_path_handoff handoff;
	struct v32_rate_path path;
	struct v32_rate_path_handoff rate_handoff;
	bool complete = false;

	CHECK(complete_conditioning(
		      &source, role, &conditioning, &handoff),
	      "rate-path conditioning failed for role %d", role);
	CHECK(v32_rate_path_init(&path, &handoff) == V32_RATE_PATH_OK &&
	      v32_rate_path_is_valid(&path) &&
	      !v32_rate_path_is_complete(&path),
	      "rate path did not initialize for role %d", role);
	*conditioning_handoff = handoff;
	*started = path;
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !complete; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;
		enum v32_rate_path_result result;

		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "rate source stopped at %u", index);
		result = v32_rate_path_sample(
			&path, sample, accepted ? &reference : NULL);
		CHECK(result == V32_RATE_PATH_OK ||
		      result == V32_RATE_PATH_COMPLETE,
		      "rate path stopped at %u: %d", index, result);
		complete = result == V32_RATE_PATH_COMPLETE;
	}
	CHECK(complete && v32_rate_path_is_complete(&path) &&
	      path.detector.aligned &&
	      path.detector.locked_rate_word == source.rate_word &&
	      path.observation_epoch ==
		      conditioning.conditioning.observation_epoch +
			      (role == V32_CALLING_TRANSMITTER ?
				       TEST_CALLING_RATE_OBSERVATIONS :
				       TEST_ANSWERING_RATE_OBSERVATIONS) &&
	      source.rate_symbols >= 2u * V32_RATE_SIGNAL_DIBITS,
	      "rate path changed the exact repeated signal for role %d", role);
	CHECK(v32_rate_path_handoff(&path, &rate_handoff) ==
		      V32_RATE_PATH_OK &&
	      v32_rate_path_handoff_is_valid(&rate_handoff) &&
	      rate_handoff.offer.rates == source.offer.rates &&
	      rate_handoff.offer.trellis_at_highest_rate ==
		      source.offer.trellis_at_highest_rate &&
	      rate_handoff.detector.aligned &&
	      rate_handoff.detector.locked_rate_word == source.rate_word &&
	      rate_handoff.observation_epoch == path.observation_epoch &&
	      rate_handoff.four_point.decoder.phase_quadrant ==
		      path.four_point.decoder.phase_quadrant &&
	      rate_handoff.four_point.decoder.descrambler.history ==
		      path.four_point.decoder.descrambler.history,
	      "rate handoff changed the aligned receiver for role %d", role);
	*completed = path;
	return true;
}

static bool test_contracts(
	const struct v32_conditioning_path_handoff *conditioning,
	const struct v32_rate_path *started,
	const struct v32_rate_path *completed)
{
	struct v32_conditioning_path_handoff bad_conditioning = *conditioning;
	struct v32_rate_path path;
	struct v32_rate_path saved;
	struct v32_rate_path_handoff handoff;
	struct v32_rate_path_handoff saved_handoff;
	struct v32_echo_reference invalid_reference = {
		.point = { NAN, 0.0 },
	};

	memset(&path, 0x5a, sizeof(path));
	saved = path;
	bad_conditioning.observation_ready =
		!bad_conditioning.observation_ready;
	CHECK(v32_rate_path_init(NULL, conditioning) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      v32_rate_path_init(&path, NULL) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      v32_rate_path_init(
		      (struct v32_rate_path *)&bad_conditioning,
		      &bad_conditioning) == V32_RATE_PATH_INVALID_ARGUMENT &&
	      v32_rate_path_init(&path, &bad_conditioning) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "rate-path initialization rejection changed its output");

	path = *started;
	saved = path;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v32_rate_path_sample(NULL, 0, NULL) ==
		      V32_RATE_PATH_INVALID_STATE &&
	      v32_rate_path_sample(
		      &path, 0,
		      (const struct v32_echo_reference *)&path) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      v32_rate_path_sample(&path, 0, &invalid_reference) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      v32_rate_path_handoff(&path, &handoff) ==
		      V32_RATE_PATH_INVALID_STATE &&
	      memcmp(&path, &saved, sizeof(path)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "rate-path rejection changed state or output");
	path.observation_epoch++;
	CHECK(!v32_rate_path_is_valid(&path) &&
	      v32_rate_path_sample(&path, 0, NULL) ==
		      V32_RATE_PATH_INVALID_STATE,
	      "rate path accepted a spliced decision epoch");

	path = *completed;
	saved = path;
	CHECK(v32_rate_path_sample(&path, 0, NULL) ==
		      V32_RATE_PATH_COMPLETE &&
	      v32_rate_path_handoff(&path, NULL) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      v32_rate_path_handoff(
		      &path, (struct v32_rate_path_handoff *)&path) ==
		      V32_RATE_PATH_INVALID_ARGUMENT &&
	      memcmp(&path, &saved, sizeof(path)) == 0,
	      "complete rate path changed on rejected calls");
	CHECK(v32_rate_path_handoff(&path, &handoff) == V32_RATE_PATH_OK &&
	      v32_rate_path_handoff_is_valid(&handoff),
	      "complete rate handoff was not valid");
	handoff.observation_ready = !handoff.observation_ready;
	CHECK(!v32_rate_path_handoff_is_valid(&handoff),
	      "rate handoff accepted mismatched readiness");
	CHECK(v32_rate_path_handoff(&path, &handoff) == V32_RATE_PATH_OK,
	      "complete rate handoff did not regenerate");
	handoff.offer.rates ^= V32_RATE_CAPABILITY_4800;
	CHECK(!v32_rate_path_handoff_is_valid(&handoff),
	      "rate handoff accepted an offer outside its locked word");
	return true;
}

int main(void)
{
	struct v32_conditioning_path_handoff conditioning;
	struct v32_rate_path started;
	struct v32_rate_path completed;

	if (!run_role(
		    V32_CALLING_TRANSMITTER, &conditioning,
		    &started, &completed) ||
	    !run_role(
		    V32_ANSWERING_TRANSMITTER, &conditioning,
		    &started, &completed) ||
	    !test_contracts(&conditioning, &started, &completed))
		return 1;
	puts("V.32 rate path tests passed");
	return 0;
}
