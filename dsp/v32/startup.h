// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_STARTUP_H
#define V32_STARTUP_H

#include "rate_signal.h"
#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* One dibit is carried per 2400-baud training symbol. */
	V32_STARTUP_TRAINING_DIBITS = 1280,
	V32_STARTUP_CONFIRMATION_DIBITS = V32_STARTUP_CONFIRMATION_SYMBOLS,
};

enum v32_endpoint_role {
	V32_CALLER,
	V32_ANSWERER,
};

enum v32_startup_phase {
	V32_CALLER_WAIT_OFFER,
	V32_CALLER_SEND_RESPONSE,
	V32_CALLER_SEND_END,
	V32_ANSWERER_TRAIN,
	V32_ANSWERER_SEND_OFFER,
	V32_ANSWERER_RETRAIN,
	V32_ANSWERER_SEND_SELECTION,
	V32_ANSWERER_SEND_END,
	V32_CONFIRM,
	V32_DATA,
};

/* Identifies the framing required for the returned dibit. */
enum v32_startup_output {
	V32_STARTUP_TRN_DIBIT,
	V32_STARTUP_DIFFERENTIAL_DIBIT,
	V32_STARTUP_SELECTED_RATE_MARK,
	V32_STARTUP_DATA,
};

enum v32_startup_result {
	V32_STARTUP_OK,
	V32_STARTUP_INVALID_ARGUMENT,
	V32_STARTUP_NO_COMMON_RATE,
};

struct v32_startup {
	struct v32_rate_signal_detector detector;
	uint16_t rate_word;
	uint16_t end_word;
	uint16_t phase_dibits;
	uint16_t receive_mark_dibits;
	enum v32_endpoint_role role;
	enum v32_startup_phase phase;
	/* Capability bit set retained independently from the chosen rate. */
	uint8_t supported_rates;
	/* Capability bits represented by rate_word. */
	uint8_t transmitted_rates;
	/* One capability bit, or zero until a rate is selected. */
	uint8_t selected_rate;
	uint8_t word_dibit;
	bool end_received;
	bool end_pending;
	bool selection_expected;
};

/* Prefer trellis above 4800; 9600 also negotiates the uncoded alternative. */
enum v32_startup_result v32_startup_init(
	struct v32_startup *startup,
	enum v32_endpoint_role role, uint8_t supported_rates);
bool v32_startup_is_valid(
	const struct v32_startup *startup);
bool v32_startup_is_complete(
	const struct v32_startup *startup);

/* Read the rate and coding retained in the locally selected E sequence.
 * A missing selection or invalid word leaves offer unchanged. */
bool v32_startup_selected_offer(
	const struct v32_startup *startup, struct v32_rate_offer *offer);

/* Call after the calling modem's second receiver conditioning completes. */
enum v32_startup_result v32_startup_expect_selection(
	struct v32_startup *startup);

/* Advance the protocol from a completed locally owned conditioning stream.
 * The physical transmitter owns the actual S, S-bar and TRN duration. */
enum v32_startup_result v32_startup_finish_conditioning(
	struct v32_startup *startup);

/* Apply already-decoded receive events without running a second rate-signal
 * detector. These transitions are shared with v32_startup_observe(). */
enum v32_startup_result v32_startup_note_rate_signal(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer);
enum v32_startup_result v32_startup_note_end_signal(
	struct v32_startup *startup,
	const struct v32_rate_offer *offer);
enum v32_startup_result v32_startup_note_confirmation(
	struct v32_startup *startup, uint8_t dibit,
	bool *connected);

enum v32_startup_result v32_startup_next_transmit(
	struct v32_startup *startup,
	enum v32_startup_output *output, uint8_t *dibit);
enum v32_startup_result v32_startup_observe(
	struct v32_startup *startup, uint8_t dibit,
	bool *connected);

const char *v32_startup_phase_name(
	enum v32_startup_phase phase);

#endif
