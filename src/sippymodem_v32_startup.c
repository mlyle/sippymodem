// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_startup.h"

#include <stddef.h>

/* This controller starts at TRN. The preceding two-wire tone, turnaround and
 * echo-canceller training phases remain outside this source-built media path. */

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
	struct sippymodem_v32_rate_offer selected = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800,
	};
	uint16_t rate_word;
	uint16_t end_word;

	return sippymodem_v32_rate_signal_encode(
		       SIPPYMODEM_V32_RATE_SIGNAL, &selected, &rate_word) ==
		       SIPPYMODEM_V32_RATE_SIGNAL_OK &&
	       sippymodem_v32_rate_signal_encode(
		       SIPPYMODEM_V32_END_SIGNAL, &selected, &end_word) ==
		       SIPPYMODEM_V32_RATE_SIGNAL_OK &&
	       startup->rate_word == rate_word && startup->end_word == end_word;
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
	       words_are_valid(startup) &&
	       startup->phase_dibits <= phase_dibit_limit(startup->phase) &&
	       startup->receive_mark_dibits <=
		       SIPPYMODEM_V32_STARTUP_CONFIRMATION_DIBITS &&
	       startup->rate_gap_dibits <=
		       SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS &&
	       startup->word_dibit < SIPPYMODEM_V32_RATE_SIGNAL_DIBITS;
}

enum sippymodem_v32_startup_result sippymodem_v32_startup_init(
	struct sippymodem_v32_startup *startup,
	enum sippymodem_v32_endpoint_role role)
{
	struct sippymodem_v32_rate_offer selected = {
		.rates = SIPPYMODEM_V32_RATE_CAPABILITY_4800,
	};
	struct sippymodem_v32_startup next = { 0 };

	if (startup == NULL || !role_is_valid(role))
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	next.role = role;
	next.phase = role == SIPPYMODEM_V32_CALLER ?
			     SIPPYMODEM_V32_CALLER_WAIT_OFFER :
			     SIPPYMODEM_V32_ANSWERER_TRAIN;
	if (sippymodem_v32_rate_signal_detector_init(&next.detector) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK ||
	    sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_RATE_SIGNAL, &selected, &next.rate_word) !=
		    SIPPYMODEM_V32_RATE_SIGNAL_OK ||
	    sippymodem_v32_rate_signal_encode(
		    SIPPYMODEM_V32_END_SIGNAL, &selected, &next.end_word) !=
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
	uint8_t next_dibit = 3;

	if (!sippymodem_v32_startup_is_valid(startup) || output == NULL ||
	    dibit == NULL)
		return SIPPYMODEM_V32_STARTUP_INVALID_ARGUMENT;
	if (startup->phase == SIPPYMODEM_V32_DATA) {
		*output = SIPPYMODEM_V32_STARTUP_DATA;
		return SIPPYMODEM_V32_STARTUP_OK;
	}
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
			if (phase_transmits_end(next.phase))
				enter_phase(&next, SIPPYMODEM_V32_CONFIRM);
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
	*output = SIPPYMODEM_V32_STARTUP_DIBIT;
	*dibit = next_dibit;
	return SIPPYMODEM_V32_STARTUP_OK;
}

static bool offer_supports_4800(
	const struct sippymodem_v32_rate_offer *offer)
{
	return (offer->rates & SIPPYMODEM_V32_RATE_CAPABILITY_4800) != 0;
}

static bool offer_selects_4800(
	const struct sippymodem_v32_rate_offer *offer)
{
	return offer_supports_4800(offer) &&
	       (offer->rates & SIPPYMODEM_V32_RATE_CAPABILITY_9600) == 0 &&
	       !offer->trellis_at_highest_rate;
}

static enum sippymodem_v32_startup_result observe_rate_event(
	struct sippymodem_v32_startup *startup,
	const struct sippymodem_v32_rate_offer *offer)
{
	if (startup->phase == SIPPYMODEM_V32_CALLER_WAIT_OFFER) {
		if (!offer_supports_4800(offer))
			return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
		enter_phase(startup, SIPPYMODEM_V32_CALLER_SEND_RESPONSE);
	} else if (startup->phase == SIPPYMODEM_V32_CALLER_SEND_RESPONSE) {
		if (startup->rate_gap_dibits <
		    SIPPYMODEM_V32_STARTUP_RATE_GAP_DIBITS) {
			startup->rate_gap_dibits = 0;
		} else {
			if (!offer_selects_4800(offer))
				return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
			enter_phase(startup, SIPPYMODEM_V32_CALLER_SEND_END);
		}
	} else if (startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_OFFER) {
		if (!offer_supports_4800(offer))
			return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
		enter_phase(startup, SIPPYMODEM_V32_ANSWERER_RETRAIN);
	}
	return SIPPYMODEM_V32_STARTUP_OK;
}

static enum sippymodem_v32_startup_result observe_end_event(
	struct sippymodem_v32_startup *startup,
	const struct sippymodem_v32_rate_offer *offer)
{
	if (!offer_selects_4800(offer))
		return SIPPYMODEM_V32_STARTUP_NO_COMMON_RATE;
	if (startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_SELECTION)
		enter_phase(startup, SIPPYMODEM_V32_ANSWERER_SEND_END);
	if (startup->phase == SIPPYMODEM_V32_CALLER_SEND_END ||
	    startup->phase == SIPPYMODEM_V32_CONFIRM ||
	    startup->phase == SIPPYMODEM_V32_ANSWERER_SEND_END) {
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
	else if (next.phase == SIPPYMODEM_V32_CALLER_SEND_END ||
		 next.phase == SIPPYMODEM_V32_ANSWERER_SEND_END ||
		 next.phase == SIPPYMODEM_V32_CONFIRM)
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
