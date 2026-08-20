// SPDX-License-Identifier: BSD-3-Clause

#include <conditioning.h>
#include <conditioning_receiver.h>
#include <constellation.h>
#include <decision.h>
#include <rate_signal.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_ACQUISITION_START =
		V32_CONDITIONING_S_SYMBOLS + 4,
	TEST_SEQUENCE_SYMBOLS = V32_CONDITIONING_S_SYMBOLS +
				V32_CONDITIONING_S_BAR_SYMBOLS +
				V32_CONDITIONING_TRN_MIN_SYMBOLS,
	TEST_MAX_SEQUENCE_SYMBOLS = V32_CONDITIONING_S_SYMBOLS +
				    V32_CONDITIONING_S_BAR_SYMBOLS +
				    V32_CONDITIONING_TRN_MAX_SYMBOLS,
	TEST_STABLE_TAIL_SYMBOLS = 2 * V32_RATE_SIGNAL_DIBITS,
	TEST_MIN_CONFIRMED_SYMBOLS =
		V32_CONDITIONING_TRN_MIN_SYMBOLS -
		V32_CONDITIONING_TRN_PREFIX_SYMBOLS,
	TEST_RATE_WORD = 0x0711,
	TEST_RATE_DIBITS = 64,
};

static uint64_t observation_epoch;

static bool generated_observation(
	uint8_t quadrant, struct v32_equalizer_observation *received)
{
	struct v32_fixed_point point;

	CHECK(v32_4800_fixed_point(
		      quadrant, &point) ==
		      V32_CONSTELLATION_OK,
	      "could not generate V.32 conditioning point %u", quadrant);
	*received = (struct v32_equalizer_observation){
		.i = point.i,
		.q = point.q,
		.phase = 0,
		.magnitude = 1,
		.epoch = ++observation_epoch,
		.lineage = UINT64_C(0x7633327465737401),
	};
	return true;
}

static bool decisions_are_equal(
	const struct v32_equalizer_decision *left,
	const struct v32_equalizer_decision *right)
{
	return memcmp(
		       &left->observation, &right->observation,
		       sizeof(left->observation)) == 0 &&
	       left->ideal_phase == right->ideal_phase &&
	       left->ideal_magnitude == right->ideal_magnitude &&
	       left->adapt_on_error == right->adapt_on_error;
}

static enum v32_conditioning_receiver_result observe(
	struct v32_conditioning_receiver *receiver,
	const struct v32_equalizer_observation *observation,
	bool *ready)
{
	struct v32_conditioning_symbol_decision decision;
	struct v32_quadrant_decision expected;
	enum v32_conditioning_receiver_result result;
	enum v32_conditioning_receiver_phase previous_phase;
	bool reset;

	if (ready == NULL)
		return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
	previous_phase = receiver != NULL ? receiver->phase :
		V32_CONDITIONING_SEARCH_S_BAR;
	result = v32_conditioning_receiver_observe(
		receiver, observation, &decision);
	if (result == V32_CONDITIONING_RECEIVER_OK) {
		reset = previous_phase == V32_CONDITIONING_TRAIN_TRN &&
			receiver->phase == V32_CONDITIONING_SEARCH_S_BAR;
		if (memcmp(
			    &decision.equalizer.observation, observation,
			    sizeof(*observation)) != 0 ||
		    decision.reference_known !=
			    decision.equalizer.adapt_on_error ||
		    (!reset && decision.reference_known &&
		     (decision.reference_quadrant >= QPSK_QUADRANT_COUNT ||
		      decision.reference_confirmed !=
			      (receiver->stable_trn_symbols != 0) ||
		      v32_decide_known_quadrant(
			      observation, decision.reference_quadrant,
			      true, &expected) != V32_DECISION_OK ||
		      !decisions_are_equal(
			      &decision.equalizer, &expected.equalizer))) ||
		    (!decision.reference_known &&
		     (decision.reference_confirmed ||
		      v32_decide_4_point(
			     observation, false, &expected) != V32_DECISION_OK ||
		      !decisions_are_equal(
			      &decision.equalizer, &expected.equalizer))))
			return V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT;
		*ready = v32_conditioning_receiver_is_complete(receiver);
	}
	return result;
}

static bool decode_relative_quadrant(
	struct v32_symbol_decoder *decoder, uint8_t *previous,
	uint8_t observed, uint8_t *dibit)
{
	uint8_t turn = (uint8_t)((observed - *previous) & 3u);
	uint8_t quadrant =
		(uint8_t)((decoder->phase_quadrant + turn) & 3u);

	*previous = observed;
	return v32_4800_symbol_decode(decoder, quadrant, dibit) ==
	       V32_FRAMING_OK;
}

struct generated_conditioning_result {
	struct v32_conditioning_receiver receiver;
	bool ready;
	bool epoch_seen;
	bool reset;
};

static bool run_generated_conditioning(
	enum v32_transmitter_role receiver_role,
	enum v32_transmitter_role transmitter_role,
	uint16_t acquisition_start, uint16_t observation_end,
	uint16_t phase_step,
	uint8_t phase_offset, struct generated_conditioning_result *result)
{
	struct v32_conditioning_transmitter transmitter;
	struct v32_conditioning_receiver receiver;
	bool ready = false;
	bool epoch_seen = false;
	bool reset = false;

	CHECK(result != NULL && acquisition_start < observation_end &&
		      observation_end <= TEST_MAX_SEQUENCE_SYMBOLS &&
		      phase_offset < QPSK_QUADRANT_COUNT &&
		      v32_conditioning_receiver_init(
			      &receiver, receiver_role) ==
			      V32_CONDITIONING_RECEIVER_OK &&
		      v32_conditioning_transmitter_init(
			      &transmitter, transmitter_role,
			      observation_end > TEST_SEQUENCE_SYMBOLS ?
				      V32_CONDITIONING_TRN_MAX_SYMBOLS :
				      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      V32_CONDITIONING_OK,
	      "V.32 generated conditioning fixture did not initialize");
	for (uint16_t symbol = 0; symbol < observation_end; ++symbol) {
		struct v32_equalizer_observation received;
		enum v32_conditioning_receiver_phase previous_phase;
		uint8_t quadrant;

		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK,
		      "V.32 generated conditioning stopped at %u", symbol);
		if (symbol < acquisition_start)
			continue;
		if (phase_offset != 0 &&
		    symbol >= V32_CONDITIONING_S_SYMBOLS +
				      V32_CONDITIONING_S_BAR_SYMBOLS + phase_step)
			quadrant = (uint8_t)((quadrant + phase_offset) & 3u);
		previous_phase = receiver.phase;
		CHECK(generated_observation(quadrant, &received) &&
		      observe(
			      &receiver, &received, &ready) ==
			      V32_CONDITIONING_RECEIVER_OK,
		      "V.32 generated conditioning receiver stopped at %u",
		      symbol);
		epoch_seen = epoch_seen ||
			     receiver.phase == V32_CONDITIONING_COUNT_S_BAR;
		reset = reset ||
			(previous_phase == V32_CONDITIONING_TRAIN_TRN &&
			 receiver.phase == V32_CONDITIONING_SEARCH_S_BAR);
	}
	*result = (struct generated_conditioning_result){
		.receiver = receiver,
		.ready = ready,
		.epoch_seen = epoch_seen,
		.reset = reset,
	};
	return true;
}

static bool test_s_bar_epoch_boundary(void)
{
	static const struct {
		uint16_t observation_end;
		enum v32_conditioning_receiver_phase phase;
		uint8_t s_bar_symbols;
		uint16_t trn_symbols;
	} boundaries[] = {
		{ V32_CONDITIONING_S_SYMBOLS,
		  V32_CONDITIONING_MATCH_TRN, 0, 0 },
		{ V32_CONDITIONING_S_SYMBOLS + 1u,
		  V32_CONDITIONING_COUNT_S_BAR, 1, 0 },
		{ V32_CONDITIONING_S_SYMBOLS +
			  V32_CONDITIONING_S_BAR_SYMBOLS - 1u,
		  V32_CONDITIONING_COUNT_S_BAR,
		  V32_CONDITIONING_S_BAR_SYMBOLS - 1u, 0 },
		{ V32_CONDITIONING_S_SYMBOLS +
			  V32_CONDITIONING_S_BAR_SYMBOLS,
		  V32_CONDITIONING_COUNT_S_BAR,
		  V32_CONDITIONING_S_BAR_SYMBOLS, 0 },
		{ V32_CONDITIONING_S_SYMBOLS +
			  V32_CONDITIONING_S_BAR_SYMBOLS + 1u,
		  V32_CONDITIONING_TRAIN_TRN, 0, 1 },
	};

	for (enum v32_transmitter_role role = V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		for (size_t index = 0; index < ARRAY_LENGTH(boundaries); ++index) {
			struct generated_conditioning_result result;

			CHECK(run_generated_conditioning(
				      role, role, 0,
				      boundaries[index].observation_end, 0, 0,
				      &result),
			      "V.32 S/S-bar boundary fixture failed for role %d",
			      role);
			CHECK(result.receiver.phase == boundaries[index].phase &&
				      result.receiver.s_bar_symbols ==
					      boundaries[index].s_bar_symbols &&
				      result.receiver.trn_symbols ==
					      boundaries[index].trn_symbols,
			      "V.32 S/S-bar boundary %zu changed for role %d",
			      index, role);
		}
	}
	return true;
}

static bool test_s_bar_epoch_survives_boundary_smear(void)
{
	for (enum v32_transmitter_role role = V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		struct v32_conditioning_transmitter transmitter;
		struct v32_conditioning_receiver receiver;
		bool ready = false;

		CHECK(v32_conditioning_receiver_init(&receiver, role) ==
			      V32_CONDITIONING_RECEIVER_OK &&
		      v32_conditioning_transmitter_init(
			      &transmitter, role,
			      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      V32_CONDITIONING_OK,
		      "V.32 smeared epoch fixture did not initialize");
		for (uint16_t symbol = 0; symbol <= V32_CONDITIONING_S_SYMBOLS;
		     ++symbol) {
			struct v32_equalizer_observation received;
			uint8_t quadrant;

			CHECK(v32_conditioning_transmitter_next(
				      &transmitter, &quadrant) ==
				      V32_CONDITIONING_OK,
			      "V.32 smeared epoch fixture stopped at %u", symbol);
			/* Model a pulse-shaped transition whose first hard decision
			 * lands in the even neighboring quadrant.  Its two-symbol I/Q
			 * displacement still carries the physical S-bar edge. */
			if (symbol == V32_CONDITIONING_S_SYMBOLS) {
				receiver.alternating_transitions = 0;
				quadrant = (uint8_t)((quadrant + 1u) & 3u);
			}
			CHECK(generated_observation(quadrant, &received) &&
			      observe(&receiver, &received, &ready) ==
				      V32_CONDITIONING_RECEIVER_OK,
			      "V.32 smeared epoch receiver stopped at %u", symbol);
		}
		CHECK(receiver.phase == V32_CONDITIONING_COUNT_S_BAR &&
			      receiver.s_bar_symbols == 1 && !ready,
		      "V.32 smeared S/S-bar boundary was not detected for role %d",
		      role);
	}
	return true;
}

static bool test_epoch_phase_slip_recovery(void)
{
	static const uint16_t phase_steps[] = {
		1,
		V32_CONDITIONING_TRN_PREFIX_SYMBOLS / 2,
		V32_CONDITIONING_TRN_PREFIX_SYMBOLS - 1u,
	};

	for (enum v32_transmitter_role role = V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		for (size_t index = 0; index < ARRAY_LENGTH(phase_steps); ++index) {
			for (uint8_t offset = 1; offset < QPSK_QUADRANT_COUNT;
			     ++offset) {
				struct generated_conditioning_result result;

				CHECK(run_generated_conditioning(
					      role, role, 0,
					      TEST_SEQUENCE_SYMBOLS,
					      phase_steps[index], offset, &result),
				      "V.32 epoch phase-slip fixture failed");
				CHECK(result.epoch_seen && !result.reset &&
					      result.ready &&
					      v32_conditioning_receiver_is_complete(
						      &result.receiver) &&
					      result.receiver.confirmed_trn_symbols >=
						      TEST_MIN_CONFIRMED_SYMBOLS &&
					      result.receiver.stable_trn_symbols >=
						      TEST_STABLE_TAIL_SYMBOLS,
				      "V.32 epoch did not recover role %d step %u offset %u",
				      role, phase_steps[index], offset);
			}
		}
	}
	return true;
}

static bool test_epoch_wrong_sequence_rejection(void)
{
	for (enum v32_transmitter_role role = V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		struct generated_conditioning_result result;
		enum v32_transmitter_role wrong_role =
			role == V32_CALLING_TRANSMITTER ?
				V32_ANSWERING_TRANSMITTER :
				V32_CALLING_TRANSMITTER;

		CHECK(run_generated_conditioning(
			      role, wrong_role, 0, TEST_MAX_SEQUENCE_SYMBOLS,
			      0, 0, &result),
		      "V.32 wrong-sequence fixture failed for role %d", role);
		CHECK(result.epoch_seen && result.reset && !result.ready &&
			      v32_conditioning_receiver_is_valid(&result.receiver) &&
			      result.receiver.phase ==
				      V32_CONDITIONING_SEARCH_S_BAR,
		      "V.32 epoch accepted the wrong role sequence for role %d",
		      role);
	}
	return true;
}

static bool test_direct_prefix_remains_exact(void)
{
	const uint16_t trn_start =
		V32_CONDITIONING_S_SYMBOLS +
		V32_CONDITIONING_S_BAR_SYMBOLS;

	for (enum v32_transmitter_role role = V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		struct generated_conditioning_result clean;
		struct generated_conditioning_result shifted;

		CHECK(run_generated_conditioning(
			      role, role, trn_start, TEST_SEQUENCE_SYMBOLS,
			      0, 0, &clean) &&
		      run_generated_conditioning(
			      role, role, trn_start, TEST_SEQUENCE_SYMBOLS,
			      V32_CONDITIONING_PREFIX_MATCH_TRANSITIONS - 3u,
			      1, &shifted),
		      "V.32 direct-prefix fixture failed for role %d", role);
		CHECK(!clean.epoch_seen && clean.ready &&
			      v32_conditioning_receiver_is_complete(&clean.receiver),
		      "V.32 direct prefix no longer acquired for role %d", role);
		CHECK(!shifted.epoch_seen && !shifted.ready &&
			      shifted.receiver.trn_symbols == 0 &&
			      !v32_conditioning_receiver_is_complete(
				      &shifted.receiver),
		      "V.32 direct prefix accepted a corrupted sequence for role %d",
		      role);
	}
	return true;
}

static bool run_sequence(
	struct v32_conditioning_receiver *receiver,
	enum v32_transmitter_role role,
	struct v32_conditioning_transmitter *finished_transmitter)
{
	struct v32_conditioning_transmitter transmitter;
	bool ready = false;

	CHECK(v32_conditioning_transmitter_init(
		      &transmitter, role,
		      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
		      V32_CONDITIONING_OK,
	      "V.32 conditioning fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_SEQUENCE_SYMBOLS; ++symbol) {
		struct v32_equalizer_observation received;
		uint8_t quadrant;

		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK,
		      "V.32 conditioning fixture stopped at symbol %u", symbol);
		if (symbol < TEST_ACQUISITION_START)
			continue;
		CHECK(generated_observation(quadrant, &received) &&
		      observe(
			      receiver, &received, &ready) ==
			      V32_CONDITIONING_RECEIVER_OK,
		      "V.32 conditioning receiver stopped at symbol %u", symbol);
		CHECK(ready == (symbol + 1u == TEST_SEQUENCE_SYMBOLS),
		      "V.32 conditioning readiness changed at symbol %u", symbol);
	}
	CHECK(ready &&
		      v32_conditioning_receiver_is_complete(receiver) &&
		      receiver->confirmed_trn_symbols ==
			      V32_CONDITIONING_TRN_MIN_SYMBOLS &&
		      receiver->stable_trn_symbols ==
			      V32_CONDITIONING_TRN_MIN_SYMBOLS,
	      "V.32 conditioning receiver did not complete");
	*finished_transmitter = transmitter;
	return true;
}

static bool check_role(enum v32_transmitter_role role)
{
	struct v32_conditioning_receiver receiver;
	struct v32_conditioning_transmitter transmitter;
	struct v32_symbol_decoder decoder;
	struct v32_equalizer_observation terminal;
	struct v32_conditioning_symbol_decision decision;
	struct v32_conditioning_symbol_decision saved_decision;
	struct v32_conditioning_receiver saved_receiver;
	struct v32_fixed_point point;

	CHECK(v32_conditioning_receiver_init(&receiver, role) ==
		      V32_CONDITIONING_RECEIVER_OK,
	      "V.32 conditioning receiver did not initialize for role %d", role);
	CHECK(run_sequence(&receiver, role, &transmitter),
	      "V.32 conditioning sequence failed for role %d", role);
	CHECK(v32_conditioning_receiver_framing_state(
		      &receiver, &decoder) ==
		      V32_CONDITIONING_RECEIVER_OK &&
		      decoder.descrambler.history ==
			      transmitter.trn_encoder.scrambler.history &&
		      decoder.descrambler.transmitter_role == role &&
		      decoder.phase_quadrant == 0,
	      "V.32 conditioning framing state changed for role %d", role);
	CHECK(v32_4800_fixed_point(receiver.previous_quadrant, &point) ==
		      V32_CONSTELLATION_OK,
	      "V.32 completed observation could not be generated");
	terminal = (struct v32_equalizer_observation){
		.i = point.i,
		.q = point.q,
		.phase = 0,
		.magnitude = 1,
		.epoch = receiver.observation_epoch + 1u,
		.lineage = receiver.observation_lineage,
	};
	memset(&decision, 0x5a, sizeof(decision));
	saved_decision = decision;
	saved_receiver = receiver;
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, &terminal, &decision) ==
		      V32_CONDITIONING_RECEIVER_COMPLETE &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "completed V.32 conditioning receiver changed state or output");
	return true;
}

static bool test_calling_source(void)
{
	return check_role(V32_CALLING_TRANSMITTER);
}

static bool test_answering_source(void)
{
	return check_role(V32_ANSWERING_TRANSMITTER);
}

static bool test_phase_slip_recovery(void)
{
	struct v32_conditioning_receiver receiver;
	struct v32_conditioning_transmitter transmitter;
	bool phase_shifted = false;
	bool ready = false;

	CHECK(v32_conditioning_receiver_init(
		      &receiver, V32_ANSWERING_TRANSMITTER) ==
		      V32_CONDITIONING_RECEIVER_OK &&
		      v32_conditioning_transmitter_init(
			      &transmitter,
			      V32_ANSWERING_TRANSMITTER,
			      V32_CONDITIONING_TRN_MIN_SYMBOLS) ==
			      V32_CONDITIONING_OK,
	      "V.32 phase-slip fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_SEQUENCE_SYMBOLS; ++symbol) {
		struct v32_equalizer_observation received;
		uint8_t quadrant;

		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK,
		      "V.32 phase-slip fixture stopped");
		if (symbol < TEST_ACQUISITION_START)
			continue;
		if (receiver.phase == V32_CONDITIONING_TRAIN_TRN)
			phase_shifted = true;
		if (phase_shifted)
			quadrant = (uint8_t)((quadrant + 1u) & 3u);
		CHECK(generated_observation(quadrant, &received) &&
		      observe(
			      &receiver, &received, &ready) ==
			      V32_CONDITIONING_RECEIVER_OK,
		      "V.32 conditioning rejected a persistent phase slip");
	}
	CHECK(phase_shifted && ready &&
		      v32_conditioning_receiver_is_complete(&receiver) &&
		      receiver.confirmed_trn_symbols ==
			      V32_CONDITIONING_TRN_MIN_SYMBOLS &&
		      receiver.stable_trn_symbols >=
			      TEST_STABLE_TAIL_SYMBOLS,
	      "V.32 conditioning did not recover the persistent phase slip");
	return true;
}

struct test_perturbation {
	uint16_t first_symbol;
	uint16_t interval;
	uint16_t stop_before;
	unsigned int count;
	bool persistent_phase;
};

static bool run_perturbed_sequence(
	enum v32_transmitter_role role,
	uint16_t acquisition_start,
	const struct test_perturbation *perturbation,
	struct v32_conditioning_receiver *finished,
	struct v32_conditioning_transmitter *stopped_transmitter,
	uint8_t *last_observed_quadrant, unsigned int *applied,
	uint16_t *boundary_trn, bool *ready, bool *reset)
{
	struct v32_conditioning_transmitter transmitter;
	struct v32_conditioning_receiver receiver;
	unsigned int injected = 0;
	uint8_t phase_offset = 0;
	uint8_t last_observed = 0;
	bool completed = false;
	bool restarted = false;
	uint16_t boundary = 0;

	CHECK(perturbation != NULL && finished != NULL && applied != NULL &&
		      boundary_trn != NULL && ready != NULL && reset != NULL &&
		      v32_conditioning_receiver_init(&receiver, role) ==
		      V32_CONDITIONING_RECEIVER_OK &&
		      v32_conditioning_transmitter_init(
			      &transmitter, role,
			      V32_CONDITIONING_TRN_MAX_SYMBOLS) ==
			      V32_CONDITIONING_OK,
	      "V.32 perturbed conditioning fixture did not initialize");
	for (uint16_t symbol = 0; symbol < TEST_MAX_SEQUENCE_SYMBOLS; ++symbol) {
		struct v32_equalizer_observation received;
		enum v32_conditioning_receiver_phase previous_phase;
		uint16_t previous_trn;
		uint8_t quadrant;

		CHECK(v32_conditioning_transmitter_next(
			      &transmitter, &quadrant) ==
			      V32_CONDITIONING_OK,
		      "V.32 perturbed conditioning fixture stopped at %u", symbol);
		if (symbol < acquisition_start)
			continue;
		if (receiver.phase == V32_CONDITIONING_TRAIN_TRN) {
			unsigned int next = perturbation->first_symbol +
				injected * perturbation->interval;

			if (injected < perturbation->count &&
			    receiver.trn_symbols >= next &&
			    receiver.trn_symbols < perturbation->stop_before) {
				if (perturbation->persistent_phase)
					phase_offset =
						(uint8_t)((phase_offset + 1u) & 3u);
				else
					quadrant = (uint8_t)((quadrant + 1u) & 3u);
				injected++;
			}
			if (perturbation->persistent_phase)
				quadrant = (uint8_t)((quadrant + phase_offset) & 3u);
		}
		last_observed = quadrant;
		previous_phase = receiver.phase;
		previous_trn = receiver.trn_symbols;
		CHECK(generated_observation(quadrant, &received) &&
		      observe(
			      &receiver, &received, &completed) ==
			      V32_CONDITIONING_RECEIVER_OK,
		      "V.32 perturbed conditioning receiver stopped at %u", symbol);
		if (previous_phase == V32_CONDITIONING_TRAIN_TRN &&
		    receiver.phase == V32_CONDITIONING_SEARCH_S_BAR) {
			restarted = true;
			boundary = (uint16_t)(previous_trn + 1u);
			break;
		}
		if (completed) {
			boundary = receiver.trn_symbols;
			break;
		}
	}
	*finished = receiver;
	if (stopped_transmitter != NULL)
		*stopped_transmitter = transmitter;
	if (last_observed_quadrant != NULL)
		*last_observed_quadrant = last_observed;
	*applied = injected;
	*boundary_trn = boundary;
	*ready = completed;
	*reset = restarted;
	return true;
}

static bool test_repeated_phase_slip_recovery(void)
{
	struct test_perturbation perturbation = {
		.first_symbol = 4 * TEST_STABLE_TAIL_SYMBOLS,
		.interval = 2 * TEST_STABLE_TAIL_SYMBOLS,
		.stop_before = TEST_MIN_CONFIRMED_SYMBOLS,
		.count = V32_CONDITIONING_TRN_MIN_SYMBOLS,
		.persistent_phase = true,
	};
	struct v32_conditioning_receiver receiver;
	unsigned int changes;
	uint16_t boundary_trn;
	bool ready;
	bool reset;

	CHECK(run_perturbed_sequence(
		      V32_ANSWERING_TRANSMITTER,
		      TEST_ACQUISITION_START, &perturbation,
		      &receiver, NULL, NULL,
		      &changes, &boundary_trn, &ready, &reset),
	      "V.32 repeated phase-slip sequence failed");
	CHECK(changes > TEST_STABLE_TAIL_SYMBOLS && !reset && ready &&
		      boundary_trn == V32_CONDITIONING_TRN_MIN_SYMBOLS &&
		      v32_conditioning_receiver_is_complete(&receiver) &&
		      receiver.confirmed_trn_symbols ==
			      V32_CONDITIONING_TRN_MIN_SYMBOLS &&
		      receiver.stable_trn_symbols >=
			      V32_CONDITIONING_TRN_PREFIX_SYMBOLS,
	      "V.32 receiver did not retain repeated confirmed phase changes");
	return true;
}

static bool check_perturbed_boundary(
	const struct test_perturbation *perturbation,
	uint16_t expected_trn, uint16_t expected_evidence,
	uint16_t expected_stable,
	bool expect_complete)
{
	struct v32_conditioning_receiver receiver;
	unsigned int applied;
	uint16_t boundary_trn;
	bool ready;
	bool reset;

	CHECK(run_perturbed_sequence(
		      V32_CALLING_TRANSMITTER,
		      TEST_ACQUISITION_START, perturbation,
		      &receiver, NULL, NULL,
		      &applied, &boundary_trn, &ready, &reset),
	      "V.32 conditioning boundary sequence failed");
	CHECK(applied == perturbation->count,
	      "V.32 conditioning fixture applied %u/%u perturbations",
	      applied, perturbation->count);
	if (expect_complete) {
		CHECK(!reset && ready && boundary_trn == expected_trn &&
			      v32_conditioning_receiver_is_complete(&receiver) &&
			      receiver.trn_symbols == expected_trn &&
			      receiver.confirmed_trn_symbols == expected_evidence &&
			      (expected_stable == 0 ||
			       receiver.stable_trn_symbols == expected_stable),
		      "V.32 receiver rejected sufficient conditioning evidence");
	} else {
		CHECK(reset && !ready && boundary_trn == expected_trn &&
			      v32_conditioning_receiver_is_valid(&receiver) &&
			      receiver.phase == V32_CONDITIONING_SEARCH_S_BAR,
		      "V.32 receiver accepted insufficient conditioning evidence");
	}
	return true;
}

static bool test_confirmed_evidence_boundary(void)
{
	const unsigned int allowed_errors =
		V32_CONDITIONING_TRN_MIN_SYMBOLS -
		TEST_MIN_CONFIRMED_SYMBOLS;
	struct test_perturbation perturbation = {
		.first_symbol = 4 * TEST_STABLE_TAIL_SYMBOLS,
		.interval = 3,
		.stop_before = V32_CONDITIONING_TRN_MIN_SYMBOLS,
		.count = allowed_errors,
	};

	if (!check_perturbed_boundary(
		    &perturbation, V32_CONDITIONING_TRN_MIN_SYMBOLS,
		    TEST_MIN_CONFIRMED_SYMBOLS, 0, true))
		return false;
	perturbation.count++;
	return check_perturbed_boundary(
		&perturbation, V32_CONDITIONING_TRN_MAX_SYMBOLS,
		TEST_MIN_CONFIRMED_SYMBOLS - 1u, 0, false);
}

static bool test_stable_tail_boundary(void)
{
	struct test_perturbation perturbation = {
		.first_symbol =
			V32_CONDITIONING_TRN_MIN_SYMBOLS -
			TEST_STABLE_TAIL_SYMBOLS - 1u,
		.interval = 1,
		.stop_before = V32_CONDITIONING_TRN_MIN_SYMBOLS,
		.count = 1,
	};

	if (!check_perturbed_boundary(
		    &perturbation, V32_CONDITIONING_TRN_MIN_SYMBOLS,
		    V32_CONDITIONING_TRN_MIN_SYMBOLS - 1u,
		    TEST_STABLE_TAIL_SYMBOLS, true))
		return false;
	perturbation.first_symbol++;
	return check_perturbed_boundary(
		&perturbation, V32_CONDITIONING_TRN_MIN_SYMBOLS + 1u,
		V32_CONDITIONING_TRN_MIN_SYMBOLS,
		TEST_STABLE_TAIL_SYMBOLS, true);
}

static bool test_extended_trn_framing_handoff(void)
{
	const struct test_perturbation perturbation = {
		.first_symbol =
			V32_CONDITIONING_TRN_MIN_SYMBOLS -
			TEST_STABLE_TAIL_SYMBOLS,
		.interval = 1,
		.stop_before = V32_CONDITIONING_TRN_MIN_SYMBOLS,
		.count = 1,
	};

	for (enum v32_transmitter_role role = V32_CALLING_TRANSMITTER;
	     role <= V32_ANSWERING_TRANSMITTER; ++role) {
		struct v32_conditioning_transmitter transmitter;
		struct v32_conditioning_receiver receiver;
		struct v32_symbol_encoder encoder;
		struct v32_symbol_decoder decoder;
		unsigned int applied;
		uint16_t boundary_trn;
		uint8_t previous_quadrant;
		bool ready;
		bool reset;

		CHECK(run_perturbed_sequence(
			      role, 0, &perturbation,
			      &receiver, &transmitter, &previous_quadrant,
			      &applied, &boundary_trn, &ready, &reset) &&
			      applied == 1 && ready && !reset && boundary_trn ==
				      V32_CONDITIONING_TRN_MIN_SYMBOLS + 1u &&
			      v32_conditioning_receiver_framing_state(
				      &receiver, &decoder) ==
				      V32_CONDITIONING_RECEIVER_OK,
		      "V.32 extended-TRN handoff boundary changed");
		while (!v32_conditioning_transmitter_is_complete(
			&transmitter)) {
			uint8_t discarded;
			uint8_t quadrant;

			CHECK(v32_conditioning_transmitter_next(
				      &transmitter, &quadrant) ==
				      V32_CONDITIONING_OK &&
			      decode_relative_quadrant(
				      &decoder, &previous_quadrant,
				      quadrant, &discarded),
			      "V.32 decoder stopped in extended TRN");
		}
		CHECK(v32_conditioning_transmitter_framing_state(
			      &transmitter, &encoder) ==
			      V32_CONDITIONING_OK,
		      "V.32 extended-TRN framing state was unavailable");
		for (unsigned int index = 0; index < TEST_RATE_DIBITS; ++index) {
			unsigned int word_index = index % V32_RATE_SIGNAL_DIBITS;
			uint8_t input = (uint8_t)(
				(TEST_RATE_WORD >> (14u - 2u * word_index)) & 3u);
			uint8_t decoded;
			uint8_t quadrant;

			CHECK(v32_4800_symbol_encode(
				      &encoder, input, &quadrant) ==
				      V32_FRAMING_OK &&
			      decode_relative_quadrant(
				      &decoder, &previous_quadrant,
				      quadrant, &decoded),
			      "V.32 extended-TRN rate framing stopped at %u", index);
			if (index >= TEST_RATE_DIBITS -
					     2u * V32_RATE_SIGNAL_DIBITS)
				CHECK(decoded == input,
				      "V.32 rate framing did not self-synchronize at %u",
				      index);
		}
	}
	return true;
}

static bool test_mapping_transition_rephase(void)
{
	const struct test_perturbation perturbation = {
		.first_symbol = V32_CONDITIONING_TRN_PREFIX_SYMBOLS - 1u,
		.interval = 1,
		.stop_before = V32_CONDITIONING_TRN_MAX_SYMBOLS,
		.count = 1,
		.persistent_phase = true,
	};
	struct v32_conditioning_receiver receiver;
	unsigned int applied;
	uint16_t boundary_trn;
	bool ready;
	bool reset;

	CHECK(run_perturbed_sequence(
		      V32_ANSWERING_TRANSMITTER,
		      TEST_ACQUISITION_START, &perturbation,
		      &receiver, NULL, NULL, &applied, &boundary_trn,
		      &ready, &reset) && ready && !reset && applied == 1 &&
		      v32_conditioning_receiver_is_complete(&receiver) &&
		      receiver.quadrant_offset != 0,
	      "V.32 receiver did not resolve the mapping-boundary phase step");
	return true;
}

static bool test_trn_prefix_acquisition(void)
{
	const struct test_perturbation perturbation = { 0 };
	struct v32_conditioning_receiver receiver;
	unsigned int applied;
	uint16_t boundary_trn;
	bool ready;
	bool reset;

	CHECK(run_perturbed_sequence(
		      V32_ANSWERING_TRANSMITTER,
		      V32_CONDITIONING_S_SYMBOLS +
			      V32_CONDITIONING_S_BAR_SYMBOLS,
		      &perturbation, &receiver, NULL, NULL, &applied,
		      &boundary_trn, &ready, &reset) && ready && !reset &&
		      v32_conditioning_receiver_is_complete(&receiver),
	      "V.32 receiver did not acquire directly from the TRN prefix");
	return true;
}

static bool test_validation(void)
{
	const struct v32_equalizer_observation valid_observation = {
		.i = 4096,
		.q = 12288,
		.phase = 0,
		.magnitude = 1,
		.epoch = 1,
		.lineage = 7,
	};
	const struct v32_equalizer_observation invalid_observation = {
		.i = 4096,
		.q = 12288,
		.epoch = 0,
		.lineage = 7,
	};
	struct v32_conditioning_receiver receiver;
	struct v32_conditioning_receiver evolving;
	struct v32_conditioning_receiver saved;
	struct v32_conditioning_symbol_decision decision;
	struct v32_conditioning_symbol_decision saved_decision;
	struct v32_equalizer_observation next_observation;
	struct v32_symbol_decoder decoder = { 0 };
	union {
		struct v32_equalizer_observation observation;
		struct v32_conditioning_symbol_decision decision;
	} input_output_overlap;
	unsigned char saved_input_output[sizeof(input_output_overlap)];

	CHECK(v32_conditioning_receiver_init(
		      &receiver, V32_CALLING_TRANSMITTER) ==
		      V32_CONDITIONING_RECEIVER_OK,
	      "V.32 conditioning validation receiver did not initialize");
	saved = receiver;
	CHECK(v32_conditioning_receiver_init(
		      NULL, V32_CALLING_TRANSMITTER) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      v32_conditioning_receiver_init(
			      &receiver,
			      (enum v32_transmitter_role)99) ==
			      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "V.32 conditioning receiver changed state on failed init");
	memset(&decision, 0x5a, sizeof(decision));
	saved_decision = decision;
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, NULL, &decision) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
	      v32_conditioning_receiver_observe(
		      &receiver, &invalid_observation, &decision) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
	      v32_conditioning_receiver_observe(
		      &receiver, &valid_observation, NULL) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "V.32 conditioning receiver validation was not transactional");
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, &valid_observation, &decision) ==
		      V32_CONDITIONING_RECEIVER_OK,
	      "V.32 conditioning receiver rejected its first observation");
	saved = receiver;
	memset(&decision, 0x5a, sizeof(decision));
	saved_decision = decision;
	next_observation = valid_observation;
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, &next_observation, &decision) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
	      "V.32 conditioning receiver accepted a repeated epoch");
	next_observation.epoch = 3;
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, &next_observation, &decision) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
	      "V.32 conditioning receiver accepted a skipped epoch");
	next_observation.epoch = 2;
	next_observation.lineage++;
	evolving = receiver;
	CHECK(v32_conditioning_receiver_observe(
		      &evolving, &next_observation, &decision) ==
		      V32_CONDITIONING_RECEIVER_OK &&
	      evolving.observation_epoch == next_observation.epoch &&
	      evolving.observation_lineage == next_observation.lineage &&
	      memcmp(&decision.equalizer.observation, &next_observation,
		     sizeof(next_observation)) == 0 &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "V.32 conditioning receiver rejected evolving lineage");
	next_observation.lineage = valid_observation.lineage;
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, &next_observation,
		      (struct v32_conditioning_symbol_decision *)&receiver) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "V.32 conditioning receiver accepted owner/output overlap");
	memset(&input_output_overlap, 0, sizeof(input_output_overlap));
	input_output_overlap.observation = next_observation;
	memcpy(saved_input_output, &input_output_overlap,
	       sizeof(saved_input_output));
	CHECK(v32_conditioning_receiver_observe(
		      &receiver, &input_output_overlap.observation,
		      &input_output_overlap.decision) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&input_output_overlap, saved_input_output,
		     sizeof(saved_input_output)) == 0,
	      "V.32 conditioning receiver accepted input/output overlap");
	memset(&decision, 0x5a, sizeof(decision));
	saved_decision = decision;
	CHECK(v32_conditioning_receiver_observe(
		      &receiver,
		      (const struct v32_equalizer_observation *)&receiver,
		      &decision) == V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "V.32 conditioning receiver accepted owner/input overlap");
	receiver = saved;
	CHECK(v32_conditioning_receiver_framing_state(
		      &receiver, &decoder) ==
		      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
	      "incomplete V.32 conditioning receiver exported state");
	receiver.prefix_fallback[0] = 1;
	CHECK(!v32_conditioning_receiver_is_valid(&receiver) &&
		      !v32_conditioning_receiver_is_complete(&receiver) &&
		      observe(
			      &receiver, &next_observation,
			      &(bool){ true }) ==
			      V32_CONDITIONING_RECEIVER_INVALID_ARGUMENT,
	      "V.32 conditioning receiver accepted corrupt state");
	receiver = saved;
	receiver.confirmed_trn_symbols = 1;
	CHECK(!v32_conditioning_receiver_is_valid(&receiver),
	      "V.32 conditioning receiver accepted idle training evidence");
	receiver = saved;
	receiver.stable_trn_symbols = 1;
	CHECK(!v32_conditioning_receiver_is_valid(&receiver),
	      "V.32 conditioning receiver accepted an idle stable tail");
	CHECK(strcmp(
		      v32_conditioning_receiver_result_name(
			      V32_CONDITIONING_RECEIVER_OK),
		      "ok") == 0 &&
		      strcmp(
			      v32_conditioning_receiver_result_name(
			      (enum v32_conditioning_receiver_result)99),
		      "unknown") == 0,
	      "V.32 conditioning receiver result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "calling source", test_calling_source },
		{ "answering source", test_answering_source },
		{ "mapping transition rephase",
		  test_mapping_transition_rephase },
		{ "phase-slip recovery", test_phase_slip_recovery },
		{ "repeated phase-slip recovery",
		  test_repeated_phase_slip_recovery },
		{ "confirmed-evidence boundary",
		  test_confirmed_evidence_boundary },
		{ "stable-tail boundary", test_stable_tail_boundary },
		{ "extended-TRN framing handoff",
		  test_extended_trn_framing_handoff },
		{ "TRN-prefix acquisition", test_trn_prefix_acquisition },
		{ "S/S-bar epoch boundary", test_s_bar_epoch_boundary },
		{ "smeared S/S-bar epoch",
		  test_s_bar_epoch_survives_boundary_smear },
		{ "epoch phase-slip recovery",
		  test_epoch_phase_slip_recovery },
		{ "epoch wrong-sequence rejection",
		  test_epoch_wrong_sequence_rejection },
		{ "direct prefix remains exact",
		  test_direct_prefix_remains_exact },
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
