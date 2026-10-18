// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_DATA_RX_H
#define V92_UPSTREAM_DATA_RX_H

#include "v92_pcm_frame.h"
#include "v92_upstream_rx.h"
#include "v92_upstream_plan.h"
#include "v92_upstream_model.h"
#include "pcm_gain.h"
#include "v92_residual.h"

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
	struct v92_residual residual;
	/* Known causal subtraction for each original raw observation. */
	int32_t correction[V92_PCM_MAPPING_FRAME_SYMBOLS];
	/* Raw-coordinate calibration from known B1u only. Gain bounds come from
	 * the training LU cell; offset is in polarity-normalized input units.
	 * Sums count acquisition decisions only. The nominal plan stays intact. */
	struct {
		double gain, minimum, maximum;
		double correlation, energy;
		double raw_sum, target_sum, offset;
		uint16_t samples;
	} calibration;
	/* Actual wire/sample pairs identify only post-codec host conditioning,
	 * not analogue channel or mapping gain. Freeze before B1u validation;
	 * zero gain means unidentified and preserves raw quality-score units. */
	struct {
		struct pcm_gain fit;
		/* Retain the rounding interval too: the estimate alone cannot
		 * certify a later, previously unseen wire level. */
		double gain, lower, upper;
	} host;
	int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t wire[V92_PCM_MAPPING_FRAME_SYMBOLS];
	/* Gain-only physical samples, before echo subtraction or polarity. */
	int16_t host_samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	bool wire_known[V92_PCM_MAPPING_FRAME_SYMBOLS];
	bool received[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint64_t input_samples;
	uint64_t received_frames;
	double adaptation;
	/* Frozen correction in equalized units; raw-coordinate plans use zero. */
	double output_bias;
	uint16_t delay_remaining;
	uint8_t symbol_count;
	int8_t polarity;
};

struct v92_upstream_data_rx {
	struct v92_pcm_frame_rx decoder;
	struct v92_upstream_data_line line;
	struct v92_upstream_model model;
	bool failed;
};

struct v92_upstream_data_frame {
	/* Qualified current-frame request, separate from delayed payload below. */
	enum v92_pcm_frame_rm rm;
	/* Residual of the CURRENT frame in signed PCM sample units: equalized
	 * filter output, or raw line input for a channel-compensated mapping.
	 * Raw-line residual compares against fitted codec reconstruction, or
	 * the linear model when no codec fit is available.
	 * Includes excursions clipped only at the frame decoder's input. */
	double squared_error;
	/* Current-frame low-margin sequence disagreement, not a payload erasure. */
	bool uncertain;
	/* Payload is delayed by the frame decoder's lookahead. A zero count
	 * means buffering or a non-payload slot; b1u and ambiguous apply only
	 * to these bits. RM signaling never becomes a fabricated payload word. */
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t bit_count;
	bool b1u;
	bool ambiguous;
};

/* Start at the first B1u decision after E2u, preserving the trained equalizer,
 * its pending physical inputs, output correction, polarity, and LU. Reset only data coding.
 * Bound the inherited adaptation step for decision-directed data tracking;
 * a smaller step, including disabled adaptation, remains unchanged. Unknown
 * decisions exceeding the receive error ceiling or farther from the
 * equalized observation than silence do not adapt the FIR, but retain their
 * full residual and decoding evidence. Known B1u frames cumulatively refine
 * LU within the original G.711 quantization
 * cell, allowing later training to correct an early biased estimate. The
 * fit scratch is discarded at B1u completion; payload never changes LU.
 * A constellation-coordinate plan instead retains polarity and the measured
 * gain, skips only the delay not already consumed by E2u detection, and
 * decodes raw line samples without another FIR or G.711 quantization. The
 * gain is refined within training LU uncertainty during initial B1u
 * acquisition, then remains fixed through B1u validation and data/control.
 * A general CPd channel with complete acquisition media can also acquire a
 * bounded causal residual inverse. It selects up to two small delayed terms
 * from the last acquisition frames, then freezes before independent B1u.
 * Original raw samples, wire cells and quality units are retained; missing
 * media after fitting takes ordinary no-path recovery.
 * A linear channel path with complete wire/host evidence also fits a bounded
 * constant offset at the end of acquisition. Subtract it in decoding, wire
 * cells and residuals; validation and payload never update the fit.
 * When actual wire codes are supplied, the same acquisition interval fits
 * post-codec host gain for call-level quality scoring, independently of the
 * mapping's calibration gain. Missing or inconsistent pairs close this fit.
 * It may also fit a two-codec observation model for a positive one-tap
 * prefilter. If those models decline, a scalar single-codec fit can predict
 * quality without changing the wire-cell decoder. All fits freeze before
 * held-out B1u. Missing acquisition media or unsupported filters retain the
 * linear receiver without a fitted codec prediction.
 * A zero-initialized plan selects the equalized filter-output path above.
 * The mapping and plan must belong to the complete CPd sent to the peer.
 * Handoff, mapping and plan must be disjoint from the receiver. Rejection
 * leaves the receiver unchanged; success clears all prior data state. */
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

/* Start the accepted fast-exchange B1u mapping at its first decision boundary,
 * AFTER all 48 old-mapping FB1u frames have entered the decoder. E2u payload
 * itself arrives ten frames late: the caller must account for that lookahead
 * when scheduling this boundary. Pending old FB1u bits are discarded, not
 * emitted as host data. Reset coding and mapping-dependent codec acquisition;
 * retain physical input history, equalizer, polarity, and the LU estimate.
 * The receive plan may change gain, but must keep the same observation domain
 * and latency. A different physical alignment needs a training handoff, not
 * this in-place switch. The new CPd must not request extended E2u (Table 30).
 * Partial-frame, pre-B1u, failed, aliased or incompatible calls change nothing. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_start_b1u(
	struct v92_upstream_data_rx *receiver,
	const struct v92_pcm_mapping *mapping,
	const struct v92_upstream_plan *plan);

/* Consume one signed 8 kHz sample. FRAME reports each twelve-symbol decision;
 * OK buffers a sample and leaves frame unchanged. Quality is reported even
 * for damaged B1u; known bits do not imply a usable line. Rejected calls
 * consume nothing and leave outputs unchanged. A new DSP failure latches
 * failed without advancing either line or coding state. Sample-only input
 * closes codec-model acquisition and is rejected once a model is active:
 * host-scaled samples must not be quantized again to invent wire codes. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame);

/* Preserve the actual wire code alongside its host-conditioned signed sample.
 * available=false denotes concealed media, not a received zero. Such input
 * cannot train a model and carries no likelihood evidence once fitted; its
 * raw residual is still reported and call-level loss recovery still applies. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_octet(
	struct v92_upstream_data_rx *receiver, uint8_t code, int16_t sample,
	bool available, struct v92_upstream_data_frame *frame);

/* Internal call-owner path for an exclusively owned receiver already
 * validated for the current media block. Validate again before releasing
 * ownership. Skip only the recursive state scan: argument/alias checks,
 * counter exhaustion, transactional DSP failure and failure latching retain
 * the checked entry point's contract. Never use this for unvalidated input. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_sample_owned(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame);
/* host_sample is the gain-only physical sample before independent echo
 * subtraction; sample is its cleaned counterpart. Supply identical samples
 * without cancellation. Clipped or gain-inconsistent evidence cannot support
 * an affine codec model; concealed samples never train the gain or model. */
enum v92_upstream_data_rx_result v92_upstream_data_rx_octet_owned(
	struct v92_upstream_data_rx *receiver, uint8_t code, int16_t sample,
	int16_t host_sample, bool available, struct v92_upstream_data_frame *frame);

const char *v92_upstream_data_rx_result_name(
	enum v92_upstream_data_rx_result result);

#endif /* V92_UPSTREAM_DATA_RX_H */
