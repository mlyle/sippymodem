// SPDX-License-Identifier: BSD-3-Clause

#include "startup.h"

#include <stddef.h>

/* The datapump owns line turnaround and physical conditioning. This owner
 * retains the rate and coding exchanged by R1/R2/R3/E. */

enum {
	V32_IMPLEMENTED_RATES = V32_RATE_CAPABILITY_4800 |
				V32_RATE_CAPABILITY_7200 |
				V32_RATE_CAPABILITY_9600 |
				V32_RATE_CAPABILITY_12000 |
				V32_RATE_CAPABILITY_14400,
};

static bool offer_is_valid(
	enum v32_rate_signal_type type,
	const struct v32_rate_offer *offer)
{
	uint16_t word;

	return offer != NULL &&
	       v32_rate_signal_encode(type, offer, &word) ==
		       V32_RATE_SIGNAL_OK;
}

static struct v32_rate_offer rate_offer(uint8_t rates)
{
	return (struct v32_rate_offer){
		.rates = rates,
		.trellis_at_highest_rate =
			v32_rate_highest(rates) > V32_RATE_CAPABILITY_4800,
	};
}

static struct v32_rate_offer offer_at_rate(
	const struct v32_rate_offer *available, uint8_t rate)
{
	struct v32_rate_offer selected = rate_offer(rate);

	if (rate == v32_rate_highest(available->rates))
		selected.trellis_at_highest_rate = available->trellis_at_highest_rate;
	return selected;
}

static bool offer_permits_selection(
	const struct v32_rate_offer *available, const struct v32_rate_offer *selected)
{
	struct v32_rate_offer coding = offer_at_rate(available, selected->rates);

	/* Trellis availability does not exclude mandatory uncoded 9600.
	 * Selecting trellis requires that it was offered at that rate. */
	return selected->rates != 0 &&
	       (selected->rates & available->rates) == selected->rates &&
	       (!selected->trellis_at_highest_rate || coding.trellis_at_highest_rate);
}

static bool decode_local_offer(
	uint16_t word, enum v32_rate_signal_type expected_type,
	uint8_t rates, struct v32_rate_offer *offer)
{
	enum v32_rate_signal_type type;
	uint16_t canonical;

	return v32_rate_signal_decode(word, &type, offer) == V32_RATE_SIGNAL_OK &&
	       type == expected_type && offer->rates == rates &&
	       v32_rate_signal_encode(type, offer, &canonical) == V32_RATE_SIGNAL_OK &&
	       canonical == word;
}

bool v32_startup_selected_offer(
	const struct v32_startup *startup, struct v32_rate_offer *offer)
{
	struct v32_rate_offer selected;

	if (startup == NULL || offer == NULL || startup->selected_rate == 0 ||
	    !decode_local_offer(startup->end_word, V32_END_SIGNAL,
			startup->selected_rate, &selected))
		return false;
	*offer = selected;
	return true;
}

static bool role_is_valid(enum v32_endpoint_role role)
{
	return role == V32_CALLER ||
	       role == V32_ANSWERER;
}

static bool phase_is_valid_for_role(
	enum v32_startup_phase phase,
	enum v32_endpoint_role role)
{
	if (phase == V32_CONFIRM || phase == V32_DATA)
		return true;
	if (role == V32_CALLER)
		return phase >= V32_CALLER_WAIT_OFFER &&
		       phase <= V32_CALLER_SEND_END;
	return phase >= V32_ANSWERER_TRAIN &&
	       phase <= V32_ANSWERER_SEND_END;
}

static bool words_are_valid(const struct v32_startup *startup)
{
	struct v32_rate_offer transmitted, selected;

	if (!decode_local_offer(startup->rate_word, V32_RATE_SIGNAL,
			startup->transmitted_rates, &transmitted))
		return false;
	if (startup->selected_rate == 0)
		return startup->end_word == 0;
	if (!v32_startup_selected_offer(startup, &selected))
		return false;
	return offer_permits_selection(&transmitted, &selected);
}

static bool rates_are_valid(const struct v32_startup *startup)
{
	if (startup->supported_rates == 0 ||
	    (startup->supported_rates & (uint8_t)~V32_IMPLEMENTED_RATES) != 0 ||
	    startup->transmitted_rates == 0 ||
	    (startup->transmitted_rates &
	     (uint8_t)~startup->supported_rates) != 0 ||
	    (startup->selected_rate & (uint8_t)~startup->supported_rates) != 0 ||
	    (startup->selected_rate != 0 &&
	     (startup->selected_rate &
	      (uint8_t)(startup->selected_rate - 1u)) != 0))
		return false;
	if (startup->role == V32_CALLER) {
		if (startup->selection_expected &&
		    (startup->phase != V32_CALLER_SEND_RESPONSE ||
		     startup->selected_rate != 0 || startup->end_pending))
			return false;
		if (startup->phase == V32_CALLER_WAIT_OFFER)
			return startup->transmitted_rates ==
				       startup->supported_rates &&
			       startup->selected_rate == 0 &&
			       !startup->selection_expected;
		if (startup->phase == V32_CALLER_SEND_RESPONSE)
			return (startup->selected_rate != 0) ==
				       startup->end_pending &&
			       (startup->selected_rate == 0 ||
				(startup->selected_rate &
				 startup->transmitted_rates) != 0);
		return startup->selected_rate != 0 &&
		       (startup->selected_rate & startup->transmitted_rates) != 0;
	}
	if (startup->phase == V32_ANSWERER_TRAIN ||
	    startup->phase == V32_ANSWERER_SEND_OFFER)
		return startup->transmitted_rates == startup->supported_rates &&
		       startup->selected_rate == 0 &&
		       !startup->selection_expected;
	return startup->selected_rate != 0 &&
	       startup->transmitted_rates == startup->selected_rate &&
	       !startup->selection_expected;
}

static bool configure_rates(
	struct v32_startup *startup, const struct v32_rate_offer *transmitted,
	const struct v32_rate_offer *selected)
{
	uint16_t rate_word;
	uint16_t end_word = 0;

	if (transmitted->rates == 0 ||
	    (transmitted->rates & (uint8_t)~startup->supported_rates) != 0 ||
	    v32_rate_signal_encode(
		    V32_RATE_SIGNAL, transmitted, &rate_word) !=
		    V32_RATE_SIGNAL_OK)
		return false;
	if (selected != NULL) {
		if (!offer_permits_selection(transmitted, selected) ||
		    v32_rate_signal_encode(V32_END_SIGNAL, selected, &end_word) !=
			    V32_RATE_SIGNAL_OK)
			return false;
	}
	startup->selected_rate = selected != NULL ? selected->rates : 0;
	startup->transmitted_rates = transmitted->rates;
	startup->rate_word = rate_word;
	startup->end_word = end_word;
	return true;
}

static uint16_t phase_dibit_limit(
	enum v32_startup_phase phase)
{
	if (phase == V32_ANSWERER_TRAIN ||
	    phase == V32_ANSWERER_RETRAIN)
		return V32_STARTUP_TRAINING_DIBITS;
	if (phase == V32_CONFIRM)
		return V32_STARTUP_CONFIRMATION_DIBITS;
	return 0;
}

bool v32_startup_is_valid(
	const struct v32_startup *startup)
{
	return startup != NULL && role_is_valid(startup->role) &&
	       phase_is_valid_for_role(startup->phase, startup->role) &&
	       v32_rate_signal_detector_is_valid(
		       &startup->detector) &&
	       rates_are_valid(startup) && words_are_valid(startup) &&
	       startup->phase_dibits <= phase_dibit_limit(startup->phase) &&
	       startup->receive_mark_dibits <=
		       V32_STARTUP_CONFIRMATION_DIBITS &&
	       startup->word_dibit < V32_RATE_SIGNAL_DIBITS &&
	       (!startup->end_pending ||
		(startup->phase == V32_CALLER_SEND_RESPONSE ||
		 startup->phase == V32_ANSWERER_SEND_SELECTION));
}

enum v32_startup_result v32_startup_init(
	struct v32_startup *startup,
	enum v32_endpoint_role role, uint8_t supported_rates)
{
	struct v32_rate_offer offer = rate_offer(supported_rates);
	struct v32_startup next = { 0 };

	if (startup == NULL || !role_is_valid(role) || supported_rates == 0 ||
	    (supported_rates & (uint8_t)~V32_IMPLEMENTED_RATES) != 0)
		return V32_STARTUP_INVALID_ARGUMENT;
	next.role = role;
	next.supported_rates = supported_rates;
	next.transmitted_rates = supported_rates;
	next.phase = role == V32_CALLER ?
			     V32_CALLER_WAIT_OFFER :
			     V32_ANSWERER_TRAIN;
	if (v32_rate_signal_detector_init(&next.detector) !=
		    V32_RATE_SIGNAL_OK ||
	    v32_rate_signal_encode(
		    V32_RATE_SIGNAL, &offer, &next.rate_word) !=
		    V32_RATE_SIGNAL_OK)
		return V32_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	return V32_STARTUP_OK;
}

bool v32_startup_is_complete(
	const struct v32_startup *startup)
{
	return v32_startup_is_valid(startup) &&
	       startup->phase == V32_DATA;
}

enum v32_startup_result v32_startup_expect_selection(
	struct v32_startup *startup)
{
	struct v32_startup next;

	if (!v32_startup_is_valid(startup) ||
	    startup->role != V32_CALLER ||
	    startup->phase != V32_CALLER_SEND_RESPONSE ||
	    startup->selected_rate != 0 || startup->end_pending ||
	    startup->selection_expected)
		return V32_STARTUP_INVALID_ARGUMENT;
	next = *startup;
	if (v32_rate_signal_detector_init(&next.detector) !=
	    V32_RATE_SIGNAL_OK)
		return V32_STARTUP_INVALID_ARGUMENT;
	next.selection_expected = true;
	*startup = next;
	return V32_STARTUP_OK;
}

static void enter_phase(
	struct v32_startup *startup,
	enum v32_startup_phase phase)
{
	startup->phase = phase;
	startup->phase_dibits = 0;
	startup->word_dibit = 0;
	startup->end_pending = false;
	startup->selection_expected = false;
}

static void add_bounded(uint16_t *value, uint16_t limit)
{
	if (*value < limit)
		(*value)++;
}

static void maybe_enter_data(struct v32_startup *startup)
{
	if (startup->phase == V32_CONFIRM &&
	    startup->end_received &&
	    startup->phase_dibits ==
		    V32_STARTUP_CONFIRMATION_DIBITS &&
	    startup->receive_mark_dibits ==
		    V32_STARTUP_CONFIRMATION_DIBITS)
		enter_phase(startup, V32_DATA);
}

static bool phase_transmits_rate(enum v32_startup_phase phase)
{
	return phase == V32_CALLER_SEND_RESPONSE ||
	       phase == V32_ANSWERER_SEND_OFFER ||
	       phase == V32_ANSWERER_SEND_SELECTION;
}

static bool phase_transmits_end(enum v32_startup_phase phase)
{
	return phase == V32_CALLER_SEND_END ||
	       phase == V32_ANSWERER_SEND_END;
}

static bool phase_transmits_bounded_training(
	enum v32_startup_phase phase)
{
	return phase == V32_ANSWERER_TRAIN ||
	       phase == V32_ANSWERER_RETRAIN;
}

static bool phase_uses_trn_framing(
	enum v32_startup_phase phase)
{
	return phase == V32_CALLER_WAIT_OFFER ||
	       phase_transmits_bounded_training(phase);
}

static void finish_training_phase(struct v32_startup *startup)
{
	enter_phase(startup,
		    startup->phase == V32_ANSWERER_TRAIN ?
			    V32_ANSWERER_SEND_OFFER :
			    V32_ANSWERER_SEND_SELECTION);
}

enum v32_startup_result v32_startup_finish_conditioning(
	struct v32_startup *startup)
{
	struct v32_startup next;

	if (!v32_startup_is_valid(startup) ||
	    !phase_transmits_bounded_training(startup->phase))
		return V32_STARTUP_INVALID_ARGUMENT;
	next = *startup;
	finish_training_phase(&next);
	if (!v32_startup_is_valid(&next))
		return V32_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	return V32_STARTUP_OK;
}

enum v32_startup_result v32_startup_next_transmit(
	struct v32_startup *startup,
	enum v32_startup_output *output, uint8_t *dibit)
{
	struct v32_startup next;
	enum v32_startup_output next_output;
	uint8_t next_dibit = 3;

	if (!v32_startup_is_valid(startup) || output == NULL ||
	    dibit == NULL)
		return V32_STARTUP_INVALID_ARGUMENT;
	if (startup->phase == V32_DATA) {
		*output = V32_STARTUP_DATA;
		return V32_STARTUP_OK;
	}
	if (startup->phase == V32_CONFIRM)
		next_output = V32_STARTUP_SELECTED_RATE_MARK;
	else
		next_output = phase_uses_trn_framing(startup->phase) ?
				      V32_STARTUP_TRN_DIBIT :
				      V32_STARTUP_DIFFERENTIAL_DIBIT;
	next = *startup;
	if (phase_transmits_rate(next.phase) || phase_transmits_end(next.phase)) {
		uint16_t word = phase_transmits_rate(next.phase) ?
					startup->rate_word : startup->end_word;

		if (v32_rate_signal_dibit(
			    word, next.word_dibit, &next_dibit) !=
		    V32_RATE_SIGNAL_OK)
			return V32_STARTUP_INVALID_ARGUMENT;
		next.word_dibit++;
		if (next.word_dibit == V32_RATE_SIGNAL_DIBITS) {
			next.word_dibit = 0;
			if (next.end_pending) {
				enter_phase(
					&next,
					next.role == V32_CALLER ?
						V32_CALLER_SEND_END :
						V32_ANSWERER_SEND_END);
			} else if (phase_transmits_end(next.phase)) {
				enter_phase(&next, V32_CONFIRM);
			}
		}
	} else if (phase_transmits_bounded_training(next.phase)) {
		add_bounded(
			&next.phase_dibits,
			V32_STARTUP_TRAINING_DIBITS);
		if (next.phase_dibits == V32_STARTUP_TRAINING_DIBITS)
			finish_training_phase(&next);
	} else if (next.phase == V32_CONFIRM) {
		add_bounded(
			&next.phase_dibits,
			V32_STARTUP_CONFIRMATION_DIBITS);
		maybe_enter_data(&next);
	}
	*startup = next;
	*output = next_output;
	*dibit = next_dibit;
	return V32_STARTUP_OK;
}

static struct v32_rate_offer common_offer(
	const struct v32_rate_offer *local, const struct v32_rate_offer *peer)
{
	uint8_t common = local->rates & peer->rates;
	uint8_t highest = v32_rate_highest(common);
	struct v32_rate_offer available = offer_at_rate(local, highest);
	struct v32_rate_offer remote = offer_at_rate(peer, highest);

	/* At 9600, use trellis only when both peers offer it. V.32 1(e)
	 * requires support for the uncoded 16-state alternative. */
	available.rates = common;
	available.trellis_at_highest_rate &= remote.trellis_at_highest_rate;
	return available;
}

static enum v32_startup_result observe_rate_event(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer)
{
	struct v32_rate_offer local;

	if (!decode_local_offer(startup->rate_word, V32_RATE_SIGNAL,
			startup->transmitted_rates, &local))
		return V32_STARTUP_INVALID_ARGUMENT;
	if (startup->phase == V32_CALLER_WAIT_OFFER) {
		struct v32_rate_offer available = common_offer(&local, offer);

		if (available.rates == 0)
			return V32_STARTUP_NO_COMMON_RATE;
		if (!configure_rates(startup, &available, NULL))
			return V32_STARTUP_INVALID_ARGUMENT;
		enter_phase(startup, V32_CALLER_SEND_RESPONSE);
	} else if (startup->phase == V32_CALLER_SEND_RESPONSE) {
		if (startup->selection_expected) {
			/* Some peers retain lower enabled rates in R3. Select its
			 * highest rate, then emit a single-rate E. The preceding
			 * conditioning boundary distinguishes R3 from repeated R1. */
			uint8_t selected = v32_rate_highest(offer->rates);
			struct v32_rate_offer selection = *offer;

			selection.rates = selected;

			if (!offer_permits_selection(&local, &selection))
				return V32_STARTUP_NO_COMMON_RATE;
			if (!configure_rates(startup, &local, &selection))
				return V32_STARTUP_INVALID_ARGUMENT;
			startup->end_pending = true;
			startup->selection_expected = false;
		}
	} else if (startup->phase == V32_ANSWERER_SEND_OFFER) {
		struct v32_rate_offer available = common_offer(&local, offer);
		uint8_t selected = v32_rate_highest(available.rates);

		if (selected == 0)
			return V32_STARTUP_NO_COMMON_RATE;
		available.rates = selected;
		if (!configure_rates(startup, &available, &available))
			return V32_STARTUP_INVALID_ARGUMENT;
		enter_phase(startup, V32_ANSWERER_RETRAIN);
	}
	return V32_STARTUP_OK;
}

static enum v32_startup_result observe_end_event(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer)
{
	struct v32_rate_offer selected;

	if (!v32_startup_selected_offer(startup, &selected) ||
	    !v32_rate_offers_match(offer, &selected))
		return V32_STARTUP_NO_COMMON_RATE;
	if (startup->phase == V32_ANSWERER_SEND_SELECTION)
		startup->end_pending = true;
	if (startup->phase == V32_CALLER_SEND_END ||
	    startup->phase == V32_CONFIRM ||
	    startup->phase == V32_ANSWERER_SEND_END ||
	    (startup->phase == V32_ANSWERER_SEND_SELECTION &&
	     startup->end_pending)) {
		startup->end_received = true;
		startup->receive_mark_dibits = 0;
	}
	return V32_STARTUP_OK;
}

static enum v32_startup_result note_signal(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer,
	enum v32_rate_signal_type type)
{
	struct v32_startup next;
	enum v32_startup_result result;

	if (!v32_startup_is_valid(startup) ||
	    !offer_is_valid(type, offer))
		return V32_STARTUP_INVALID_ARGUMENT;
	next = *startup;
	result = type == V32_RATE_SIGNAL ?
		observe_rate_event(&next, offer) :
		observe_end_event(&next, offer);
	if (result != V32_STARTUP_OK)
		return result;
	if (!v32_startup_is_valid(&next))
		return V32_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	return V32_STARTUP_OK;
}

enum v32_startup_result v32_startup_note_rate_signal(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer)
{
	return note_signal(startup, offer, V32_RATE_SIGNAL);
}

enum v32_startup_result v32_startup_note_end_signal(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer)
{
	return note_signal(startup, offer, V32_END_SIGNAL);
}

static void observe_confirmation_dibit(
	struct v32_startup *startup, uint8_t dibit)
{
	if (!startup->end_received)
		return;
	if (dibit == 3) {
		add_bounded(
			&startup->receive_mark_dibits,
			V32_STARTUP_CONFIRMATION_DIBITS);
	} else {
		startup->receive_mark_dibits = 0;
	}
	maybe_enter_data(startup);
}

enum v32_startup_result v32_startup_note_confirmation(
	struct v32_startup *startup, uint8_t dibit,
	bool *connected)
{
	struct v32_startup next;

	if (!v32_startup_is_valid(startup) || dibit >= 4 ||
	    connected == NULL)
		return V32_STARTUP_INVALID_ARGUMENT;
	next = *startup;
	observe_confirmation_dibit(&next, dibit);
	if (!v32_startup_is_valid(&next))
		return V32_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	*connected = next.phase == V32_DATA;
	return V32_STARTUP_OK;
}

enum v32_startup_result v32_startup_observe(
	struct v32_startup *startup, uint8_t dibit,
	bool *connected)
{
	struct v32_startup next;
	struct v32_rate_offer offer;
	enum v32_rate_signal_event event;
	enum v32_startup_result result =
		V32_STARTUP_OK;

	if (!v32_startup_is_valid(startup) || dibit >= 4 ||
	    connected == NULL)
		return V32_STARTUP_INVALID_ARGUMENT;
	if (startup->phase == V32_DATA) {
		*connected = true;
		return V32_STARTUP_OK;
	}
	next = *startup;
	if (v32_rate_signal_detector_observe(
		    &next.detector, dibit, &event, &offer) !=
	    V32_RATE_SIGNAL_OK)
		return V32_STARTUP_INVALID_ARGUMENT;
	if (event == V32_RATE_SIGNAL_DETECTED)
		result = v32_startup_note_rate_signal(&next, &offer);
	else if (event == V32_END_SIGNAL_DETECTED)
		result = v32_startup_note_end_signal(&next, &offer);
	else if (event == V32_CLEARDOWN_SIGNAL_DETECTED)
		result = V32_STARTUP_NO_COMMON_RATE;
	else if (next.end_received)
		result = v32_startup_note_confirmation(
			&next, dibit, connected);
	if (result != V32_STARTUP_OK)
		return result;
	*startup = next;
	*connected = next.phase == V32_DATA;
	return V32_STARTUP_OK;
}

const char *v32_startup_phase_name(
	enum v32_startup_phase phase)
{
	switch (phase) {
	case V32_CALLER_WAIT_OFFER:
		return "caller training";
	case V32_CALLER_SEND_RESPONSE:
		return "caller rate response";
	case V32_CALLER_SEND_END:
		return "caller end";
	case V32_ANSWERER_TRAIN:
		return "answerer training";
	case V32_ANSWERER_SEND_OFFER:
		return "answerer rate offer";
	case V32_ANSWERER_RETRAIN:
		return "answerer receiver training";
	case V32_ANSWERER_SEND_SELECTION:
		return "answerer rate selection";
	case V32_ANSWERER_SEND_END:
		return "answerer end";
	case V32_CONFIRM:
		return "confirmation";
	case V32_DATA:
		return "data";
	}
	return "unknown";
}
