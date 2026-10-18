// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_RU_LINE_RX_H
#define V92_RU_LINE_RX_H

#include "v92_ru_rx.h"
#include "v92_upstream_rx.h"

/* Keep the trained control-channel FIR and its output correction running
 * alongside PCM data reception.
 * Data's precoder/prefilter compensation and decision-directed adaptation do
 * not describe the raw Ru waveform. Do not adapt this retained FIR to data.
 * Its own decision delay, polarity and frame phase remain authoritative. */
struct v92_ru_line_rx {
	struct qam_equalizer equalizer;
	struct v92_ru_rx signal;
	uint64_t input_samples;
	double adaptation;
	double output_bias;
	enum v92_constellation_size constellation;
	enum g711_law law;
	int8_t polarity;
};

/* The completed E2u handoff's next delayed output is the first B1u symbol,
 * including when E2u was extended. The caller owns that validated boundary. */
bool v92_ru_line_rx_init_after_e2u(struct v92_ru_line_rx *receiver,
				const struct v92_upstream_rx *handoff);
bool v92_ru_line_rx_is_valid(const struct v92_ru_line_rx *receiver);

/* Consume a signed 8 kHz sample. COMPLETE consumes the final delayed Ru-bar
 * symbol; subsequent calls consume nothing. The warm FIR then owns the first
 * decision_delay TRN2u inputs. Invalid calls leave the receiver unchanged. */
enum v92_ru_rx_result v92_ru_line_rx_sample(struct v92_ru_line_rx *receiver,
					  int16_t sample);

#endif /* V92_RU_LINE_RX_H */
