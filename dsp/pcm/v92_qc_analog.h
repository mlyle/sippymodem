// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_QC_ANALOG_H
#define V92_QC_ANALOG_H

#include "pcm_s_pattern_rx.h"
#include "single_tone.h"
#include "v92_qc1.h"

enum v92_qc_analog_stage {
	V92_QC_ANALOG_INVALID,
	V92_QC_ANALOG_QTS,
	V92_QC_ANALOG_ANSPCM,
	V92_QC_ANALOG_TONEQ,
	V92_QC_ANALOG_TRAIL_SILENCE,
	V92_QC_ANALOG_PHASE2,
	V92_QC_ANALOG_V8,
};

struct v92_qc_analog {
	struct pcm_s_pattern_rx qts;
	struct single_tone_detector anspcm;
	struct single_tone_transmitter toneq;
	enum v92_qc_analog_stage stage;
	uint16_t position;
	uint16_t answer_deadline;
};

/* Start after QC1/QCA1: local analogue, peer digital, exactly one ACK.
 * QC1 has already qualified/transmitted ANSam, permitting TONEq as soon as
 * ANSpcm qualifies (V.92 9.2.1.3/9.2.3.3). The owner admits retained history,
 * conditions receive samples and owns the overall call timeout. This exchange
 * neither stores history nor decides whether Phase 2 may be shortened. */
bool v92_qc_analog_init(struct v92_qc_analog *exchange, enum g711_law law,
	const struct v92_qc1_message *local, const struct v92_qc1_message *peer,
	int16_t toneq_amplitude);

/* One conditioned 8 kHz input sample and one linear transmit sample.
 * PHASE2 includes all 75 ms of post-TONEq silence. V8 requests normal V.8,
 * not a shortened success. Terminal calls and invalid arguments change no
 * output or state; output must not overlap exchange. State copies have no
 * borrowed storage. */
enum v92_qc_analog_stage v92_qc_analog_sample(
	struct v92_qc_analog *exchange, int16_t input, int16_t *output);

#endif
