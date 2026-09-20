// SPDX-License-Identifier: BSD-3-Clause

#include "v92_ru_line_rx.h"

#include <stddef.h>

bool v92_ru_line_rx_is_valid(const struct v92_ru_line_rx *receiver)
{
	return receiver != NULL &&
	       qam_equalizer_is_handoff_ready(&receiver->equalizer) &&
	       receiver->equalizer.tap_count == V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS &&
	       receiver->equalizer.decision_delay == V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	       v92_ru_rx_is_valid(&receiver->signal) &&
	       receiver->input_samples >= receiver->equalizer.decision_delay &&
	       qam_adaptation_is_valid(receiver->adaptation) &&
	       v92_phase4_modulator_bits_per_symbol(receiver->constellation) != 0 &&
	       g711_law_is_valid(receiver->law) &&
	       (receiver->polarity == -1 || receiver->polarity == 1);
}

bool v92_ru_line_rx_init_after_e2u(struct v92_ru_line_rx *receiver,
				const struct v92_upstream_rx *handoff)
{
	struct v92_ru_line_rx initialized;
	if (receiver == NULL || !v92_upstream_rx_is_valid(handoff) ||
	    handoff->failed || handoff->training_tail_remaining != 0)
		return false;
	initialized = (struct v92_ru_line_rx){
		.equalizer = handoff->line.equalizer,
		.input_samples = handoff->line.input_samples,
		.adaptation = handoff->line.adaptation,
		.constellation = handoff->line.demodulator.constellation,
		.law = handoff->line.law,
		.polarity = handoff->line.polarity,
	};
	if (!v92_ru_rx_init(&initialized.signal, handoff->line.demodulator.lu, 0) ||
	    !v92_ru_line_rx_is_valid(&initialized))
		return false;
	*receiver = initialized;
	return true;
}

enum v92_ru_rx_result v92_ru_line_rx_sample(struct v92_ru_line_rx *receiver,
					  int16_t sample)
{
	struct v92_ru_line_rx next;
	struct qam_point input, output;
	bool ready;
	enum v92_ru_rx_result result;
	if (receiver == NULL)
		return V92_RU_RX_INVALID_ARGUMENT;
	if (!v92_ru_line_rx_is_valid(receiver))
		return V92_RU_RX_INVALID_STATE;
	if (receiver->signal.bar_symbols == V92_RU_RX_BAR_SYMBOLS)
		return V92_RU_RX_COMPLETE;
	if (receiver->input_samples == UINT64_MAX)
		return V92_RU_RX_INVALID_STATE;
	next = *receiver;
	input = (struct qam_point){ .in_phase = next.polarity * (double)sample };
	if (qam_equalizer_push(&next.equalizer, &input, &output, &ready) !=
	    QAM_EQUALIZER_OK || !ready)
		return V92_RU_RX_INVALID_STATE;
	/* Ru uses the retained FIR without adapting it. Discard this output's
	 * adaptation opportunity without constructing a gradient snapshot. */
	next.equalizer.can_adapt = false;
	result = v92_ru_rx_sample(&next.signal, output.in_phase);
	if (result == V92_RU_RX_INVALID_ARGUMENT || result == V92_RU_RX_INVALID_STATE)
		return result;
	next.input_samples++;
	*receiver = next;
	return result;
}
