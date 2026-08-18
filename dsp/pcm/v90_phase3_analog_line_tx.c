// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_analog_line_tx.h"

#include <stddef.h>

static enum v90_phase3_analog_line_tx_result line_result(
	enum v34_training_line_result result)
{
	switch (result) {
	case V34_TRAINING_LINE_OK:
		return V90_PHASE3_ANALOG_LINE_TX_OK;
	case V34_TRAINING_LINE_INVALID_ARGUMENT:
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT;
	case V34_TRAINING_LINE_OUT_OF_RANGE:
		return V90_PHASE3_ANALOG_LINE_TX_OUT_OF_RANGE;
	case V34_TRAINING_LINE_CLIPPING_RISK:
		return V90_PHASE3_ANALOG_LINE_TX_CLIPPING_RISK;
	case V34_TRAINING_LINE_NUMERIC_FAILURE:
		return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
	}
	return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
}

static bool symbols_are_initial(
	const struct v90_phase3_analog_tx *symbols)
{
	return symbols->stage == V90_PHASE3_ANALOG_TX_PREFIX &&
	       symbols->sequence.prefix.stage == V34_PHASE3_TRANSMITTER_S &&
	       symbols->sequence.prefix.state.s.emitted_symbols == 0;
}

bool v90_phase3_analog_line_tx_owned_state_is_valid(
	const struct v90_phase3_analog_line_tx *tx)
{
	if (tx == NULL || !v34_training_line_is_valid(&tx->line) ||
	    tx->line.carrier.channel.sample_rate != V90_PHASE3_SAMPLE_RATE ||
	    tx->line.profile.local_role != V34_ANSWERING_STATION ||
	    tx->line.profile.md_symbol_count != 0 ||
	    !v90_phase3_analog_tx_owned_state_is_valid(&tx->symbols) ||
	    tx->leading_silence_samples >
		    V90_PHASE3_ANALOG_LEADING_SILENCE_SAMPLES)
		return false;
	if (tx->leading_silence_samples == 0)
		return true;
	return tx->line.generated_samples == 0 &&
	       symbols_are_initial(&tx->symbols);
}

bool v90_phase3_analog_line_tx_is_valid(
	const struct v90_phase3_analog_line_tx *tx)
{
	return v90_phase3_analog_line_tx_owned_state_is_valid(tx) &&
	       v90_phase3_analog_tx_is_valid(&tx->symbols);
}

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_init(
	struct v90_phase3_analog_line_tx *tx,
	const struct v34_phase3_profile *analog_profile,
	int16_t nominal_peak_amplitude)
{
	struct v90_phase3_analog_line_tx initialized = {
		.leading_silence_samples =
			V90_PHASE3_ANALOG_LEADING_SILENCE_SAMPLES,
	};
	enum v90_phase3_analog_line_tx_result result;

	if (tx == NULL || !v34_phase3_profile_is_valid(analog_profile) ||
	    analog_profile->local_role != V34_ANSWERING_STATION ||
	    analog_profile->md_symbol_count != 0)
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT;
	result = line_result(v34_training_line_init(
		&initialized.line, analog_profile, V90_PHASE3_SAMPLE_RATE,
		nominal_peak_amplitude));
	if (result != V90_PHASE3_ANALOG_LINE_TX_OK)
		return result;
	if (v90_phase3_analog_tx_init(&initialized.symbols) !=
		    V90_PHASE3_ANALOG_TX_OK ||
	    !v90_phase3_analog_line_tx_is_valid(&initialized))
		return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
	*tx = initialized;
	return V90_PHASE3_ANALOG_LINE_TX_OK;
}

static enum v90_phase3_analog_line_tx_result control_result(
	struct v90_phase3_analog_line_tx *tx,
	enum v90_phase3_analog_tx_result result)
{
	if (result == V90_PHASE3_ANALOG_TX_INVALID_STATE)
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE;
	if (result != V90_PHASE3_ANALOG_TX_OK ||
	    !v90_phase3_analog_line_tx_is_valid(tx))
		return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
	return V90_PHASE3_ANALOG_LINE_TX_OK;
}

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_start_dil_ja(
	struct v90_phase3_analog_line_tx *tx,
	const struct v90_dil_parameters *parameters)
{
	struct v90_phase3_analog_line_tx next;
	enum v90_phase3_analog_line_tx_result result;

	if (!v90_phase3_analog_line_tx_is_valid(tx))
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT;
	next = *tx;
	result = control_result(
		&next,
		v90_phase3_analog_tx_start_dil_ja(
			&next.symbols, parameters));
	if (result == V90_PHASE3_ANALOG_LINE_TX_OK)
		*tx = next;
	return result;
}

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_start_ja(
	struct v90_phase3_analog_line_tx *tx)
{
	struct v90_dil_parameters parameters;

	if (v90_dil_profile_init(
		    &parameters, V90_DIL_PROFILE_NONE) != V90_DIL_OK)
		return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
	return v90_phase3_analog_line_tx_start_dil_ja(tx, &parameters);
}

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_finish_ja(
	struct v90_phase3_analog_line_tx *tx)
{
	struct v90_phase3_analog_line_tx next;
	enum v90_phase3_analog_line_tx_result result;

	if (!v90_phase3_analog_line_tx_is_valid(tx))
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT;
	next = *tx;
	result = control_result(
		&next, v90_phase3_analog_tx_finish_ja(&next.symbols));
	if (result == V90_PHASE3_ANALOG_LINE_TX_OK)
		*tx = next;
	return result;
}

bool v90_phase3_analog_line_tx_owned_handoff_is_ready(
	const struct v90_phase3_analog_line_tx *tx)
{
	return v90_phase3_analog_line_tx_owned_state_is_valid(tx) &&
	       tx->leading_silence_samples == 0 &&
	       tx->symbols.stage == V90_PHASE3_ANALOG_TX_COMPLETE &&
	       v34_training_line_needs_point(&tx->line);
}

bool v90_phase3_analog_line_tx_is_handoff_ready(
	const struct v90_phase3_analog_line_tx *tx)
{
	return v90_phase3_analog_line_tx_is_valid(tx) &&
	       v90_phase3_analog_line_tx_owned_handoff_is_ready(tx);
}

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_sample(
	struct v90_phase3_analog_line_tx *tx, int16_t *sample)
{
	struct v90_phase3_analog_line_tx next;
	struct qam_point point;
	const struct qam_point *point_pointer = NULL;
	enum v34_training_line_result waveform_result;
	int16_t generated;
	bool accepting;
	bool accepted;

	if (!v90_phase3_analog_line_tx_owned_state_is_valid(tx) ||
	    sample == NULL)
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT;
	if (v90_phase3_analog_line_tx_owned_handoff_is_ready(tx))
		return V90_PHASE3_ANALOG_LINE_TX_COMPLETE;
	next = *tx;
	if (next.leading_silence_samples != 0) {
		next.leading_silence_samples--;
		if (!v90_phase3_analog_line_tx_owned_state_is_valid(&next))
			return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
		*tx = next;
		*sample = 0;
		return V90_PHASE3_ANALOG_LINE_TX_OK;
	}
	accepting = v34_training_line_needs_point(&next.line);
	if (accepting) {
		if (v90_phase3_analog_tx_next(&next.symbols, &point) !=
		    V90_PHASE3_ANALOG_TX_OK)
			return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
		point_pointer = &point;
	}
	waveform_result = v34_training_line_sample(
		&next.line, point_pointer, &generated, &accepted);
	if (waveform_result != V34_TRAINING_LINE_OK)
		return line_result(waveform_result);
	if (accepted != accepting ||
	    !v90_phase3_analog_line_tx_owned_state_is_valid(&next))
		return V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR;
	*tx = next;
	*sample = generated;
	return V90_PHASE3_ANALOG_LINE_TX_OK;
}

enum v90_phase3_analog_line_tx_result v90_phase3_analog_line_tx_handoff(
	const struct v90_phase3_analog_line_tx *tx,
	struct v34_training_line *line)
{
	if (!v90_phase3_analog_line_tx_is_valid(tx) || line == NULL)
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT;
	if (!v90_phase3_analog_line_tx_is_handoff_ready(tx))
		return V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE;
	*line = tx->line;
	return V90_PHASE3_ANALOG_LINE_TX_OK;
}

const char *v90_phase3_analog_line_tx_result_name(
	enum v90_phase3_analog_line_tx_result result)
{
	switch (result) {
	case V90_PHASE3_ANALOG_LINE_TX_OK:
		return "ok";
	case V90_PHASE3_ANALOG_LINE_TX_COMPLETE:
		return "complete";
	case V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_ANALOG_LINE_TX_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE3_ANALOG_LINE_TX_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE3_ANALOG_LINE_TX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
