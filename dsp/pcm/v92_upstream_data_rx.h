// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_DATA_RX_H
#define V92_UPSTREAM_DATA_RX_H

#include "v92_pcm_frame.h"
#include "v92_upstream_rx.h"
#include "v92_upstream_plan.h"

enum v92_upstream_data_rx_result {
	V92_UPSTREAM_DATA_RX_OK,
	V92_UPSTREAM_DATA_RX_FRAME,
	V92_UPSTREAM_DATA_RX_FAILED,
	V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT,
	V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS,
	V92_UPSTREAM_DATA_RX_INVALID_STATE,
	V92_UPSTREAM_DATA_RX_NO_PATH,
	V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE,
};

/* Keep per-sample line state separate from the larger frame decoder so a
 * buffered sample need not copy all competing coding histories. */
struct v92_upstream_data_line {
	struct qam_equalizer equalizer;
	union {
		struct qam_equalizer_observation observations[V92_PCM_MAPPING_FRAME_SYMBOLS];
		int32_t raw[V92_PCM_MAPPING_FRAME_SYMBOLS];
	};
	struct v92_upstream_plan plan;
	/* Raw-coordinate calibration from known B1u only. Bounds come from
	 * the training LU cell; the CPd's nominal receive plan stays intact. */
	struct {
		double gain, minimum, maximum;
		double correlation, energy;
	} calibration;
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint64_t input_samples;
	uint64_t received_frames;
	double adaptation;
	uint16_t delay_remaining;
	uint8_t symbol_count;
	int8_t polarity;
};

struct v92_upstream_data_rx {
	struct v92_pcm_frame_rx decoder;
	struct v92_upstream_data_line line;
	bool failed;
};

struct v92_upstream_data_frame {
	/* Residual of the CURRENT frame in signed PCM sample units: equalized
	 * filter output, or raw line input for a channel-compensated mapping.
	 * Raw-line residual includes companding noise against the linear model.
	 * Includes excursions clipped only at the frame decoder's input. */
	double squared_error;
	/* Payload is delayed by the frame decoder's lookahead. A zero count
	 * means no payload is ready; b1u and ambiguous apply only to these bits. */
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t bit_count;
	bool b1u;
	bool ambiguous;
};

/* Start at the first B1u decision after E2u, preserving the trained equalizer,
 * its pending physical inputs, polarity, and LU. Reset only data coding.
 * Bound the inherited adaptation step for decision-directed data tracking;
 * a smaller step, including disabled adaptation, remains unchanged. Known
 * B1u frames refine LU and its surviving interval only within the original
 * G.711 quantization cell; ordinary payload never changes that estimate.
 * A constellation-coordinate plan instead retains polarity and the measured
 * gain, skips only the delay not already consumed by E2u detection, and
 * decodes raw line samples without another FIR or G.711 quantization. The
 * gain is refined within training LU uncertainty during initial B1u
 * acquisition, then remains fixed through B1u validation and data/control.
 * A zero-initialized plan selects the equalized filter-output path above.
 * The mapping and plan must belong to the complete CPd sent to the peer. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_init(
	struct v92_upstream_data_rx *receiver,
	const struct v92_upstream_rx *handoff,
	const struct v92_pcm_mapping *mapping,
	const struct v92_upstream_plan *plan);
bool v92_upstream_data_rx_is_valid(
	const struct v92_upstream_data_rx *receiver);
/* This marks the received B1u boundary, not a quality-based online decision. */
bool v92_upstream_data_rx_b1u_complete(
	const struct v92_upstream_data_rx *receiver);

/* Reset control coding at an aligned decoded frame boundary after B1u,
 * not at the earlier physical-input boundary. Retain the line, counters,
 * trellis, filters, and delayed payload. After a successful reset, the next
 * V92_PCM_RX_DELAY_FRAMES delivered frames still precede this boundary.
 * The caller owns request detection and routing by the delayed boundary.
 * Failed, partial-frame, and pre-B1u calls change nothing. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_reset_control(
	struct v92_upstream_data_rx *receiver);

/* Consume one signed 8 kHz sample. FRAME reports each twelve-symbol decision;
 * OK buffers a sample and leaves frame unchanged. Quality is reported even
 * for damaged B1u; known bits do not imply a usable line. Rejected calls
 * consume nothing and leave outputs unchanged. A new DSP failure latches
 * failed without advancing either line or coding state. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame);

/* Internal call-owner path for an exclusively owned receiver already
 * validated for the current media block. Validate again before releasing
 * ownership. Skip only the recursive state scan: argument/alias checks,
 * counter exhaustion, transactional DSP failure and failure latching retain
 * the checked entry point's contract. Never use this for unvalidated input. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_sample_owned(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame);

const char *v92_upstream_data_rx_result_name(
	enum v92_upstream_data_rx_result result);

#endif /* V92_UPSTREAM_DATA_RX_H */
