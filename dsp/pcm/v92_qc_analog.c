// SPDX-License-Identifier: BSD-3-Clause

#include "v92_qc_analog.h"
#include "memory_region.h"
#include "v92_qc_signal.h"

#include <stddef.h>

enum {
	QC_TONEQ_MIN_SAMPLES = 400,
	QC_SILENCE_SAMPLES = 600,
	QC_ANSWER_TIMEOUT_SAMPLES = 16000,
};

bool v92_qc_analog_init(struct v92_qc_analog *exchange, enum g711_law law,
	const struct v92_qc1_message *local, const struct v92_qc1_message *peer,
	int16_t toneq_amplitude)
{
	struct v92_qc_analog initialized = { 0 };
	struct v90_pcm_symbol qts;
	const struct single_tone_detector_profile profile = {
		.minimum_line_power = 256.0 * 256.0,
		.minimum_carrier_share = 0.65,
		.maximum_carrier_share = 1.20,
		.sample_rate_hz = 8000,
		/* ANSpcm's fundamental is 8000 * 79/301 Hz. The 10 ms
		 * detector window admits that offset and its phase reversals. */
		.frequency_hz = 2100,
		.window_samples = 80,
		.acquire_samples = 160,
		.loss_samples = 160,
	};

	if (exchange == NULL || local == NULL || peer == NULL ||
	    local->sender != V90_ANALOG_MODEM || peer->sender != V90_DIGITAL_MODEM ||
	    local->acknowledgment == peer->acknowledgment ||
	    v92_qc1_bit_count(local) == 0 || v92_qc1_bit_count(peer) == 0 ||
	    !v92_qts_symbol(law, local->signal.qts_index, 0, &qts) ||
	    pcm_s_pattern_rx_init(&initialized.qts, law, qts.ucode) != PCM_S_PATTERN_RX_OK ||
	    single_tone_detector_init_profile(&initialized.anspcm, &profile) != SINGLE_TONE_OK ||
	    single_tone_transmitter_init(&initialized.toneq, 980, toneq_amplitude) != SINGLE_TONE_OK)
		return false;
	initialized.stage = V92_QC_ANALOG_QTS;
	initialized.answer_deadline = local->acknowledgment ? QC_ANSWER_TIMEOUT_SAMPLES : 0;
	*exchange = initialized;
	return true;
}

enum v92_qc_analog_stage v92_qc_analog_sample(
	struct v92_qc_analog *exchange, int16_t input, int16_t *output)
{
	struct single_tone_event event;
	int16_t generated = 0;

	if (exchange == NULL || output == NULL ||
	    memory_regions_overlap(exchange, sizeof(*exchange), output, sizeof(*output)) ||
	    exchange->stage < V92_QC_ANALOG_QTS || exchange->stage > V92_QC_ANALOG_V8)
		return V92_QC_ANALOG_INVALID;
	if (exchange->stage == V92_QC_ANALOG_PHASE2 || exchange->stage == V92_QC_ANALOG_V8)
		return exchange->stage;
	if (exchange->stage == V92_QC_ANALOG_QTS) {
		uint8_t octet = exchange->qts.law == G711_LAW_PCMU ?
			g711_encode_pcmu(input) : g711_encode_pcma(input);
		enum pcm_s_pattern_rx_result result = pcm_s_pattern_rx_receive(&exchange->qts, octet);

		if (result == PCM_S_PATTERN_RX_SEQUENCE_COMPLETE)
			exchange->stage = V92_QC_ANALOG_ANSPCM;
		else if (result == PCM_S_PATTERN_RX_INVALID_SEQUENCE)
			exchange->stage = V92_QC_ANALOG_V8;
		else if (result != PCM_S_PATTERN_RX_OK && result != PCM_S_PATTERN_RX_ACQUIRED &&
			 result != PCM_S_PATTERN_RX_REVERSAL_DETECTED)
			return V92_QC_ANALOG_INVALID;
	} else if (exchange->stage == V92_QC_ANALOG_TRAIL_SILENCE) {
		if (++exchange->position == QC_SILENCE_SAMPLES)
			exchange->stage = V92_QC_ANALOG_PHASE2;
	} else {
		if (single_tone_detector_sample(&exchange->anspcm, input, &event) != SINGLE_TONE_OK)
			return V92_QC_ANALOG_INVALID;
		if (exchange->stage == V92_QC_ANALOG_ANSPCM) {
			if (event.kind == SINGLE_TONE_STARTED)
				exchange->stage = V92_QC_ANALOG_TONEQ;
		} else {
			/* A fast ANSpcm drop must not truncate the mandatory 50 ms
			 * TONEq. Conversely, elapsed time alone is not peer agreement. */
			if (exchange->position == QC_TONEQ_MIN_SAMPLES && !exchange->anspcm.tone_present) {
				exchange->stage = V92_QC_ANALOG_TRAIL_SILENCE;
				exchange->position = 1; /* This output is already silence. */
			} else {
				if (single_tone_transmitter_sample(&exchange->toneq, &generated) != SINGLE_TONE_OK)
					return V92_QC_ANALOG_INVALID;
				if (exchange->position < QC_TONEQ_MIN_SAMPLES)
					exchange->position++;
			}
		}
	}
	/* V.92 9.2.3.3 bounds the answerer's wait for ANSpcm, including
	 * QTS. Once it qualifies, only the owner's overall deadline applies. */
	if (exchange->answer_deadline && --exchange->answer_deadline == 0 &&
	    (exchange->stage == V92_QC_ANALOG_QTS || exchange->stage == V92_QC_ANALOG_ANSPCM))
		exchange->stage = V92_QC_ANALOG_V8;
	*output = generated;
	return exchange->stage;
}
