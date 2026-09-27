// SPDX-License-Identifier: BSD-3-Clause

#include <conditioning.h>
#include <b1_path.h>
#include <carrier.h>
#include <conditioning_path.h>
#include <constellation.h>
#include <data_path.h>
#include <end_path.h>
#include <qam.h>
#include <rate_path.h>
#include <trellis.h>

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
	TEST_SELECTED_MODES = 6,
	TEST_DATA_GROUPS = 64,
};

struct generated_source {
	struct qam_modulator modulator;
	struct v32_conditioning_transmitter conditioning;
	struct v32_symbol_encoder rate_encoder;
	struct v32_trellis_encoder trellis_encoder;
	struct qam_point point;
	struct v32_rate_offer offer;
	struct v32_rate_offer selected_offer;
	uint16_t rate_word;
	uint16_t selected_word;
	uint16_t end_word;
	unsigned int rate_dibit;
	unsigned int rate_symbols;
	unsigned int selected_symbols;
	unsigned int retrain_symbols;
	uint32_t b1_history;
	bool rate_started;
	bool selected_started;
	bool retrain_started;
	bool retrain_alternating;
};

static const struct v32_rate_offer SELECTED_OFFERS[TEST_SELECTED_MODES] = {
	{
		.rates = V32_RATE_CAPABILITY_4800,
		.trellis_at_highest_rate = false,
	},
	{
		.rates = V32_RATE_CAPABILITY_7200,
		.trellis_at_highest_rate = true,
	},
	{
		.rates = V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = false,
	},
	{
		.rates = V32_RATE_CAPABILITY_9600,
		.trellis_at_highest_rate = true,
	},
	{
		.rates = V32_RATE_CAPABILITY_12000,
		.trellis_at_highest_rate = true,
	},
	{
		.rates = V32_RATE_CAPABILITY_14400,
		.trellis_at_highest_rate = true,
	},
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

static uint8_t selected_trellis_data_bits(
	const struct v32_rate_offer *offer)
{
	if (!offer->trellis_at_highest_rate)
		return 0;
	switch (offer->rates) {
	case V32_RATE_CAPABILITY_7200:
		return V32_7200_DATA_BITS;
	case V32_RATE_CAPABILITY_9600:
		return V32_9600_TRELLIS_DATA_BITS;
	case V32_RATE_CAPABILITY_12000:
		return V32_12000_DATA_BITS;
	case V32_RATE_CAPABILITY_14400:
		return V32_14400_DATA_BITS;
	default:
		return 0;
	}
}

static bool source_init(
	struct generated_source *source,
	enum v32_transmitter_role role,
	const struct v32_rate_offer *selected_offer)
{
	struct qam_channel channel;

	memset(source, 0, sizeof(*source));
	source->offer = role_offer(role);
	source->selected_offer = *selected_offer;
	source->offer.rates |= selected_offer->rates;
	if (role == V32_ANSWERING_TRANSMITTER &&
	    selected_offer->trellis_at_highest_rate &&
	    selected_offer->rates >= V32_RATE_CAPABILITY_9600)
		source->offer.trellis_at_highest_rate = true;
	return v32_carrier_channel(&channel) == V32_CARRIER_OK &&
	       qam_modulator_init(
		       &source->modulator, &channel,
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
	if (word == UINT16_MAX) {
		uint8_t data_bits = selected_trellis_data_bits(
			&source->selected_offer);

		if (source->selected_started)
			return false;
		if (data_bits != 0 &&
		    v32_trellis_encoder_init(
			    &source->trellis_encoder, data_bits,
			    V32_TRELLIS_INITIAL_DIFFERENTIAL_OUTPUT) !=
			    V32_TRELLIS_OK)
			return false;
		if (source->selected_offer.rates ==
			    V32_RATE_CAPABILITY_9600 &&
		    data_bits == 0)
			source->rate_encoder.phase_quadrant = 0;
		source->selected_started = true;
	}
	source->rate_word = word;
	return true;
}

static bool source_next_selected_point(struct generated_source *source)
{
	uint8_t data_bits = selected_trellis_data_bits(
		&source->selected_offer);
	bool b1 = source->selected_symbols < V32_B1_PATH_SYMBOLS;
	unsigned int payload = b1 ? 0 :
		source->selected_symbols - V32_B1_PATH_SYMBOLS;
	uint8_t input;
	uint8_t signal;
	uint8_t retrain_quadrant;

	if (source->retrain_started) {
		retrain_quadrant = source->retrain_alternating &&
					   (source->retrain_symbols & 1u) != 0 ?
				   2u : 0u;
		source->retrain_symbols++;
		return v32_4800_constellation_point(
			       retrain_quadrant, &source->point) ==
		       V32_CONSTELLATION_OK;
	}

	if (data_bits != 0) {
		uint8_t scrambled = 0;

		input = b1 ?
			(uint8_t)((UINT8_C(1) << data_bits) - 1u) :
			(uint8_t)(payload & ((UINT8_C(1) << data_bits) - 1u));
		if (data_bits == V32_14400_DATA_BITS) {
			for (int shift = 3; shift >= 0; shift -= 3) {
				uint8_t group;

				if (v32_scramble_group(
					    &source->rate_encoder.scrambler,
					    (uint8_t)((input >> shift) & 7u),
					    3, &group) != V32_FRAMING_OK)
					return false;
				scrambled |= (uint8_t)(group << shift);
			}
		} else if (v32_scramble_group(
				   &source->rate_encoder.scrambler, input,
				   data_bits, &scrambled) != V32_FRAMING_OK) {
			return false;
		}
		if (v32_trellis_encode(
			    &source->trellis_encoder, scrambled, &signal) !=
			    V32_TRELLIS_OK ||
		    v32_trellis_constellation_point(
			    data_bits, signal, &source->point) !=
			    V32_CONSTELLATION_OK)
			return false;
	} else if (source->selected_offer.rates ==
		   V32_RATE_CAPABILITY_9600) {
		input = b1 ? 15 : (uint8_t)(payload & 15u);
		if (v32_9600_symbol_encode(
			    &source->rate_encoder, input, &signal) !=
			    V32_FRAMING_OK ||
		    v32_constellation_point(signal, &source->point) !=
			    V32_CONSTELLATION_OK)
			return false;
	} else {
		input = b1 ? 3 : (uint8_t)(payload & 3u);
		if (v32_4800_symbol_encode(
			    &source->rate_encoder, input, &signal) !=
			    V32_FRAMING_OK ||
		    v32_4800_constellation_point(signal, &source->point) !=
			    V32_CONSTELLATION_OK)
			return false;
	}
	source->selected_symbols++;
	if (source->selected_symbols == V32_B1_PATH_SYMBOLS)
		source->b1_history = source->rate_encoder.scrambler.history;
	return true;
}

static bool source_next_point(struct generated_source *source)
{
	uint8_t quadrant;

	if (source->selected_started)
		return source_next_selected_point(source);

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

static bool consume_data_batch(
	const struct v32_data_batch *batch, uint8_t bit_count,
	size_t *groups, bool *multiple)
{
	uint8_t mask = (uint8_t)((UINT8_C(1) << bit_count) - 1u);

	CHECK(batch->count <= V32_DATA_PATH_MAX_GROUPS &&
	      !batch->retrain_requested,
	      "ordinary data requested retrain or exceeded its batch bound");
	*multiple = *multiple || batch->count > 1;
	for (uint8_t index = 0; index < batch->count; ++index) {
		const struct v32_data_group *group = &batch->groups[index];
		uint8_t expected = (uint8_t)(*groups & mask);

		CHECK(group->bit_count == bit_count && group->value == expected,
		      "data group %zu changed: %u/%u bits %u/%u", *groups,
		      group->value, expected, group->bit_count, bit_count);
		(*groups)++;
	}
	return true;
}

static bool detect_remote_retrain(
	const struct generated_source *online_source,
	const struct v32_data_path *online_data,
	enum v32_transmitter_role source_role,
	const struct v32_rate_offer *selected_offer)
{
	struct generated_source source = *online_source;
	struct v32_data_path data = *online_data;
	struct v32_data_batch batch;
	bool requested = false;

	source.retrain_started = true;
	source.retrain_alternating =
		source_role == V32_ANSWERING_TRANSMITTER;
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !requested; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;

		CHECK(source_sample(&source, &sample, &reference, &accepted) &&
		      v32_data_path_sample(
			      &data, sample, accepted ? &reference : NULL,
			      &batch) == V32_DATA_PATH_OK &&
		      batch.count <= V32_DATA_PATH_MAX_GROUPS,
		      "retrain path stopped at %u for role/mode %d/%u:%u",
		      index, source_role, selected_offer->rates,
		      selected_offer->trellis_at_highest_rate);
		requested = batch.retrain_requested;
	}
	CHECK(requested && source.retrain_symbols >=
			      V32_DATA_RETRAIN_OBSERVATIONS &&
	      source.retrain_symbols <=
		      V32_DATA_RETRAIN_OBSERVATIONS + 128u &&
	      v32_data_path_is_valid(&data),
	      "equalized retrain was not bounded for role/mode %d/%u:%u (%u symbols, %u evidence)",
	      source_role, selected_offer->rates,
	      selected_offer->trellis_at_highest_rate,
	      source.retrain_symbols,
	      data.retrain.repeated_pair_observations);
	return true;
}

static bool complete_conditioning(
	struct generated_source *source,
	enum v32_transmitter_role role,
	const struct v32_rate_offer *selected_offer,
	bool cleardown,
	struct v32_conditioning_path *conditioning,
	struct v32_conditioning_path_handoff *handoff)
{
	bool complete = false;

	CHECK(source_init(source, role, selected_offer) &&
	      v32_conditioning_path_init(
		      conditioning, role, 0,
		      V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_CONDITIONING_PATH_OK,
	      "rate-path conditioning did not initialize for role %d", role);
	if (cleardown)
		source->rate_word = 0x0111;
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

static bool test_cleardown(enum v32_transmitter_role role)
{
	struct generated_source source;
	struct v32_conditioning_path conditioning;
	struct v32_conditioning_path_handoff handoff;
	struct v32_rate_path rate;
	struct v32_rate_path_handoff rejected;
	enum v32_rate_path_result result;
	bool cleared = false;

	CHECK(complete_conditioning(&source, role, &SELECTED_OFFERS[0],
				    true, &conditioning, &handoff),
	      "cleardown conditioning did not complete");
	result = v32_rate_path_init(&rate, &handoff);
	CHECK(result == V32_RATE_PATH_OK, "cleardown rate path did not initialize");
	for (unsigned int index = 0; index < TEST_LINE_SAMPLE_LIMIT && !cleared; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;

		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "cleardown source stopped");
		result = v32_rate_path_sample(&rate, sample, accepted ? &reference : NULL);
		CHECK(result == V32_RATE_PATH_OK || result == V32_RATE_PATH_CLEARDOWN,
		      "cleardown became a usable rate or invalid state: %d", result);
		cleared = result == V32_RATE_PATH_CLEARDOWN;
	}
	CHECK(cleared && v32_rate_path_is_valid(&rate) &&
	      !v32_rate_path_is_complete(&rate) &&
	      v32_rate_path_handoff(&rate, &rejected) == V32_RATE_PATH_INVALID_STATE,
	      "cleardown was lost or exposed a negotiated-rate handoff");
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
	const struct v32_rate_offer *selected_offer,
	struct v32_conditioning_path_handoff *conditioning_handoff,
	struct v32_rate_path *started,
	struct v32_rate_path *completed,
	struct v32_end_path *end_started,
	struct v32_end_path *end_completed,
	struct v32_end_path_handoff *completed_end_handoff,
	struct v32_b1_path *b1_started,
	struct v32_b1_path *b1_completed,
	struct v32_data_path *data_started,
	struct v32_data_path *data_advanced)
{
	struct generated_source source;
	struct v32_conditioning_path conditioning;
	struct v32_conditioning_path_handoff handoff;
	struct v32_rate_path path;
	struct v32_rate_path_handoff rate_handoff;
	struct v32_end_path end;
	struct v32_end_path_handoff end_handoff;
	struct v32_b1_path b1;
	struct v32_data_path data;
	struct v32_data_batch batch;
	enum v32_equalizer_adaptation end_adaptation;
	uint64_t data_start_epoch;
	size_t data_groups = 0;
	unsigned int end_source_limit;
	unsigned int b1_source_start = 0;
	bool end_sequence_started = false;
	bool multiple_data_groups = false;
	bool complete = false;

	CHECK(complete_conditioning(
		      &source, role, selected_offer, false, &conditioning, &handoff),
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
	if (role == V32_CALLING_TRANSMITTER &&
	    selected_offer->rates == V32_RATE_CAPABILITY_4800)
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
	      source.rate_symbols == b1_source_start &&
	      source.selected_symbols != 0 && source.rate_word == UINT16_MAX,
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
	*completed_end_handoff = end_handoff;
	{
		enum v32_b1_path_result result = v32_b1_path_init(
			&b1, &end_handoff);

		CHECK(result == V32_B1_PATH_OK ||
		      result == V32_B1_PATH_COMPLETE,
		      "B1 path did not initialize for role/mode %d/%u:%u: %d",
		      role, selected_offer->rates,
		      selected_offer->trellis_at_highest_rate, result);
		complete = result == V32_B1_PATH_COMPLETE;
	}
	CHECK(v32_b1_path_is_valid(&b1),
	      "B1 path initialized invalid for role/mode %d/%u:%u", role,
	      selected_offer->rates,
	      selected_offer->trellis_at_highest_rate);
	CHECK(b1.receive.line.gain.gain_mode == AGC_GAIN_FROZEN &&
	      b1.receive.line.gain.gain_mantissa_q15 ==
		      end_handoff.receive.line.gain.gain_mantissa_q15 &&
	      b1.receive.line.gain.gain_shift ==
		      end_handoff.receive.line.gain.gain_shift &&
	      memcmp(&end_handoff, completed_end_handoff, sizeof(end_handoff)) == 0,
	      "B1 handoff did not retain trained gain or changed its source");
	*b1_started = b1;
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && !complete; ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;
		enum v32_b1_path_result result;

		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "B1 source stopped at %u for role/mode %d/%u:%u", index,
		      role, selected_offer->rates,
		      selected_offer->trellis_at_highest_rate);
		result = v32_b1_path_sample(
			&b1, sample, accepted ? &reference : NULL);
		CHECK(result == V32_B1_PATH_OK ||
		      result == V32_B1_PATH_COMPLETE,
		      "B1 path stopped at %u for role/mode %d/%u:%u: %d (%u/%u, %d/%d e%llu)",
		      index, role, selected_offer->rates,
		      selected_offer->trellis_at_highest_rate, result,
		      b1.observations, source.selected_symbols,
		      b1.receive.equalizer.pending_observation.i,
		      b1.receive.equalizer.pending_observation.q,
		      (unsigned long long)
			      b1.receive.equalizer.pending_observation.epoch);
		complete = result == V32_B1_PATH_COMPLETE;
	}
	CHECK(complete && v32_b1_path_is_complete(&b1) &&
	      b1.receive.line.gain.gain_mode == AGC_GAIN_FROZEN &&
	      b1.receive.line.gain.gain_mantissa_q15 ==
		      b1_started->receive.line.gain.gain_mantissa_q15 &&
	      b1.receive.line.gain.gain_shift ==
		      b1_started->receive.line.gain.gain_shift &&
	      b1.observations == V32_B1_PATH_SYMBOLS &&
	      b1.observation_epoch ==
		      end_handoff.observation_epoch + V32_B1_PATH_SYMBOLS &&
	      b1.receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      (!b1.receive.equalizer.decision_pending ||
	       b1.receive.equalizer.pending_observation.epoch ==
		       b1.observation_epoch + 1u) &&
	      source.selected_symbols > V32_B1_PATH_SYMBOLS &&
	      source.b1_history != 0,
	      "B1 boundary changed for role/mode %d/%u:%u", role,
	      selected_offer->rates,
	      selected_offer->trellis_at_highest_rate);
	if (b1.mode == V32_B1_MODE_UNCODED)
		CHECK(b1.receiver.uncoded.decoder.descrambler.history ==
			      source.b1_history && b1.b1_matches,
		      "uncoded B1 descrambler changed for role %d", role);
	if (b1.mode == V32_B1_MODE_TRELLIS)
		CHECK(b1.receiver.trellis.b1_observations ==
			      V32_B1_PATH_SYMBOLS &&
		      b1.receiver.trellis.b1_flush_symbols == 0 &&
		      b1.receiver.trellis.b1_matches == b1.b1_matches,
		      "trellis B1 state changed for role/mode %d/%u", role,
		      selected_offer->rates);
	*b1_completed = b1;
	data_start_epoch = b1.observation_epoch;
	CHECK(v32_data_path_init(&data, &b1, &batch) ==
		      V32_DATA_PATH_OK &&
	      v32_data_path_is_valid(&data) &&
	      consume_data_batch(
		      &batch, data.b1.data_bits_per_symbol,
		      &data_groups, &multiple_data_groups),
	      "data path did not retain the B1 boundary for role/mode %d/%u:%u",
	      role, selected_offer->rates,
	      selected_offer->trellis_at_highest_rate);
	CHECK(data.b1.receive.line.gain.gain_mode == AGC_GAIN_FROZEN &&
	      data.b1.receive.line.gain.gain_mantissa_q15 ==
		      b1.receive.line.gain.gain_mantissa_q15 &&
	      data.b1.receive.line.gain.gain_shift ==
		      b1.receive.line.gain.gain_shift &&
	      memcmp(&b1, b1_completed, sizeof(b1)) == 0,
	      "data handoff did not retain trained gain or changed its B1 source");
	*data_started = data;
	for (unsigned int index = 0;
	     index < TEST_LINE_SAMPLE_LIMIT && data_groups < TEST_DATA_GROUPS;
	     ++index) {
		struct v32_echo_reference reference;
		int16_t sample;
		bool accepted;

		CHECK(source_sample(&source, &sample, &reference, &accepted),
		      "data source stopped at %u for role/mode %d/%u:%u", index,
		      role, selected_offer->rates,
		      selected_offer->trellis_at_highest_rate);
		CHECK(v32_data_path_sample(
			      &data, sample, accepted ? &reference : NULL,
			      &batch) == V32_DATA_PATH_OK &&
		      consume_data_batch(
			      &batch, data.b1.data_bits_per_symbol,
			      &data_groups, &multiple_data_groups),
		      "data path stopped at %u for role/mode %d/%u:%u", index,
		      role, selected_offer->rates,
		      selected_offer->trellis_at_highest_rate);
	}
	CHECK(data_groups >= TEST_DATA_GROUPS && multiple_data_groups &&
	      v32_data_path_is_valid(&data) &&
	      data.b1.receive.line.gain.gain_mode == AGC_GAIN_FROZEN &&
	      data.b1.receive.line.gain.gain_mantissa_q15 ==
		      data_started->b1.receive.line.gain.gain_mantissa_q15 &&
	      data.b1.receive.line.gain.gain_shift ==
		      data_started->b1.receive.line.gain.gain_shift &&
	      data.b1.observations == V32_B1_PATH_SYMBOLS &&
	      data.b1.receive.equalizer.adaptation ==
		      V32_EQUALIZER_ADAPTATION_FINE &&
	      data.b1.observation_epoch - data_start_epoch ==
		      data_groups +
			      (data.b1.mode == V32_B1_MODE_TRELLIS ?
				       V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS : 0),
	      "data boundary changed for role/mode %d/%u:%u (%zu groups)",
	      role, selected_offer->rates,
	      selected_offer->trellis_at_highest_rate, data_groups);
	if (data.b1.mode == V32_B1_MODE_TRELLIS)
		CHECK(data.b1.receiver.trellis.b1_flush_symbols ==
			      V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS &&
		      v32_trellis_receiver_b1_is_confirmed(
			      &data.b1.receiver.trellis) ==
			      data.b1.b1_matches,
		      "trellis data did not drain B1 for role/mode %d/%u",
		      role, selected_offer->rates);
	*data_advanced = data;
	CHECK(detect_remote_retrain(
		      &source, &data, role, selected_offer),
	      "remote retrain detection failed for role/mode %d/%u:%u", role,
	      selected_offer->rates,
	      selected_offer->trellis_at_highest_rate);
	return true;
}

static bool test_contracts(
	const struct v32_conditioning_path_handoff *conditioning,
	const struct v32_rate_path *started,
	const struct v32_rate_path *completed,
	const struct v32_end_path *end_started,
	const struct v32_end_path *end_completed,
	const struct v32_end_path_handoff *completed_end_handoff,
	const struct v32_b1_path *b1_started,
	const struct v32_b1_path *b1_completed,
	const struct v32_data_path *data_started,
	const struct v32_data_path *data_advanced)
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
	struct v32_end_path_handoff bad_end_handoff;
	struct v32_b1_path b1;
	struct v32_b1_path saved_b1;
	struct v32_data_path data;
	struct v32_data_path saved_data;
	struct v32_data_batch batch;
	struct v32_data_batch saved_batch;
	union {
		struct v32_rate_path_handoff rate;
		struct v32_end_path end;
	} overlapping_init, saved_overlapping_init;
	union {
		struct v32_end_path_handoff end;
		struct v32_b1_path b1;
	} overlapping_b1, saved_overlapping_b1;
	union {
		struct v32_b1_path b1;
		struct v32_data_path data;
	} overlapping_data, saved_overlapping_data;

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
	handoff.observation.i ^= 1;
	CHECK(!v32_rate_path_handoff_is_valid(&handoff),
	      "rate handoff accepted an altered observation");
	CHECK(v32_rate_path_handoff(&path, &handoff) == V32_RATE_PATH_OK,
	      "complete rate handoff did not regenerate");
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
	end_handoff.observation.i ^= 1;
	CHECK(!v32_end_path_handoff_is_valid(&end_handoff),
	      "end handoff accepted an altered observation");
	CHECK(v32_end_path_handoff(&end, &end_handoff) == V32_END_PATH_OK,
	      "complete end handoff did not regenerate");
	end_handoff.selected_offer.rates ^= V32_RATE_CAPABILITY_4800;
	CHECK(!v32_end_path_handoff_is_valid(&end_handoff),
	      "end handoff accepted an invalid selected rate");
	end = *end_completed;
	end.detector.window = 1;
	CHECK(!v32_end_path_is_valid(&end),
	      "end path accepted a nonreset completed detector");

	memset(&b1, 0x5a, sizeof(b1));
	saved_b1 = b1;
	bad_end_handoff = *completed_end_handoff;
	bad_end_handoff.observation_ready =
		!bad_end_handoff.observation_ready;
	overlapping_b1.end = *completed_end_handoff;
	saved_overlapping_b1 = overlapping_b1;
	CHECK(v32_b1_path_init(NULL, completed_end_handoff) ==
		      V32_B1_PATH_INVALID_ARGUMENT &&
	      v32_b1_path_init(&b1, NULL) ==
		      V32_B1_PATH_INVALID_ARGUMENT &&
	      v32_b1_path_init(
		      &overlapping_b1.b1, &overlapping_b1.end) ==
		      V32_B1_PATH_INVALID_ARGUMENT &&
	      v32_b1_path_init(&b1, &bad_end_handoff) ==
		      V32_B1_PATH_INVALID_ARGUMENT &&
	      memcmp(&b1, &saved_b1, sizeof(b1)) == 0 &&
	      memcmp(
		      &overlapping_b1, &saved_overlapping_b1,
		      sizeof(overlapping_b1)) == 0,
	      "B1 initialization rejection changed state or input");

	b1 = *b1_started;
	saved_b1 = b1;
	CHECK(v32_b1_path_sample(NULL, 0, NULL) ==
		      V32_B1_PATH_INVALID_STATE &&
	      v32_b1_path_sample(
		      &b1, 0,
		      (const struct v32_echo_reference *)&b1) ==
		      V32_B1_PATH_INVALID_ARGUMENT &&
	      v32_b1_path_sample(&b1, 0, &invalid_reference) ==
		      V32_B1_PATH_INVALID_ARGUMENT &&
	      memcmp(&b1, &saved_b1, sizeof(b1)) == 0,
	      "B1 sample rejection changed state");
	b1.observation_epoch++;
	CHECK(!v32_b1_path_is_valid(&b1) &&
	      v32_b1_path_sample(&b1, 0, NULL) ==
		      V32_B1_PATH_INVALID_STATE,
	      "B1 path accepted a spliced decision epoch");
	b1 = *b1_started;
	b1.mode = V32_B1_MODE_TRELLIS;
	CHECK(!v32_b1_path_is_valid(&b1),
	      "B1 path accepted a mismatched receiver mode");

	b1 = *b1_completed;
	saved_b1 = b1;
	CHECK(v32_b1_path_sample(&b1, 0, NULL) ==
		      V32_B1_PATH_COMPLETE &&
	      memcmp(&b1, &saved_b1, sizeof(b1)) == 0,
	      "complete B1 path changed on another sample");

	memset(&data, 0x5a, sizeof(data));
	memset(&batch, 0xa5, sizeof(batch));
	saved_data = data;
	saved_batch = batch;
	overlapping_data.b1 = *b1_completed;
	saved_overlapping_data = overlapping_data;
	CHECK(v32_data_path_init(NULL, b1_completed, &batch) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_init(&data, NULL, &batch) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_init(&data, b1_completed, NULL) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_init(
		      &overlapping_data.data, &overlapping_data.b1,
		      &batch) == V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_init(
		      &data, b1_completed,
		      (struct v32_data_batch *)&data) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_init(&data, b1_started, &batch) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      memcmp(&data, &saved_data, sizeof(data)) == 0 &&
	      memcmp(&batch, &saved_batch, sizeof(batch)) == 0 &&
	      memcmp(
		      &overlapping_data, &saved_overlapping_data,
		      sizeof(overlapping_data)) == 0,
	      "data initialization rejection changed state or output");

	data = *data_started;
	saved_data = data;
	memset(&batch, 0xa5, sizeof(batch));
	saved_batch = batch;
	CHECK(v32_data_path_sample(NULL, 0, NULL, &batch) ==
		      V32_DATA_PATH_INVALID_STATE &&
	      v32_data_path_sample(&data, 0, NULL, NULL) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_sample(
		      &data, 0,
		      (const struct v32_echo_reference *)&data,
		      &batch) == V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_sample(
		      &data, 0, NULL,
		      (struct v32_data_batch *)&data) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      v32_data_path_sample(
		      &data, 0, &invalid_reference, &batch) ==
		      V32_DATA_PATH_INVALID_ARGUMENT &&
	      memcmp(&data, &saved_data, sizeof(data)) == 0 &&
	      memcmp(&batch, &saved_batch, sizeof(batch)) == 0,
	      "data sample rejection changed state or output");
	data.b1.observation_epoch++;
	CHECK(!v32_data_path_is_valid(&data) &&
	      v32_data_path_sample(&data, 0, NULL, &batch) ==
		      V32_DATA_PATH_INVALID_STATE,
	      "data path accepted a spliced decision epoch");
	data = *data_advanced;
	data.b1.mode = V32_B1_MODE_TRELLIS;
	CHECK(!v32_data_path_is_valid(&data),
	      "data path accepted a mismatched receiver mode");
	data = *data_advanced;
	data.retrain.repeated_pair_observations =
		V32_DATA_RETRAIN_OBSERVATIONS;
	CHECK(!v32_data_path_is_valid(&data),
	      "data path accepted an escaped retrain counter");
	return true;
}

static bool test_uncoded_phase_handoff(void)
{
	struct v32_end_path_handoff end = { 0 };

	CHECK(v32_receive_path_init(
		      &end.receive, 0, V32_ECHO_MIN_ROUND_TRIP_SYMBOLS) ==
		      V32_RECEIVE_PATH_OK &&
	      v32_receive_path_set_adaptation(
		      &end.receive, V32_EQUALIZER_ADAPTATION_DISABLED) ==
		      V32_RECEIVE_PATH_OK,
	      "phase handoff receive owner did not initialize");
	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t phase = 0; phase < 4; ++phase) {
			CHECK(v32_symbol_decoder_init(
				      &end.four_point.decoder,
				      (enum v32_transmitter_role)role, phase) ==
				      V32_FRAMING_OK,
			      "phase handoff framing did not initialize");
			end.four_point.decoder.descrambler.history = 0x123456;
			for (unsigned int mode = 0; mode < 2; ++mode) {
				struct v32_b1_path b1;
				uint8_t bits = mode == 0 ? 2 : 4;

				end.selected_offer.rates = mode == 0 ?
					V32_RATE_CAPABILITY_4800 :
					V32_RATE_CAPABILITY_9600;
				CHECK(v32_end_path_handoff_is_valid(&end) &&
				      v32_b1_path_init(&b1, &end) == V32_B1_PATH_OK &&
				      end.four_point.decoder.phase_quadrant == phase &&
				      b1.observations == 0 &&
				      b1.data_bits_per_symbol == bits &&
				      b1.receiver.uncoded.decoder.phase_quadrant ==
					      (mode == 0 ? phase : 0) &&
				      b1.receiver.uncoded.decoder.descrambler.history ==
					      0x123456 &&
				      b1.receiver.uncoded.decoder.descrambler
					      .transmitter_role ==
					      (enum v32_transmitter_role)role,
				      "phase handoff changed at role/phase/bits %u/%u/%u",
				      role, phase, bits);
			}
		}
	}
	return true;
}

int main(void)
{
	struct v32_conditioning_path_handoff conditioning;
	struct v32_rate_path started;
	struct v32_rate_path completed;
	struct v32_end_path end_started;
	struct v32_end_path end_completed;
	struct v32_end_path_handoff completed_end_handoff;
	struct v32_b1_path b1_started;
	struct v32_b1_path b1_completed;
	struct v32_data_path data_started;
	struct v32_data_path data_advanced;

	for (size_t mode = 0; mode < TEST_SELECTED_MODES; ++mode) {
		for (unsigned int role = 0; role < 2; ++role) {
			struct v32_conditioning_path_handoff next_conditioning;
			struct v32_rate_path next_started;
			struct v32_rate_path next_completed;
			struct v32_end_path next_end_started;
			struct v32_end_path next_end_completed;
			struct v32_end_path_handoff next_end_handoff;
			struct v32_b1_path next_b1_started;
			struct v32_b1_path next_b1_completed;
			struct v32_data_path next_data_started;
			struct v32_data_path next_data_advanced;

			if (!run_role(
				    (enum v32_transmitter_role)role,
				    &SELECTED_OFFERS[mode], &next_conditioning,
				    &next_started, &next_completed,
				    &next_end_started, &next_end_completed,
				    &next_end_handoff, &next_b1_started,
				    &next_b1_completed, &next_data_started,
				    &next_data_advanced))
				return 1;
			if (mode == 0 &&
			    role == V32_ANSWERING_TRANSMITTER) {
				conditioning = next_conditioning;
				started = next_started;
				completed = next_completed;
				end_started = next_end_started;
				end_completed = next_end_completed;
				completed_end_handoff = next_end_handoff;
				b1_started = next_b1_started;
				b1_completed = next_b1_completed;
				data_started = next_data_started;
				data_advanced = next_data_advanced;
			}
		}
	}
	if (!test_cleardown(V32_CALLING_TRANSMITTER) ||
	    !test_cleardown(V32_ANSWERING_TRANSMITTER) ||
	    !test_uncoded_phase_handoff() || !test_contracts(
		    &conditioning, &started, &completed,
		    &end_started, &end_completed, &completed_end_handoff,
		    &b1_started, &b1_completed, &data_started,
		    &data_advanced))
		return 1;
	puts("V.32 rate path tests passed");
	return 0;
}
