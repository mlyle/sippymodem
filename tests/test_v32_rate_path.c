// SPDX-License-Identifier: BSD-3-Clause

#include <conditioning.h>
#include <conditioning_path.h>
#include <constellation.h>
#include <end_path.h>
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
	struct v32_rate_offer selected_offer;
	uint16_t rate_word;
	uint16_t selected_word;
	uint16_t end_word;
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

static struct v32_rate_offer selected_offer(void)
{
	return (struct v32_rate_offer){
		.rates = V32_RATE_CAPABILITY_4800,
		.trellis_at_highest_rate = false,
	};
}

static bool source_init(
	struct generated_source *source,
	enum v32_transmitter_role role)
{
	memset(source, 0, sizeof(*source));
	source->offer = role_offer(role);
	source->selected_offer = selected_offer();
	return qam_modulator_init(
		       &source->modulator, &V32_CHANNEL,
		       TEST_AMPLITUDE) == QAM_OK &&
	       v32_conditioning_transmitter_init(
		       &source->conditioning, role,
		       V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		       V32_CONDITIONING_OK &&
	       v32_rate_signal_encode(
		       V32_RATE_SIGNAL, &source->offer,
		       &source->rate_word) == V32_RATE_SIGNAL_OK &&
	       v32_rate_signal_encode(
		       V32_RATE_SIGNAL, &source->selected_offer,
		       &source->selected_word) == V32_RATE_SIGNAL_OK &&
	       v32_rate_signal_encode(
		       V32_END_SIGNAL, &source->selected_offer,
		       &source->end_word) == V32_RATE_SIGNAL_OK;
}

static bool source_set_word(
	struct generated_source *source, uint16_t word)
{
	if (!source->rate_started || source->rate_dibit != 0)
		return false;
	source->rate_word = word;
	return true;
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

static bool reject_wrong_end(
	const struct generated_source *started_source,
	const struct v32_rate_path_handoff *rate,
	const struct v32_rate_offer *selected_offer)
{
	struct generated_source source = *started_source;
	struct v32_rate_offer wrong_offer = {
		.rates = V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = false,
	};
	struct v32_end_path path;
	uint16_t wrong_word;
	unsigned int wrong_start = 0;
	bool wrong_started = false;
	bool rejected = false;

	CHECK(v32_rate_signal_encode(
		      V32_END_SIGNAL, &wrong_offer, &wrong_word) ==
		      V32_RATE_SIGNAL_OK &&
	      v32_end_path_init(
		      &path, rate, selected_offer,
		      V32_EQUALIZER_ADAPTATION_FINE) == V32_END_PATH_OK,
	      "wrong-E control did not initialize");
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !rejected; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;
		enum v32_end_path_result result;

		if (!wrong_started && source.rate_dibit == 0) {
			wrong_start = source.rate_symbols;
			CHECK(source_set_word(&source, wrong_word),
			      "wrong-E control did not enter E");
			wrong_started = true;
		}
		if (wrong_started &&
		    source.rate_symbols ==
			    wrong_start + V32_RATE_SIGNAL_DIBITS &&
		    source.rate_word == wrong_word)
			CHECK(source_set_word(&source, UINT16_MAX),
			      "wrong-E control did not leave E");
		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "wrong-E source stopped at %u", index);
		result = v32_end_path_sample(
			&path, sample, accepted ? &reference : NULL);
		if (result == V32_END_PATH_SEQUENCE_REJECTED) {
			CHECK(wrong_started && source.rate_word == UINT16_MAX &&
			      v32_end_path_is_valid(&path) &&
			      !v32_end_path_is_complete(&path) &&
			      path.receive.equalizer.decision_pending &&
			      path.observation_epoch != UINT64_MAX &&
			      path.observation_epoch + 1u ==
				      path.receive.equalizer
					      .pending_observation.epoch &&
			      path.detector.aligned &&
			      path.detector.locked_rate_word != wrong_word &&
			      path.selected_offer.rates == selected_offer->rates &&
			      path.target_adaptation ==
				      V32_EQUALIZER_ADAPTATION_FINE &&
			      !path.end_received,
			      "wrong E committed semantic state");
			rejected = true;
		} else {
			CHECK(result == V32_END_PATH_OK,
			      "wrong-E control stopped at %u: %d",
			      index, result);
		}
	}
	CHECK(rejected, "wrong selected E was not rejected");
	return true;
}

static bool run_role(
	enum v32_transmitter_role role,
	struct v32_conditioning_path_handoff *conditioning_handoff,
	struct v32_rate_path *started,
	struct v32_rate_path *completed,
	struct v32_end_path *end_started,
	struct v32_end_path *end_completed)
{
	struct generated_source source;
	struct v32_conditioning_path conditioning;
	struct v32_conditioning_path_handoff handoff;
	struct v32_rate_path path;
	struct v32_rate_path_handoff rate_handoff;
	struct v32_end_path end;
	struct v32_end_path_handoff end_handoff;
	enum v32_equalizer_adaptation end_adaptation;
	unsigned int end_source_limit;
	unsigned int b1_source_start = 0;
	bool end_sequence_started = false;
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
	if (role == V32_CALLING_TRANSMITTER)
		CHECK(reject_wrong_end(
			      &source, &rate_handoff,
			      &source.selected_offer),
		      "wrong-E control failed");

	end_adaptation = role == V32_CALLING_TRANSMITTER ?
		V32_EQUALIZER_ADAPTATION_DISABLED :
		V32_EQUALIZER_ADAPTATION_FINE;
	CHECK(v32_end_path_init(
		      &end, &rate_handoff, &source.selected_offer,
		      V32_EQUALIZER_ADAPTATION_FINE) == V32_END_PATH_OK &&
	      v32_end_path_is_valid(&end) &&
	      !v32_end_path_is_complete(&end),
	      "end path did not initialize for role %d", role);
	if (role == V32_CALLING_TRANSMITTER)
		CHECK(v32_end_path_set_adaptation(
			      &end,
			      V32_EQUALIZER_ADAPTATION_DISABLED) ==
			      V32_END_PATH_OK &&
		      end.target_adaptation ==
			      V32_EQUALIZER_ADAPTATION_DISABLED,
		      "answering endpoint did not disable at R3");
	*end_started = end;
	end_source_limit = 0;
	complete = false;
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !complete; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;
		enum v32_end_path_result result;

		if (!end_sequence_started && source.rate_dibit == 0) {
			end_source_limit = source.rate_symbols +
				2u * V32_RATE_SIGNAL_DIBITS;
			b1_source_start =
				(role == V32_ANSWERING_TRANSMITTER ?
					 end_source_limit : source.rate_symbols) +
				V32_RATE_SIGNAL_DIBITS;
			CHECK(source_set_word(
				      &source,
				      role == V32_ANSWERING_TRANSMITTER ?
					      source.selected_word :
					      source.end_word),
			      "source did not enter its post-rate sequence");
			end_sequence_started = true;
		}
		if (end_sequence_started &&
		    role == V32_ANSWERING_TRANSMITTER &&
		    source.rate_symbols == end_source_limit &&
		    source.rate_word != source.end_word)
			CHECK(source_set_word(&source, source.end_word),
			      "answering source did not enter E");
		if (end_sequence_started &&
		    source.rate_symbols == b1_source_start &&
		    source.rate_word == source.end_word)
			CHECK(source_set_word(&source, UINT16_MAX),
			      "source did not enter B1 after E");
		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "end source stopped at %u", index);
		result = v32_end_path_sample(
			&end, sample, accepted ? &reference : NULL);
		CHECK(result == V32_END_PATH_OK ||
		      result == V32_END_PATH_COMPLETE,
		      "end path stopped at %u: %d", index, result);
		complete = result == V32_END_PATH_COMPLETE;
	}
	CHECK(complete && end_sequence_started &&
	      v32_end_path_is_complete(&end) &&
	      end.receive.equalizer.adaptation == end_adaptation &&
	      end.target_adaptation == end_adaptation &&
	      end.selected_offer.rates == source.selected_offer.rates &&
	      end.selected_offer.trellis_at_highest_rate ==
		      source.selected_offer.trellis_at_highest_rate &&
	      source.rate_symbols > b1_source_start &&
	      source.rate_word == UINT16_MAX,
	      "end path changed the selected E sequence for role %d", role);
	CHECK(v32_end_path_handoff(&end, &end_handoff) ==
		      V32_END_PATH_OK &&
	      v32_end_path_handoff_is_valid(&end_handoff) &&
	      end_handoff.selected_offer.rates ==
		      source.selected_offer.rates &&
	      end_handoff.receive.equalizer.adaptation == end_adaptation &&
	      end_handoff.observation_epoch == end.observation_epoch &&
	      end_handoff.four_point.decoder.descrambler.history ==
		      end.four_point.decoder.descrambler.history,
	      "end handoff changed the selected receiver for role %d", role);
	*end_completed = end;
	return true;
}

static bool test_contracts(
	const struct v32_conditioning_path_handoff *conditioning,
	const struct v32_rate_path *started,
	const struct v32_rate_path *completed,
	const struct v32_end_path *end_started,
	const struct v32_end_path *end_completed)
{
	struct v32_conditioning_path_handoff bad_conditioning = *conditioning;
	struct v32_rate_path path;
	struct v32_rate_path saved;
	struct v32_rate_path_handoff handoff;
	struct v32_rate_path_handoff saved_handoff;
	struct v32_rate_path_handoff bad_handoff;
	struct v32_echo_reference invalid_reference = {
		.point = { NAN, 0.0 },
	};
	struct v32_rate_offer invalid_selection = { 0 };
	struct v32_rate_offer outside_selection = {
		.rates = V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	};
	struct v32_end_path end;
	struct v32_end_path saved_end;
	struct v32_end_path_handoff end_handoff;
	struct v32_end_path_handoff saved_end_handoff;
	union {
		struct v32_rate_path_handoff rate;
		struct v32_end_path end;
	} overlapping_init, saved_overlapping_init;

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

	CHECK(v32_rate_path_handoff(&path, &handoff) == V32_RATE_PATH_OK,
	      "rate handoff did not regenerate for end init contracts");
	saved_handoff = handoff;
	bad_handoff = handoff;
	bad_handoff.observation_ready = !bad_handoff.observation_ready;
	overlapping_init.rate = handoff;
	saved_overlapping_init = overlapping_init;
	memset(&end, 0x5a, sizeof(end));
	saved_end = end;
	CHECK(v32_end_path_init(
		      NULL, &handoff, &end_started->selected_offer,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, NULL, &end_started->selected_offer,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, &handoff, NULL,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &overlapping_init.end, &overlapping_init.rate,
		      &end_started->selected_offer,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, &handoff,
		      (const struct v32_rate_offer *)&end,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, &bad_handoff, &end_started->selected_offer,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, &handoff, &invalid_selection,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, &handoff, &outside_selection,
		      V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_init(
		      &end, &handoff, &end_started->selected_offer,
		      (enum v32_equalizer_adaptation)3) ==
	      V32_END_PATH_INVALID_ARGUMENT &&
	      memcmp(&end, &saved_end, sizeof(end)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0 &&
	      memcmp(
		      &overlapping_init, &saved_overlapping_init,
		      sizeof(overlapping_init)) == 0,
	      "end-path initialization rejection changed its inputs or output");

	end = *end_started;
	saved_end = end;
	memset(&end_handoff, 0x5a, sizeof(end_handoff));
	saved_end_handoff = end_handoff;
	CHECK(v32_end_path_set_adaptation(
		      NULL, V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_STATE &&
	      v32_end_path_set_adaptation(
		      &end, (enum v32_equalizer_adaptation)3) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_sample(
		      &end, 0,
		      (const struct v32_echo_reference *)&end) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_sample(&end, 0, &invalid_reference) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_handoff(&end, &end_handoff) ==
		      V32_END_PATH_INVALID_STATE &&
	      memcmp(&end, &saved_end, sizeof(end)) == 0 &&
	      memcmp(&end_handoff, &saved_end_handoff,
		     sizeof(end_handoff)) == 0,
	      "end-path rejection changed state or output");
	end.observation_epoch++;
	CHECK(!v32_end_path_is_valid(&end) &&
	      v32_end_path_sample(&end, 0, NULL) ==
		      V32_END_PATH_INVALID_STATE,
	      "end path accepted a spliced decision epoch");
	end = *end_started;
	end.end_received = true;
	CHECK(!v32_end_path_is_valid(&end),
	      "end path accepted completion without a reset detector");

	end = *end_completed;
	saved_end = end;
	CHECK(v32_end_path_sample(&end, 0, NULL) ==
		      V32_END_PATH_COMPLETE &&
	      v32_end_path_set_adaptation(
		      &end, V32_EQUALIZER_ADAPTATION_FINE) ==
		      V32_END_PATH_INVALID_STATE &&
	      v32_end_path_handoff(&end, NULL) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      v32_end_path_handoff(
		      &end, (struct v32_end_path_handoff *)&end) ==
		      V32_END_PATH_INVALID_ARGUMENT &&
	      memcmp(&end, &saved_end, sizeof(end)) == 0,
	      "complete end path changed on rejected calls");
	CHECK(v32_end_path_handoff(&end, &end_handoff) ==
		      V32_END_PATH_OK &&
	      v32_end_path_handoff_is_valid(&end_handoff),
	      "complete end handoff was not valid");
	end_handoff.selected_offer.rates ^= V32_RATE_CAPABILITY_4800;
	CHECK(!v32_end_path_handoff_is_valid(&end_handoff),
	      "end handoff accepted an invalid selected rate");
	end = *end_completed;
	end.detector.window = 1;
	CHECK(!v32_end_path_is_valid(&end),
	      "end path accepted a nonreset completed detector");
	return true;
}

int main(void)
{
	struct v32_conditioning_path_handoff conditioning;
	struct v32_rate_path started;
	struct v32_rate_path completed;
	struct v32_end_path end_started;
	struct v32_end_path end_completed;

	if (!run_role(
		    V32_CALLING_TRANSMITTER, &conditioning,
		    &started, &completed, &end_started, &end_completed) ||
	    !run_role(
		    V32_ANSWERING_TRANSMITTER, &conditioning,
		    &started, &completed, &end_started, &end_completed) ||
	    !test_contracts(
		    &conditioning, &started, &completed,
		    &end_started, &end_completed))
		return 1;
	puts("V.32 rate path tests passed");
	return 0;
}
