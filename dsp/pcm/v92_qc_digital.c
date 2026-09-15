// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc_digital.h"
#include "memory_region.h"

#include <stddef.h>

enum {
	/* V.92 9.2.2.1/3 and 9.2.4.1/3, at the 8 kHz PCM clock. */
	QC_SILENCE_SAMPLES = 600,
	QC_ANSWER_TIMEOUT_SAMPLES = 16000,
};

bool v92_qc_digital_init(struct v92_qc_digital *exchange, enum g711_law law,
	const struct v92_qc1_message *local, const struct v92_qc1_message *peer)
{
	struct v92_qc_digital initialized = { 0 };
	const struct single_tone_detector_profile profile = {
		.minimum_line_power = 256.0 * 256.0,
		.minimum_carrier_share = 0.65,
		.maximum_carrier_share = 1.20,
		.sample_rate_hz = 8000,
		.frequency_hz = 980, /* TONEq, V.92 8.2.5. */
		.window_samples = 80,
		.acquire_samples = 400 - 80 + 1,
		.loss_samples = 160,
	};

	if (exchange == NULL || local == NULL || peer == NULL ||
	    local->sender != V90_DIGITAL_MODEM ||
	    peer->sender != V90_ANALOG_MODEM ||
	    local->acknowledgment == peer->acknowledgment ||
	    peer->signal.qts_index >= 15 ||
	    v92_qc1_bit_count(local) == 0 || v92_qc1_bit_count(peer) == 0 ||
	    !v92_anspcm_init(&initialized.anspcm, law,
		local->signal.anspcm_level) ||
	    single_tone_detector_init_profile(&initialized.toneq, &profile) !=
		SINGLE_TONE_OK)
		return false;
	initialized.stage = V92_QC_DIGITAL_LEAD_SILENCE;
	initialized.qts_index = peer->signal.qts_index;
	/* Only the answerer has the 9.2.4.3 two-second fallback deadline;
	 * it includes the leading silence and QTS, not just ANSpcm. */
	initialized.answer_deadline = local->acknowledgment ?
		QC_ANSWER_TIMEOUT_SAMPLES : 0;
	*exchange = initialized;
	return true;
}

enum v92_qc_digital_stage v92_qc_digital_sample(
	struct v92_qc_digital *exchange, int16_t input,
	struct v90_pcm_symbol *output)
{
	struct single_tone_event event;

	if (exchange == NULL || output == NULL ||
	    memory_regions_overlap(exchange, sizeof(*exchange),
		output, sizeof(*output)) ||
	    exchange->stage < V92_QC_DIGITAL_LEAD_SILENCE ||
	    exchange->stage > V92_QC_DIGITAL_V8)
		return V92_QC_DIGITAL_INVALID;
	if (exchange->stage == V92_QC_DIGITAL_PHASE2 ||
	    exchange->stage == V92_QC_DIGITAL_V8)
		return exchange->stage;
	switch (exchange->stage) {
	case V92_QC_DIGITAL_LEAD_SILENCE:
	case V92_QC_DIGITAL_TRAIL_SILENCE:
		if (!v90_pcm_symbol_make(output, exchange->anspcm.law, 0, true))
			return V92_QC_DIGITAL_INVALID;
		if (++exchange->position == QC_SILENCE_SAMPLES) {
			exchange->position = 0;
			exchange->stage = exchange->stage == V92_QC_DIGITAL_LEAD_SILENCE ?
				V92_QC_DIGITAL_QTS : V92_QC_DIGITAL_PHASE2;
		}
		break;
	case V92_QC_DIGITAL_QTS:
		if (!v92_qts_symbol(exchange->anspcm.law, exchange->qts_index,
			exchange->position, output))
			return V92_QC_DIGITAL_INVALID;
		if (++exchange->position == V92_QTS_TOTAL_SYMBOLS) {
			exchange->position = 0;
			exchange->stage = V92_QC_DIGITAL_ANSPCM;
		}
		break;
	case V92_QC_DIGITAL_ANSPCM:
		if (single_tone_detector_sample(&exchange->toneq, input, &event) !=
			SINGLE_TONE_OK || !v92_anspcm_sample(&exchange->anspcm, output))
			return V92_QC_DIGITAL_INVALID;
		if (event.kind == SINGLE_TONE_STARTED)
			exchange->stage = V92_QC_DIGITAL_TRAIL_SILENCE;
		break;
	default:
		return V92_QC_DIGITAL_INVALID;
	}
	if (exchange->answer_deadline != 0 && --exchange->answer_deadline == 0 &&
	    exchange->stage == V92_QC_DIGITAL_ANSPCM)
		exchange->stage = V92_QC_DIGITAL_V8;
	return exchange->stage;
}
