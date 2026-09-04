// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_E_LINE_RX_H
#define V90_PHASE4_E_LINE_RX_H

#include "v34_data_line_receiver.h"
#include "v34_phase4_e_receiver.h"
#include "v90_phase4_cp_line_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase4_e_line_rx_result {
	/* A line sample was accepted without producing an equalized symbol. */
	V90_PHASE4_E_LINE_RX_OK,
	V90_PHASE4_E_LINE_RX_SYMBOL,
	/* The accepted sample produced the final E decision. */
	V90_PHASE4_E_LINE_RX_E_COMPLETE,
	/* E was already complete; no sample was consumed. */
	V90_PHASE4_E_LINE_RX_COMPLETE,
	V90_PHASE4_E_LINE_RX_FAILED,
	V90_PHASE4_E_LINE_RX_INVALID_ARGUMENT,
	V90_PHASE4_E_LINE_RX_INVALID_STATE,
	V90_PHASE4_E_LINE_RX_NUMERIC_FAILURE,
};

/* Own the digital modem's continuous conventional-upstream line after an
 * accepted CP-prime record. Carrier timing, equalizer delay, adaptation,
 * descrambler history, and differential phase cross the boundary unchanged.
 * The equalizer already owns the first delayed E inputs. */
struct v90_phase4_e_line_rx {
	struct qam_demodulator demodulator;
	struct qam_equalizer equalizer;
	struct qam_equalizer compact_equalizer;
	struct v34_phase4_e_receiver symbols;
	struct v34_phase3_profile transmitting_profile;
	struct v90_cp_parameters cp;
	double adaptation;
	bool tap_adaptation_held;
	bool failed;
	bool compact_equalizer_active;
};

enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_init(
	struct v90_phase4_e_line_rx *rx,
	const struct v90_phase4_cp_line_rx *completed_cp);
bool v90_phase4_e_line_rx_is_valid(
	const struct v90_phase4_e_line_rx *rx);
bool v90_phase4_e_line_rx_has_failed(
	const struct v90_phase4_e_line_rx *rx);
bool v90_phase4_e_line_rx_is_complete(
	const struct v90_phase4_e_line_rx *rx);

/* Consume one signed 8 kHz sample. Complete and rejected calls consume
 * nothing. A terminal DSP error consumes the sample and latches failure
 * because the continuous waveform cannot be replayed. */
enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_sample(
	struct v90_phase4_e_line_rx *rx, int16_t sample);

/* Initialize the continuous B1/data receiver at the exact completed-E
 * boundary. The destination changes only on success. */
enum v90_phase4_e_line_rx_result v90_phase4_e_line_rx_start_data(
	const struct v90_phase4_e_line_rx *rx,
	struct v34_data_line_receiver *data_rx,
	const struct v34_data_line_receiver_configuration *configuration);

const char *v90_phase4_e_line_rx_result_name(
	enum v90_phase4_e_line_rx_result result);

#endif /* V90_PHASE4_E_LINE_RX_H */
