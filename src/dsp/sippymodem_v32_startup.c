// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_startup.h"

#include <stddef.h>

/* This controller starts at TRN. The preceding two-wire tone, turnaround and
 * echo-canceller training phases remain outside this source-built media path. */

enum {
	V32_IMPLEMENTED_RATES = SIPPYMODEM_V32_RATE_CAPABILITY_4800 |
				SIPPYMODEM_V32_RATE_CAPABILITY_7200 |
				SIPPYMODEM_V32_RATE_CAPABILITY_9600 |
				SIPPYMODEM_V32_RATE_CAPABILITY_12000,
};

static uint8_t highest_rate(uint8_t rates);

static bool rate_requires_trellis(uint8_t rate)
{
	return rate == SIPPYMODEM_V32_RATE_CAPABILITY_7200 ||
	       rate == SIPPYMODEM_V32_RATE_CAPABILITY_12000 ||
	       rate == SIPPYMODEM_V32_RATE_CAPABILITY_14400;
}

static struct sippymodem_v32_rate_offer rate_offer(uint8_t rates)
{
	return (struct sippymodem_v32_rate_offer){
		.rates = rates,
		.trellis_at_highest_rate = rate_requires_trellis(
			highest_rate(rates)),
	};
}

static bool role_is_valid(enum sippymodem_v32_endpoint_role role)
{
	return role == SIPPYMODEM_V32_CALLER ||
	       role == SIPPYMODEM_V32_ANSWERER;
}

static bool phase_is_valid_for_role(
	enum sippymodem_v32_startup_phase phase,
	enum sippymodem_v32_endpoint_role role)
{
	if (phase == SIPPYMODEM_V32_CONFIRM || phase == SIPPYMODEM_V32_DATA)
		return true;
	if (role == SIPPYMODEM_V32_CALLER)
		return phase >= SIPPYMODEM_V32_CALLER_WAIT_OFFER &&
		       phase <= SIPPYMODEM_V32_CALLER_SEND_END;
	return phase >= SIPPYMODEM_V32_ANSWERER_TRAIN &&
	       phase <= SIPPYMODEM_V32_ANSWERER_SEND_END;
}

static bool words_are_valid(const struct sippymodem_v32_startup *startup)
{
	struct sippymodem_v32_rate_offer offer = rate_offer(
		startup->selected_rate != 0 ? startup->selected_rate :
					       startup->supported_rates);
	uint16_t rate_word;
	uint16_t end_word = 0;

	if (sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_RATE_SIGNAL, &offer, &rate_word) !=
	    SIPPYMODEM_V32_RATE_SIGNAL_OK)
		return false;
	if (startup->selected_rate != 0 &&
	    sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_END_SIGNAL, &offer, &end_word) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK)
		return false;
	return startup->rate_word == rate_word && startup->end_word == end_word;
}

static bool rates_are_valid(const struct sippymodem_v32_startup *startup)
{
	bool selection_required;

	if (startup->supported_rates == 0 ||
	    (startup->supported_rates & (uint8_t)~V32_IMPLEMENTED_RATES) != 0 ||
	    (startup->selected_rate & (uint8_t)~startup->supported_rates) != 0 ||
	    (startup->selected_rate != 0 &&
	     (startup->selected_rate &
	      (uint8_t)(startup->selected_rate - 1u)) != 0))
		return false;
	selection_required =
		startup->phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE ||
		startup->phase == SIPPYMODEM_V32_CALLER_SEND_END ||
		startup->phase == SIPPYMODEM_V32_ANSWERER_RETRAIN ||
		startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION ||
		startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_END ||
		startup->phase == SIPPYMODEM_V32_CONFIRM ||
		startup->phase == SIPPYMODEM_V32_DATA;
	return selection_required == (startup->selected_rate != 0);
}

static bool configure_selected_rate(
	struct sippymodem_v32_startup *startup, uint8_t selected_rate)
{
	struct sippymodem_v32_rate_offer selected = rate_offer(selected_rate);
	uint16_t rate_word;
	uint16_t end_word;

	if ((selected_rate & startup->supported_rates) == 0 ||
	    (selected_rate & (uint8_t)(selected_rate - 1u)) != 0 ||
	    sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_RATE_SIGNAL, &selected, &rate_word) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK ||
	    sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_END_SIGNAL, &selected, &end_word) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK)
		return false;
	startup->selected_rate = selected_rate;
	startup->rate_word = rate_word;
	startup->end_word = end_word;
	return true;
}

static uint16_t phase_dibit_limit(
	enum sippymodem_v32_startup_phase phase)
{
	if (phase == SIPPYMODEM_V32_ANSWERER_TRAIN ||
	    phase == SIPPYMODEM_V32_ANSWERER_RETRAIN)
		return SIPPYMODEM_V32_STARTUP_TRAINING_DIBITS;
	if (phase == SIPPYMODEM_V32_CONFIRM)
		return SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS;
	return 0;
}

bool sippymodem_v32_startup_is_valid(
	const struct sippymodem_v32_startup *startup)
{
	return startup != NULL && role_is_valid(startup->role) &&
	       phase_is_valid_for_role(startup->phase, startup->role) &&
	       sippymodem_v32_rate_signal_detector_is_valid(
		       &startup->detector) &&
	       rates_are_valid(startup) && words_are_valid(startup) &&
	       startup->phase_dibits <= phase_dibit_limit(startup->phase) &&
	       startup->receive_mark_dibits <=
		       SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS &&
	       startup->rate_gap_dibits <=
		       SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS &&
	       startup->word_dibit < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS &&
	       (!startup->end_pending ||
		(startup->phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE ||
		 startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION));
}

enum sippymodem_v32_startup_result sippymodem_v32_startup_init(
	struct sippymodem_v32_startup *startup,
	enum sippymodem_v32_endpoint_role role, uint8_t supported_rates)
{
	struct sippymodem_v32_rate_offer offer = rate_offer(supported_rates);
	struct sippymodem_v32_startup next = { 0 };

	if (startup == NULL || !role_is_valid(role) || supported_rates == 0 ||
	    (supported_rates & (uint8_t)~V32_IMPLEMENTED_RATES) != 0)
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	next.role = role;
	next.supported_rates = supported_rates;
	next.phase = role == SIPPYMODEM_V32_CALLER ?
			     SIPPYMODEM_V32_CALLER_WAIT_OFFER :
			     SIPPYMODEM_V32_ANSWERER_TRAIN;
	if (sippymodem_v32_rate_signal_detector_init(&next.detector) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK ||
	    sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_RATE_SIGNAL, &offer, &next.rate_word) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK)
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	*startup = next;
	return SIPPYMODEM_V32_STARTUP_OK;
}

bool sippymodem_v32_startup_is_complete(
	const struct sippymodem_v32_startup *startup)
{
	return sippymodem_v32_startup_is_valid(startup) &&
	       startup->phase == SIPPYMODEM_V32_DATA;
}

static void enter_phase(
	struct sippymodem_v32_startup *startup,
	enum sippymodem_v32_startup_phase phase)
{
	startup->phase = phase;
	startup->phase_dibits = 0;
	startup->rate_gap_dibits = 0;
	startup->word_dibit = 0;
	startup->end_pending = false;
}

static void add_bounded(uint16_t *value, uint16_t limit)
{
	if (*value < limit)
		(*value)++;
}

static void maybe_enter_data(struct sippymodem_v32_startup *startup)
{
	if (startup->phase == SIPPYMODEM_V32_CONFIRM &&
	    startup->end_received &&
	    startup->phase_dibits ==
		    SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS &&
	    startup->receive_mark_dibits ==
		    SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS)
		enter_phase(startup, SIPPYMODEM_V32_DATA);
}

static bool phase_transmits_rate(enum sippymodem_v32_startup_phase phase)
{
	return phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE ||
	       phase == SIPPYMODEM_V32_ANSWERER_SEND_OFFER ||
	       phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION;
}

static bool phase_transmits_end(enum sippymodem_v32_startup_phase phase)
{
	return phase == SIPPYMODEM_V32_CALLER_SEND_END ||
	       phase == SIPPYMODEM_V32_ANSWERER_SEND_END;
}

static bool phase_transmits_bounded_training(
	enum sippymodem_v32_startup_phase phase)
{
	return phase == SIPPYMODEM_V32_ANSWERER_TRAIN ||
	       phase == SIPPYMODEM_V32_ANSWERER_RETRAIN;
}

static bool phase_uses_trn_framing(
	enum sippymodem_v32_startup_phase phase)
{
	return phase == SIPPYMODEM_V32_CALLER_WAIT_OFFER ||
	       phase_transmits_bounded_training(phase);
}

static void finish_training_phase(struct sippymodem_v32_startup *startup)
{
	enter_phase(startup,
		    startup->phase == SIPPYMODEM_V32_ANSWERER_TRAIN ?
			    SIPPYMODEM_V32_ANSWERER_SEND_OFFER :
			    SIPPYMODEM_V32_ANSWERER_SEND_SELECTION);
}

enum sippymodem_v32_startup_result sippymodem_v32_startup_next_transmit(
	struct sippymodem_v32_startup *startup,
	enum sippymodem_v32_startup_output *output, uint8_t *dibit)
{
	struct sippymodem_v32_startup next;
	enum sippymodem_v32_startup_output next_output;
	uint8_t next_dibit = 3;

	if (!sippymodem_v32_startup_is_valid(startup) || output == NULL ||
	    dibit == NULL)
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	if (startup->phase == SIPPYMODEM_V32_DATA) {
		*output = SIPPYMODEM_V32_STARTUP_DATA;
		return SIPPYMODEM_V32_STARTUP_OK;
	}
	if (startup->phase == SIPPYMODEM_V32_CONFIRM)
		next_output = SIPPYMODEM_V32_STARTUP_SELECTED_RATE_MARK;
	else
		next_output = phase_uses_trn_framing(startup->phase) ?
				      SIPPYMODEM_V32_STARTUP_TRN_DIBIT :
				      SIPPYMODEM_V32_STARTUP_DIFFERENTIAL_DIBIT;
	next = *startup;
	if (phase_transmits_rate(next.phase) || phase_transmits_end(next.phase)) {
		uint16_t word = phase_transmits_rate(next.phase) ?
					startup->rate_word : startup->end_word;

		if (sippymodem_v32_rate_signal_dibit(
			    word, next.word_dibit, &next_dibit) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK)
			return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
		next.word_dibit++;
		if (next.word_dibit == SIPPYMODEM_V32_RATE_SIGNAL_DIBITS) {
			next.word_dibit = 0;
			if (next.end_pending) {
				enter_phase(
					&next,
					next.role == SIPPYMODEM_V32_CALLER ?
						SIPPYMODEM_V32_CALLER_SEND_END :
						SIPPYMODEM_V32_ANSWERER_SEND_END);
			} else if (phase_transmits_end(next.phase)) {
				enter_phase(&next, SIPPYMODEM_V32_CONFIRM);
			}
		}
	} else if (phase_transmits_bounded_training(next.phase)) {
		add_bounded(
			&next.phase_dibits,
			SIPPYMODEM_V32_STARTUP_TRAINING_DIBITS);
		if (next.phase_dibits == SIPPYMODEM_V32_STARTUP_TRAINING_DIBITS)
			finish_training_phase(&next);
	} else if (next.phase == SIPPYMODEM_V32_CONFIRM) {
		add_bounded(
			&next.phase_dibits,
			SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS);
		maybe_enter_data(&next);
	}
	*startup = next;
	*output = next_output;
	*dibit = next_dibit;
	return SIPPYMODEM_V32_STARTUP_OK;
}

static uint8_t highest_rate(uint8_t rates)
{
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_14400) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_14400;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_12000) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_12000;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_9600) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_9600;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_7200) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_7200;
	if ((rates & SIPPYMODEM_V32_RATE_CAPABILITY_4800) != 0)
		return SIPPYMODEM_V32_RATE_CAPABILITY_4800;
	return 0;
}

static uint8_t select_highest_common_rate(
	const struct sippymodem_v32_startup *startup,
	const struct sippymodem_v32_rate_offer *offer)
{
	uint8_t common = startup->supported_rates & offer->rates;

	if (offer->trellis_at_highest_rate &&
	    highest_rate(offer->rates) == SIPPYMODEM_V32_RATE_CAPABILITY_9600)
		common &= (uint8_t)~highest_rate(offer->rates);
	return highest_rate(common);
}

static bool offer_selects_rate(
	const struct sippymodem_v32_rate_offer *offer, uint8_t selected_rate)
{
	return offer->trellis_at_highest_rate ==
		       rate_requires_trellis(selected_rate) &&
	       highest_rate(offer->rates) == selected_rate;
}

static enum sippymodem_v32_startup_result observe_rate_event(
	struct sippymodem_v32_startup *startup,
	const struct sippymodem_v32_rate_offer *offer)
{
	if (startup->phase == SIPPYMODEM_V32_CALLER_WAIT_OFFER) {
		uint8_t selected = select_highest_common_rate(startup, offer);

		if (selected == 0)
			return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
		if (!configure_selected_rate(startup, selected))
			return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
		enter_phase(startup, SIPPYMODEM_V32_CALLER_SEND_RESPONSE);
	} else if (startup->phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE) {
		if (startup->rate_gap_dibits <
		    SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS) {
			startup->rate_gap_dibits = 0;
		} else {
			if (!offer_selects_rate(offer, startup->selected_rate))
				return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
			startup->rate_gap_dibits = 0;
			startup->end_pending = true;
		}
	} else if (startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_OFFER) {
		uint8_t selected = select_highest_common_rate(startup, offer);

		if (selected == 0)
			return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
		if (!configure_selected_rate(startup, selected))
			return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
		enter_phase(startup, SIPPYMODEM_V32_ANSWERER_RETRAIN);
	}
	return SIPPYMODEM_V32_STARTUP_OK;
}

static enum sippymodem_v32_startup_result observe_end_event(
	struct sippymodem_v32_startup *startup,
	const struct sippymodem_v32_rate_offer *offer)
{
	if (!offer_selects_rate(offer, startup->selected_rate))
		return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
	if (startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION)
		startup->end_pending = true;
	if (startup->phase == SIPPYMODEM_V32_CALLER_SEND_END ||
	    startup->phase == SIPPYMODEM_V32_CONFIRM ||
	    startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_END ||
	    (startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION &&
	     startup->end_pending)) {
		startup->end_received = true;
		startup->receive_mark_dibits = 0;
	}
	return SIPPYMODEM_V32_STARTUP_OK;
}

static void observe_rate_gap(
	struct sippymodem_v32_startup *startup, bool was_aligned)
{
	if (startup->phase != SIPPYMODEM_V32_CALLER_SEND_RESPONSE)
		return;
	if (!startup->detector.aligned &&
	    (was_aligned || startup->rate_gap_dibits != 0))
		add_bounded(
			&startup->rate_gap_dibits,
			SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS);
}

static void observe_confirmation_dibit(
	struct sippymodem_v32_startup *startup, uint8_t dibit)
{
	if (!startup->end_received)
		return;
	if (dibit == 3) {
		add_bounded(
			&startup->receive_mark_dibits,
			SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS);
	} else {
		startup->receive_mark_dibits = 0;
	}
	maybe_enter_data(startup);
}

enum sippymodem_v32_startup_result sippymodem_v32_startup_observe(
	struct sippymodem_v32_startup *startup, uint8_t dibit,
	bool *connected)
{
	struct sippymodem_v32_startup next;
	struct sippymodem_v32_rate_offer offer;
	enum sippymodem_v32_rate_signal_event event;
	enum sippymodem_v32_startup_result result =
		SIPPYMODEM_V32_STARTUP_OK;
	bool was_aligned;

	if (!sippymodem_v32_startup_is_valid(startup) || dibit >= 4 ||
	    connected == NULL)
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	if (startup->phase == SIPPYMODEM_V32_DATA) {
		*connected = true;
		return SIPPYMODEM_V32_STARTUP_OK;
	}
	next = *startup;
	was_aligned = next.detector.aligned;
	if (sippymodem_v32_rate_signal_detector_observe(
		    &next.detector, dibit, &event, &offer) !=
	    SIPPYMODEM_V32_RATE_SIGNAL_OK)
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	observe_rate_gap(&next, was_aligned);
	if (event == SIPPYMODEM_V32_RATE_SIGNAL_DETECTED)
		result = observe_rate_event(&next, &offer);
	else if (event == SIPPYMODEM_V32_END_SIGNAL_DETECTED)
		result = observe_end_event(&next, &offer);
	else if (next.end_received)
		observe_confirmation_dibit(&next, dibit);
	if (result != SIPPYMODEM_V32_STARTUP_OK)
		return result;
	*startup = next;
	*connected = next.phase == SIPPYMODEM_V32_DATA;
	return SIPPYMODEM_V32_STARTUP_OK;
}

const char *sippymodem_v32_startup_phase_name(
	enum sippymodem_v32_startup_phase phase)
{
	switch (phase) {
	case SIPPYMODEM_V32_CALLER_WAIT_OFFER:
		return "caller training";
	case SIPPYMODEM_V32_CALLER_SEND_RESPONSE:
		return "caller rate response";
	case SIPPYMODEM_V32_CALLER_SEND_END:
		return "caller end";
	case SIPPYMODEM_V32_ANSWERER_TRAIN:
		return "answerer training";
	case SIPPYMODEM_V32_ANSWERER_SEND_OFFER:
		return "answerer rate offer";
	case SIPPYMODEM_V32_ANSWERER_RETRAIN:
		return "answerer receiver training";
	case SIPPYMODEM_V32_ANSWERER_SEND_SELECTION:
		return "answerer rate selection";
	case SIPPYMODEM_V32_ANSWERER_SEND_END:
		return "answerer end";
	case SIPPYMODEM_V32_CONFIRM:
		return "confirmation";
	case SIPPYMODEM_V32_DATA:
		return "data";
	}
	return "unknown";
}

const char *sippymodem_v32_startup_result_name(
	enum sippymodem_v32_startup_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_STARTUP_OK:
		return "ok";
	case SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE:
		return "no common rate";
	}
	return "unknown";
}
