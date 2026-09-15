// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_QC_DIGITAL_H
#define V92_QC_DIGITAL_H

#include "single_tone.h"
#include "v92_qc1.h"
#include "v92_qc_signal.h"

enum v92_qc_digital_stage {
	V92_QC_DIGITAL_INVALID,
	V92_QC_DIGITAL_LEAD_SILENCE,
	V92_QC_DIGITAL_QTS,
	V92_QC_DIGITAL_ANSPCM,
	V92_QC_DIGITAL_TRAIL_SILENCE,
	V92_QC_DIGITAL_PHASE2,
	V92_QC_DIGITAL_V8,
};

struct v92_qc_digital {
	struct single_tone_detector toneq;
	struct v92_anspcm anspcm;
	enum v92_qc_digital_stage stage;
	uint16_t position;
	uint16_t answer_deadline;
	uint8_t qts_index;
};

/* Start immediately after the QC1/QCA1 exchange: local must be digital,
 * peer analogue, and exactly one message an acknowledgement. The caller
 * owns history admission, FSK exchange and any overall call deadline.
 * No message or history storage is borrowed. Invalid init changes no state. */
bool v92_qc_digital_init(struct v92_qc_digital *exchange, enum g711_law law,
	const struct v92_qc1_message *local, const struct v92_qc1_message *peer);

/* Consume one 8 kHz input sample and emit one exact PCM symbol. Return the
 * stage after that sample. PHASE2 completes 75 ms of handoff silence; V8
 * requires the owner to restart ANSam/normal V.8, never shortened Phase 2.
 * Once terminal, repeated calls consume/emit nothing and retain that stage.
 * Invalid arguments consume/emit nothing. Output must not alias exchange. */
enum v92_qc_digital_stage v92_qc_digital_sample(
	struct v92_qc_digital *exchange, int16_t input,
	struct v90_pcm_symbol *output);

#endif
