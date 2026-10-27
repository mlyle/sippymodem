// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>
#include <memory_region.h>
#include <v92_cpd_direct.h>
#include <v92_cpd_channel.h>
#include <v92_data_quality.h>
#include <v92_e2u_rx.h>
#include <v92_phase4_digital_station.h>
#include <v92_phase4_record_tx.h>
#include <v92_ru_line_rx.h>
#include <v92_upstream_control_line_rx.h>
#include <v92_upstream_control.h>
#include <v92_upstream_data_rx.h>
#include <v92_upstream_data_tx.h>

#include "test.h"
#include "integer_math.h"
#include "packed_bits.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Keep numerical waveform and rejection oracles independent of the media
 * scheduler. They supply no next physical sample until the current fit ends.
 * Deferred-acquisition cases below call the actual streaming entry points. */
static void fixture_complete_fit(struct v92_upstream_data_rx *receiver,
	enum v92_upstream_data_rx_result result)
{
	if (result != V92_UPSTREAM_DATA_RX_FRAME) return;
	unsigned steps = 0;
	while (receiver->model.fitting) {
		assert(++steps < 2048);
		v92_upstream_model_progress(receiver);
	}
}

static enum v92_upstream_data_rx_result fixture_sample_owned(
	struct v92_upstream_data_rx *receiver, int16_t sample, struct v92_upstream_data_frame *frame)
{
	enum v92_upstream_data_rx_result result = v92_upstream_data_rx_sample_owned(receiver, sample, frame);
	fixture_complete_fit(receiver, result);
	return result;
}

static enum v92_upstream_data_rx_result fixture_octet_owned(
	struct v92_upstream_data_rx *receiver, uint8_t code, int16_t sample, int16_t host, bool available, struct v92_upstream_data_frame *frame)
{
	enum v92_upstream_data_rx_result result = v92_upstream_data_rx_octet_owned(receiver, code, sample, host, available, frame);
	fixture_complete_fit(receiver, result);
	return result;
}

static enum v92_upstream_data_rx_result fixture_sample(
	struct v92_upstream_data_rx *receiver, int16_t sample, struct v92_upstream_data_frame *frame)
{
	enum v92_upstream_data_rx_result result = v92_upstream_data_rx_sample(receiver, sample, frame);
	fixture_complete_fit(receiver, result);
	return result;
}

static enum v92_upstream_data_rx_result fixture_octet(
	struct v92_upstream_data_rx *receiver, uint8_t code, int16_t sample, bool available, struct v92_upstream_data_frame *frame)
{
	enum v92_upstream_data_rx_result result = v92_upstream_data_rx_octet(receiver, code, sample, available, frame);
	fixture_complete_fit(receiver, result);
	return result;
}

enum { TEST_LU = 4096, DATA_FRAMES = 80 };

static const struct v92_upstream_plan direct_plan;

struct channel {
	double gain;
	double postcursor;
	/* Zero keeps the original codec output; nonzero scales after G.711. */
	double receive_gain;
	int16_t receive_offset;
	int16_t offset;
	int16_t previous;
	double residual_first, residual_second;
	int16_t history[6];
	unsigned cursor;
	enum g711_law law;
	int8_t polarity;
	uint8_t code;
};

static int16_t wire_sample(struct channel *channel, int16_t sample)
{
	int16_t mixed = (int16_t)lround(channel->gain * sample +
		channel->postcursor * channel->previous + channel->offset +
		channel->residual_first * channel->history[(channel->cursor + 2) % 6] +
		channel->residual_second * channel->history[channel->cursor]);

	channel->history[channel->cursor] = sample;
	channel->cursor = (channel->cursor + 1) % 6;
	channel->previous = sample;
	int16_t decoded = channel->law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(mixed)) :
		g711_decode_pcma(g711_encode_pcma(mixed));
	channel->code = channel->law == G711_LAW_PCMU ?
		g711_encode_pcmu(channel->polarity * decoded) :
		g711_encode_pcma(channel->polarity * decoded);
	return channel->polarity * (channel->receive_gain != 0.0 ?
		(int16_t)lround(decoded * channel->receive_gain) : decoded) +
		channel->polarity * channel->receive_offset;
}

static void payload(uint8_t *bits, size_t count, unsigned int frame)
{
	uint32_t random = UINT32_C(0x39458672) ^ frame;

	for (size_t bit = 0u; bit < count; ++bit) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[bit] = (uint8_t)(random >> 31);
	}
}

static bool source(void *context, uint8_t *bits, size_t count)
{
	unsigned int *frame = context;

	payload(bits, count, (*frame)++);
	return true;
}

static bool receive_compared(
	struct v92_upstream_data_rx *receiver, int16_t sample,
	struct v92_upstream_data_frame *frame,
	enum v92_upstream_data_rx_result *result)
{
	struct v92_upstream_data_rx owned = *receiver;
	struct v92_upstream_data_frame output;

	memcpy(&output, frame, sizeof(output));
	CHECK(v92_upstream_data_rx_is_valid(receiver), "owned fixture is not validated");
	*result = fixture_sample(receiver, sample, frame);
	CHECK(fixture_sample_owned(&owned, sample, &output) == *result &&
	      memcmp(receiver, &owned, sizeof(owned)) == 0 &&
	      memcmp(frame, &output, sizeof(output)) == 0 &&
	      v92_upstream_data_rx_is_valid(&owned),
	      "owned/checked receive changed result, output, state, or invariants");
	return true;
}

/* Preflight must protect BOTH decoders before either one's coding changes.
 * Inactive alternate storage is not a decoder and must remain untouched. */
static bool check_control_reset(const struct v92_upstream_data_rx *receiver)
{
	bool paired = v92_upstream_model_has_alternate(receiver);
	struct v92_upstream_data_rx actual, expected;

	for (unsigned branch = 0u; branch < (paired ? 2u : 1u); ++branch)
		for (unsigned fault = 0u; fault < 5u; ++fault) {
			actual = *receiver;
			struct v92_pcm_frame_rx *decoder = branch ?
				&actual.model.alternate : &actual.decoder;

			if (fault == 0u) decoder->path_count = 0u;
			if (fault == 1u) decoder->paths[0].differential = 2u;
			if (fault == 2u) decoder->paths[0].pending[0][0] = 2u;
			if (fault == 3u) actual.failed = true;
			if (fault == 4u) {
				decoder->paths[0].differential = 2u;
				actual.failed = true;
			}
			expected = actual;
			CHECK(v92_upstream_data_rx_reset_control(&actual) ==
				(fault == 3u ? V92_UPSTREAM_DATA_RX_FAILED : V92_UPSTREAM_DATA_RX_INVALID_STATE) &&
			      !memcmp(&actual, &expected, sizeof(actual)),
			      "control reset mutated state after failed preflight: branch=%u fault=%u", branch, fault);
		}
	actual = *receiver;
	if (!paired)
		memset(&actual.model.alternate, 0xa5, sizeof(actual.model.alternate));
	for (unsigned branch = 0u; branch < (paired ? 2u : 1u); ++branch) {
		struct v92_pcm_frame_rx *decoder = branch ? &actual.model.alternate : &actual.decoder;

		for (size_t path = 0u; path < decoder->path_count; ++path) {
			decoder->paths[path].descrambler.history = 7u;
			decoder->paths[path].differential = 1u;
			decoder->paths[path].rm_frames = V92_PCM_RM_QUALIFY_FRAMES;
			decoder->paths[path].rm_prime_frames = 2u;
		}
	}
	CHECK(v92_upstream_data_rx_is_valid(&actual), "reset fixture is not valid");
	expected = actual;
	for (unsigned branch = 0u; branch < (paired ? 2u : 1u); ++branch) {
		struct v92_pcm_frame_rx *decoder = branch ? &expected.model.alternate : &expected.decoder;

		for (size_t path = 0u; path < decoder->path_count; ++path) {
			decoder->paths[path].descrambler.history = 0u;
			decoder->paths[path].differential = 0u;
			decoder->paths[path].rm_frames = 0u;
			decoder->paths[path].rm_prime_frames = 0u;
		}
	}
	CHECK(v92_upstream_data_rx_reset_control(&actual) == V92_UPSTREAM_DATA_RX_OK &&
	      !memcmp(&actual, &expected, sizeof(actual)) && v92_upstream_data_rx_is_valid(&actual) &&
	      v92_upstream_data_rx_reset_control(&actual) == V92_UPSTREAM_DATA_RX_OK &&
	      !memcmp(&actual, &expected, sizeof(actual)),
	      "control reset changed retained state or was not idempotent");
	return true;
}

/* A trained FIR with the real causal delay, not a zero-delay stand-in. Feed
 * E2u's first delayed samples before exporting its receive-side boundary. */
static bool prepare_e2u_at_level(
	struct channel *channel, bool extended,
	struct v92_phase4_record_tx *transmitter, struct v92_e2u_rx *receiver,
	int16_t lu, int16_t measured_lu, double adaptation,
	enum v92_constellation_size constellation)
{
	struct v92_phase4_modulator modulator;
	struct v92_pcm_phase4_trn2u_line_handoff handoff = {
		.input_samples = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
		.training_symbols = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
		.adaptation = adaptation,
		.output_bias = -channel->receive_offset,
		.law = channel->law,
		.polarity = channel->polarity,
	};
	struct v92_upstream_rx signal;
	struct qam_point reference = { .in_phase = measured_lu };
	struct qam_point output;
	int16_t level;
	bool ready;

	CHECK(v92_phase4_modulator_init(&modulator, constellation,
			0u, lu) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_demodulator_init(&handoff.demodulator,
			constellation, 0u, measured_lu) == V92_PHASE4_MODULATOR_OK &&
	      v92_phase4_record_tx_init_e2u(transmitter, &modulator, extended) ==
		      V92_PHASE4_RECORD_TX_OK &&
	      qam_equalizer_init_geometry(&handoff.equalizer, &reference, &reference,
			V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS,
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY) == QAM_EQUALIZER_OK,
	      "E2u fixture initialization failed");
	/* Finite inverse of the known two-tap channel; B1u continues adaptation. */
	double coefficient = channel->receive_gain != 0.0 ? 1.0 / channel->receive_gain : 1.0;
	for (size_t tap = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     tap < handoff.equalizer.tap_count; ++tap) {
		handoff.equalizer.coefficients[tap].in_phase = coefficient;
		coefficient *= -channel->postcursor / channel->gain;
	}
	for (size_t index = 0u; index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		int16_t sample = lu;
		struct qam_point input;

		if (index != 0u)
			CHECK(v92_phase4_record_tx_next(transmitter, &sample) ==
				      V92_PHASE4_RECORD_TX_OK, "E2u lookahead failed");
		input = (struct qam_point){
			.in_phase = channel->polarity * wire_sample(channel, sample),
		};
		CHECK(qam_equalizer_push(&handoff.equalizer, &input, &output, &ready) ==
			      QAM_EQUALIZER_OK, "FIR fill failed");
	}
	CHECK(ready && qam_equalizer_adapt(&handoff.equalizer, &output, 0.0) ==
		      QAM_EQUALIZER_OK &&
	      v92_phase4_modulator_level(constellation, measured_lu, 0u, 0u,
			&level) == V92_PHASE4_MODULATOR_OK, "FIR did not become warm");
	level = channel->law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(level)) :
		g711_decode_pcma(g711_encode_pcma(level));
	for (size_t index = 0u; index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index)
		handoff.pending_training_targets[index] = level;
	CHECK(v92_upstream_rx_init(&signal, &handoff) == V92_UPSTREAM_RX_OK,
	      "line initialization failed");
	signal.training_tail_remaining = 0u;
	CHECK(v92_e2u_rx_init(receiver, &signal, extended) == V92_E2U_RX_OK,
	      "E2u receiver initialization failed");
	return true;
}

static bool prepare_e2u(
	struct channel *channel, bool extended,
	struct v92_phase4_record_tx *transmitter, struct v92_e2u_rx *receiver)
{
	return prepare_e2u_at_level(channel, extended, transmitter, receiver,
		TEST_LU, (int16_t)lround(TEST_LU * channel->gain), 0.002, V92_CONSTELLATION_4_POINT);
}

static bool check_calibration_interval(
	const struct v92_upstream_data_rx *receiver, int16_t lu)
{
	const struct v92_upstream_data_line *line = &receiver->line;
	struct g711_interval cell;
	double gain = line->plan.gain;
	double minimum = line->calibration.minimum;
	double maximum = line->calibration.maximum;

	CHECK(g711_quantization_interval(receiver->decoder.mapping.law, lu, &cell),
	      "calibration LU has no codec cell");
	CHECK(minimum >= 1.0 / 32768.0 && minimum <= gain &&
	      maximum >= gain && maximum <= 32768.0,
	      "calibration excludes nominal gain: LU %d gain %a bounds %a/%a",
	      lu, gain, minimum, maximum);
	/* Evaluate the rational bounds in wider precision. Check their values
	 * independently of the constructor's ordering of double operations. */
	long double lower = fmaxl(1.0L / 32768.0L,
		(long double)gain * lu / cell.upper);
	long double upper = fminl(32768.0L,
		(long double)gain * lu / fmaxl(1.0L, cell.lower));

	CHECK(fabsl(minimum - lower) <= 2 * DBL_EPSILON * lower &&
	      fabsl(maximum - upper) <= 2 * DBL_EPSILON * upper,
	      "calibration changed the codec-cell interval");
	CHECK((lu != cell.upper || minimum == gain) &&
	      (lu != fmax(1.0, cell.lower) || maximum == gain),
	      "calibration rounded away an exact nominal cell edge");
	return true;
}

static bool test_data_initialization(void)
{
	/* Include lower/upper codec-cell edges and the measured gain that
	 * exposed a one-ULP nominal-gain exclusion during an actual retrain. */
	const int16_t levels[] = { 2043, 2, 3, 1916, 2047, 2048, 4095, TEST_LU, 16000 };
	const double gains[] = { 0x1.872c17a5244p-1, 1.0 / 32768.0,
		nextafter(1.0 / 32768.0, 1.0), 1.0 / 3.0, 0.7, 1.0, 4096.0,
		nextafter(32768.0, 1.0), 32768.0 };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (size_t level = 0; level < ARRAY_LENGTH(levels); ++level) {
				struct channel channel = { .gain = 1.0, .law = law, .polarity = polarity };
				struct v92_phase4_record_tx e2u_tx;
				struct v92_e2u_rx e2u_rx;
				CHECK(prepare_e2u_at_level(&channel, false, &e2u_tx, &e2u_rx,
					TEST_LU, TEST_LU, 0.001, V92_CONSTELLATION_4_POINT),
					"initialization handoff fixture failed");
				/* Exercise valid constructor bounds even at LU cells too small
				 * to carry this fixture's E2u waveform without codec erasures. */
				int16_t target;
				CHECK(v92_phase4_demodulator_init(&e2u_rx.signal.line.demodulator,
					V92_CONSTELLATION_4_POINT, 0, levels[level]) == V92_PHASE4_MODULATOR_OK &&
				      v92_phase4_modulator_level(V92_CONSTELLATION_4_POINT, levels[level], 0, 0,
					&target) == V92_PHASE4_MODULATOR_OK, "LU boundary fixture failed");
				for (size_t i = 0; i < ARRAY_LENGTH(e2u_rx.signal.line.pending_training_targets); ++i)
					e2u_rx.signal.line.pending_training_targets[i] = g711_quantize(law, target);
				for (uint8_t rate = 1; rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
					struct v92_cpd_parameters cpd;
					struct v92_pcm_mapping mapping;
					CHECK(v92_cpd_direct_init(&cpd, law, rate, false) == V92_CPD_OK &&
					      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK,
					      "initialization mapping fixture failed");
					for (size_t gain = 0; gain < ARRAY_LENGTH(gains); ++gain)
						for (unsigned domain = 0; domain < 2; ++domain) {
							struct v92_upstream_plan plan = domain ? (struct v92_upstream_plan){
								.gain = gains[gain], .observation = V92_PCM_FRAME_CONSTELLATION,
								.delay = gain % 2 ? V92_B1U_SYMBOLS - 1 :
									V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
							} : direct_plan;
							struct v92_upstream_data_rx receiver, expected = { 0 };
							expected.line.plan = plan;
							expected.line.input_samples = e2u_rx.signal.line.input_samples;
							expected.line.polarity = polarity;
							if (domain) {
								CHECK(v92_pcm_frame_rx_init_constellation(&expected.decoder, &mapping) ==
									V92_PCM_FRAME_OK, "constellation initialization fixture failed");
								expected.line.calibration.gain = plan.gain;
								pcm_gain_init(&expected.line.host.fit);
								expected.line.delay_remaining = plan.delay -
									V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
							} else {
								CHECK(v92_pcm_frame_rx_init(&expected.decoder, &mapping, levels[level]) ==
								      V92_PCM_FRAME_OK, "filter initialization fixture failed");
								expected.line.equalizer = e2u_rx.signal.line.equalizer;
								expected.line.adaptation = 0.001;
							}
							v92_upstream_model_init(&expected);
							memset(&receiver, 0xa5, sizeof(receiver));
							CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) ==
							      V92_UPSTREAM_DATA_RX_OK && v92_upstream_data_rx_is_valid(&receiver),
							      "fresh handoff rejected: law %u rate %u domain %u level %d gain %a",
							      law, rate, domain, levels[level], gains[gain]);
							if (domain) {
								CHECK(check_calibration_interval(&receiver, levels[level]),
								      "calibration interval contract failed");
								/* These two values have their numerical checks above;
								 * retain exact checks for all other initialized state. */
								expected.line.calibration.minimum = receiver.line.calibration.minimum;
								expected.line.calibration.maximum = receiver.line.calibration.maximum;
							}
							CHECK(!memcmp(&receiver, &expected, sizeof(receiver)),
							      "fresh handoff changed state: law %u rate %u domain %u level %d gain %g",
							      law, rate, domain, levels[level], gains[gain]);
							CHECK(v92_upstream_data_rx_init(&receiver, (const void *)&receiver.model,
								&mapping, &plan) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
							      !memcmp(&receiver, &expected, sizeof(receiver)),
							      "aliased training handoff changed receiver");
							struct v92_upstream_rx failed = e2u_rx.signal;
							failed.failed = true;
							CHECK(v92_upstream_data_rx_init(&receiver, &failed, &mapping, &plan) ==
							      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS &&
							      !memcmp(&receiver, &expected, sizeof(receiver)),
							      "failed training handoff changed receiver");
						}
				}
			}
	return true;
}

struct idle_source {
	unsigned int frames;
	unsigned int idle_frames;
};

static bool after_idle(void *context, uint8_t *bits, size_t count)
{
	struct idle_source *source = context;

	if (source->frames < source->idle_frames)
		memset(bits, 1, count);
	else
		payload(bits, count, source->frames - source->idle_frames);
	source->frames++;
	return true;
}

static bool check_quantized_level_tracking(
	enum g711_law law, uint8_t rate, bool drift, int16_t lu, bool select,
	unsigned burst_symbols, unsigned noise_peak, int16_t receive_offset)
{
	unsigned idle_frames = noise_peak ? 32u : burst_symbols ? 64u : 2000u;
	unsigned payload_frames = burst_symbols ? 128u : 400u;
	struct channel channel = { .gain = 1.0, .law = law, .polarity = 1,
		.receive_offset = receive_offset };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_rx handoff;
	struct v92_upstream_data_tx transmitter;
	struct v92_upstream_data_rx receiver;
	struct idle_source source = { .idle_frames = idle_frames };
	unsigned int delivered = 0u;
	double b1u_error = 0.0;
	double burst_error = 0.0;
	unsigned damaged = 0u;
	uint32_t random = UINT32_C(0xac62e5b9) ^ ((uint32_t)rate << 16u) ^ (uint16_t)lu;
	int16_t trained_lu = 0;
	int16_t trained_minimum = 0, trained_maximum = 0;
	int16_t measured_lu = law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(lu)) :
		g711_decode_pcma(g711_encode_pcma(lu));

	/* TRN1u estimates LU from quantized samples, not the transmitter's
	 * exact scalar. Carry the fast training step through real E2u, then
	 * require usable B1u and clean idle/payload. Dense 40k/48k cases check
	 * quantization stability; the lower-rate case checks ongoing tracking. */
	CHECK((select ? v92_cpd_direct_select(&cpd, law, rate, measured_lu, false) :
			v92_cpd_direct_init(&cpd, law, rate, false)) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, lu) ==
		      V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u_at_level(&channel, false, &e2u_tx, &e2u_rx,
			lu, measured_lu, 0.05, V92_CONSTELLATION_4_POINT), "tracking fixture failed");
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;
		enum v92_e2u_rx_result result;

		if (!v92_phase4_record_tx_is_complete(&e2u_tx))
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) ==
				      V92_PHASE4_RECORD_TX_OK, "tracking E2u stopped");
		else
			CHECK(v92_upstream_data_tx_next(&transmitter, after_idle, &source,
					&sample) == V92_UPSTREAM_DATA_TX_OK, "tracking B1u stopped");
		result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE,
		      "tracking E2u did not match");
	}
	CHECK(v92_e2u_rx_handoff(&e2u_rx, &handoff) == V92_E2U_RX_OK &&
	      v92_upstream_data_rx_init(&receiver, &handoff, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK &&
	      receiver.line.adaptation > 0.0 &&
	      receiver.line.adaptation < handoff.line.adaptation &&
	      receiver.line.output_bias == -receive_offset &&
	      memcmp(&receiver.line.equalizer, &handoff.line.equalizer,
		      sizeof(handoff.line.equalizer)) == 0,
	      "tracking handoff changed learned coefficients or pending inputs");
	for (size_t tap = 0u; tap < handoff.line.equalizer.tap_count; ++tap)
		CHECK(handoff.line.equalizer.coefficients[tap].in_phase ==
			      (tap == handoff.line.equalizer.decision_delay ? 1.0 : 0.0) &&
		      handoff.line.equalizer.coefficients[tap].quadrature == 0.0,
		      "E2u quantized LU invented a channel tap: law=%u LU=%d tap=%zu", law, lu, tap);
	for (unsigned int symbol = 0u;
	     symbol < (V92_B1U_FRAMES + idle_frames + payload_frames +
		       V92_PCM_RX_DELAY_FRAMES) * V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		struct v92_upstream_data_frame frame;
		enum v92_upstream_data_rx_result result;

		/* Slow postcursor drift requires continuing adaptation; freezing
		 * the learned FIR is not a substitute for stable data tracking. */
		if (drift)
			channel.postcursor = 0.02 * symbol /
				((V92_B1U_FRAMES + idle_frames + payload_frames) *
				 V92_PCM_MAPPING_FRAME_SYMBOLS);
		CHECK(v92_upstream_data_tx_next(&transmitter, after_idle, &source,
				&sample) == V92_UPSTREAM_DATA_TX_OK, "tracking transmitter stopped");
		/* Perturb real source samples before companding, after clean B1u
		 * and eight payload frames. No erasure hints or relaxed comparisons. */
		if (noise_peak && source.frames > idle_frames + 8u) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			long noisy = lround(sample + noise_peak * (2.0 * (random >> 16u) / 65535.0 - 1.0));

			CHECK(noisy >= INT16_MIN && noisy <= INT16_MAX, "noise clipped source");
			sample = (int16_t)noisy;
		}
		int16_t received = wire_sample(&channel, sample);
		/* Damage real post-training media without erasure metadata. A-law
		 * silence is nonzero, so unguarded normalized adaptation can turn
		 * this short dropout into permanent corruption of a trained FIR. */
		if (symbol >= (V92_B1U_FRAMES + 16u) * V92_PCM_MAPPING_FRAME_SYMBOLS + 5u &&
		    damaged < burst_symbols) {
			received = law == G711_LAW_PCMU ?
				g711_decode_pcmu(g711_encode_pcmu(0)) :
				g711_decode_pcma(g711_encode_pcma(0));
			++damaged;
		}
		result = fixture_sample(&receiver, received, &frame);
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "tracking receiver stopped");
		if (result == V92_UPSTREAM_DATA_RX_FRAME &&
		    receiver.line.received_frames <= V92_B1U_FRAMES)
			b1u_error += frame.squared_error;
		else if (result == V92_UPSTREAM_DATA_RX_FRAME && damaged)
			burst_error += frame.squared_error;
		if (receiver.line.received_frames == V92_B1U_FRAMES) {
			trained_lu = receiver.decoder.upstream_level;
			trained_minimum = receiver.decoder.minimum_level;
			trained_maximum = receiver.decoder.maximum_level;
		}
		if (receiver.line.received_frames > V92_B1U_FRAMES)
			CHECK(receiver.decoder.upstream_level == trained_lu &&
			      receiver.decoder.minimum_level == trained_minimum &&
			      receiver.decoder.maximum_level == trained_maximum,
			      "unknown payload changed the trained LU");
		if (result != V92_UPSTREAM_DATA_RX_FRAME || frame.bit_count == 0u)
			continue;
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

		if (delivered < V92_B1U_FRAMES + idle_frames)
			memset(expected, 1, mapping.data_bits_per_frame);
		else
			payload(expected, mapping.data_bits_per_frame,
				delivered - V92_B1U_FRAMES - idle_frames);
		/* Only the bounded damaged/reacquisition interval may disagree.
		 * Keep exact framing, then verify every subsequent idle/payload bit. */
		bool recovering = burst_symbols && delivered >= V92_B1U_FRAMES + 14u &&
			delivered < V92_B1U_FRAMES + 48u;
		CHECK(frame.bit_count == mapping.data_bits_per_frame && (recovering || (!frame.ambiguous &&
		      memcmp(expected, frame.bits, frame.bit_count) == 0)),
		      "law %u rate %u drift %u frame %u changed data: LU=%d ambiguous=%u error=%.2f",
		      (unsigned int)law, rate, drift, delivered, receiver.decoder.upstream_level,
		      frame.ambiguous, frame.squared_error);
		delivered++;
	}
	CHECK(receiver.line.output_bias == -receive_offset &&
	      delivered == V92_B1U_FRAMES + idle_frames + payload_frames && damaged == burst_symbols,
	      "tracking lost payload frames");
	CHECK(!burst_symbols || burst_error > (double)burst_symbols *
	      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "burst residual was hidden instead of reporting damaged media");
	CHECK(trained_minimum <= lu && trained_maximum >= lu,
	      "training excluded the transmitted LU");
	CHECK(b1u_error <= (double)V92_B1U_SYMBOLS *
		      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "law %u rate %u rejected clean quantized B1u: RMS %.2f",
	      (unsigned int)law, rate, sqrt(b1u_error / V92_B1U_SYMBOLS));
	return true;
}

static bool test_quantized_level_tracking(
	enum g711_law law, uint8_t rate, bool drift, int16_t lu, bool select,
	unsigned burst_symbols, unsigned noise_peak)
{
	return check_quantized_level_tracking(law, rate, drift, lu, select,
		burst_symbols, noise_peak, 0);
}

static bool test_affine_data(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int offset = -768; offset <= 768; offset += 1536)
			CHECK(check_quantized_level_tracking(law, 19u, false, TEST_LU,
				false, 0, 0, offset), "retained correction lost idle/payload data");
	return true;
}

enum stream_control { STREAM_DATA, STREAM_RESET, STREAM_RM };

static bool run_stream(enum g711_law law, int8_t polarity, bool extended,
		       uint8_t rate, double gain, double postcursor, enum stream_control control)
{
	enum { RM_START = V92_B1U_FRAMES + 16, RM_PRIME = RM_START + 32,
		RM_END = RM_PRIME + 2 };
	bool rm = control == STREAM_RM, requested = false;
	struct channel channel = { .gain = gain, .postcursor = postcursor,
		.law = law, .polarity = polarity };
	bool distorted = gain != 1.0 || postcursor != 0.0;
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_rx handoff;
	struct v92_upstream_data_tx transmitter;
	struct v92_upstream_data_rx receiver;
	unsigned int source_frames = 0u;
	unsigned int delivered = 0u;
	unsigned int decisions = 0u;
	double b1u_error = 0.0;
	struct v92_pcm_frame_tx b1u_reference;
	struct v92_pcm_frame expected_b1u;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];

	CHECK(v92_cpd_direct_init(&cpd, law, rate, extended) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, TEST_LU) ==
		      V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u(&channel, extended, &e2u_tx, &e2u_rx), "fixture failed");
	memset(ones, 1, mapping.data_bits_per_frame);
	CHECK(v92_pcm_frame_tx_init(&b1u_reference, &mapping) == V92_PCM_FRAME_OK,
	      "B1u reference initialization failed");
	for (unsigned int frame = 0u; frame < V92_B1U_FRAMES; ++frame)
		CHECK(v92_pcm_frame_encode(&b1u_reference, ones, mapping.data_bits_per_frame,
				&expected_b1u) == V92_PCM_FRAME_OK, "B1u reference stopped");
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;
		enum v92_e2u_rx_result result;

		if (!v92_phase4_record_tx_is_complete(&e2u_tx))
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) ==
				      V92_PHASE4_RECORD_TX_OK, "E2u transmitter stopped");
		else
			CHECK(v92_upstream_data_tx_next(&transmitter, source, &source_frames,
					&sample) == V92_UPSTREAM_DATA_TX_OK, "B1u lookahead stopped");
		result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE,
		      "law %u distorted %u E2u did not match: %s", (unsigned int)law,
		      distorted, v92_e2u_rx_result_name(result));
	}
	CHECK(v92_e2u_rx_handoff(&e2u_rx, &handoff) == V92_E2U_RX_OK &&
	      v92_upstream_data_rx_init(&receiver, &handoff, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK &&
	      transmitter.b1u_symbols_remaining == V92_B1U_SYMBOLS -
		      V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      memcmp(&receiver.line.equalizer, &handoff.line.equalizer,
		      sizeof(handoff.line.equalizer)) == 0,
	      "E2u lost pending B1u inputs or changed the trained FIR");
	for (unsigned int symbol = 0u;
	     symbol < (V92_B1U_FRAMES + DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES + (rm ? 34u : 0u)) *
		      V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		struct v92_upstream_data_frame frame;
		struct v92_upstream_data_frame saved;
		enum v92_upstream_data_rx_result result;

		memset(&frame, 0xa5, sizeof(frame));
		saved = frame;
		/* Two epochs closer than traceback: both pending generations must
		 * survive. Transmit and equalized receive boundaries differ by the
		 * trained FIR's physical delay. No request acquisition is simulated. */
		if (control == STREAM_RESET && transmitter.queue_count == 0u &&
		    (source_frames == 16u || source_frames == 18u)) {
			struct v92_upstream_data_tx expected = transmitter;

			expected.encoder.state.scrambler.history = 0u;
			expected.encoder.state.differential = 0u;
			CHECK(v92_upstream_data_tx_reset_control(&transmitter) ==
				      V92_UPSTREAM_DATA_TX_OK &&
			      memcmp(&transmitter, &expected, sizeof(expected)) == 0,
			      "transmit control reset changed retained state");
		}
		if (control == STREAM_RESET && receiver.line.symbol_count == 0u &&
		    (receiver.line.received_frames == V92_B1U_FRAMES + 16u ||
		     receiver.line.received_frames == V92_B1U_FRAMES + 18u)) {
			struct v92_upstream_data_rx expected = receiver;

			for (size_t path = 0u; path < expected.decoder.path_count; ++path) {
				expected.decoder.paths[path].descrambler.history = 0u;
				expected.decoder.paths[path].differential = 0u;
			}
			CHECK(v92_upstream_data_rx_reset_control(&receiver) ==
				      V92_UPSTREAM_DATA_RX_OK &&
			      memcmp(&receiver, &expected, sizeof(expected)) == 0,
			      "receive control reset changed retained state");
		}
		if (control == STREAM_RESET && receiver.line.received_frames == V92_B1U_FRAMES + 16u &&
		    receiver.line.symbol_count != 0u) {
			struct v92_upstream_data_rx expected = receiver;

			CHECK(v92_upstream_data_rx_reset_control(&receiver) ==
				      V92_UPSTREAM_DATA_RX_INVALID_STATE &&
			      memcmp(&receiver, &expected, sizeof(expected)) == 0,
			      "partial equalized frame accepted a control reset");
		}
		if (control == STREAM_RESET && source_frames == 17u && transmitter.queue_count != 0u) {
			struct v92_upstream_data_tx expected = transmitter;

			CHECK(v92_upstream_data_tx_reset_control(&transmitter) ==
				      V92_UPSTREAM_DATA_TX_INVALID_STATE &&
			      memcmp(&transmitter, &expected, sizeof(expected)) == 0,
			      "partial transmit frame accepted a control reset");
		}
		if (rm && !requested && source_frames == 16u && transmitter.queue_count == 0u) {
			CHECK(v92_upstream_data_tx_start_rm(&transmitter) == V92_UPSTREAM_DATA_TX_OK,
			      "stream RM request failed");
			requested = true;
		}
		if (rm && receiver.line.symbol_count == 0u && receiver.line.received_frames == RM_END)
			CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_OK,
			      "stream RM control handoff failed");
		enum v92_upstream_data_tx_result tx_result = v92_upstream_data_tx_next(
			&transmitter, source, &source_frames, &sample);
		CHECK(tx_result == V92_UPSTREAM_DATA_TX_OK ||
		      (rm && tx_result == V92_UPSTREAM_DATA_TX_RM_COMPLETE), "data transmitter stopped");
		CHECK(receive_compared(&receiver, wire_sample(&channel, sample), &frame, &result),
		      "owned stream diverged");
		CHECK(result == (symbol % V92_PCM_MAPPING_FRAME_SYMBOLS == 11u ?
			      V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "law %u rate %u distortion %u symbol %u stopped: %s",
		      (unsigned int)law, rate, distorted, symbol,
		      v92_upstream_data_rx_result_name(result));
		CHECK(receiver.line.input_samples == handoff.line.input_samples + symbol + 1u &&
		      v92_upstream_data_rx_b1u_complete(&receiver) == (symbol + 1u >= V92_B1U_SYMBOLS),
		      "physical B1u boundary changed");
		if (result != V92_UPSTREAM_DATA_RX_FRAME) {
			CHECK(memcmp(&frame, &saved, sizeof(frame)) == 0, "buffering wrote output");
			continue;
		}
		if (decisions < V92_B1U_FRAMES)
			b1u_error += frame.squared_error;
		enum v92_pcm_frame_rm expected_rm = !rm || decisions < RM_START +
			V92_PCM_RM_QUALIFY_FRAMES - 1u || decisions >= RM_END ? V92_PCM_FRAME_RM_NONE :
			decisions < RM_PRIME ? V92_PCM_FRAME_RM_SIGNAL :
			decisions == RM_PRIME ? V92_PCM_FRAME_RM_PRIME : V92_PCM_FRAME_RM_COMPLETE;
		CHECK(frame.rm == expected_rm, "law %u rate %u frame %u RM stage %u != %u",
		      law, rate, decisions, frame.rm, expected_rm);
		unsigned output_frame = decisions - V92_PCM_RX_DELAY_FRAMES;
		bool request_slot = rm && output_frame >= RM_START && output_frame < RM_END;
		if (!request_slot)
			CHECK((frame.bit_count != 0u) == (decisions >= V92_PCM_RX_DELAY_FRAMES),
			      "payload delay changed");
		if (rm && decisions >= RM_START + V92_PCM_RM_QUALIFY_FRAMES - 1u &&
		    output_frame < RM_END)
			CHECK(frame.bit_count == 0u, "qualified RM leaked delayed data");
		decisions++;
		if (decisions == V92_B1U_FRAMES) {
			bool retained = false;
			for (size_t path = 0u; path < receiver.decoder.path_count; ++path) {
				const struct v92_pcm_frame_rx_path *state = &receiver.decoder.paths[path];
				retained |= state->trellis.state == b1u_reference.state.trellis.state &&
					state->differential == b1u_reference.state.differential &&
					state->descrambler.history == b1u_reference.state.scrambler.history &&
					memcmp(&state->filter, &b1u_reference.state.filter,
						sizeof(state->filter)) == 0;
			}
			CHECK(retained, "law %u rate %u distortion %u lost known B1u history",
			      (unsigned int)law, rate, distorted);
		}
		if (frame.bit_count == 0u || request_slot)
			continue;
		CHECK(frame.b1u == (delivered < V92_B1U_FRAMES), "delayed B1u misclassified");
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

		if (frame.b1u)
			memset(expected, 1, mapping.data_bits_per_frame);
		else
			payload(expected, mapping.data_bits_per_frame, delivered - V92_B1U_FRAMES);
		CHECK(frame.bit_count == mapping.data_bits_per_frame && !frame.ambiguous &&
		      memcmp(expected, frame.bits, frame.bit_count) == 0,
		      "law %u rate %u distortion %u frame %u changed payload (ambiguous %u residual %.3f)",
		      (unsigned int)law, rate, distorted, delivered, frame.ambiguous,
		      frame.squared_error);
		delivered++;
	}
	CHECK(delivered == V92_B1U_FRAMES + DATA_FRAMES &&
	      (postcursor == 0.0 || b1u_error > 0u), "delivery or distortion evidence changed");
	return true;
}

struct fast_control_source {
	struct v92_upstream_control control;
	uint8_t record[V92_CPU_MAX_FRAME_BYTES];
	size_t bits, index;
	unsigned data_frames;
	bool active;
};

static bool fast_control_source(void *context, uint8_t *bits, size_t count)
{
	struct fast_control_source *source_state = context;
	if (!source_state->active)
		return source(&source_state->data_frames, bits, count);
	if (source_state->control.record_complete) {
		/* Only flush receive lookahead after the final control record. This
		 * fixture does not claim to exercise E2u/FB1u or a complete FPE. */
		if (v92_upstream_control_can_finish(&source_state->control)) {
			memset(bits, 1, count);
			return true;
		}
		if (!v92_upstream_control_start(&source_state->control, count,
			    source_state->record, sizeof(source_state->record), &source_state->bits))
			return false;
		source_state->index = 0;
	}
	if (count > source_state->bits - source_state->index)
		return false;
	for (size_t bit = 0; bit < count; ++bit)
		bits[bit] = packed_msb_bit(source_state->record, source_state->index++);
	return true;
}

static bool downstream_control_contracts(const struct v92_downstream_control *initial,
	const struct v92_cpd_parameters *cpd, const struct v92_cpu_parameters *previous)
{
	const struct v92_suvd_parameters status = { 0 };
	for (unsigned invalid = 0; invalid < 8; ++invalid) {
		struct v92_downstream_control candidate = *initial;
		struct v92_cpu_parameters cpu = *previous;
		struct v92_cpd_parameters selection = *cpd;
		struct v92_suvd_parameters suvd = status;
		const struct v92_cpu_parameters *prior = &cpu;
		switch (invalid) {
		case 0: prior = NULL; break;
		case 1: cpu.data_rate_number--; break;
		case 2: cpu.data_rate_number = 0; break;
		case 3: cpu.law = (enum g711_law)99; break;
		case 4: suvd.request_silence = true; break;
		case 5: suvd.acknowledges_cpu = true; break;
		case 6: selection.acknowledges_cpu = true; break;
		case 7: selection.extend_e2u = true; break;
		}
		CHECK(!v92_downstream_control_init(&candidate, &initial->transmit, &suvd, &selection, prior, 80) &&
		      memcmp(&candidate, initial, sizeof(candidate)) == 0, "invalid FPE entry %u changed state", invalid);
	}
	struct v92_downstream_control selected = *initial;
	struct v92_cpd_parameters extended = *cpd;
	extended.extend_e2u = true;
	CHECK(!v92_downstream_control_select(&selected, &extended) &&
	      memcmp(&selected, initial, sizeof(selected)) == 0, "FPE selected extended E2u");
	struct pcm_downstream_rx peer;
	CHECK(pcm_downstream_rx_init(&peer, PCM_PROTOCOL_V92, &initial->transmit.encoder.mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK, "early SUVu peer failed");
	struct v92_control_event early = { .type = V92_CONTROL_EVENT_SUVU };
	CHECK(v92_downstream_control_receive(&selected, &early) == V92_DOWNSTREAM_CONTROL_OK, "early SUVu rejected");
	unsigned statuses = 0, selections = 0;
	for (unsigned clock = 0; clock < 4000 && !selections; ++clock) {
		uint8_t octet;
		struct pcm_downstream_event record;
		CHECK(v92_downstream_control_transmit(&selected, &octet), "early SUVu stopped first transmit");
		enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(&peer, octet, &record);
		CHECK(result == PCM_DOWNSTREAM_RX_BUFFERING || result == PCM_DOWNSTREAM_RX_SUVD ||
		      result == PCM_DOWNSTREAM_RX_CPD, "early SUVu corrupted downstream record: %u", result);
		statuses += result == PCM_DOWNSTREAM_RX_SUVD;
		selections += result == PCM_DOWNSTREAM_RX_CPD;
		CHECK(!selections || statuses == 1, "CPd preceded the first complete SUVd");
	}
	CHECK(selections == 1 && selected.cpd_start_frame != 0 && !selected.failed,
	      "early SUVu did not produce SUVd then CPd");
	selected = *initial;
	struct v92_control_event event = { .type = V92_CONTROL_EVENT_CPUS };
	event.record.cpus.parameters.data_rate_number = previous->data_rate_number - 1;
	/* A lost initial SUVu must not make repeated, valid CPus invisible.
	 * The previous parameters identify the short selection, but the actual
	 * status must still arrive before CPd or completion is authorized. */
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_OK &&
	      selected.cpu_received && !selected.suvu_received && !selected.peer_confirmation_received &&
	      selected.cpu.data_rate_number == previous->data_rate_number - 1 &&
	      selected.cpu.law == previous->law && selected.cpu.sign_redundancy == previous->sign_redundancy &&
	      selected.cpu.constellation_count == previous->constellation_count &&
	      memcmp(selected.cpu.transmit_constellations, previous->transmit_constellations,
		      sizeof(selected.cpu.transmit_constellations)) == 0 &&
	      v90_pcm_mapping_equal(&selected.transmit.encoder.mapping, &initial->transmit.encoder.mapping),
	      "CPus lost its previous parameters or replaced the active wire mapping");
	CHECK(pcm_downstream_rx_init(&peer, PCM_PROTOCOL_V92, &initial->transmit.encoder.mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK, "CPus recovery peer failed");
	unsigned unacknowledged = 0, acknowledged = 0;
	for (unsigned clock = 0; clock < 4000 && !acknowledged; ++clock) {
		uint8_t octet;
		struct pcm_downstream_event record;
		CHECK(v92_downstream_control_transmit(&selected, &octet), "CPus recovery output failed");
		enum pcm_downstream_rx_result result = pcm_downstream_rx_receive(&peer, octet, &record);
		CHECK(result == PCM_DOWNSTREAM_RX_BUFFERING || result == PCM_DOWNSTREAM_RX_SUVD,
		      "CPus bypassed the missing SUVu");
		if (result == PCM_DOWNSTREAM_RX_SUVD) {
			CHECK(record.control.record.suvd.frame_result == V92_SUVD_OK, "CPus recovery status was damaged");
			if (record.control.record.suvd.parameters.acknowledges_cpu)
				acknowledged++;
			else
				unacknowledged++;
		}
		CHECK(!selected.suvu_received && !selected.cpd_start_frame &&
		      !selected.acknowledgement_end_frame && !v92_downstream_control_is_ed_ready(&selected) &&
		      v92_downstream_control_e2u_delay(&selected) == UINT64_MAX &&
		      !v92_downstream_control_note_e2u(&selected), "CPus recovery completed before SUVu/CPd");
	}
	CHECK(unacknowledged && acknowledged, "CPus recovery lost initial status or did not acknowledge");
	event = (struct v92_control_event){ .type = V92_CONTROL_EVENT_SUVU };
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_OK, "SUVu did not open selection");
	event = (struct v92_control_event){ .type = V92_CONTROL_EVENT_CPUS };
	event.record.cpus.parameters.data_rate_number = previous->data_rate_number - 1;
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_OK && selected.cpu_received &&
	      !selected.peer_confirmation_received && selected.cpu.data_rate_number == previous->data_rate_number - 1 &&
	      v90_pcm_mapping_equal(&selected.transmit.encoder.mapping, &initial->transmit.encoder.mapping),
	      "short CPu lost previous parameters or replaced the active data mapping");
	event.record.cpus.parameters.acknowledges_cpd = true;
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_OK && selected.peer_confirmation_received &&
	      !v92_downstream_control_is_ed_ready(&selected), "received acknowledgment counted as transmitted");
	event.record.cpus.parameters.acknowledges_cpd = false;
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_REJECTED && selected.failed,
	      "conflicting repeated CPus was accepted");
	for (unsigned invalid = 0; invalid < 4; ++invalid) {
		selected = *initial;
		event = (struct v92_control_event){ .type = V92_CONTROL_EVENT_CPU };
		event.record.cpu.parameters = *previous;
		event.record.cpu.parameters.data_rate_number = 0;
		switch (invalid) {
		case 0: event.record.cpu.frame_result = V92_CPU_CRC_MISMATCH; break;
		case 1: event.record.cpu.parameters.law = (enum g711_law)99; break;
		case 2: event.record.cpu.parameters.sign_redundancy = 4; break;
		case 3:
			event.type = V92_CONTROL_EVENT_CPUS;
			event.record.cpus.frame_result = V92_CPU_CRC_MISMATCH;
			event.record.cpus.parameters.data_rate_number = 0;
			break;
		}
		CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_ERROR && !selected.cleardown.active,
		      "invalid zero-rate record %u caused cleardown", invalid);
	}
	/* The peer codec is not the local network law, including in a valid
	 * zero-rate request. Cleardown must retain the active wire mapping. */
	selected = *initial;
	event = (struct v92_control_event){ .type = V92_CONTROL_EVENT_CPU };
	event.record.cpu.parameters = *previous;
	event.record.cpu.parameters.data_rate_number = 0;
	event.record.cpu.parameters.law = previous->law == G711_LAW_PCMU ?
		G711_LAW_PCMA : G711_LAW_PCMU;
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_OK && selected.cleardown.active &&
	      selected.cleardown.peer_requested && !selected.failed &&
	      v90_pcm_mapping_equal(&selected.transmit.encoder.mapping, &initial->transmit.encoder.mapping),
	      "peer codec blocked cleardown or changed the network mapping");
	selected = *initial;
	event = (struct v92_control_event){ .type = V92_CONTROL_EVENT_CPUS_REJECTED };
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_OK && memcmp(&selected, initial, sizeof(selected)) == 0,
	      "damaged control record changed exchange state");
	event = (struct v92_control_event){ .type = V92_CONTROL_EVENT_SUVU };
	event.record.suvu.parameters.request_silence = true;
	CHECK(v92_downstream_control_receive(&selected, &event) == V92_DOWNSTREAM_CONTROL_REJECTED && selected.failed,
	      "FPE accepted an SUVu silence request");
	uint8_t octet = 123;
	CHECK(!v92_downstream_control_transmit(&selected, &octet) && octet == 123,
	      "failed exchange emitted a network octet");
	return true;
}

static bool test_fast_control(enum g711_law law, uint8_t rate)
{
	struct channel channel = { .gain = 1.0, .law = law, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_tx tx;
	struct v92_upstream_data_rx rx;
	struct v92_control_rx parser;
	struct v92_downstream_control digital;
	struct pcm_downstream_tx downstream;
	struct pcm_downstream_rx peer;
	struct v90_pcm_mapping old_downstream;
	struct fast_control_source input = { 0 };
	struct v92_suvu_parameters suvu = { .prefilter_level_db_q2_2 = -16 };
	struct v92_cpu_parameters cpu = {
		.law = law, .trn1d_ratio_q3_13 = 0x2000, .data_rate_number = 11,
		.sign_redundancy = 1, .spectral_lookahead_frames = 1,
		.constellation_count = 1,
	};
	for (unsigned ucode = 16; ucode < 112; ++ucode)
		cpu.transmit_constellations[0].ucode_mask[ucode / 8] |= (uint8_t)(1u << (ucode % 8));
	struct v92_cpu_parameters previous_cpu = cpu;
	previous_cpu.data_rate_number = rate == 1 ? 9 : rate == 16 ? 17 : 22;
	previous_cpu.acknowledges_cpd = true;
	const struct v92_suvd_parameters suvd = { 0 };
	CHECK(v92_cpd_direct_init(&cpd, law, rate, false) == V92_CPD_OK &&
	      v90_pcm_mapping_init_from_v92_cpu(&old_downstream, &previous_cpu, previous_cpu.law) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(&downstream, PCM_PROTOCOL_V92, &old_downstream) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(&peer, PCM_PROTOCOL_V92, &old_downstream,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_downstream_control_init(&digital, &downstream, &suvd, &cpd, &previous_cpu, 80) &&
	      !digital.cpu_received && !digital.peer_confirmation_received && !digital.cpu.acknowledges_cpd &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&tx, &mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK &&
	      v92_upstream_control_init(&input.control, &suvu, &cpu) &&
	      v92_control_rx_init_upstream(&parser, mapping.data_bits_per_frame) == V92_CONTROL_RX_OK &&
	      prepare_e2u(&channel, false, &e2u_tx, &e2u_rx), "fast control fixture failed");
	CHECK(downstream_control_contracts(&digital, &cpd, &previous_cpu), "digital FPE contract failed");
	CHECK(!v92_downstream_control_silence_requested(&digital) &&
	      !v92_downstream_control_note_e2u(&digital), "FPE entered the silence procedure");
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&e2u_tx))
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "fast control E2u failed");
		else
			CHECK(v92_upstream_data_tx_next(&tx, fast_control_source, &input, &sample) ==
			      V92_UPSTREAM_DATA_TX_OK, "fast control B1u failed");
		enum v92_e2u_rx_result result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE, "E2u receive failed");
	}
	CHECK(v92_upstream_data_rx_init(&rx, &e2u_rx.signal, &mapping, &direct_plan) ==
	      V92_UPSTREAM_DATA_RX_OK, "fast control data handoff failed");
	bool requested = false, receiving = false;
	unsigned suv_count = 0, cpu_count = 0, ack_count = 0, cpd_count = 0;
	for (unsigned i = 0; i < 6000; ++i) {
		if (!requested && input.data_frames == 16 && !tx.queue_count) {
			CHECK(v92_upstream_data_tx_start_rm(&tx) == V92_UPSTREAM_DATA_TX_OK, "RM failed");
			requested = true;
		}
		int16_t sample;
		enum v92_upstream_data_tx_result result = v92_upstream_data_tx_next(
			&tx, fast_control_source, &input, &sample);
		CHECK(result == V92_UPSTREAM_DATA_TX_OK || result == V92_UPSTREAM_DATA_TX_RM_COMPLETE,
		      "fast control transmitter stopped");
		if (result == V92_UPSTREAM_DATA_TX_RM_COMPLETE)
			input.active = true;
		if (input.active && !input.control.record_complete &&
		    input.index == input.bits && !tx.queue_count)
			CHECK(v92_upstream_control_complete(&input.control), "control wire completion failed");
		struct v92_upstream_data_frame frame;
		enum v92_upstream_data_rx_result received = fixture_sample(
			&rx, wire_sample(&channel, sample), &frame);
		CHECK(received == V92_UPSTREAM_DATA_RX_OK || received == V92_UPSTREAM_DATA_RX_FRAME,
		      "fast control receiver stopped");
		/* Exercise both control owners through the preceding data mappings.
		 * Rf/Rf-bar and the final E2u/FB1u/B1u handoff are not included here. */
		if (receiving) {
			uint8_t octet;
			struct pcm_downstream_event event;
			CHECK(v92_downstream_control_transmit(&digital, &octet), "digital control stopped");
			enum pcm_downstream_rx_result r = pcm_downstream_rx_receive(&peer, octet, &event);
			CHECK(r == PCM_DOWNSTREAM_RX_BUFFERING || r == PCM_DOWNSTREAM_RX_SUVD || r == PCM_DOWNSTREAM_RX_CPD,
			      "downstream control rejected: %u", r);
			if (!v92_upstream_control_can_finish(&input.control)) {
				if (r == PCM_DOWNSTREAM_RX_SUVD)
					CHECK(!event.control.record.suvd.parameters.request_silence &&
					      v92_upstream_control_note_suvd(&input.control, &event.control.record.suvd.parameters),
					      "SUVd failed");
				if (r == PCM_DOWNSTREAM_RX_CPD) {
					cpd_count++;
					CHECK(event.control.record.cpd.parameters.data_rate_number == cpd.data_rate_number &&
					      v92_upstream_control_note_cpd(&input.control,
						      event.control.record.cpd.parameters.acknowledges_cpu), "CPd failed");
				}
			}
			if (!peer.ed_requested && v92_upstream_control_can_finish(&input.control) &&
			    (r == PCM_DOWNSTREAM_RX_SUVD || r == PCM_DOWNSTREAM_RX_CPD))
				CHECK(pcm_downstream_rx_expect_ed(&peer) == PCM_DOWNSTREAM_RX_OK, "Ed receive boundary lost");
		}
		if (received != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (frame.rm == V92_PCM_FRAME_RM_COMPLETE) {
			CHECK(v92_upstream_data_rx_reset_control(&rx) == V92_UPSTREAM_DATA_RX_OK,
			      "RM did not preserve the control receive boundary");
			receiving = true;
		}
		if (!receiving || !frame.bit_count)
			continue;
		CHECK(!frame.ambiguous && !frame.b1u, "control did not retain the data coding epoch");
		for (size_t bit = 0; bit < frame.bit_count; ++bit) {
			struct v92_control_event event;
			CHECK(v92_control_rx_receive_bit(&parser, frame.bits[bit], &event) == V92_CONTROL_RX_OK,
			      "fast control parser stopped");
			if (event.type == V92_CONTROL_EVENT_SUVU) {
				CHECK(!event.record.suvu.parameters.request_silence, "FPE SUVu requested silence");
				ack_count += event.record.suvu.parameters.acknowledges_cpd;
				suv_count++;
			} else if (event.type == V92_CONTROL_EVENT_CPU) {
				CHECK(!cpu_count++ && event.record.cpu.parameters.data_rate_number == cpu.data_rate_number,
				      "CPu selection changed");
			} else {
				CHECK(event.type == V92_CONTROL_EVENT_NONE, "control wire record rejected: %u", event.type);
			}
			CHECK(v92_downstream_control_receive(&digital, &event) == V92_DOWNSTREAM_CONTROL_OK, "digital rejected upstream record %u", event.type);
		}
		if (ack_count && input.control.record_complete && v92_upstream_control_can_finish(&input.control) &&
		    v92_downstream_control_is_ed_ready(&digital) && peer.ed_requested) {
			CHECK(cpu_count == 1 && cpd_count == 1 && input.control.completed_cpu_records == 1 &&
			      input.data_frames == 16 && memcmp(&tx.encoder.mapping, &mapping, sizeof(mapping)) == 0 &&
			      memcmp(&digital.transmit.encoder.mapping, &old_downstream, sizeof(old_downstream)) == 0 &&
			      digital.cpu.data_rate_number == cpu.data_rate_number && digital.previous_cpu,
			      "fast control changed old mapping, repeated CPu, or fetched host payload");
			CHECK(!v92_downstream_control_select(&digital, &cpd), "accepted CPd selection changed in flight");
			struct v92_phase4_ed_tx tail;
			CHECK(v92_phase4_ed_tx_init(&tail, &digital.transmit,
				      digital.transmit_stage == V92_DOWNSTREAM_CONTROL_CPD ? &digital.initial_cpd : NULL),
			      "data-mode Ed tail was rejected");
			for (unsigned clock = 0; clock < 2000; ++clock) {
				struct pcm_downstream_symbol symbol;
				enum pcm_downstream_tx_result r = v92_phase4_ed_tx_next(&tail, &symbol);
				if (r == PCM_DOWNSTREAM_TX_COMPLETE) {
					CHECK(pcm_downstream_rx_is_complete(&peer) &&
					      v90_pcm_mapping_equal(&tail.transmit.encoder.mapping, &old_downstream),
					      "Ed missing or old data mapping replaced before handoff");
					return true;
				}
				CHECK(r == PCM_DOWNSTREAM_TX_OK, "data-mode Ed tail stopped");
				struct pcm_downstream_event event;
				enum pcm_downstream_rx_result received = pcm_downstream_rx_receive(&peer, symbol.pcm.octet, &event);
				CHECK(received == PCM_DOWNSTREAM_RX_BUFFERING || received == PCM_DOWNSTREAM_RX_CPD ||
				      received == PCM_DOWNSTREAM_RX_SUVD || received == PCM_DOWNSTREAM_RX_ED,
				      "invalid data-mode Ed tail: %u", received);
			}
			CHECK(false, "data-mode Ed tail never completed");
		}
	}
	CHECK(false, "law %u rate %u fast control did not finish: SUVu=%u CPu=%u ack=%u",
	      law, rate, suv_count, cpu_count, ack_count);
}

static bool b1u_switch_contracts(const struct v92_upstream_data_rx *initial,
	const struct v92_pcm_mapping *mapping, const struct v92_upstream_plan *plan)
{
	for (unsigned fault = 0; fault < 12; ++fault) {
		struct v92_upstream_data_rx rx = *initial;
		struct v92_pcm_mapping selection = *mapping;
		struct v92_upstream_plan proposed = *plan;
		const struct v92_pcm_mapping *map = &selection;
		const struct v92_upstream_plan *next_plan = &proposed;
		enum v92_upstream_data_rx_result expected = V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT;
		switch (fault) {
		case 0: map = NULL; break;
		case 1: next_plan = NULL; break;
		case 2: map = &rx.decoder.mapping; break;
		case 3: next_plan = &rx.line.plan; break;
		case 4: selection.data_bits_per_frame = 0; expected = V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS; break;
		case 5: selection.cpd.extend_e2u = true; expected = V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS; break;
		case 6: proposed.delay++; expected = V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS; break;
		case 7: rx.failed = true; expected = V92_UPSTREAM_DATA_RX_FAILED; break;
		case 8: rx.line.symbol_count = 12; expected = V92_UPSTREAM_DATA_RX_INVALID_STATE; break;
		case 9: rx.line.received_frames = 0; expected = V92_UPSTREAM_DATA_RX_INVALID_STATE; break;
		case 10: {
			struct v92_cpd_parameters cpd;
			enum g711_law other = mapping->law == G711_LAW_PCMU ? G711_LAW_PCMA : G711_LAW_PCMU;
			CHECK(v92_cpd_direct_init(&cpd, other, 1, false) == V92_CPD_OK &&
			      v92_pcm_mapping_init(&selection, other, &cpd) == V92_PCM_MAPPING_OK,
			      "conflicting-law fixture failed");
			expected = V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
			break;
		}
		case 11:
			proposed = plan->observation == V92_PCM_FRAME_CONSTELLATION ? direct_plan :
				(struct v92_upstream_plan){ .observation = V92_PCM_FRAME_CONSTELLATION,
					.gain = 1.0, .delay = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY };
			expected = V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS;
			break;
		}
		struct v92_upstream_data_rx saved = rx;
		CHECK(v92_upstream_data_rx_start_b1u(&rx, map, next_plan) == expected &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0, "B1u handoff fault %u changed state", fault);
	}
	CHECK(v92_upstream_data_rx_start_b1u(NULL, mapping, plan) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT,
	      "null B1u receiver accepted");
	if (plan->observation == V92_PCM_FRAME_CONSTELLATION)
		for (unsigned wide = 0; wide < 2; ++wide)
			for (unsigned upper = 0; upper < 2; ++upper) {
				struct v92_upstream_data_rx rx = *initial;
				struct v92_upstream_plan proposed = *plan;

				/* Valid old bounds can exceed the supported range only
				 * AFTER rebasing. Reject before replacing either decoder. */
				rx.line.plan.gain = rx.line.calibration.gain = 1.0;
				rx.line.calibration.minimum = wide ? .5 : 1.0;
				rx.line.calibration.maximum = wide ? 2.0 : 1.0;
				proposed.gain = upper ? 32768.0 : 1.0 / 32768.0;
				CHECK(v92_upstream_data_rx_is_valid(&rx), "rebasing fixture is invalid");
				struct v92_upstream_data_rx expected = rx;
				if (!wide) {
					expected.line.plan = proposed;
					expected.line.received_frames = 0;
					expected.line.calibration.gain = proposed.gain;
					expected.line.calibration.minimum = proposed.gain;
					expected.line.calibration.maximum = proposed.gain;
					expected.line.calibration.correlation = expected.line.calibration.energy = 0;
					expected.line.calibration.raw_sum = expected.line.calibration.target_sum = 0;
					expected.line.calibration.samples = 0;
					pcm_gain_init(&expected.line.host.fit);
					expected.line.host.gain = expected.line.host.lower = expected.line.host.upper = 0;
					CHECK(v92_pcm_frame_rx_init_constellation(&expected.decoder, mapping) == V92_PCM_FRAME_OK,
					      "boundary decoder fixture failed");
					v92_upstream_model_init(&expected);
				}
				CHECK(v92_upstream_data_rx_start_b1u(&rx, mapping, &proposed) ==
				      (wide ? V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS : V92_UPSTREAM_DATA_RX_OK) &&
				      !memcmp(&rx, &expected, sizeof(rx)) && v92_upstream_data_rx_is_valid(&rx),
				      "B1u gain boundary changed retained state: wide=%u upper=%u", wide, upper);
			}
	return true;
}

/* Two codec conversions with channel memory, followed by inverted polarity. */
static uint8_t finish_codec_sample(enum g711_law law, int16_t sample, int16_t history[2])
{
	int16_t q = law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
	int16_t mixed = (int16_t)lround(-(.9 * q + .08 * history[0] + .02 * history[1]));
	history[1] = history[0];
	history[0] = q;
	return law == G711_LAW_PCMU ? g711_encode_pcmu(mixed) : g711_encode_pcma(mixed);
}

/* A real sample-level handoff with mock control payload: E2u + 48 FB1u use
 * the old coder continuously, then a different CPd starts B1u and data. The
 * separately tested control owners, not this fixture, authorize the finish. */
static bool test_fast_finish(enum g711_law law, uint8_t old_rate, uint8_t new_rate, bool codec)
{
	enum { CONTROL = 80, E2U = 144, NEW = E2U + 1 + V92_B1U_FRAMES, END = NEW + V92_B1U_FRAMES + 26 };
	struct channel channel = { .gain = 1.0, .law = law, .polarity = -1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping old_mapping, new_mapping;
	struct v92_upstream_plan old_plan = direct_plan, new_plan = direct_plan;
	struct v92_upstream_channel measured = { 0 };
	struct v92_upstream_data_tx tx;
	struct v92_upstream_data_rx rx;
	struct v92_phase4_record_tx initial_tx;
	struct v92_e2u_rx initial_rx;
	uint8_t expected[END][V92_PCM_MAPPING_MAX_BITS] = { { 0 } };
	int16_t wire[END * V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t history[2] = { 0 };
	unsigned switch_at = 0, epoch = 0, fb1u_count = 0, data_count = 0;
	if (codec) {
		uint32_t random = 0x35791bdu;
		for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);
			uint8_t code = finish_codec_sample(law, sample, history);
			int16_t decoded = law == G711_LAW_PCMU ? g711_decode_pcmu(code) : g711_decode_pcma(code);
			CHECK(v92_upstream_channel_sample(&measured, sample, (int16_t)lround(-.75 * decoded)),
			      "finish channel measurement failed");
		}
		history[0] = history[1] = 0;
	}
	CHECK(v92_cpd_direct_init(&cpd, law, old_rate, false) == V92_CPD_OK &&
	      (!codec || v92_cpd_channel_design(&cpd, &measured, law, TEST_LU, &old_plan)) &&
	      v92_pcm_mapping_init(&old_mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_cpd_direct_init(&cpd, law, new_rate, false) == V92_CPD_OK &&
	      (!codec || v92_cpd_channel_design(&cpd, &measured, law, TEST_LU, &new_plan)) &&
	      v92_pcm_mapping_init(&new_mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&tx, &old_mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK,
	      "fast finish setup failed");
	for (unsigned f = 0; f < END; ++f) {
		if (f == CONTROL)
			CHECK(v92_upstream_data_tx_reset_control(&tx) == V92_UPSTREAM_DATA_TX_OK, "control reset failed");
		if (f == E2U)
			CHECK(v92_upstream_data_tx_start_e2u(&tx) == V92_UPSTREAM_DATA_TX_OK, "E2u start failed");
		if (f == NEW)
			CHECK(v92_upstream_data_tx_init(&tx, &new_mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK,
			      "new B1u start failed");
		if (f < V92_B1U_FRAMES || (f > E2U && f < NEW + V92_B1U_FRAMES))
			memset(expected[f], 1, tx.encoder.mapping.data_bits_per_frame);
		else if (f != E2U)
			payload(expected[f], tx.encoder.mapping.data_bits_per_frame, f);
		unsigned input_frame = f;
		for (unsigned s = 0; s < V92_PCM_MAPPING_FRAME_SYMBOLS; ++s) {
			unsigned index = f * V92_PCM_MAPPING_FRAME_SYMBOLS + s;
			CHECK(v92_upstream_data_tx_next(&tx, source, &input_frame,
				      &wire[index]) ==
			      (f == NEW - 1 && s == V92_PCM_MAPPING_FRAME_SYMBOLS - 1 ?
				      V92_UPSTREAM_DATA_TX_FB1U_COMPLETE : V92_UPSTREAM_DATA_TX_OK),
			      "finish TX stopped at frame %u", f);
			if (codec)
				wire[index] = finish_codec_sample(law, wire[index], history);
		}
		CHECK(input_frame == f + (f >= V92_B1U_FRAMES && (f < E2U || f >= NEW + V92_B1U_FRAMES)),
		      "finish/B1u consumed host payload");
	}
	CHECK(prepare_e2u(&channel, false, &initial_tx, &initial_rx), "initial E2u failed");
	/* The codec fixture starts with an established training handoff, as in
	 * test_codec_model; the direct fixture also decodes actual initial E2u. */
	unsigned cursor = codec ? V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY : 0;
	while (!codec && !v92_e2u_rx_is_complete(&initial_rx)) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&initial_tx))
			CHECK(v92_phase4_record_tx_next(&initial_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "initial E2u TX failed");
		else
			sample = wire[cursor++];
		enum v92_e2u_rx_result r = v92_e2u_rx_sample(&initial_rx, wire_sample(&channel, sample));
		CHECK(r == V92_E2U_RX_OK || r == V92_E2U_RX_COMPLETE, "initial E2u RX failed");
	}
	CHECK(v92_upstream_data_rx_init(&rx, &initial_rx.signal, &old_mapping, &old_plan) ==
	      V92_UPSTREAM_DATA_RX_OK, "finish data init failed");
	for (; cursor < ARRAY_LENGTH(wire); ++cursor) {
		unsigned input_frame = epoch + rx.line.received_frames;
		if (!rx.line.symbol_count && input_frame == CONTROL)
			CHECK(v92_upstream_data_rx_reset_control(&rx) == V92_UPSTREAM_DATA_RX_OK, "RX reset failed");
		if (switch_at && input_frame == switch_at && !epoch && !rx.line.symbol_count) {
			CHECK(switch_at == NEW && b1u_switch_contracts(&rx, &new_mapping, &new_plan) &&
			      (!codec || (rx.model.valid == 3 && old_plan.gain != new_plan.gain)),
			      "finish handoff contracts failed");
			struct v92_upstream_data_line line = rx.line;
			enum v92_upstream_data_rx_result switched = v92_upstream_data_rx_start_b1u(&rx, &new_mapping, &new_plan);
			CHECK(switched == V92_UPSTREAM_DATA_RX_OK,
			      "new B1u RX failed: %u law=%u rates=%u->%u gain=%g->%g calibrated=%g [%g,%g]",
			      switched, law, old_rate, new_rate, line.plan.gain, new_plan.gain,
			      line.calibration.gain, line.calibration.minimum, line.calibration.maximum);
			line.received_frames = 0;
			line.plan = new_plan;
			if (codec) {
				double ratio = new_plan.gain / old_plan.gain;
				line.calibration.gain *= ratio;
				line.calibration.minimum = fmin(new_plan.gain, line.calibration.minimum * ratio);
				line.calibration.maximum = fmax(new_plan.gain, line.calibration.maximum * ratio);
				line.calibration.correlation = line.calibration.energy = 0;
				line.calibration.raw_sum = line.calibration.target_sum = 0;
				line.calibration.samples = 0;
				pcm_gain_init(&line.host.fit);
				line.host.gain = line.host.lower = line.host.upper = 0.0;
				CHECK(!rx.model.valid && rx.model.acquiring, "new B1u retained the old codec model");
			}
			else {
				const struct v92_pcm_level_fit cleared = { 0 };
				CHECK(memcmp(&rx.model.level, &cleared, sizeof(cleared)) == 0,
				      "new B1u retained the old level fit");
			}
			epoch = NEW;
			CHECK(v92_upstream_data_rx_is_valid(&rx) && memcmp(&line, &rx.line, sizeof(line)) == 0,
			      "B1u handoff disturbed warm physical state");
		}
		struct v92_upstream_data_frame frame;
		uint8_t code = (uint8_t)wire[cursor];
		int16_t decoded = law == G711_LAW_PCMU ? g711_decode_pcmu(code) : g711_decode_pcma(code);
		enum v92_upstream_data_rx_result r = codec ? fixture_octet(&rx, code,
			(int16_t)lround(.75 * decoded), true, &frame) :
			fixture_sample(&rx, wire_sample(&channel, wire[cursor]), &frame);
		CHECK(r == V92_UPSTREAM_DATA_RX_OK || r == V92_UPSTREAM_DATA_RX_FRAME, "finish RX failed: %u", r);
		if (rx.line.symbol_count) {
			struct v92_upstream_data_rx saved = rx;
			CHECK(v92_upstream_data_rx_start_b1u(&rx, &new_mapping, &new_plan) ==
			      V92_UPSTREAM_DATA_RX_INVALID_STATE && memcmp(&rx, &saved, sizeof(rx)) == 0,
			      "partial-frame B1u handoff accepted");
		}
		if (codec && r == V92_UPSTREAM_DATA_RX_FRAME && rx.line.received_frames == V92_B1U_ACQUISITION_FRAMES)
			CHECK(rx.model.valid == 3 && !rx.model.acquiring && fabs(rx.line.host.gain - .75) < 1e-12,
			      "B1u did not fit new codec models and host gain");
		if (r != V92_UPSTREAM_DATA_RX_FRAME || !frame.bit_count)
			continue;
		unsigned delivered = epoch + rx.line.received_frames - 1 - V92_PCM_RX_DELAY_FRAMES;
		CHECK(!frame.ambiguous && memcmp(frame.bits, expected[delivered], frame.bit_count) == 0,
		      "finish bits differ: law %u rates %u->%u frame %u", law, old_rate, new_rate, delivered);
		if (delivered == E2U)
			switch_at = rx.line.received_frames + V92_B1U_FRAMES - V92_PCM_RX_DELAY_FRAMES;
		else if (delivered > E2U && delivered < NEW)
			fb1u_count++;
		else if (delivered >= NEW + V92_B1U_FRAMES)
			data_count++;
	}
	unsigned delayed = codec ? (new_plan.delay + V92_PCM_MAPPING_FRAME_SYMBOLS - 1) /
		V92_PCM_MAPPING_FRAME_SYMBOLS : 1;
	CHECK(epoch == NEW && fb1u_count == V92_B1U_FRAMES - V92_PCM_RX_DELAY_FRAMES &&
	      data_count == 26 - V92_PCM_RX_DELAY_FRAMES - delayed,
	      "finish incomplete: epoch=%u FB1u=%u data=%u", epoch, fb1u_count, data_count);
	return true;
}

static bool test_owned_boundaries(void)
{
	struct channel channel = { .gain = 1.0, .law = G711_LAW_PCMU, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_rx initial, receiver;
	struct v92_upstream_data_frame frame, sentinel;

	CHECK(prepare_e2u(&channel, false, &e2u_tx, &e2u_rx) &&
	      v92_cpd_direct_init(&cpd, channel.law, 1u, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, channel.law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_rx_init(&initial, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "owned boundary fixture failed");
	memset(&sentinel, 0xa5, sizeof(sentinel));
	for (unsigned fault = 0; fault < 4; ++fault) {
		receiver = initial;
		frame = sentinel;
		enum v92_upstream_data_rx_result expected, result;

		if (fault == 0) {
			receiver.failed = true;
			expected = V92_UPSTREAM_DATA_RX_FAILED;
		} else if (fault == 1) {
			receiver.line.input_samples = UINT64_MAX;
			expected = V92_UPSTREAM_DATA_RX_INVALID_STATE;
		} else {
			expected = V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE;
			if (fault == 2) {
				receiver.line.equalizer.history[0].in_phase = DBL_MAX;
				receiver.line.equalizer.coefficients[1].in_phase = 2.0;
			} else {
				for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS - 1; ++i)
					CHECK(fixture_sample(&receiver, 0, &frame) ==
						      V92_UPSTREAM_DATA_RX_OK, "owned late-failure buffering failed");
				receiver.line.observations[0].history[0].in_phase = DBL_MAX;
			}
		}
		struct v92_upstream_data_rx saved = receiver;
		if (fault >= 2)
			saved.failed = true;
		CHECK(receive_compared(&receiver, fault == 2 ? INT16_MAX : 0, &frame, &result) &&
		      result == expected && memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
		      memcmp(&frame, &sentinel, sizeof(frame)) == 0,
		      "owned failure consumed input or wrote output: %u", fault);
	}
	receiver = initial;
	CHECK(fixture_sample_owned(NULL, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      fixture_sample_owned(&receiver, 0, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      fixture_sample_owned(&receiver, 0, (void *)&receiver) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &initial, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &sentinel, sizeof(frame)) == 0,
	      "owned arguments or aliased output mutated state");
	return true;
}

static bool test_rejection_and_failure(void)
{
	struct channel channel = { .gain = 1.0, .law = G711_LAW_PCMU, .polarity = 1 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_rx receiver;
	struct v92_upstream_data_rx saved;
	struct v92_upstream_data_frame frame;
	struct v92_upstream_data_frame saved_frame;

	CHECK(prepare_e2u(&channel, false, &e2u_tx, &e2u_rx) &&
	      v92_cpd_direct_init(&cpd, channel.law, 1u, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, channel.law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "rejection fixture failed");
	const double slower_steps[] = { 0.0, 0.001 };
	for (size_t index = 0u; index < sizeof(slower_steps) / sizeof(slower_steps[0]); ++index) {
		struct v92_upstream_rx slower = e2u_rx.signal;
		struct v92_upstream_data_rx initialized;

		slower.line.adaptation = slower_steps[index];
		CHECK(v92_upstream_data_rx_init(&initialized, &slower, &mapping, &direct_plan) ==
			      V92_UPSTREAM_DATA_RX_OK &&
		      initialized.line.adaptation == slower_steps[index] &&
		      memcmp(&initialized.line.equalizer, &slower.line.equalizer,
			      sizeof(slower.line.equalizer)) == 0,
		      "data handoff increased a slower step or changed the learned FIR");
	}
	saved = receiver;
	memset(&frame, 0xa5, sizeof(frame));
	saved_frame = frame;
	CHECK(!v92_upstream_data_rx_is_valid(NULL) &&
	      !v92_upstream_data_rx_b1u_complete(NULL) &&
	      v92_upstream_data_rx_reset_control(NULL) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      v92_upstream_data_rx_start_b1u(&receiver, &mapping, &direct_plan) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      v92_upstream_data_rx_init(NULL, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, NULL, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, NULL, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &receiver.line.plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &receiver.decoder.mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      fixture_sample(NULL, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      fixture_sample(&receiver, 0, NULL) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      fixture_sample(&receiver, 0, (void *)&receiver) ==
		      V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "invalid call changed state or output");
	e2u_rx.signal.training_tail_remaining = 1u;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "unconsumed TRN2u tail accepted");
	e2u_rx.signal.training_tail_remaining = 0u;
	mapping.law = G711_LAW_PCMA;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS,
	      "conflicting PCM law accepted");
	receiver.line.input_samples = UINT64_MAX;
	CHECK(fixture_sample(&receiver, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "input clock overflow accepted");
	receiver = saved;
	receiver.failed = true;
	struct v92_upstream_data_rx failed = receiver;

	CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_FAILED &&
	      memcmp(&receiver, &failed, sizeof(failed)) == 0,
	      "control reset revived a failed receiver");
	receiver.line.symbol_count = V92_PCM_MAPPING_FRAME_SYMBOLS;
	failed = receiver;
	CHECK(v92_upstream_data_rx_reset_control(&receiver) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
	      memcmp(&receiver, &failed, sizeof(failed)) == 0,
	      "control reset changed invalid line state");
	receiver = saved;
	receiver.line.equalizer.history[0].in_phase = DBL_MAX;
	receiver.line.equalizer.coefficients[1].in_phase = 2.0;
	saved = receiver;
	saved.failed = true;
	CHECK(v92_upstream_data_rx_is_valid(&receiver) &&
	      fixture_sample(&receiver, INT16_MAX, &frame) ==
		      V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      fixture_sample(&receiver, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_FAILED &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "DSP failure advanced state, changed output, or did not latch");
	mapping.law = G711_LAW_PCMU;
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "late-failure initialization failed");
	for (size_t sample = 0u; sample < V92_PCM_MAPPING_FRAME_SYMBOLS - 1u; ++sample)
		CHECK(fixture_sample(&receiver, 0, &frame) ==
			      V92_UPSTREAM_DATA_RX_OK, "frame buffering failed");
	receiver.line.observations[0].history[0].in_phase = DBL_MAX;
	saved = receiver;
	saved.failed = true;
	CHECK(v92_upstream_data_rx_is_valid(&receiver) &&
	      fixture_sample(&receiver, 0, &frame) ==
		      V92_UPSTREAM_DATA_RX_NUMERIC_FAILURE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
	      "late adaptation failure committed the decoded frame");
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "damaged-B1u initialization failed");
	double training_error = 0.0;

	for (size_t sample = 0u; sample < V92_B1U_SYMBOLS; ++sample) {
		enum v92_upstream_data_rx_result result =
			fixture_sample(&receiver, 0, &frame);

		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "damaged training failed to report evidence");
		if (result == V92_UPSTREAM_DATA_RX_FRAME)
			training_error += frame.squared_error;
	}
	CHECK(training_error > (double)V92_B1U_SYMBOLS *
		      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR &&
	      v92_upstream_data_rx_b1u_complete(&receiver),
	      "known B1u bits hid unusable samples or moved the boundary");
	/* Once decisions are unknown, a large excursion must report damage
	 * without adapting the FIR toward that unreliable decoded point. */
	memset(receiver.line.equalizer.coefficients, 0,
	       sizeof(receiver.line.equalizer.coefficients));
	receiver.line.equalizer.coefficients[0].in_phase = 4.0;
	struct qam_point coefficients[QAM_EQUALIZER_MAX_TAP_COUNT];

	memcpy(coefficients, receiver.line.equalizer.coefficients, sizeof(coefficients));
	for (size_t sample = 0u; sample < V92_PCM_MAPPING_FRAME_SYMBOLS; ++sample)
		CHECK(fixture_sample(&receiver, INT16_MAX, &frame) ==
			      (sample + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS ?
				       V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "unusable data stopped reporting evidence");
	CHECK(frame.squared_error > 12.0 * 65535.0 * 65535.0 &&
	      memcmp(coefficients, receiver.line.equalizer.coefficients, sizeof(coefficients)) == 0,
	      "unusable data hid its residual or trained the FIR");
	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &direct_plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "clipped-input initialization failed");
	memset(receiver.line.equalizer.coefficients, 0,
	       sizeof(receiver.line.equalizer.coefficients));
	receiver.line.equalizer.coefficients[0].in_phase = 4.0;
	for (size_t sample = 0u; sample < V92_PCM_MAPPING_FRAME_SYMBOLS; ++sample)
		CHECK(fixture_sample(&receiver, INT16_MAX, &frame) ==
			      (sample + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS ?
				       V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "finite equalizer excursion stopped reception");
	CHECK(frame.squared_error > 12.0 * 65535.0 * 65535.0,
	      "fixed-point clipping hid the equalizer residual");
	struct v92_upstream_plan plan = {
		.gain = 2.0, .delay = 60u, .observation = V92_PCM_FRAME_CONSTELLATION,
	};

	CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &plan) ==
		      V92_UPSTREAM_DATA_RX_OK, "raw-plan initialization failed");
	saved = receiver;
	for (unsigned int mutation = 0u; mutation < 5u; ++mutation) {
		struct v92_upstream_plan bad = plan;

		switch (mutation) {
		case 0: bad.gain = NAN; break;
		case 1: bad.delay = 0u; break;
		case 2: bad.gain = 0.0; break;
		case 3: bad.delay = V92_B1U_SYMBOLS; break;
		case 4: bad.observation = V92_PCM_FRAME_FILTER_OUTPUT; break;
		}
		CHECK(v92_upstream_data_rx_init(&receiver, &e2u_rx.signal, &mapping, &bad) ==
			      V92_UPSTREAM_DATA_RX_INVALID_PARAMETERS &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0, "invalid raw plan changed receiver");
	}
	receiver.decoder.observation = V92_PCM_FRAME_FILTER_OUTPUT;
	CHECK(!v92_upstream_data_rx_is_valid(&receiver), "conflicting receive coordinates accepted");
	saved_frame = frame;
	for (unsigned int mutation = 0u; mutation < 15u; ++mutation) {
		receiver = saved;
		switch (mutation) {
		case 0: receiver.line.calibration.gain = NAN; break;
		case 1: receiver.line.calibration.minimum = 0.0; break;
		case 2: receiver.line.calibration.maximum = INFINITY; break;
		case 3: receiver.line.calibration.gain = plan.gain * 2.0; break;
		case 4: receiver.line.calibration.correlation = NAN; break;
		case 5: receiver.line.calibration.energy = -1.0; break;
		case 6: receiver.line.calibration.energy = 1.0; break;
		case 7: receiver.line.calibration.correlation = 1.0; break;
		case 8: receiver.line.calibration.offset = NAN; break;
		case 9: receiver.line.calibration.offset = V92_DATA_MAX_RMS_ERROR + 1.0; break;
		case 10: receiver.line.calibration.raw_sum = INFINITY; break;
		case 11: receiver.line.calibration.target_sum = NAN; break;
		case 12: receiver.line.calibration.samples = 1u; break;
		case 13: receiver.line.calibration.samples = V92_B1U_SYMBOLS; break;
		case 14: receiver.line.calibration.raw_sum = 1.0; break;
		}
		struct v92_upstream_data_rx invalid = receiver;

		CHECK(!v92_upstream_data_rx_is_valid(&receiver) &&
		      fixture_sample(&receiver, 0, &frame) == V92_UPSTREAM_DATA_RX_INVALID_STATE &&
		      memcmp(&receiver, &invalid, sizeof(invalid)) == 0 &&
		      memcmp(&frame, &saved_frame, sizeof(frame)) == 0,
		      "invalid gain calibration consumed a sample or changed output");
	}
	for (unsigned int damage = 0u; damage < 4u; ++damage) {
		uint32_t random = UINT32_C(873941);

		receiver = saved;
		training_error = 0.0;
		for (unsigned int i = 0u; i < (unsigned int)V92_B1U_SYMBOLS + plan.delay - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int16_t sample = damage == 0u ? 0 : damage == 1u ? 12000 :
				damage == 2u ? (int16_t)((int)(random >> 16) - 32768) : INT16_MAX;
			enum v92_upstream_data_rx_result result = fixture_sample(&receiver, sample, &frame);

			CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
			      "damaged raw training stopped evidence collection");
			if (result == V92_UPSTREAM_DATA_RX_FRAME)
				training_error += frame.squared_error;
		}
		CHECK(v92_upstream_data_rx_b1u_complete(&receiver) &&
		      training_error > (double)V92_B1U_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR &&
		      receiver.line.calibration.gain >= receiver.line.calibration.minimum &&
		      receiver.line.calibration.gain <= receiver.line.calibration.maximum,
		      "bounded raw calibration hid damaged B1u (%u)", damage);
		if (damage == 0u)
			CHECK(receiver.line.calibration.gain == plan.gain && receiver.line.calibration.energy == 0.0,
			      "silence changed nominal gain");
	}
	receiver = saved;
	receiver.line.polarity = -1;
	receiver.line.delay_remaining = 0u;
	CHECK(fixture_sample(&receiver, INT16_MIN, &frame) == V92_UPSTREAM_DATA_RX_OK &&
	      receiver.line.raw[0] == 32768 && receiver.line.samples[0] == INT16_MAX,
	      "polarity reversal overflowed or lost original raw excursion");
	for (unsigned int i = 1u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		CHECK(fixture_sample(&receiver, INT16_MIN, &frame) ==
			      (i + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS ? V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "raw excursion stopped receiving");
	CHECK(frame.squared_error > 12.0 * 20000.0 * 20000.0,
	      "raw decoder clipping hid the residual");
	return true;
}

enum channel_fault {
	CHANNEL_OK,
	CHANNEL_NOISE,
	CHANNEL_WRONG_B1,
	CHANNEL_LOSS,
	CHANNEL_EXCESSIVE_OFFSET,
	CHANNEL_RESIDUAL,
};

/* E2u recognizes its last control symbol after already consuming ten B1u
 * samples. The channel-compensating filter has a different causal delay:
 * receive its remaining prefix exactly once, not as B1u or payload. */
struct channel_plan_workspace {
	struct v92_upstream_data_rx receiver;
	struct v92_upstream_data_rx comparison_rx;
	struct v92_upstream_data_rx owned;
	struct v92_upstream_data_rx lost;
	struct v92_upstream_data_rx seed_saved;
};

static bool check_channel_plan(
	struct channel_plan_workspace *work,
	enum g711_law law, int polarity, bool extended, bool biased, bool conditioned,
	bool changed_history, bool wire, int offset, enum channel_fault fault, const int *seed_offset)
{
	/* Match the captured marginal B1u's training amplitude. */
	int16_t lu = offset ? TEST_LU / 2 : TEST_LU;
	struct channel channel = {
		.gain = 1.0, .law = law, .polarity = polarity,
		.receive_gain = conditioned ? 0.75 : 0.0,
	};
	struct v92_upstream_channel measured = { 0 };
	struct v92_upstream_plan plan;
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_upstream_data_tx transmitter;
	struct v92_upstream_data_line trained;
	uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
	unsigned int source_frames = 0u, leading = 0u, delivered = 0u;
	double b1_error = 0.0, comparison_error = 0.0;
	unsigned comparison_bad_bits = 0u, comparison_payload = 0u;
	bool bad_b1 = false;
	uint32_t random = UINT32_C(9376271);

	for (unsigned int i = 0u; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);

		CHECK(v92_upstream_channel_sample(&measured, sample,
			    polarity * wire_sample(&channel, sample)), "channel measurement failed");
	}
	CHECK(v92_cpd_direct_init(&cpd, law, 1u, extended) == V92_CPD_OK &&
	      v92_cpd_channel_design(&cpd, &measured, law, lu, &plan),
	      "channel-plan design failed");
	if (wire) {
		/* A small residual prefilter tap exercises the general-coordinate
		 * path, outside the optional pure-delay codec-model fitter. */
		struct v92_cpd_filter *filter = &cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD];
		CHECK(plan.delay + 1u < ARRAY_LENGTH(filter->coefficient), "prefilter has no postcursor capacity");
		if (filter->count < plan.delay + 2u)
			filter->count = plan.delay + 2u;
		filter->coefficient[plan.delay + 1u] = filter->coefficient[plan.delay] / 256;
	}
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&transmitter, &mapping, lu) == V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u_at_level(&channel, extended, &e2u_tx, &e2u_rx,
		      lu, lu, 0.002, V92_CONSTELLATION_4_POINT), "channel-plan fixture failed");
	/* A receive-side coding-context mismatch must settle without poisoning
	 * the gain fit. This tests acquisition, not a different normative seed. */
	transmitter.encoder.state.scrambler.history = changed_history;
	/* A sparse training-level estimate can bias the nominal inverse without
	 * changing the peer's CPd waveform. Known B1u must refine that scalar. */
	if (biased)
		plan.gain *= 1.02;
	while (!v92_e2u_rx_is_complete(&e2u_rx)) {
		int16_t sample;

		CHECK(leading < V92_B1U_SYMBOLS, "E2u did not complete before B1u ended");
		if (!v92_phase4_record_tx_is_complete(&e2u_tx)) {
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "E2u transmit failed");
		} else {
			CHECK(v92_upstream_data_tx_next(&transmitter, source, &source_frames, &sample) ==
				      V92_UPSTREAM_DATA_TX_OK, "early B1u transmit failed");
			++leading;
		}
		enum v92_e2u_rx_result result = v92_e2u_rx_sample(&e2u_rx, wire_sample(&channel, sample));

		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE, "E2u receive failed");
	}
	CHECK(leading == V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY &&
	      v92_upstream_data_rx_init(&work->receiver, &e2u_rx.signal, &mapping, &plan) == V92_UPSTREAM_DATA_RX_OK &&
	      work->receiver.line.delay_remaining == plan.delay - leading, "E2u/data delay lost");
	CHECK(!wire || !work->receiver.model.acquiring, "wire fixture entered pure-delay fitter");
	channel.offset = offset;
	if (fault == CHANNEL_RESIDUAL) {
		channel.residual_first = -.027;
		channel.residual_second = .011;
	}
	if (seed_offset) {
		struct v92_upstream_seed seed = {
			.residual = { .active = true, .offset = *seed_offset,
				.history = { 99.0 }, .cursor = 3 },
			.gain = work->receiver.line.calibration.gain,
		};
		work->seed_saved = work->receiver;
		CHECK(!v92_upstream_model_seed(NULL, &seed) &&
		      !v92_upstream_model_seed(&work->receiver, NULL), "seed accepted a null owner/input");
		seed.gain = NAN;
		CHECK(!v92_upstream_model_seed(&work->receiver, &seed), "seed accepted a nonfinite gain");
		seed.gain = work->receiver.line.calibration.maximum * 1.01;
		CHECK(!v92_upstream_model_seed(&work->receiver, &seed) &&
		      !memcmp(&work->receiver, &work->seed_saved, sizeof(work->seed_saved)), "out-of-cell seed changed receiver");
		seed.gain = work->receiver.line.calibration.gain;
		CHECK(v92_upstream_model_seed(&work->receiver, &seed) && v92_upstream_data_rx_is_valid(&work->receiver) &&
		      !memcmp(&work->receiver.decoder, &work->seed_saved.decoder, sizeof(work->seed_saved.decoder)) &&
		      memory_region_is_zero(work->receiver.model.seed.residual.history, sizeof(seed.residual.history)) &&
		      work->receiver.model.seed.residual.cursor == 0, "seed retained coding or physical history");
	}
	work->comparison_rx = work->receiver;
	if (biased)
		work->comparison_rx.line.calibration.minimum = work->comparison_rx.line.calibration.maximum = plan.gain;
	else
		work->comparison_rx.line.polarity = -polarity;
	for (unsigned int symbol = 0u; delivered < DATA_FRAMES &&
	     symbol < (V92_B1U_FRAMES + DATA_FRAMES + V92_PCM_RX_DELAY_FRAMES + 6u) *
		     V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t sample;
		struct v92_upstream_data_frame frame;

		bool injected = false;
		if (fault == CHANNEL_WRONG_B1 && transmitter.b1u_symbols_remaining == V92_PCM_MAPPING_FRAME_SYMBOLS &&
		    transmitter.queue_count == 0u) {
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
			struct v92_pcm_frame wrong;
			memset(bits, 1, mapping.data_bits_per_frame);
			bits[mapping.data_bits_per_frame - 1u] = 0u;
			CHECK(v92_pcm_frame_encode(&transmitter.encoder, bits, mapping.data_bits_per_frame, &wrong) ==
			      V92_PCM_FRAME_OK, "wrong held-out B1u encode failed");
			for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
				CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, lu,
				      wrong.v[i], &transmitter.queue[i]) == V92_PCM_FRAME_OK, "wrong B1u scale failed");
			sample = transmitter.queue[0];
			transmitter.queue[0] = 0;
			transmitter.queue_count = V92_PCM_MAPPING_FRAME_SYMBOLS - 1u;
			--transmitter.b1u_symbols_remaining;
			injected = true;
		}
		CHECK(injected || v92_upstream_data_tx_next(&transmitter, source, &source_frames, &sample) ==
			      V92_UPSTREAM_DATA_TX_OK, "channel-plan data transmit failed");
		if (fault == CHANNEL_NOISE && work->receiver.line.received_frames >= V92_B1U_ACQUISITION_FRAMES &&
		    work->receiver.line.received_frames < V92_B1U_FRAMES) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			sample += (int)(random >> 24) - 128;
		}
		int16_t input = wire_sample(&channel, sample);
		bool first_frame = work->receiver.line.received_frames == 0u &&
			work->receiver.line.symbol_count == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u;
		struct v92_pcm_frame_decision predicted;
		int32_t raw[V92_PCM_MAPPING_FRAME_SYMBOLS];

		if (first_frame) {
			struct v92_pcm_frame_rx decoder = work->receiver.decoder;
			int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

			memcpy(samples, work->receiver.line.samples, sizeof(samples));
			memcpy(raw, work->receiver.line.raw, sizeof(raw));
			raw[V92_PCM_MAPPING_FRAME_SYMBOLS - 1u] = polarity * (int32_t)input;
			samples[V92_PCM_MAPPING_FRAME_SYMBOLS - 1u] = (int16_t)lround(
				polarity * input * work->receiver.line.calibration.gain);
			CHECK(v92_pcm_frame_decode(&decoder, samples, true, bits,
					mapping.data_bits_per_frame, &predicted) ==
				      V92_PCM_FRAME_OK, "linear quality prediction failed");
		}
		enum v92_upstream_data_rx_result result;
		if (wire) {
			work->owned = work->receiver;
			struct v92_upstream_data_frame output;

			memset(&frame, 0xa5, sizeof(frame)); output = frame;
			bool available = fault != CHANNEL_LOSS || work->receiver.line.received_frames != 20u;
			result = fixture_octet(&work->receiver, channel.code, input, available, &frame);
			CHECK(fixture_octet_owned(&work->owned, channel.code, input, input,
			      available, &output) == result && !memcmp(&work->owned, &work->receiver, sizeof(work->owned)) &&
			      !memcmp(&output, &frame, sizeof(output)) && v92_upstream_data_rx_is_valid(&work->receiver),
			      "owned wire-cell stream diverged");
		} else {
			CHECK(receive_compared(&work->receiver, input, &frame, &result),
			      "owned raw-coordinate stream diverged");
		}

		if (first_frame) {
			double error = 0.0;

			for (size_t i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
				double residual = raw[i] -
					predicted.predicted_samples[i] / work->receiver.line.calibration.gain;

				error += residual * residual;
			}
			CHECK(result == V92_UPSTREAM_DATA_RX_FRAME && fabs(frame.squared_error - error) < 1e-6,
			      "raw quality rounded, clipped or requantized its linear prediction");
		}

		{
			struct v92_upstream_data_frame comparison;
			enum v92_upstream_data_rx_result comparison_result =
				fixture_sample(&work->comparison_rx, input, &comparison);

			CHECK(comparison_result == result, "B1u comparison lost frame timing");
			if (comparison_result == V92_UPSTREAM_DATA_RX_FRAME) {
				unsigned frames = work->comparison_rx.line.received_frames;
				if (frames > V92_B1U_ACQUISITION_FRAMES && frames <= V92_B1U_FRAMES)
					comparison_error += comparison.squared_error;
				if (comparison.b1u && frames > V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
					comparison_bad_bits += comparison.ambiguous ||
						comparison.bit_count != mapping.data_bits_per_frame;
					for (unsigned bit = 0u; bit < comparison.bit_count; ++bit)
						comparison_bad_bits += comparison.bits[bit] != 1u;
				}
				if (comparison.bit_count && !comparison.b1u) {
					uint8_t wanted[V92_PCM_MAPPING_MAX_BITS];
					payload(wanted, mapping.data_bits_per_frame, comparison_payload++);
					CHECK(offset || (!comparison.ambiguous && comparison.bit_count == mapping.data_bits_per_frame &&
					      memcmp(wanted, comparison.bits, comparison.bit_count) == 0),
					      "comparison channel lost payload");
				}
			}
		}

		if (fault == CHANNEL_EXCESSIVE_OFFSET && result == V92_UPSTREAM_DATA_RX_NO_PATH) {
			return true;
		}
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "channel-plan receive failed: %d", result);
		CHECK(work->receiver.line.received_frames ==
			(symbol + 1u < plan.delay - leading ? 0u :
			 (symbol + 1u - (plan.delay - leading)) / V92_PCM_MAPPING_FRAME_SYMBOLS),
		      "channel-plan frame boundary moved");
		if (result != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (!wire)
			CHECK(memory_region_is_zero(&work->receiver.line.host, sizeof(work->receiver.line.host)),
			      "sample-only input invented host gain");
		else if (work->receiver.line.received_frames >= V92_B1U_ACQUISITION_FRAMES && fault != CHANNEL_LOSS)
			CHECK(fabs(work->receiver.line.host.gain - (conditioned ? .75 : 1.0)) < 0.001,
			      "wire cells confused host gain with analogue calibration");
		if (work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES &&
		    work->receiver.line.received_frames <= V92_B1U_FRAMES)
			b1_error += frame.squared_error;
		if (!work->receiver.model.acquiring && !work->receiver.model.valid &&
		    work->receiver.line.received_frames + V92_PCM_RX_DELAY_FRAMES <= V92_B1U_ACQUISITION_FRAMES)
			CHECK(work->receiver.line.calibration.gain == plan.gain &&
			      work->receiver.line.calibration.correlation == 0.0 && work->receiver.line.calibration.energy == 0.0,
			      "unsettled coding predictions changed the acquisition gain at frame %llu",
			      (unsigned long long)work->receiver.line.received_frames);
		if (work->receiver.line.received_frames == V92_B1U_ACQUISITION_FRAMES)
			trained = work->receiver.line;
		else if (work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES)
			CHECK(memcmp(&work->receiver.line.calibration, &trained.calibration, sizeof(trained.calibration)) == 0 &&
			      memcmp(&work->receiver.line.host, &trained.host, sizeof(trained.host)) == 0,
			      "B1u validation or payload continued analogue/host gain calibration");
		if (frame.bit_count && frame.b1u &&
		    work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			bad_b1 |= frame.ambiguous;
			for (unsigned i = 0; i < frame.bit_count; ++i)
				bad_b1 |= frame.bits[i] != 1u;
		}
		if (fault && fault != CHANNEL_RESIDUAL && work->receiver.line.received_frames == V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			double host_gain = conditioned ? .75 : 1.0;
			if (fault == CHANNEL_NOISE)
				CHECK(b1_error / (host_gain * host_gain) >
				      V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
				      "frozen offset hid held-out noise");
			if (fault == CHANNEL_WRONG_B1)
				CHECK(bad_b1, "offset fitting hid a validly coded wrong B1u bit");
			if (fault == CHANNEL_LOSS)
				CHECK(work->receiver.line.calibration.offset == 0.0 && !work->receiver.line.residual.active,
				      "missing acquisition media supplied an offset fit");
			if (fault == CHANNEL_EXCESSIVE_OFFSET)
				CHECK(bad_b1 || b1_error / (host_gain * host_gain) >
				      V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
				      "an aliased offset fit hid a damaged B1u");
			return true;
		}
		if (work->receiver.line.received_frames >= V92_B1U_ACQUISITION_FRAMES)
			CHECK(memory_region_is_zero(&work->receiver.model.seed, sizeof(work->receiver.model.seed)),
			      "acquisition seed survived into held-out B1u");
		if (frame.bit_count == 0u || frame.b1u)
			continue;
		payload(expected, mapping.data_bits_per_frame, delivered);
		CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
		      memcmp(frame.bits, expected, frame.bit_count) == 0,
		      "channel-plan payload lost: law %u polarity %d wire %d frame %u",
		      law, polarity, wire, delivered);
		++delivered;
	}
	struct v92_upstream_data_line saved = work->receiver.line;

	CHECK(delivered == DATA_FRAMES, "channel-plan data did not arrive");
	/* Qualify the same held-out window as the call owner. Acquisition
	 * residual is still reported and independently checked above. */
	double quality_gain = wire ? work->receiver.line.host.gain : 1.0;
	CHECK(b1_error / (quality_gain * quality_gain) <
	      V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "channel-plan B1u residual too large: %g law=%u polarity=%d conditioned=%u offset=%d fit=%g gain=%g [%g,%g] nominal=%g residual=%g lags=%u,%u beta=%g,%g",
	      sqrt(b1_error / V92_B1U_VALIDATION_SYMBOLS), law, polarity, conditioned, offset,
	      work->receiver.line.calibration.offset, work->receiver.line.calibration.gain,
	      work->receiver.line.calibration.minimum, work->receiver.line.calibration.maximum, plan.gain,
	      work->receiver.line.residual.offset, work->receiver.line.residual.delay[0], work->receiver.line.residual.delay[1],
	      work->receiver.line.residual.coefficient[0], work->receiver.line.residual.coefficient[1]);
	CHECK(comparison_payload == DATA_FRAMES, "comparison channel shortened payload");
	if (offset) {
		CHECK(fabs(work->receiver.line.calibration.offset + work->receiver.line.residual.offset - offset * (conditioned ? .75 : 1.0)) < 16.0,
		      "offset fit missed the physical bias: %g expected %g", work->receiver.line.calibration.offset + work->receiver.line.residual.offset,
		      offset * (conditioned ? .75 : 1.0));
		CHECK(!bad_b1 && b1_error < comparison_error / 2.0,
		      "offset correction did not improve independently validated B1u");
	}
	if (biased) {
		CHECK(comparison_error / (quality_gain * quality_gain) >
		      V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
		      "fixed gain hid bad training: %g", sqrt(comparison_error / V92_B1U_VALIDATION_SYMBOLS));
		CHECK(work->receiver.line.calibration.gain < plan.gain &&
		      work->receiver.line.calibration.gain >= work->receiver.line.calibration.minimum,
		      "gain fixture did not recover rejected B1u within training bounds");
	} else if (!offset) {
		/* Unconstrained coding can acquire the inverted constellation too:
		 * polarity alone is not damage. Require its independent B1u and all
		 * payload to pass, not a large forced-history acquisition residual. */
		CHECK(comparison_bad_bits == 0u && comparison_error <
		      V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
		      "inverted constellation did not acquire valid B1u");
	}
	if (fault == CHANNEL_RESIDUAL) {
		work->lost = work->receiver;
		struct v92_upstream_data_frame output, unchanged;
		memset(&output, 0xa5, sizeof(output)); unchanged = output;
		CHECK(work->receiver.line.residual.active &&
		      fixture_octet(&work->lost, channel.code, 0, false, &output) == V92_UPSTREAM_DATA_RX_NO_PATH &&
		      work->lost.failed && v92_upstream_data_rx_is_valid(&work->lost) &&
		      !memcmp(&output, &unchanged, sizeof(output)) &&
		      !memcmp(&work->lost.line.residual, &work->receiver.line.residual, sizeof(work->receiver.line.residual)),
		      "missing data media advanced residual history or corrupted receiver state");
	}
	CHECK(memcmp(&work->receiver.line.plan, &plan, sizeof(plan)) == 0,
	      "B1u calibration changed the negotiated receive plan");
	CHECK(check_control_reset(&work->receiver) &&
	      v92_upstream_data_rx_reset_control(&work->receiver) == V92_UPSTREAM_DATA_RX_OK &&
	      memcmp(&saved, &work->receiver.line, sizeof(saved)) == 0,
	      "control reset changed channel plan or line timing");
	return true;
}

static bool run_channel_plan(
	enum g711_law law, int polarity, bool extended, bool biased, bool conditioned,
	bool changed_history, bool wire, int offset, enum channel_fault fault, const int *seed_offset)
{
	struct channel_plan_workspace *work = malloc(sizeof(*work));
	CHECK(work, "channel plan workspace allocation failed");
	bool result = check_channel_plan(work, law, polarity, extended, biased, conditioned,
		changed_history, wire, offset, fault, seed_offset);
	free(work);
	return result;
}

static bool test_channel_plan(
	enum g711_law law, int polarity, bool extended, bool biased, bool conditioned,
	bool changed_history, bool wire, int offset, enum channel_fault fault)
{
	return run_channel_plan(law, polarity, extended, biased, conditioned,
		changed_history, wire, offset, fault, NULL);
}

static bool test_rate_request_contracts(const struct v92_upstream_rx *handoff)
{
	struct v92_ru_line_rx receiver, saved;
	CHECK(v92_ru_line_rx_init_after_e2u(&receiver, handoff), "warm Ru handoff failed");
	saved = receiver;
	CHECK(!v92_ru_line_rx_init_after_e2u(NULL, handoff) &&
	      !v92_ru_line_rx_init_after_e2u(&receiver, NULL) &&
	      !v92_ru_line_rx_is_valid(NULL) &&
	      v92_ru_line_rx_sample(NULL, 0) == V92_RU_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0, "null Ru contract changed state");
	for (unsigned i = 0; i < 3; ++i) {
		struct v92_upstream_rx invalid = *handoff;
		if (i == 0)
			invalid.failed = true;
		else if (i == 1)
			invalid.training_tail_remaining = 1;
		else
			invalid.line.equalizer.coefficients[0].in_phase = NAN;
		CHECK(!v92_ru_line_rx_init_after_e2u(&receiver, &invalid) &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0, "invalid E2u handoff changed Ru receiver");
	}
	for (unsigned i = 0; i < 5; ++i) {
		receiver = saved;
		switch (i) {
		case 0: receiver.input_samples = UINT64_MAX; break;
		case 1: receiver.equalizer.coefficients[0].in_phase = NAN; break;
		case 2: receiver.signal.phase = V92_RU_RX_FRAME_SYMBOLS; break;
		case 3: receiver.equalizer.decision_delay--; break;
		case 4: receiver.polarity = 0; break;
		}
		struct v92_ru_line_rx before = receiver;
		CHECK(v92_ru_line_rx_sample(&receiver, INT16_MIN) == V92_RU_RX_INVALID_STATE &&
		      memcmp(&receiver, &before, sizeof(before)) == 0, "invalid Ru sample changed state");
	}
	return true;
}

struct rate_exchange {
	struct v92_phase4_digital_station station;
	struct pcm_downstream_rx peer;
	unsigned zero_acknowledgements;
	unsigned withheld_transmit_samples;
	bool complete;
};

static bool rate_exchange_init(struct rate_exchange *exchange,
	const struct v92_ru_line_rx *ru, const struct v92_cpd_parameters *cpd,
	const struct v92_upstream_plan *plan)
{
	struct v92_cpt_parameters cpt = {
		.law = ru->law, .selected_rate_index = 9, .constellation_count = 1,
		.sign_redundancy = 1, .spectral_lookahead = 2,
	};
	struct v92_cpu_parameters cpu = {
		.law = ru->law, .data_rate_number = 11, .constellation_count = 1,
		.sign_redundancy = 1, .spectral_lookahead_frames = 2,
		.shaping_a1_q1_6 = 7, .acknowledges_cpd = true,
	};
	struct v92_cpd_parameters previous = *cpd;
	struct v90_pcm_mapping training, data, control;
	struct pcm_downstream_tx tx;

	for (unsigned ucode = 16; ucode < 112; ++ucode)
		cpu.transmit_constellations[0].ucode_mask[ucode / 8] |= (uint8_t)(1u << (ucode % 8));
	for (unsigned ucode = 24; ucode < 120; ucode += 12)
		cpt.transmit_constellations[0].ucode_mask[ucode / 8] |= (uint8_t)(1u << (ucode % 8));
	previous.acknowledges_cpu = true;
	/* A responder can still be sending Rd when Ru-bar ends. Keep receiving
	 * the peer's fresh TRN2u while the independent transmit clock waits. */
	exchange->withheld_transmit_samples = ru->constellation == V92_CONSTELLATION_8_POINT ? 384u : 0u;
	CHECK(v90_pcm_mapping_init_from_v92_cpt(&training, &cpt, cpt.law) == V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_init_from_v92_cpu(&data, &cpu, cpu.law) == V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_init_rate_renegotiation(&control, &training, &data) == V90_PCM_MAPPING_OK &&
	      pcm_downstream_tx_init(&tx, PCM_PROTOCOL_V92, &control) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(&exchange->peer, PCM_PROTOCOL_V92, &control,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK,
	      "rate exchange downstream mapping failed");
	struct v92_phase4_digital_station *station = &exchange->station;
	memset(station, 0xa5, sizeof(*station));
	struct v92_phase4_digital_station saved = *station;
	for (unsigned invalid = 0; invalid < 5; ++invalid) {
		struct v92_ru_line_rx candidate_ru = *ru;
		struct v92_upstream_plan candidate_plan = *plan;
		struct v92_cpu_parameters candidate_cpu = cpu;
		struct v92_cpd_parameters candidate_cpd = previous;
		switch (invalid) {
		case 0: candidate_ru.signal.bar_symbols--; break;
		case 1: candidate_plan.delay = UINT16_MAX; break;
		case 2: candidate_cpu.data_rate_number = 0; break;
		case 3: candidate_cpd.data_rate_number = 0; break;
		case 4: candidate_cpu.law = (enum g711_law)99; break;
		}
		CHECK(v92_phase4_digital_station_init_after_ru(station, &candidate_ru, &tx,
			      &candidate_cpd, &candidate_plan, &candidate_cpu, 161) != V92_PHASE4_DIGITAL_STATION_OK &&
		      memcmp(station, &saved, sizeof(saved)) == 0,
		      "invalid rate exchange input %u changed station", invalid);
	}
	CHECK(v92_phase4_digital_station_init_after_ru(station, ru, &tx,
		      &previous, plan, &cpu, 161) == V92_PHASE4_DIGITAL_STATION_OK &&
	      v92_phase4_digital_rx_is_renegotiating(&station->receive) &&
	      !station->control.cpu_received && !station->control.suvu_received &&
	      !station->control.cpu.acknowledges_cpd && !station->control.peer_confirmation_received &&
	      !station->control.transmit_acknowledges_cpu && !station->control.cleardown.active &&
	      !v92_phase4_digital_station_is_ed_ready(station) &&
	      station->control.transmit.control_configured &&
	      memcmp(&station->upstream_plan, plan, sizeof(*plan)) == 0,
	      "rate exchange reused acknowledgements or lost the receive plan");
	cpu.acknowledges_cpd = false;
	CHECK(memcmp(&station->control.cpu, &cpu, sizeof(cpu)) == 0 &&
	      v92_cpd_decode(station->control.initial_cpd.sequence, sizeof(station->control.initial_cpd.sequence),
		      station->control.initial_cpd.sequence_bits, station->control.initial_cpd.frame_bits, &previous) == V92_CPD_OK &&
	      !previous.acknowledges_cpu, "rate exchange lost prior CPu or reused old CPd acknowledgement");
	return true;
}

static bool rate_exchange_clock(struct rate_exchange *exchange, const int16_t *sample)
{
	struct v92_phase4_digital_station *station = &exchange->station;
	if (sample) {
		enum v92_phase4_digital_station_result r = v92_phase4_digital_station_receive(station, *sample);
		CHECK(r == V92_PHASE4_DIGITAL_STATION_OK || r == V92_PHASE4_DIGITAL_STATION_EVENT,
		      "post-Ru station receive failed: %s", v92_phase4_digital_station_result_name(r));
	}
	if (exchange->withheld_transmit_samples) {
		exchange->withheld_transmit_samples--;
		return true;
	}
	uint8_t octet;
	enum v92_phase4_digital_station_result r = v92_phase4_digital_station_transmit(station, &octet);
	if (r == V92_PHASE4_DIGITAL_STATION_CLEARDOWN_COMPLETE) {
		exchange->complete = true;
		CHECK(exchange->zero_acknowledgements && !station->control.transmit.ed_requested &&
		      station->control.transmit.transmitted_symbols >= station->control.cleardown.acknowledgement_end &&
		      station->control.transmit.transmitted_symbols >= station->control.cleardown.cp_end + 881u,
		      "cleardown completed without wire acknowledgement/deadline");
		return true;
	}
	CHECK(r == V92_PHASE4_DIGITAL_STATION_OK, "post-Ru station transmit failed: %s",
	      v92_phase4_digital_station_result_name(r));
	struct pcm_downstream_event event;
	enum pcm_downstream_rx_result received = pcm_downstream_rx_receive(&exchange->peer, octet, &event);
	CHECK(received == PCM_DOWNSTREAM_RX_BUFFERING || received == PCM_DOWNSTREAM_RX_TRN2D_FRAME ||
	      received == PCM_DOWNSTREAM_RX_SUVD || received == PCM_DOWNSTREAM_RX_CPD,
	      "post-Ru downstream receiver failed: %s", pcm_downstream_rx_result_name(received));
	if (received == PCM_DOWNSTREAM_RX_CPD && !event.control.record.cpd.parameters.data_rate_number) {
		CHECK(event.control.record.cpd.frame_result == V92_CPD_OK &&
		      event.control.record.cpd.parameters.acknowledges_cpu,
		      "zero-rate CPd did not acknowledge peer cleardown");
		exchange->zero_acknowledgements++;
	}
	return true;
}

static bool test_rate_request_line(enum g711_law law, int polarity,
				   bool extended, bool compensated,
				   enum v92_constellation_size constellation)
{
	struct channel channel = { .gain = 0.75, .postcursor = 0.10,
		.law = law, .polarity = polarity };
	struct v92_upstream_plan plan = { 0 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx record;
	struct v92_e2u_rx e2u;
	struct v92_upstream_data_tx data_tx;
	struct v92_upstream_data_rx data_rx;
	struct v92_ru_line_rx ru;
	struct v92_pcm_phase4_trn2u training_tx;
	struct v92_pcm_phase4_trn2u_line_rx training_rx, before;
	struct v92_pcm_phase4_trn2u_line_handoff handoff;
	struct v92_upstream_control_line_rx control;
	struct rate_exchange exchange = { 0 };
	unsigned source_frames = 0, delivered = 0;
	CHECK(v92_cpd_direct_init(&cpd, law, 1, extended) == V92_CPD_OK,
	      "rate-request mapping failed");
	if (compensated) {
		struct v92_upstream_channel measured = { 0 };
		uint32_t random = 9376271;
		for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
			random = random * UINT32_C(1664525) + UINT32_C(1013904223);
			int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);
			CHECK(v92_upstream_channel_sample(&measured, sample,
				    polarity * wire_sample(&channel, sample)), "measurement failed");
		}
		CHECK(v92_cpd_channel_design(&cpd, &measured, law, TEST_LU, &plan),
		      "channel design failed");
	}
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_tx_init(&data_tx, &mapping, TEST_LU) == V92_UPSTREAM_DATA_TX_OK &&
	      prepare_e2u_at_level(&channel, extended, &record, &e2u,
		TEST_LU, (int16_t)lround(TEST_LU * channel.gain), 0.002, constellation),
	      "rate-request fixture failed");
	while (!v92_e2u_rx_is_complete(&e2u)) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&record))
			CHECK(v92_phase4_record_tx_next(&record, &sample) ==
			      V92_PHASE4_RECORD_TX_OK, "E2u transmit failed");
		else
			CHECK(v92_upstream_data_tx_next(&data_tx, source, &source_frames, &sample) ==
			      V92_UPSTREAM_DATA_TX_OK, "B1u transmit failed");
		enum v92_e2u_rx_result r = v92_e2u_rx_sample(&e2u, wire_sample(&channel, sample));
		CHECK(r == V92_E2U_RX_OK || r == V92_E2U_RX_COMPLETE, "E2u failed");
	}
	CHECK(v92_upstream_data_rx_init(&data_rx, &e2u.signal, &mapping, &plan) ==
		V92_UPSTREAM_DATA_RX_OK && v92_ru_line_rx_init_after_e2u(&ru, &e2u.signal),
	      "parallel data/control-channel handoff failed");
	CHECK(test_rate_request_contracts(&e2u.signal), "rate-request contracts failed");
	memset(&training_rx, 0xa5, sizeof(training_rx));
	before = training_rx;
	CHECK(v92_pcm_phase4_trn2u_line_rx_init_after_ru(NULL, &ru) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init_after_ru(&training_rx, NULL) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_ARGUMENT &&
	      v92_pcm_phase4_trn2u_line_rx_init_after_ru(&training_rx, &ru) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_INVALID_STATE &&
	      memcmp(&training_rx, &before, sizeof(before)) == 0,
	      "unobserved Ru-bar manufactured a training boundary");
	/* Flush the decoder and either line plan before switching the transmitter
	 * at a physical frame boundary. The raw Ru FIR has its own shorter delay. */
	unsigned flush_frames = V92_PCM_RX_DELAY_FRAMES +
		(plan.delay + ru.equalizer.decision_delay + V92_PCM_MAPPING_FRAME_SYMBOLS - 1u) /
		V92_PCM_MAPPING_FRAME_SYMBOLS;
	while (source_frames < DATA_FRAMES + flush_frames || data_tx.queue_count) {
		int16_t sample;
		struct v92_upstream_data_frame frame;
		CHECK(v92_upstream_data_tx_next(&data_tx, source, &source_frames, &sample) ==
		      V92_UPSTREAM_DATA_TX_OK, "data transmit failed");
		sample = wire_sample(&channel, sample);
		CHECK(v92_ru_line_rx_sample(&ru, sample) == V92_RU_RX_BUFFERING,
		      "data caused a false rate request");
		enum v92_upstream_data_rx_result r = fixture_sample(&data_rx, sample, &frame);
		CHECK(r == V92_UPSTREAM_DATA_RX_OK || r == V92_UPSTREAM_DATA_RX_FRAME,
		      "data receive failed before rate request");
		if (r == V92_UPSTREAM_DATA_RX_FRAME && frame.bit_count && !frame.b1u) {
			uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
			payload(expected, mapping.data_bits_per_frame, delivered++);
			CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
			      memcmp(frame.bits, expected, frame.bit_count) == 0,
			      "PCM payload changed before rate request");
		}
	}
	CHECK(delivered >= DATA_FRAMES &&
	      memcmp(ru.equalizer.coefficients, e2u.signal.line.equalizer.coefficients,
		     sizeof(ru.equalizer.coefficients)) == 0,
	      "lost data (%u frames, law=%u polarity=%d extended=%u compensated=%u) or adapted the raw control channel",
	      delivered, law, polarity, extended, compensated);
	CHECK(v92_pcm_phase4_trn2u_init(&training_tx, ru.constellation, 0, TEST_LU) ==
	      V92_PCM_PHASE4_TRN2U_OK, "new training epoch failed");
	unsigned delay = ru.equalizer.decision_delay, detected = 0;
	for (unsigned i = 0; i < 408 + delay; ++i) {
		int16_t sample = i % 6u < 3u ? TEST_LU : -TEST_LU;
		if (i >= 384 && i < 408)
			sample = -sample;
		if (i >= 408)
			CHECK(v92_pcm_phase4_trn2u_next(&training_tx, &sample) ==
			      V92_PCM_PHASE4_TRN2U_OK, "early TRN2u failed");
		enum v92_ru_rx_result r = v92_ru_line_rx_sample(&ru, wire_sample(&channel, sample));
		if (r == V92_RU_RX_DETECTED)
			detected++;
		CHECK(r == V92_RU_RX_BUFFERING || r == V92_RU_RX_DETECTED ||
		      (r == V92_RU_RX_BAR_STARTED && i == 384 + delay) ||
		      (r == V92_RU_RX_COMPLETE && i == 407 + delay),
		      "lost delayed Ru boundary at %u: %u", i, r);
	}
	struct v92_ru_line_rx completed = ru;
	CHECK(detected == 1 && ru.signal.bar_symbols == 24 &&
	      v92_ru_line_rx_sample(&ru, 123) == V92_RU_RX_COMPLETE &&
	      memcmp(&ru, &completed, sizeof(ru)) == 0 &&
	      v92_pcm_phase4_trn2u_line_rx_init_after_ru(&training_rx, &ru) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_OK &&
	      memcmp(&training_rx.equalizer, &ru.equalizer, sizeof(ru.equalizer)) == 0 &&
	      training_rx.demodulator.descrambler.history == 0 &&
	      training_rx.demodulator.previous_encoded_sign == 0 && training_rx.renegotiation,
	      "Ru-bar handoff lost filter history or failed to reset coding");
	CHECK(rate_exchange_init(&exchange, &ru, &cpd, &plan), "rate exchange did not start");
	struct v92_cpu_parameters retained_cpu = exchange.station.control.cpu;
	while (!v92_pcm_phase4_trn2u_line_rx_can_handoff(&training_rx)) {
		int16_t sample;
		CHECK(training_tx.emitted_symbols < 12 &&
		      v92_pcm_phase4_trn2u_next(&training_tx, &sample) == V92_PCM_PHASE4_TRN2U_OK,
		      "early control boundary missed");
		sample = wire_sample(&channel, sample);
		enum v92_pcm_phase4_trn2u_line_rx_result r = v92_pcm_phase4_trn2u_line_rx_sample(&training_rx, sample);
		CHECK(rate_exchange_clock(&exchange, &sample), "rate exchange training failed");
		CHECK(r == V92_PCM_PHASE4_TRN2U_LINE_RX_OK ||
		      r == V92_PCM_PHASE4_TRN2U_LINE_RX_ALIGNED, "new TRN2u receive failed");
	}
	CHECK(v92_pcm_phase4_trn2u_line_rx_handoff(&training_rx, &handoff) ==
		V92_PCM_PHASE4_TRN2U_LINE_RX_OK && handoff.renegotiation &&
	      handoff.training_symbols == 12 &&
	      v92_upstream_control_line_rx_init(&control, &handoff) == V92_UPSTREAM_CONTROL_LINE_RX_OK,
	      "renegotiation control handoff failed");
	handoff.renegotiation = false;
	CHECK(!v92_pcm_phase4_trn2u_line_handoff_is_valid(&handoff),
	      "renegotiation weakened the initial training minimum");
	/* Exercise early SUVd-triggered termination and the ordinary 2400T train.
	 * The transmitter, not the control hunter, owns that policy. */
	unsigned train_symbols = compensated ? 1632 : 2400;
	while (training_tx.emitted_symbols < train_symbols) {
		int16_t sample;
		struct v92_control_event event;
		CHECK(v92_pcm_phase4_trn2u_next(&training_tx, &sample) == V92_PCM_PHASE4_TRN2U_OK,
		      "rate exchange training stopped");
		sample = wire_sample(&channel, sample);
		CHECK(v92_upstream_control_line_rx_sample(&control, sample, &event) == V92_UPSTREAM_CONTROL_LINE_RX_OK &&
		      rate_exchange_clock(&exchange, &sample), "training became a control record");
	}
	CHECK(exchange.station.receive.channel.symbols == 12 &&
	      v92_phase4_digital_station_select_upstream(&exchange.station, &cpd, &plan) ==
		      V92_PHASE4_DIGITAL_STATION_INVALID_STATE,
	      "short RRN training was treated as a complete channel measurement");
	const struct v92_suvu_parameters suvu = { .prefilter_level_db_q2_2 = -16 };
	struct v92_cpus_parameters cpus = { 0 };
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
	size_t bits;
	unsigned mapping_bits = V92_PCM_MAPPING_FRAME_SYMBOLS *
		v92_phase4_modulator_bits_per_symbol(constellation);
	unsigned suvu_count = 0, cpu_count = 0;
	CHECK(v92_suvu_encode(&suvu, mapping_bits, frame, sizeof(frame), &bits) == V92_SUVU_OK &&
	      v92_phase4_record_tx_init(&record, &training_tx.modulator, frame, sizeof(frame), bits) ==
		V92_PHASE4_RECORD_TX_OK, "SUVu encode failed");
	for (unsigned which = 0; which < 7; ++which) {
		if (which) {
			cpus.data_rate_number = which < 5 ? 10 : 0;
			cpus.acknowledges_cpd = which >= 3;
			CHECK(v92_cpus_encode(&cpus, mapping_bits, frame, sizeof(frame), &bits) == V92_CPU_OK &&
			      v92_phase4_record_tx_start_next(&record, frame, sizeof(frame), bits) ==
				V92_PHASE4_RECORD_TX_OK, "CPus encode failed");
		}
		while (!v92_phase4_record_tx_is_complete(&record)) {
			int16_t sample;
			struct v92_control_event event;
			CHECK(v92_phase4_record_tx_next(&record, &sample) == V92_PHASE4_RECORD_TX_OK,
			      "control transmit stopped");
			sample = wire_sample(&channel, sample);
			enum v92_upstream_control_line_rx_result r = v92_upstream_control_line_rx_sample(
				&control, sample, &event);
			CHECK(rate_exchange_clock(&exchange, &sample), "rate exchange control failed");
			CHECK(r == V92_UPSTREAM_CONTROL_LINE_RX_OK || r == V92_UPSTREAM_CONTROL_LINE_RX_EVENT,
			      "control receive stopped");
			if (r != V92_UPSTREAM_CONTROL_LINE_RX_EVENT)
				continue;
			if (event.type == V92_CONTROL_EVENT_SUVU) {
				CHECK(event.record.suvu.frame_result == V92_SUVU_OK &&
				      event.record.suvu.parameters.prefilter_level_db_q2_2 == -16,
				      "SUVu CRC/parameters did not survive rate request");
				suvu_count++;
			} else {
				CHECK(event.type == V92_CONTROL_EVENT_CPUS && event.record.cpus.frame_result == V92_CPU_OK &&
				      (event.record.cpus.parameters.data_rate_number == 0 ||
				       event.record.cpus.parameters.data_rate_number == 10),
				      "CPus did not survive rate request");
				cpu_count++;
			}
		}
		if (which == 2 || which == 4) {
			struct v92_cpu_parameters selected;
			retained_cpu.data_rate_number = 10;
			retained_cpu.acknowledges_cpd = which == 4;
			CHECK(v92_phase4_digital_station_cpu_parameters(&exchange.station, &selected) &&
			      memcmp(&selected, &retained_cpu, sizeof(selected)) == 0 &&
			      exchange.station.control.peer_confirmation_received == (which == 4) &&
			      !exchange.station.control.cleardown.active,
			      "CPus lost the retained mapping or reused an old acknowledgement");
		}
	}
	CHECK(suvu_count == 1 && cpu_count >= 1, "missing post-Ru status/cleardown record");
	CHECK(exchange.station.control.cleardown.active && exchange.station.control.cleardown.peer_requested &&
	      !v92_phase4_digital_station_is_ed_ready(&exchange.station), "zero CPus did not select cleardown");
	for (unsigned i = 0; i < 4000 && !exchange.complete; ++i)
		CHECK(rate_exchange_clock(&exchange, NULL), "rate exchange cleardown stopped");
	CHECK(exchange.complete, "post-Ru zero-rate exchange did not release in bounded time");
	return true;
}

/* Choose other legal points without changing the encoded bits or trellis.
 * Multiples of four preserve V.34 subset labels; multiples of stride*M
 * preserve V.92 6.4.2's residue class. Reconstruct the actual CPd waveform
 * and histories independently of the transmitter's minimum-power policy. */
static bool peer_frame(struct v92_pcm_frame_tx *tx, struct v92_pcm_frame *frame,
	struct v92_pcm_frame_filter_state initial, unsigned index, unsigned *changed)
{
	const struct v92_cpd_parameters *cpd = &tx->mapping.cpd;
	const struct v92_cpd_filter *p1 = &cpd->filter[V92_CPD_PRECODER_FEEDBACK];
	const struct v92_cpd_filter *z2 = &cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD];
	unsigned history = p1->count;

	CHECK(!cpd->filter[V92_CPD_PRECODER_FEED_FORWARD].count &&
	      !cpd->filter[V92_CPD_PREFILTER_FEEDBACK].count && z2->count,
	      "peer fixture requires feedback precoding and FIR prefiltering");
	if (history < z2->count - 1u) history = z2->count - 1u;
	for (unsigned s = 0; s < V92_PCM_MAPPING_FRAME_SYMBOLS; ++s) {
		const struct v92_cpd_constellation *set =
			&cpd->constellation[cpd->frame_constellation[s % V92_PCM_MAPPING_PHASES]];
		unsigned step = cpd->modulus[s] * (s % 4u == 3u ? 2u : 1u);
		int eta = frame->signed_index[s];

		CHECK(step % 4u == 0u, "peer fixture cannot preserve trellis labels");
		if ((index + s) % 3u == 0u) {
			if (eta + (int)step < set->count) { eta += step; ++*changed; }
			else if (eta - (int)step >= -(int)set->count) { eta -= step; ++*changed; }
		}
		frame->signed_index[s] = eta;
		frame->u[s] = (eta < 0 ? -1 : 1) * set->positive_linear[eta < 0 ? -eta - 1 : eta];
		int64_t x = (int64_t)frame->u[s] * 16384;
		for (unsigned k = 0; k < p1->count; ++k)
			x += (int64_t)initial.history[k] * p1->coefficient[k];
		frame->x[s] = round_shift_s64_to_s32_saturating(x, 14);
		int64_t v = (int64_t)frame->x[s] * z2->coefficient[0];
		for (unsigned k = 1; k < z2->count; ++k)
			v += (int64_t)initial.history[k - 1] * z2->coefficient[k];
		frame->v[s] = round_shift_s64_to_s32_saturating(v, 15);
		if (history) {
			memmove(initial.history + 1, initial.history, (history - 1u) * sizeof(initial.history[0]));
			initial.history[0] = frame->x[s];
		}
	}
	tx->state.filter = initial;
	return true;
}

/* Two codec conversions with channel memory and post-codec host gain. Keep
 * wire octets: requantizing the .75-scaled samples produces different cells. */
static int codec_echo(unsigned index)
{
	return 4 * ((int)(index * 17u % 43u) - 21);
}

/* The single final codec quantizes both signal and echo, which is then
 * independently removed at the host. Keep real wire octets and both host
 * sample versions; fitted predictions must not replace bit validation. */
struct scalar_model_workspace {
	struct v92_upstream_data_rx receiver;
	struct v92_upstream_data_rx initial;
	struct v92_upstream_data_rx independent;
	struct v92_upstream_data_rx linear;
	struct v92_upstream_data_rx saved;
	struct v92_upstream_data_rx clipped;
	struct v92_upstream_data_rx retained;
	struct v92_upstream_data_rx invalid;
};

static bool check_scalar_model(struct scalar_model_workspace *work, enum g711_law law, int polarity, bool wrong_bit)
{
	enum { FRAMES = V92_B1U_FRAMES + 24 + V92_PCM_RX_DELAY_FRAMES, PREFIX = 6 };
	struct channel channel = { .gain = .9, .law = law, .polarity = polarity };
	struct v92_upstream_channel measured = { 0 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_upstream_plan plan;
	struct v92_pcm_frame_tx transmitter;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	uint8_t codes[(FRAMES + PREFIX) * V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint32_t random = UINT32_C(0x35791bd);
	unsigned delivered = 0, bad = 0;
	double frozen = 0., error = 0., linear_error = 0.;
	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; i++) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);
		CHECK(v92_upstream_channel_sample(&measured, sample, polarity * wire_sample(&channel, sample)),
			"scalar channel measurement failed");
	}
	CHECK(v92_cpd_direct_select(&cpd, law, 19u, TEST_LU, false) == V92_CPD_OK &&
	      v92_cpd_channel_design(&cpd, &measured, law, TEST_LU, &plan) &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      prepare_e2u(&channel, false, &e2u_tx, &e2u_rx) &&
	      v92_upstream_data_rx_init(&work->receiver, &e2u_rx.signal, &mapping, &plan) == V92_UPSTREAM_DATA_RX_OK &&
	      work->receiver.model.acquiring, "scalar initialization failed");
	work->initial = work->receiver;
	for (unsigned f = 0; f < FRAMES + PREFIX; f++) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame frame;
		if (f < V92_B1U_FRAMES) memset(bits, 1, mapping.data_bits_per_frame);
		else payload(bits, mapping.data_bits_per_frame, f - V92_B1U_FRAMES);
		if (wrong_bit && f == V92_B1U_FRAMES - 1u) bits[0] = 0;
		CHECK(v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame, &frame) ==
			V92_PCM_FRAME_OK, "scalar encode failed");
		for (unsigned s = 0; s < V92_PCM_MAPPING_FRAME_SYMBOLS; s++) {
			int16_t sample;
			unsigned i = f * V92_PCM_MAPPING_FRAME_SYMBOLS + s;
			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LU, frame.v[s], &sample) == V92_PCM_FRAME_OK,
				"scalar frame scale failed");
			double mixed = polarity * (.9 * sample + codec_echo(i));
			CHECK(fabs(mixed) < 32000, "scalar fixture clipped");
			codes[i] = law == G711_LAW_PCMU ? g711_encode_pcmu((int16_t)lround(mixed)) :
				g711_encode_pcma((int16_t)lround(mixed));
		}
	}
	for (unsigned i = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; work->receiver.line.received_frames < FRAMES; i++) {
		struct v92_upstream_data_frame frame = { 0 }, compared = { 0 };
		work->independent = work->receiver;
		work->linear = work->receiver;
		work->linear.model.scalar_gain = 0.;
		int16_t host = law == G711_LAW_PCMU ? g711_decode_pcmu(codes[i]) : g711_decode_pcma(codes[i]);
		int16_t sample = host - polarity * codec_echo(i);
		enum v92_upstream_data_rx_result result = fixture_octet_owned(
			&work->receiver, codes[i], sample, host, true, &frame);
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
			"scalar receiver stopped: %u at %u", result, i);
		CHECK(fixture_octet_owned(&work->independent, codes[i], sample, host, true, &compared) == result &&
		      !memcmp(&work->receiver, &work->independent, sizeof(work->receiver)) && !memcmp(&frame, &compared, sizeof(frame)) &&
		      v92_upstream_data_rx_is_valid(&work->receiver), "scalar receivers shared mutable state");
		if (result != V92_UPSTREAM_DATA_RX_FRAME) continue;
		/* Removing only the quality predictor must leave every coding
		 * decision intact. Its original residual remains an independent
		 * comparison, not a fitting target during held-out B1u. */
		if (frozen) {
			CHECK(fixture_octet_owned(&work->linear, codes[i], sample, host, true, &compared) == result &&
			      !memcmp(&work->linear.decoder, &work->receiver.decoder, sizeof(work->linear.decoder)) &&
			      compared.bit_count == frame.bit_count && compared.ambiguous == frame.ambiguous &&
			      !memcmp(compared.bits, frame.bits, frame.bit_count), "scalar predictor changed decoded bits");
			if (work->receiver.line.received_frames <= V92_B1U_FRAMES) linear_error += compared.squared_error;
		}
		if (work->receiver.line.received_frames == V92_B1U_ACQUISITION_FRAMES) {
			CHECK(!work->receiver.model.acquiring && !work->receiver.model.valid && work->receiver.model.scalar_gain > 0.,
				"single codec fit missing: tandem=%u scalar=%g", work->receiver.model.valid, work->receiver.model.scalar_gain);
			frozen = work->receiver.model.scalar_gain;
		} else if (work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES) {
			CHECK(work->receiver.model.scalar_gain == frozen, "held-out data changed scalar fit");
			if (work->receiver.line.received_frames <= V92_B1U_FRAMES) error += frame.squared_error;
		}
		if (!frame.bit_count) continue;
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];
		if (delivered < V92_B1U_FRAMES) {
			for (unsigned b = 0; b < frame.bit_count; b++) bad += frame.bits[b] != 1;
			bad += frame.ambiguous;
		} else {
			payload(expected, mapping.data_bits_per_frame, delivered - V92_B1U_FRAMES);
			CHECK(!frame.ambiguous && !memcmp(frame.bits, expected, frame.bit_count), "scalar payload corrupted");
		}
		++delivered;
	}
	CHECK(delivered == FRAMES - V92_PCM_RX_DELAY_FRAMES && (wrong_bit ? bad > 0 : bad == 0),
		"scalar B1u bit validation or payload count changed: %u bad", bad);
	CHECK(error / V92_B1U_VALIDATION_SYMBOLS < V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
		"scalar B1u RMS failed: %g", sqrt(error / V92_B1U_VALIDATION_SYMBOLS));
	CHECK(error < linear_error, "single-codec prediction did not improve held-out quality");
	work->saved = work->receiver;
	struct v92_upstream_data_frame output, before;
	memset(&output, 0xa5, sizeof(output)); before = output;
	CHECK(fixture_sample(&work->receiver, 0, &output) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      !memcmp(&work->receiver, &work->saved, sizeof(work->saved)) && !memcmp(&output, &before, sizeof(output)),
	      "scalar prediction invented missing wire evidence");
	for (unsigned fault = 0; fault < 3; fault++) {
		work->clipped = work->saved;
		uint8_t code = law == G711_LAW_PCMU ? g711_encode_pcmu(4096) : g711_encode_pcma(4096);
		int16_t host = law == G711_LAW_PCMU ? g711_decode_pcmu(code) : g711_decode_pcma(code);
		int16_t sample = fault == 0 ? INT16_MAX : host;
		if (fault == 1) host = INT16_MAX;
		if (fault == 2) host += 8;
		for (unsigned s = 0; s < V92_PCM_MAPPING_FRAME_SYMBOLS - 1u; s++)
			CHECK(fixture_octet_owned(&work->clipped, code, sample, host, true, &output) ==
				V92_UPSTREAM_DATA_RX_OK, "scalar clipping prefix failed");
		work->retained = work->clipped;
		work->retained.failed = true;
		CHECK(fixture_octet_owned(&work->clipped, code, sample, host, true, &output) ==
			V92_UPSTREAM_DATA_RX_NO_PATH && !memcmp(&work->clipped, &work->retained, sizeof(work->retained)) &&
			!memcmp(&output, &before, sizeof(output)), "scalar clipping advanced state or released payload");
	}
	for (unsigned s = 0; s < V92_PCM_MAPPING_FRAME_SYMBOLS; s++)
		CHECK(fixture_octet(&work->receiver, 0u, INT16_MAX, false, &output) ==
			(s == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u ? V92_UPSTREAM_DATA_RX_FRAME :
			 V92_UPSTREAM_DATA_RX_OK), "scalar erasure stopped receiver");
	CHECK(output.squared_error > 1e9 && work->receiver.model.scalar_gain == frozen &&
	      !memcmp(&work->receiver.line.host, &work->saved.line.host, sizeof(work->saved.line.host)),
	      "scalar erasure hid its residual or changed the fit");
	work->receiver = work->saved;
	CHECK(check_control_reset(&work->receiver) &&
	      v92_upstream_data_rx_reset_control(&work->receiver) == V92_UPSTREAM_DATA_RX_OK &&
	      work->receiver.model.scalar_gain == frozen && !memcmp(&work->receiver.line, &work->saved.line, sizeof(work->saved.line)),
	      "control reset discarded the established scalar channel");
	for (unsigned fault = 0; fault < 5; fault++) {
		work->invalid = work->saved;
		if (fault == 0) work->invalid.model.scalar_gain = NAN;
		if (fault == 1) work->invalid.model.scalar_gain = -1.;
		if (fault == 2) work->invalid.model.acquiring = true;
		if (fault == 3) work->invalid.model.coefficient = 0;
		if (fault == 4) work->invalid.line.host.gain = 0.;
		CHECK(!v92_upstream_data_rx_is_valid(&work->invalid), "invalid scalar state accepted");
	}
	CHECK(v92_upstream_data_rx_init(&work->receiver, &e2u_rx.signal, &mapping, &plan) == V92_UPSTREAM_DATA_RX_OK &&
	      !memcmp(&work->receiver, &work->initial, sizeof(work->initial)), "scalar fit survived a new training epoch");
	return true;
}

static bool test_scalar_model(enum g711_law law, int polarity, bool wrong_bit)
{
	struct scalar_model_workspace *work = malloc(sizeof(*work));
	CHECK(work, "scalar model workspace allocation failed");
	bool result = check_scalar_model(work, law, polarity, wrong_bit);
	free(work);
	return result;
}

/* Keep independent receiver snapshots off the thread stack, including under
 * sanitizers. The wrapper frees the workspace after any failed CHECK. */
struct codec_model_workspace {
	struct v92_upstream_data_rx receiver;
	struct v92_upstream_data_rx initial;
	struct v92_upstream_data_rx invalid;
	struct v92_upstream_data_rx owned;
	struct v92_upstream_data_rx restarted;
	struct v92_upstream_data_rx selected;
	struct v92_upstream_data_rx selected_before;
	struct v92_upstream_data_rx full;
	struct v92_upstream_data_rx saved;
	struct v92_upstream_data_rx conditioned;
	struct v92_upstream_data_rx retained;
	struct v92_upstream_data_rx paired;
};

static bool check_codec_model(struct codec_model_workspace *work, int polarity, bool rm, uint8_t rate, bool peer, bool echo, unsigned deferred_block)
{
	bool deferred = deferred_block != 0;
	unsigned block_decodes = 0;
	enum { FRAMES = V92_B1U_FRAMES + 36 + V92_PCM_RX_DELAY_FRAMES, PREFIX = 6 + (V92_UPSTREAM_INPUT_QUEUE + 11) / 12 };
	enum { RM_START = V92_B1U_FRAMES + 16, RM_PRIME = RM_START + 32, RM_END = RM_PRIME + 2 };
	unsigned frames = FRAMES + (rm ? 34u : 0u);
	struct channel training = { .gain = 1.0, .law = G711_LAW_PCMU, .polarity = polarity };
	struct v92_upstream_channel measured = { 0 };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_upstream_plan plan;
	struct v92_pcm_frame_tx transmitter;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_observation frozen[2];
	uint8_t codes[(FRAMES + 34 + PREFIX) * V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint32_t random = 0x35791bdu;
	int16_t previous = 0, second = 0;
	unsigned delivered = 0u;
	unsigned changed = 0u, queued_peak = 0;
	bool fitting_seen = false;
	double b1_error = 0.0;

	for (unsigned i = 0u; i < V92_UPSTREAM_CHANNEL_SYMBOLS; ++i) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		int16_t sample = (int16_t)((int)(random >> 19) - TEST_LU);
		int16_t q = g711_decode_pcmu(g711_encode_pcmu(sample));
		int16_t observed = g711_decode_pcmu(g711_encode_pcmu(
			(int16_t)lround(.9 * q + .08 * previous + .02 * second)));

		CHECK(v92_upstream_channel_sample(&measured, sample,
			(int16_t)lround(.75 * observed)), "codec channel measurement failed");
		second = previous;
		previous = q;
	}
	CHECK(v92_cpd_direct_select(&cpd, G711_LAW_PCMU, rate, TEST_LU, false) == V92_CPD_OK &&
	      v92_cpd_channel_design(&cpd, &measured, G711_LAW_PCMU, TEST_LU, &plan),
							"codec channel design failed");
	if (deferred_block == 40) {
		struct v92_cpd_filter *feedback = &cpd.filter[V92_CPD_PRECODER_FEEDBACK];
		feedback->count = 3;
		feedback->coefficient[2] = 1;
	}
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) == V92_PCM_FRAME_OK &&
	      prepare_e2u(&training, false, &e2u_tx, &e2u_rx) &&
	      v92_upstream_data_rx_init(&work->receiver, &e2u_rx.signal, &mapping, &plan) ==
		V92_UPSTREAM_DATA_RX_OK && work->receiver.model.acquiring,
	      "codec model initialization failed");
	work->initial = work->receiver;
	for (unsigned fault = 0; fault < 4; ++fault) {
		work->invalid = work->initial;
		if (fault == 0)
			work->invalid.line.host.gain = .75;
		else if (fault == 1)
			work->invalid.line.host.fit.lower = NAN;
		else if (fault == 2)
			work->invalid.line.host.fit.energy = -1;
		else
			work->invalid.line.host.fit.upper = -1;
		CHECK(!v92_upstream_data_rx_is_valid(&work->invalid), "invalid host acquisition accepted");
	}
	previous = second = 0;
	for (unsigned f = 0u; f < frames + PREFIX; ++f) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_filter_state initial_filter = transmitter.state.filter;

		if (f < V92_B1U_FRAMES)
			memset(bits, 1, mapping.data_bits_per_frame);
		else
			payload(bits, mapping.data_bits_per_frame,
				f - V92_B1U_FRAMES - (rm && f >= RM_END ? 34u : 0u));
		/* Both coding hypotheses must retain two closely-spaced control
		 * epochs even while older payload is still in traceback. */
		if (rm ? f == RM_END : f == V92_B1U_FRAMES + 16u || f == V92_B1U_FRAMES + 18u) {
			transmitter.state.scrambler.history = 0u;
			transmitter.state.differential = 0u;
		}
		CHECK((rm && f >= RM_START && f < RM_END ?
			v92_pcm_frame_encode_rm(&transmitter.mapping, &transmitter.state, f >= RM_PRIME, &frame) :
			v92_pcm_frame_encode(&transmitter, bits, mapping.data_bits_per_frame, &frame)) ==
			V92_PCM_FRAME_OK, "codec frame encode failed");
		if (peer && !peer_frame(&transmitter, &frame, initial_filter, f, &changed))
			return false;
		for (unsigned s = 0u; s < V92_PCM_MAPPING_FRAME_SYMBOLS; ++s) {
			int16_t sample;

			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LU, frame.v[s], &sample) ==
				V92_PCM_FRAME_OK, "codec frame scale failed");
			CHECK(sample > -32000 && sample < 32000, "codec fixture clipped");
			int16_t q = g711_decode_pcmu(g711_encode_pcmu(sample));
			codes[f * V92_PCM_MAPPING_FRAME_SYMBOLS + s] = g711_encode_pcmu(
				(int16_t)lround(polarity * (.9 * q + .08 * previous + .02 * second +
					(echo ? codec_echo(f * V92_PCM_MAPPING_FRAME_SYMBOLS + s) : 0))));
			second = previous;
			previous = q;
		}
	}
	for (unsigned i = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     work->receiver.line.received_frames < frames; ++i) {
		struct v92_upstream_data_frame frame = { 0 }, compared = { 0 };
		if (deferred && (i - V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY) % deferred_block == 0) {
			v92_upstream_data_rx_begin_block_owned(&work->receiver, deferred_block);
			block_decodes = 0;
		}
		work->owned = work->receiver;
		bool eof = deferred && i >= plan.delay + frames * V92_PCM_MAPPING_FRAME_SYMBOLS;
		CHECK(i < plan.delay + frames * V92_PCM_MAPPING_FRAME_SYMBOLS + 4 * V92_UPSTREAM_INPUT_QUEUE,
			"retired media drain did not finish");
		uint8_t code = eof ? 0 : codes[i];
		int16_t host = (int16_t)lround(.75 * g711_decode_pcmu(code));
		int16_t sample = host - (echo ? polarity * 3 * (codec_echo(i) / 4) : 0);

		if (work->receiver.line.symbol_count == 0u &&
		    (rm ? work->receiver.line.received_frames == RM_END :
		     work->receiver.line.received_frames == V92_B1U_FRAMES + 16u ||
		     work->receiver.line.received_frames == V92_B1U_FRAMES + 18u)) {
			struct v92_upstream_data_line saved_line = work->receiver.line;
			CHECK(work->receiver.model.valid == 3u &&
			      check_control_reset(&work->receiver) &&
			      v92_upstream_data_rx_reset_control(&work->receiver) == V92_UPSTREAM_DATA_RX_OK &&
			      memcmp(&saved_line, &work->receiver.line, sizeof(saved_line)) == 0,
			      "paired codec control reset failed");
			work->owned = work->receiver;
		}
		enum v92_upstream_data_rx_result result = eof ?
			v92_upstream_data_rx_drain_owned(&work->receiver, &frame) : echo ? (deferred ? v92_upstream_data_rx_octet_owned : fixture_octet_owned)(
			&work->receiver, code, sample, host, true, &frame) : (deferred ? v92_upstream_data_rx_octet : fixture_octet)(
			&work->receiver, code, sample, true, &frame);

		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "codec receiver stopped: %u at sample %u", result, i);
		CHECK((eof ? v92_upstream_data_rx_drain_owned(&work->owned, &compared) :
			(deferred ? v92_upstream_data_rx_octet_owned : fixture_octet_owned)(&work->owned, code, sample, host, true, &compared)) == result &&
		      memcmp(&work->receiver, &work->owned, sizeof(work->owned)) == 0 &&
		      memcmp(&frame, &compared, sizeof(frame)) == 0 &&
		      v92_upstream_data_rx_is_valid(&work->receiver), "codec owned/checked disagreement");

		CHECK(work->receiver.line.input_samples + work->receiver.input.count == work->initial.line.input_samples +
			(eof ? plan.delay + frames * V92_PCM_MAPPING_FRAME_SYMBOLS : i + 1) -
			V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY, "queued input accounting changed");
		if (work->receiver.input.count > queued_peak) queued_peak = work->receiver.input.count;
		if (work->receiver.model.fitting && !fitting_seen) {
			fitting_seen = true;
			work->restarted = work->receiver;
			CHECK(v92_upstream_data_rx_init(&work->restarted, &e2u_rx.signal, &mapping, &plan) ==
				V92_UPSTREAM_DATA_RX_OK && !memcmp(&work->restarted, &work->initial, sizeof(work->initial)),
				"restart retained a suspended fit or queued media");
		}
		if (result != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (deferred && work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES &&
			work->receiver.line.received_frames <= V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES)
			CHECK(++block_decodes <= (deferred_block == 40 ? 2u : 1u),
				"paired B1 decode exceeded its native block budget");
		if (work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES &&
		    work->receiver.line.received_frames <= V92_B1U_FRAMES)
			b1_error += frame.squared_error;
		if (work->receiver.line.received_frames == V92_B1U_ACQUISITION_FRAMES) {
			CHECK(deferred ? work->receiver.model.fitting && !work->receiver.model.valid :
				work->receiver.model.valid == 3u && !work->receiver.model.acquiring,
				"codec B1u acquisition boundary changed");
			CHECK(fabs(work->receiver.line.host.gain - .75) < 1e-12 &&
			      memory_region_is_zero(&work->receiver.line.host.fit, sizeof(work->receiver.line.host.fit)),
			      "host gain did not freeze at the acquisition boundary");
			if (!deferred) memcpy(frozen, work->receiver.model.model, sizeof(frozen));
		} else if (work->receiver.line.received_frames > V92_B1U_ACQUISITION_FRAMES) {
			if (deferred && work->receiver.line.received_frames == V92_B1U_ACQUISITION_FRAMES + 1) {
				CHECK(!work->receiver.model.fitting && work->receiver.model.valid == 3, "deferred model not complete before validation");
				memcpy(frozen, work->receiver.model.model, sizeof(frozen));
			}
			CHECK(memcmp(frozen, work->receiver.model.model, sizeof(frozen)) == 0 &&
			      fabs(work->receiver.line.host.gain - .75) < 1e-12 &&
			      memory_region_is_zero(&work->receiver.line.host.fit, sizeof(work->receiver.line.host.fit)),
			      "held-out B1u or payload retrained the codec models");
		} else {
			CHECK(work->receiver.line.host.gain == 0.0 && pcm_gain_is_valid(&work->receiver.line.host.fit),
			      "host gain froze before the acquisition boundary");
		}
		if (deferred && work->receiver.line.received_frames == V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			work->selected = work->receiver;
			unsigned other = work->selected.model.primary ^ 1u;
			work->selected.line.received_frames--;
			work->selected_before = work->selected;
			CHECK(!v92_upstream_model_finish_b1u(&work->selected) && !memcmp(&work->selected, &work->selected_before, sizeof(work->selected_before)),
				"channel selection ran selected_before the final independent B1u bit");
			work->selected = work->receiver;
			work->selected.model.cost[other] = UINT64_C(16) * V92_PCM_RX_AMBIGUITY_COST - 1;
			work->selected_before = work->selected;
			CHECK(!v92_upstream_model_finish_b1u(&work->selected) && !memcmp(&work->selected, &work->selected_before, sizeof(work->selected_before)),
				"channel selection discarded an uncertain model");
			work->selected.model.cost[other]++;
			work->selected_before = work->selected;
			CHECK(v92_upstream_model_finish_b1u(&work->selected) &&
				work->selected.model.valid == (1u << work->selected.model.primary) &&
				!memcmp(&work->selected.decoder, &work->selected_before.decoder, sizeof(work->selected.decoder)) &&
				!memcmp(work->selected.model.model, work->selected_before.model.model, sizeof(work->selected.model.model)) &&
				!memcmp(&work->selected.line, &work->selected_before.line, sizeof(work->selected.line)) &&
				memory_region_is_zero(&work->selected.model.alternate, sizeof(work->selected.model.alternate)) &&
				v92_upstream_data_rx_is_valid(&work->selected), "selected channel changed coding, fits, or physical state");
		}
		unsigned f = (unsigned)work->receiver.line.received_frames - 1u;
		enum v92_pcm_frame_rm expected_rm = !rm || f < RM_START +
			V92_PCM_RM_QUALIFY_FRAMES - 1u || f >= RM_END ? V92_PCM_FRAME_RM_NONE :
			f < RM_PRIME ? V92_PCM_FRAME_RM_SIGNAL :
			f == RM_PRIME ? V92_PCM_FRAME_RM_PRIME : V92_PCM_FRAME_RM_COMPLETE;
		CHECK(frame.rm == expected_rm, "codec RM stage differs at %u: %u/%u", f, frame.rm, expected_rm);
		if (rm && f >= RM_START + V92_PCM_RM_QUALIFY_FRAMES - 1u &&
		    f < RM_END + V92_PCM_RX_DELAY_FRAMES)
			CHECK(!frame.bit_count, "codec RM leaked a delayed word");
		if (!frame.bit_count || (rm && f >= RM_START + V92_PCM_RX_DELAY_FRAMES &&
			f < RM_END + V92_PCM_RX_DELAY_FRAMES))
			continue;
		uint8_t expected[V92_PCM_MAPPING_MAX_BITS];

		if (delivered < V92_B1U_FRAMES)
			memset(expected, 1, mapping.data_bits_per_frame);
		else
			payload(expected, mapping.data_bits_per_frame, delivered - V92_B1U_FRAMES);
		CHECK(!frame.ambiguous && frame.bit_count == mapping.data_bits_per_frame &&
		      memcmp(frame.bits, expected, frame.bit_count) == 0,
		      "codec payload frame %u changed (ambiguous=%u)", delivered, frame.ambiguous);
		++delivered;
	}
	CHECK(delivered == FRAMES - V92_PCM_RX_DELAY_FRAMES, "codec payload count changed");
	CHECK(!peer || changed > V92_B1U_SYMBOLS / 4u, "peer fixture kept canonical points");
	CHECK(b1_error / (.75 * .75 * V92_B1U_VALIDATION_SYMBOLS) <=
	      V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR, "codec B1u failed quality");
	if (deferred) {
		CHECK(fitting_seen && queued_peak > 0 && queued_peak < V92_UPSTREAM_INPUT_QUEUE &&
			!work->receiver.input.count,
			"deferred fit did not retain bounded input");
		work->full = work->receiver;
		work->full.input.count = V92_UPSTREAM_INPUT_QUEUE;
		struct v92_upstream_data_frame out = {0}, before = out;
		CHECK(v92_upstream_data_rx_octet_owned(&work->full, 0xff, 0, 0, true, &out) ==
			V92_UPSTREAM_DATA_RX_NO_PATH && work->full.failed && !memcmp(&out, &before, sizeof(out)),
			"full input queue silently dropped media");
		printf("Deferred codec peak retained samples=%u\n", queued_peak);
		return true;
	}
	work->saved = work->receiver;
	/* Gain changes and clipping must not be interpreted as independently
	 * known echo. The failed frame retains both coding paths and outputs. */
	for (unsigned fault = 0; fault < 3; fault++) {
		work->conditioned = work->saved;
		struct v92_upstream_data_frame out, before;
		memset(&out, 0xa5, sizeof(out)); before = out;
		uint8_t code = g711_encode_pcmu(4096);
		int16_t host = (int16_t)lround(.75 * g711_decode_pcmu(code));
		int16_t sample = host;
		if (fault == 0) sample = INT16_MAX;
		if (fault == 1) host = INT16_MAX;
		if (fault == 2) host += 8;
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS - 1; i++)
			CHECK(fixture_octet_owned(&work->conditioned, code, sample, host, true, &out) ==
				V92_UPSTREAM_DATA_RX_OK, "conditioning fault prefix stopped");
		work->retained = work->conditioned;
		work->retained.failed = true;
		CHECK(fixture_octet_owned(&work->conditioned, code, sample, host, true, &out) ==
			V92_UPSTREAM_DATA_RX_NO_PATH && !memcmp(&work->conditioned, &work->retained, sizeof(work->retained)) &&
			!memcmp(&out, &before, sizeof(out)), "conditioning fault changed coding state or payload");
	}
	for (unsigned fault = 0; fault < 7; ++fault) {
		work->invalid = work->saved;
		if (fault == 0)
			work->invalid.line.host.gain = NAN;
		else if (fault == 1)
			work->invalid.line.host.gain = -1;
		else if (fault == 2)
			work->invalid.line.host.lower = NAN;
		else if (fault == 3)
			work->invalid.line.host.upper = INFINITY;
		else if (fault == 4)
			work->invalid.line.host.lower = work->invalid.line.host.gain + .1;
		else if (fault == 5)
			work->invalid.line.host.upper = work->invalid.line.host.gain - .1;
		else
			pcm_gain_init(&work->invalid.line.host.fit);
		CHECK(!v92_upstream_data_rx_is_valid(&work->invalid), "invalid frozen host gain accepted");
	}
	/* With no new likelihood evidence, equally ranked codec hypotheses can
	 * disagree about an old non-payload slot. Never compare unwritten bits
	 * from the suppressed branch or release a placeholder from the winner. */
	for (unsigned primary = 0u; primary < 2u; ++primary)
	for (unsigned suppressed = 0u; suppressed < 2u; ++suppressed) {
		work->paired = work->saved;
		struct v92_upstream_data_frame frame = { 0 };

		work->paired.model.primary = (uint8_t)primary;
		work->paired.model.cost[0] = work->paired.model.cost[1] = 0u;
		work->paired.model.alternate = work->paired.decoder;
		struct v92_pcm_frame_rx *branch = suppressed == primary ?
			&work->paired.decoder : &work->paired.model.alternate;
		for (unsigned i = 0u; i < branch->path_count; ++i)
			branch->paths[i].nondata_frames = 1u;
		CHECK(v92_upstream_data_rx_is_valid(&work->paired), "paired non-payload fixture invalid");
		/* Advance both model-indexed paths independently. A normalized
		 * primary-1 tie must transfer model 0 into the primary owner. */
		struct v92_pcm_frame_rx expected[2];
		const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS] = { 0 };
		const bool received[V92_PCM_MAPPING_FRAME_SYMBOLS] = { false };

		expected[primary] = work->paired.decoder;
		expected[primary ^ 1u] = work->paired.model.alternate;
		for (unsigned model = 0u; model < 2u; ++model) {
			struct v92_pcm_frame_channel channel = {
				.model = &work->paired.model.model[model], .received = received,
			};
			struct v92_pcm_frame_decision decision;
			struct v92_pcm_frame_likelihood likelihood;
			uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

			CHECK(v92_pcm_frame_decode_observed(&expected[model], samples, false,
				bits, mapping.data_bits_per_frame, &decision, &channel, &likelihood) ==
				V92_PCM_FRAME_OK && likelihood.cost == 0u,
				"erased codec path changed its normalized metric");
		}
		for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
			CHECK(fixture_octet(&work->paired, 0u, 0, false, &frame) ==
				(i == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u ? V92_UPSTREAM_DATA_RX_FRAME :
				 V92_UPSTREAM_DATA_RX_OK), "paired non-payload decoding failed");
		CHECK(work->paired.model.valid == 3u && work->paired.model.primary == 0u &&
			work->paired.model.cost[0] == 0u && work->paired.model.cost[1] == 0u &&
			(suppressed ? frame.bit_count == mapping.data_bits_per_frame && frame.ambiguous :
			 frame.bit_count == 0u) && v92_upstream_data_rx_is_valid(&work->paired),
			"tied codec models mishandled branch %u's non-payload slot", suppressed);
		CHECK(memcmp(&work->paired.decoder, &expected[0], sizeof(expected[0])) == 0 &&
			memcmp(&work->paired.model.alternate, &expected[1], sizeof(expected[1])) == 0,
			"tied codec models changed primary/alternate decoder ownership");
	}
	struct v92_upstream_data_frame output, unchanged;
	memset(&output, 0xa5, sizeof(output));
	unchanged = output;
	CHECK(fixture_sample(&work->receiver, 0, &output) == V92_UPSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      memcmp(&work->saved, &work->receiver, sizeof(work->saved)) == 0 &&
	      memcmp(&unchanged, &output, sizeof(output)) == 0,
	      "active codec model invented a wire code");
	/* Erasures have no likelihood evidence, but must still report raw
	 * residual. They cannot silently disappear from the quality guard. */
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i)
		CHECK(fixture_octet(&work->receiver, 0u, INT16_MAX, false, &output) ==
		      (i == V92_PCM_MAPPING_FRAME_SYMBOLS - 1u ? V92_UPSTREAM_DATA_RX_FRAME :
			V92_UPSTREAM_DATA_RX_OK), "erased codec frame stopped");
	CHECK(output.squared_error > 1e9 &&
	      memcmp(&work->receiver.line.host, &work->saved.line.host, sizeof(work->saved.line.host)) == 0,
	      "erased media hid its raw residual or retrained host gain");
	/* Finite model parameters can still overflow at a codec boundary. A
	 * failed frame must not partly advance either coding hypothesis. */
	work->receiver = work->saved;
	for (unsigned i = 0u; i < 2u; ++i) {
		work->receiver.model.model[i].quantizer_gain = DBL_MAX;
		for (unsigned j = 0u; j < V92_OBSERVATION_INTERVALS; ++j)
			work->receiver.model.model[i].intervals[j].coefficient[0] = DBL_MAX;
	}
	CHECK(v92_upstream_data_rx_is_valid(&work->receiver), "numeric-failure fixture is invalid");
	work->owned = work->receiver;
	struct v92_upstream_data_frame owned_output = output;
	for (unsigned i = 0u; i < V92_PCM_MAPPING_FRAME_SYMBOLS - 1u; ++i)
		CHECK(fixture_octet(&work->receiver, 0u, 100, true, &output) ==
		      V92_UPSTREAM_DATA_RX_OK &&
		      fixture_octet_owned(&work->owned, 0u, 100, 100, true, &owned_output) ==
		      V92_UPSTREAM_DATA_RX_OK && memcmp(&work->receiver, &work->owned, sizeof(work->owned)) == 0 &&
		      memcmp(&output, &owned_output, sizeof(output)) == 0,
		      "checked/owned numeric-failure buffering differs");
	work->saved = work->receiver;
	unchanged = output;
	CHECK(fixture_octet(&work->receiver, 0u, 100, true, &output) ==
	      V92_UPSTREAM_DATA_RX_NO_PATH &&
	      fixture_octet_owned(&work->owned, 0u, 100, 100, true, &owned_output) ==
	      V92_UPSTREAM_DATA_RX_NO_PATH, "unrepresentable codec frame did not fail");
	work->saved.failed = true;
	CHECK(memcmp(&work->saved, &work->receiver, sizeof(work->saved)) == 0 &&
	      memcmp(&work->saved, &work->owned, sizeof(work->saved)) == 0 &&
	      memcmp(&unchanged, &owned_output, sizeof(output)) == 0 &&
	      memcmp(&unchanged, &output, sizeof(output)) == 0,
	      "failed codec frame advanced state or wrote payload");
	/* No retained fit may cross a mapping/retrain epoch. */
	CHECK(v92_upstream_data_rx_init(&work->receiver, &e2u_rx.signal, &mapping, &plan) ==
	      V92_UPSTREAM_DATA_RX_OK && memcmp(&work->initial, &work->receiver, sizeof(work->initial)) == 0,
	      "new training epoch retained codec state");
	for (unsigned missing = 0u; missing < 3u; ++missing) {
		work->receiver = work->initial;
		/* Keep the coded sample stream aligned. Only the first frame lacks
		 * matching wire evidence; later pairs must not reopen acquisition. */
		for (unsigned i = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
		     work->receiver.line.received_frames <= V92_B1U_ACQUISITION_FRAMES; ++i) {
			uint8_t code = codes[i];
			int16_t sample = (int16_t)lround(.75 * g711_decode_pcmu(code));
			bool first = work->receiver.line.received_frames == 0u;
			enum v92_upstream_data_rx_result result = first && missing == 0u ?
				fixture_sample(&work->receiver, sample, &output) :
				fixture_octet(&work->receiver, first && missing == 2u ? 0xffu : code,
					sample, !(first && missing == 1u), &output);
			CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
			      "closed acquisition stopped linear fallback");
		}
		CHECK(!work->receiver.model.acquiring && !work->receiver.model.valid && !work->receiver.model.scalar_gain &&
		      memory_region_is_zero(&work->receiver.line.host, sizeof(work->receiver.line.host)) &&
		      v92_upstream_data_rx_is_valid(&work->receiver), "missing or inconsistent media trained a model");
	}
	/* A multi-tap prefilter is outside this model, not a one-tap fit with
	 * silently ignored coefficients. Keep the existing linear receiver. */
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = plan.delay + 2u;
	cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[plan.delay + 1u] = 1;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_upstream_data_rx_init(&work->receiver, &e2u_rx.signal, &mapping, &plan) ==
	      V92_UPSTREAM_DATA_RX_OK && !work->receiver.model.acquiring && !work->receiver.model.valid && !work->receiver.model.scalar_gain,
	      "unsupported prefilter enabled the codec model");
	for (unsigned i = V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY;
	     work->receiver.line.received_frames < V92_B1U_ACQUISITION_FRAMES; ++i) {
		uint8_t code = codes[i];
		enum v92_upstream_data_rx_result result = fixture_octet(&work->receiver, code,
			(int16_t)lround(.75 * g711_decode_pcmu(code)), true, &output);
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
		      "general prefilter host acquisition stopped");
	}
	CHECK(fabs(work->receiver.line.host.gain - .75) < 1e-12 && !work->receiver.model.valid,
	      "host gain depended on the mapping's codec-model support");
	return true;
}

static bool test_codec_model(int polarity, bool rm, uint8_t rate, bool peer, bool echo, unsigned deferred_block)
{
	struct codec_model_workspace *work = malloc(sizeof(*work));
	CHECK(work, "codec model workspace allocation failed");
	bool result = check_codec_model(work, polarity, rm, rate, peer, echo, deferred_block);
	free(work);
	return result;
}

/* A validly coded wrong bit in the final 40k B1u frame has RMS error below
 * the call's limit. Known-one decoding alone must not qualify this waveform.
 * Also reset control at frame 48, while ten B1u decisions remain buffered. */
static bool test_direct_b1_validation(enum g711_law law, int polarity, bool fault, bool control)
{
	struct channel channel = { .gain = 1.0, .law = law, .polarity = polarity };
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_phase4_record_tx e2u_tx;
	struct v92_e2u_rx e2u_rx;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame encoded;
	struct v92_upstream_data_rx rx;
	bool ready = false, wrong_seen = false;
	unsigned frame_number = 0u, symbol = 0u, delivered = 0u;
	int16_t trained_level = 0, minimum = 0, maximum = 0;
	double error = 0.0;

	CHECK(prepare_e2u(&channel, false, &e2u_tx, &e2u_rx) &&
	      v92_cpd_direct_select(&cpd, law, 13u, TEST_LU, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK, "direct B1 fixture failed");
	for (unsigned clock = 0u; clock < 2000u && delivered < V92_B1U_FRAMES; ++clock) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&e2u_tx)) {
			CHECK(v92_phase4_record_tx_next(&e2u_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
				"E2u transmit");
		} else {
			if (symbol == 0u) {
				uint8_t bits[V92_PCM_MAPPING_MAX_BITS];

				memset(bits, 1, mapping.data_bits_per_frame);
				if (control && frame_number == V92_B1U_FRAMES)
					CHECK(v92_pcm_frame_tx_reset_control(&tx) == V92_PCM_FRAME_OK, "control TX reset");
				if (fault && frame_number == V92_B1U_FRAMES - 1u)
					bits[55] = 0u;
				CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &encoded) ==
					V92_PCM_FRAME_OK, "encode");
			}
			CHECK(v92_pcm_frame_scale(mapping.cpd.four_times_gain_q0_16, TEST_LU, encoded.v[symbol], &sample) ==
				V92_PCM_FRAME_OK, "scale");
			if (++symbol == V92_PCM_MAPPING_FRAME_SYMBOLS) {
				symbol = 0u;
				++frame_number;
			}
		}
		sample = wire_sample(&channel, sample);
		if (!ready) {
			enum v92_e2u_rx_result result = v92_e2u_rx_sample(&e2u_rx, sample);
			CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE, "E2u receive");
			if (result == V92_E2U_RX_COMPLETE) {
				struct v92_upstream_rx handoff;
				CHECK(v92_e2u_rx_handoff(&e2u_rx, &handoff) == V92_E2U_RX_OK &&
					v92_upstream_data_rx_init(&rx, &handoff, &mapping, &direct_plan) ==
					V92_UPSTREAM_DATA_RX_OK, "handoff");
				ready = true;
			}
			continue;
		}
		struct v92_upstream_data_frame frame = { 0 };
		enum v92_upstream_data_rx_result result;

		CHECK(receive_compared(&rx, sample, &frame, &result), "checked/owned B1 mismatch");
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME, "receive");
		if (result != V92_UPSTREAM_DATA_RX_FRAME)
			continue;
		if (rx.line.received_frames < V92_B1U_FRAMES) {
			CHECK(rx.model.level.frames == rx.line.received_frames,
			      "B1u level fit missed a frame");
			for (unsigned fault = 0u; fault < 3u; ++fault) {
				struct v92_upstream_data_rx invalid = rx;
				if (fault == 0u)
					invalid.model.level.frames--;
				else if (fault == 1u)
					invalid.model.level.minimum = 0;
				else
					invalid.model.level.maximum = rx.decoder.upstream_level - 1;
				CHECK(!v92_upstream_data_rx_is_valid(&invalid), "invalid level fit was accepted");
			}
		} else {
			const struct v92_pcm_level_fit cleared = { 0 };
			CHECK(memcmp(&rx.model.level, &cleared, sizeof(cleared)) == 0,
			      "level fit survived B1u training");
		}
		if (rx.line.received_frames == V92_B1U_ACQUISITION_FRAMES) {
			trained_level = rx.decoder.upstream_level;
			minimum = rx.decoder.minimum_level;
			maximum = rx.decoder.maximum_level;
		}
		if (rx.line.received_frames > V92_B1U_ACQUISITION_FRAMES &&
		    rx.line.received_frames < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			CHECK(rx.model.alternate.upstream_level == trained_level &&
			      rx.model.alternate.minimum_level == minimum && rx.model.alternate.maximum_level == maximum,
			      "validation changed the acquired level");
			struct v92_upstream_data_rx invalid = rx;
			invalid.model.alternate.pending_frames = 0u;
			CHECK(!v92_upstream_data_rx_is_valid(&invalid), "invalid validator was accepted");
		}
		if (control && rx.line.received_frames == V92_B1U_FRAMES) {
			struct v92_pcm_frame_rx expected = rx.model.alternate;

			CHECK(check_control_reset(&rx) &&
			      v92_pcm_frame_rx_reset_control(&expected) == V92_PCM_FRAME_OK &&
			      v92_upstream_data_rx_reset_control(&rx) == V92_UPSTREAM_DATA_RX_OK &&
			      memcmp(&rx.model.alternate, &expected, sizeof(expected)) == 0,
			      "control reset left the delayed B1 validator in the old epoch");
		}
		if (rx.line.received_frames <= V92_B1U_FRAMES)
			error += frame.squared_error;
		if (frame.bit_count == 0u)
			continue;
		CHECK(frame.b1u && frame.bit_count == mapping.data_bits_per_frame, "classification");
		if (delivered == V92_B1U_FRAMES - 1u)
			wrong_seen = frame.ambiguous || frame.bits[55] != 1u;
		else
			CHECK(!frame.ambiguous, "clean B1 prefix became ambiguous");
		++delivered;
	}
	CHECK(delivered == V92_B1U_FRAMES && wrong_seen == fault, "known bits concealed wrong B1u: law=%u polarity=%d",
		law, polarity);
	CHECK(error < V92_B1U_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
		"wrong-bit fixture failed merely through residual");
	const struct v92_pcm_frame_rx cleared = { 0 };
	CHECK(memcmp(&rx.model.alternate, &cleared, sizeof(cleared)) == 0,
		"validator survived final B1 decision");
	return true;
}

/* A physical residual channel at unrelated delays must be learned from
 * acquisition, then decode a different held-out sequence without refitting. */
static bool test_residual_inverse(void)
{
	enum { TRAIN = V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	       TOTAL = TRAIN + 240 };
	int32_t points[TOTAL], raw[TOTAL];
	const unsigned delays[][2] = { { 0, 0 }, { 4, 0 }, { 3, 9 }, { 1, 12 } };
	for (unsigned fixture = 0; fixture < 2u * ARRAY_LENGTH(delays); fixture++) {
		const unsigned *delay = delays[fixture % ARRAY_LENGTH(delays)];
		bool wide = fixture >= ARRAY_LENGTH(delays);
		double minimum = wide ? 1.9 : .8, maximum = wide ? 2.1 : .9;
		uint32_t random = 71371;
		for (unsigned i = 0; i < TOTAL; i++) {
			random = random * 1664525u + 1013904223u;
			points[i] = wide ? (int)(random >> 16) - 32768 : (int)(random >> 20) - 2048;
			if (wide && i % 17u == 0u) points[i] = 32768;
			if (wide && i % 19u == 0u) points[i] = -32768;
			double value = points[i];
			if (delay[0] && i >= delay[0]) value -= .027 * points[i - delay[0]];
			if (delay[1] && i >= delay[1]) value += .011 * points[i - delay[1]];
			raw[i] = (int32_t)lround((wide ? .5 : 1.2) * value + 128 + ((int)(random & 3u) - 1));
		}
		struct v92_residual filter = { 0 }, copy;
		double gain = 0, score = INFINITY;
		CHECK(v92_residual_fit(&filter, points, raw, minimum, maximum, &gain, &score) && isfinite(score) &&
		      v92_residual_is_valid(&filter), "residual fit rejected an identifiable channel");
		CHECK(filter.delay[0] == delay[0] && filter.delay[1] == delay[1],
		      "residual fit invented or missed delayed channel memory");
		for (unsigned edge = 0; edge < 2; edge++) {
			const double bounds[2][2] = { { .832, .835 }, { 1.995, 2.005 } };
			double bound = bounds[wide][edge], fitted_gain = 0;
			struct v92_residual bounded = { 0 };
			CHECK(v92_residual_fit(&bounded, points, raw, bound, bound, &fitted_gain, NULL) &&
			      fitted_gain == bound && bounded.delay[0] == delay[0] &&
			      bounded.delay[1] == delay[1], "gain boundary changed the delay hypothesis");
		}
		for (unsigned fault = 0; fault < 6; fault++) {
			copy = filter;
			if (fault == 0) copy.cursor = V92_RESIDUAL_LAGS;
			if (fault == 1) copy.count = 3;
			if (fault == 2) copy.offset = NAN;
			if (fault == 3) copy.history[0] = INFINITY;
			if (fault == 4) copy.coefficient[0] = .11;
			if (fault == 5) { copy.active = false; copy.offset = 1; }
			CHECK(!v92_residual_is_valid(&copy), "invalid residual state was accepted");
		}
		copy = filter;
		double energy = 0;
		for (unsigned i = TRAIN; i < TOTAL; i++) {
			int32_t correction, other;
			CHECK(v92_residual_sample(&filter, raw[i], 1, true, &correction) &&
			      v92_residual_sample(&copy, raw[i], -1, true, &other) && correction == other &&
			      !memcmp(&filter, &copy, sizeof(filter)), "independent filters or polarity diverged");
			double error = (raw[i] - correction) * gain - points[i];
			energy += error * error;
		}
		CHECK(energy < 16. * (TOTAL - TRAIN),
		      "frozen residual inverse missed held-out points");
		copy = filter;
		int32_t correction = 123;
		CHECK(!v92_residual_sample(&filter, 0, 1, false, &correction) && correction == 123 &&
		      !memcmp(&copy, &filter, sizeof(filter)), "missing media advanced the inverse");
		copy.offset = -512;
		struct v92_residual saved = copy;
		CHECK(!v92_residual_sample(&copy, INT16_MAX, 1, true, &correction) &&
		      !memcmp(&copy, &saved, sizeof(copy)), "inverse clipped an out-of-range sample");
		saved = filter;
		double saved_score = score;
		CHECK(!v92_residual_fit(&filter, points, raw, NAN, .9, &gain, &score) && score == saved_score &&
		      !memcmp(&filter, &saved, sizeof(filter)), "invalid bounds changed the inverse");
		for (unsigned i = 0; i < TRAIN; i++) raw[i] += 2048;
		CHECK(!v92_residual_fit(&filter, points, raw, minimum, maximum, &gain, NULL) &&
		      !memcmp(&filter, &saved, sizeof(filter)), "excessive DC bias supplied an inverse");
		memset(points, 0, sizeof(points));
		CHECK(!v92_residual_fit(&filter, points, raw, minimum, maximum, &gain, NULL),
		      "singular acquisition supplied an inverse");
	}
	return true;
}

/* Exact delayed channels expose a raw-history handoff transient even when
 * later samples are clean. Check every sample immediately after fitting or
 * translating, without granting the inverse another settling interval. */
static bool test_residual_transition(void)
{
	enum { TRAIN = V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	       TOTAL = TRAIN + 240 };
	const unsigned delays[][2] = { { 3, 11 }, { 1, 12 } };
	const double gains[] = { .5, 1., 2. };
	int32_t points[TOTAL], raw[TOTAL], shifted[TRAIN];
	for (unsigned d = 0; d < ARRAY_LENGTH(delays); d++)
		for (unsigned g = 0; g < ARRAY_LENGTH(gains); g++) {
			uint32_t random = 0x197241;
			for (unsigned i = 0; i < TOTAL; i++) {
				random ^= random << 13; random ^= random >> 17; random ^= random << 5;
				points[i] = ((int)(random % 4) * 2 - 3) * 4000;
				double value = points[i];
				if (i >= delays[d][0]) value += .06 * points[i - delays[d][0]];
				if (i >= delays[d][1]) value -= .03 * points[i - delays[d][1]];
				raw[i] = (int32_t)lround(value / gains[g] + 41.);
			}
			for (int shift = -128; shift <= 128; shift += 128) {
				struct v92_residual filter = { 0 };
				double gain;
				CHECK(v92_residual_fit(&filter, points, raw, gains[g], gains[g], &gain, NULL) &&
				      filter.count == 2 && filter.delay[0] == delays[d][0] &&
				      filter.delay[1] == delays[d][1], "transition channel fit failed");
				if (shift) {
					for (unsigned i = 0; i < TRAIN; i++) shifted[i] = points[i] + shift;
					CHECK(v92_residual_translate(&filter, points, shifted, raw, gain),
					      "transition channel translation failed");
				}
				for (unsigned i = TRAIN; i < TOTAL; i++) {
					int32_t correction;
					CHECK(v92_residual_sample(&filter, raw[i], 1, true, &correction),
					      "transition inverse rejected a physical sample");
					double error = (raw[i] - correction) * gain - (points[i] + shift);
					CHECK(fabs(error) <= gain / 2. + 1e-8,
					      "inverse handoff transient: delays=%u/%u gain=%g shift=%d sample=%u error=%g",
					      delays[d][0], delays[d][1], gain, shift, i - TRAIN, error);
				}
			}
		}
	return true;
}

/* Early unconstrained decisions must not teach the residual fitter their
 * errors. The known-bit candidate ends at acquisition; different later data
 * and a deliberately wrong held-out B1 bit remain independently decoded. */
static bool test_residual_acquisition(unsigned prefix, bool damaged, int transient, bool recover,
	bool seeded, enum v92_cpd_trellis trellis)
{
	const double scale = recover ? .1 : 1.;
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx decoder;
	struct v92_upstream_model model = { .residual_acquiring = true };
	struct v92_upstream_data_line line = {
		.plan.observation = V92_PCM_FRAME_CONSTELLATION,
		.calibration = { .gain = 1.272 * scale, .minimum = 1.18 * scale, .maximum = 1.30 * scale },
		.host.gain = 1., .polarity = 1,
	};
	CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMU, recover ? 12 : 4, false) == V92_CPD_OK,
	      "acquisition mapping init failed");
	cpd.trellis = trellis;
	if (seeded)
		model.seed = (struct v92_upstream_seed){
			.residual = { .active = true, .offset = 128. },
			.gain = 1.237 * scale,
		};
	cpd.constellation[0].count = 32;
	for (unsigned i = 0; i < 32; i++) {
		if (recover)
			CHECK(g711_ucode_sample(G711_LAW_PCMU, (uint8_t)(2 * i + 1), true,
			      &cpd.constellation[0].positive_linear[i]), "channel grid failed");
		else cpd.constellation[0].positive_linear[i] = (int16_t)((2 * i + 1) * 256);
	}
	if (recover)
		for (unsigned i = 0; i < V92_CPD_MODULUS_COUNT; i++)
			cpd.modulus[i] = i % 4 == 3 ? 17 : 34;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&decoder, &mapping) == V92_PCM_FRAME_OK,
	      "acquisition coding init failed");
	double history[6] = { 0 }, error = 0.;
	unsigned cursor = 0, errors = 0, payload_frames = 0;
	struct v92_residual frozen = { 0 };
	for (unsigned frame = 0; frame < prefix + 68; frame++) {
		struct v92_pcm_frame encoded = { 0 };
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		if (frame >= prefix) {
			if (frame - prefix < V92_B1U_FRAMES) memset(bits, 1, mapping.data_bits_per_frame);
			else payload(bits, mapping.data_bits_per_frame, frame - prefix - V92_B1U_FRAMES);
			if (damaged && frame - prefix == 40) bits[7] = 0;
			CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &encoded) == V92_PCM_FRAME_OK,
			      "acquisition source failed");
		} else {
			for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) encoded.u[i] = 256;
		}
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) {
			double point = encoded.u[i];
			int16_t raw = (int16_t)lround((point - .027 * history[(cursor + 2) % 6] +
				.011 * history[cursor]) / (1.237 * scale) + 128.);
			history[cursor] = point; cursor = (cursor + 1) % 6;
			raw = g711_quantize(G711_LAW_PCMU, raw);
			/* An acquisition disturbance must not contaminate the
			 * descrambler used by the otherwise clean validation half. */
			if (frame == (recover ? 7u : V92_B1U_ACQUISITION_FRAMES - 1u) && i == V92_PCM_MAPPING_FRAME_SYMBOLS - 1)
				raw += transient;
			int32_t correction;
			CHECK(v92_residual_sample(&line.residual, raw, 1, true, &correction), "residual sample failed");
			line.raw[i] = raw; line.received[i] = line.wire_known[i] = true;
			line.samples[i] = (int16_t)lround((raw - correction) * line.calibration.gain);
			if (frame >= V92_B1U_ACQUISITION_FRAMES && frame < V92_B1U_FRAMES) {
				double residual = raw - correction - point / line.calibration.gain;
				error += residual * residual;
			}
		}
		struct v92_pcm_frame_decision decision;
		if (recover) {
			double predictions[V92_PCM_MAPPING_FRAME_SYMBOLS];

			if (frame == 7u) {
				/* The retained candidate cannot excuse missing media or an
				 * absent coding history. Rejection preserves the decoder. */
				for (unsigned unavailable = 0; unavailable < 3; unavailable++) {
					struct v92_upstream_model rejected = model;
					struct v92_pcm_frame_rx saved = decoder;
					struct v92_upstream_data_line incomplete = line;

					if (unavailable == 0) incomplete.received[0] = false;
					else if (unavailable == 1) incomplete.wire_known[0] = false;
					else rejected.alternate.path_count = 0;
					CHECK(v92_upstream_model_decode(&rejected, &saved, &incomplete,
					      decoded, &decision, predictions, NULL) == V92_PCM_FRAME_NO_PATH &&
					      !memcmp(&saved, &decoder, sizeof(saved)), "invalid acquisition recovered");
				}
			}
			CHECK(v92_upstream_model_decode(&model, &decoder, &line, decoded,
			      &decision, predictions, NULL) == V92_PCM_FRAME_OK,
			      "acquisition failed at frame %u", frame);
			CHECK(decoder.pending_frames == (frame < V92_PCM_RX_DELAY_FRAMES ?
			      frame + 1u : V92_PCM_RX_DELAY_FRAMES), "prefix lost its traceback slot");
			if (frame == V92_PCM_RX_DELAY_FRAMES)
				CHECK(!decision.bits_ready, "control prefix became payload");
		} else {
			CHECK(v92_pcm_frame_decode(&decoder, line.samples, frame < prefix + V92_B1U_FRAMES,
			      decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK, "independent decode failed");
		}
		v92_upstream_model_observe(&model, &decision, &line);
		v92_upstream_model_fit(&model, &line, &decoder);
		if (frame == V92_B1U_ACQUISITION_FRAMES - 1) {
			frozen = line.residual;
			CHECK(frozen.active && memory_region_is_zero(&model.alternate, sizeof(model.alternate)),
			      "acquisition did not release alternate decoder");
		} else if (frame >= V92_B1U_ACQUISITION_FRAMES) {
			CHECK(line.residual.offset == frozen.offset && line.residual.count == frozen.count &&
			      !memcmp(line.residual.coefficient, frozen.coefficient, sizeof(frozen.coefficient)),
			      "validation or payload retrained the residual model");
		}
		if (decision.bits_ready && frame >= prefix + V92_PCM_RX_DELAY_FRAMES + V92_B1U_ACQUISITION_FRAMES) {
			unsigned source_frame = frame - prefix - V92_PCM_RX_DELAY_FRAMES;
			if (source_frame < V92_B1U_FRAMES) {
				for (unsigned i = 0; i < mapping.data_bits_per_frame; i++) errors += decoded[i] != 1;
			} else {
				payload(bits, mapping.data_bits_per_frame, source_frame - V92_B1U_FRAMES);
				CHECK(!decision.ambiguous && !memcmp(decoded, bits, mapping.data_bits_per_frame),
				      "residual acquisition corrupted later payload");
				payload_frames++;
			}
		}
		line.received_frames++;
	}
	CHECK(errors == (unsigned)damaged && payload_frames == 10 &&
	      error < V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "acquisition missed independent data: prefix=%u damaged=%u errors=%u rms=%g payload=%u",
	      prefix, damaged, errors, sqrt(error / V92_B1U_VALIDATION_SYMBOLS), payload_frames);
	return true;
}

/* A cold primary can follow the wrong coding history through a control
 * prefix. The physical seed describes the channel, not that history. Search
 * B1u independently, then decode its held-out half and different payload
 * without imposing their bits. A coded wrong B1u bit must remain visible. */
static bool test_acquisition_history(enum v92_cpd_trellis trellis,
	unsigned state, unsigned prefix, bool damaged)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx decoder;
	struct v92_upstream_model model = {
		.residual_acquiring = true,
		.seed = { .gain = 1.370,
			.residual = { .active = true, .offset = 128., .count = 2,
				.delay = { 4, 6 }, .coefficient = { -.027, .011 } } },
	};
	struct v92_upstream_data_line line = {
		.plan.observation = V92_PCM_FRAME_CONSTELLATION,
		.calibration = { .gain = 1.367, .minimum = 1.313, .maximum = 1.390 },
		.host.gain = 1., .polarity = 1,
	};
	CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMU, 1, false) == V92_CPD_OK,
	      "history acquisition mapping failed");
	cpd.trellis = trellis;
	cpd.constellation[0].count = 16;
	for (unsigned i = 0; i < 16; i++)
		cpd.constellation[0].positive_linear[i] = (int16_t)((2 * i + 1) * 256);
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&decoder, &mapping) == V92_PCM_FRAME_OK,
	      "history acquisition initialization failed");
	tx.state.trellis.state = (uint8_t)state;
	double history[6] = { 0 }, error = 0.;
	unsigned cursor = 0, errors = 0, payload_frames = 0;
	struct v92_residual frozen = { 0 };
	for (unsigned frame = 0; frame < prefix + V92_B1U_FRAMES + 20; frame++) {
		struct v92_pcm_frame encoded = { 0 };
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		if (frame >= prefix) {
			unsigned source = frame - prefix;
			if (source < V92_B1U_FRAMES) memset(bits, 1, mapping.data_bits_per_frame);
			else payload(bits, mapping.data_bits_per_frame, source - V92_B1U_FRAMES);
			if (damaged && source == 40) bits[7] = 0;
			CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &encoded) ==
			      V92_PCM_FRAME_OK, "history acquisition source failed");
		} else {
			for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) encoded.u[i] = 256;
		}
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; i++) {
			double point = encoded.u[i];
			int16_t raw = (int16_t)lround((point - .027 * history[(cursor + 2) % 6] +
				.011 * history[cursor]) / 1.370 + 128.);
			history[cursor] = point;
			cursor = (cursor + 1) % 6;
			raw = g711_quantize(G711_LAW_PCMU, raw);
			int32_t correction;
			CHECK(v92_residual_sample(&line.residual, raw, 1, true, &correction),
			      "history acquisition correction failed");
			line.raw[i] = raw;
			line.received[i] = line.wire_known[i] = true;
			line.samples[i] = (int16_t)lround((raw - correction) * line.calibration.gain);
			if (frame >= V92_B1U_ACQUISITION_FRAMES && frame < V92_B1U_FRAMES) {
				double residual = raw - correction - point / line.calibration.gain;
				error += residual * residual;
			}
		}
		struct v92_pcm_frame_decision decision;
		/* Fault injection: keep the primary's cold zero history throughout
		 * acquisition. The independent candidate must supply the fresh fit. */
		enum v92_pcm_frame_result result = frame < V92_B1U_ACQUISITION_FRAMES ?
			v92_pcm_frame_acquire_model(&decoder, line.samples, decoded,
				mapping.data_bits_per_frame, &decision) :
			v92_pcm_frame_decode(&decoder, line.samples, frame < prefix + V92_B1U_FRAMES,
				decoded, mapping.data_bits_per_frame, &decision);
		CHECK(result == V92_PCM_FRAME_OK, "history acquisition decode failed");
		v92_upstream_model_observe(&model, &decision, &line);
		v92_upstream_model_fit(&model, &line, &decoder);
		if (frame + 1 == V92_B1U_ACQUISITION_FRAMES) {
			frozen = line.residual;
			CHECK(frozen.active && memory_region_is_zero(&model.alternate, sizeof(model.alternate)) &&
			      memory_region_is_zero(&model.seed, sizeof(model.seed)),
			      "history acquisition retained its candidate or missed the fit");
		} else if (frame >= V92_B1U_ACQUISITION_FRAMES) {
			CHECK(line.residual.offset == frozen.offset && line.residual.count == frozen.count &&
			      !memcmp(line.residual.delay, frozen.delay, sizeof(frozen.delay)) &&
			      !memcmp(line.residual.coefficient, frozen.coefficient, sizeof(frozen.coefficient)),
			      "held-out B1u or payload changed the history acquisition fit");
		}
		if (decision.bits_ready && frame >= prefix + V92_PCM_RX_DELAY_FRAMES +
		    V92_B1U_ACQUISITION_FRAMES) {
			unsigned source = frame - prefix - V92_PCM_RX_DELAY_FRAMES;
			if (source < V92_B1U_FRAMES) {
				for (unsigned i = 0; i < mapping.data_bits_per_frame; i++) errors += decoded[i] != 1;
			} else {
				payload(bits, mapping.data_bits_per_frame, source - V92_B1U_FRAMES);
				CHECK(!decision.ambiguous && !memcmp(decoded, bits, mapping.data_bits_per_frame),
				      "history acquisition corrupted payload");
				payload_frames++;
			}
		}
		line.received_frames++;
	}
	CHECK(errors == (unsigned)damaged && payload_frames == 10 &&
	      error < V92_B1U_VALIDATION_SYMBOLS * V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
	      "history acquisition failed: code=%u state=%u prefix=%u errors=%u rms=%g payload=%u",
	      trellis, state, prefix, errors, sqrt(error / V92_B1U_VALIDATION_SYMBOLS), payload_frames);
	return true;
}

static bool test_residual_translation(void)
{
	enum { COUNT = V92_B1U_ACQUISITION_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS };
	int32_t points[COUNT], shifted[COUNT], raw[COUNT];
	struct v92_residual fitted;
	double gain, score;
	for (unsigned i = 0; i < COUNT; ++i) {
		points[i] = (int32_t)((i * 7919u) % 401u) * 4 - 800;
		raw[i] = points[i] / 4 + 128;
	}
	CHECK(v92_residual_fit(&fitted, points, raw, 4., 4., &gain, &score),
	      "translation source fit failed");
	for (int shift = -512; shift <= 512; shift += 512) {
		struct v92_residual translated = fitted;
		for (unsigned i = 0; i < COUNT; ++i) shifted[i] = points[i] + shift;
		CHECK(v92_residual_translate(&translated, points, shifted, raw, gain),
		      "exact translated fit rejected");
		double response = 1.;
		for (unsigned i = 0; i < fitted.count; ++i) response += fitted.coefficient[i];
		CHECK(translated.count == fitted.count &&
		      !memcmp(translated.delay, fitted.delay, sizeof(fitted.delay)) &&
		      !memcmp(translated.coefficient, fitted.coefficient, sizeof(fitted.coefficient)) &&
		      fabs(translated.offset - (fitted.offset - shift * response / gain)) < 1e-12,
		      "translation changed the response or failed to absorb the DC shift");
		for (unsigned i = 0; i < V92_RESIDUAL_LAGS; ++i)
			CHECK(translated.history[i] == raw[COUNT - V92_RESIDUAL_LAGS + i] - translated.offset,
			      "translation retained the wrong raw history");
	}
	for (unsigned fault = 0; fault < 5; ++fault) {
		struct v92_residual next = fitted, saved;
		for (unsigned i = 0; i < COUNT; ++i) shifted[i] = points[i] + 512;
		double trial_gain = gain;
		if (fault == 0) shifted[COUNT - 1]++;
		else if (fault == 1)
			shifted[COUNT - (V92_PCM_RX_DELAY_FRAMES + 1) *
				V92_PCM_MAPPING_FRAME_SYMBOLS]++;
		else if (fault == 2) trial_gain = NAN;
		else if (fault == 3) next.cursor = 1;
		else for (unsigned i = 0; i < COUNT; ++i) shifted[i] = points[i] + 8192;
		saved = next;
		CHECK(!v92_residual_translate(&next, points, shifted, raw, trial_gain) &&
		      !memcmp(&next, &saved, sizeof(next)),
		      "invalid translation changed the fit: fault %u", fault);
	}
	CHECK(!v92_residual_translate(NULL, points, shifted, raw, gain) &&
	      !v92_residual_translate(&fitted, NULL, shifted, raw, gain) &&
	      !v92_residual_translate(&fitted, points, NULL, raw, gain) &&
	      !v92_residual_translate(&fitted, points, shifted, NULL, gain),
	      "translation accepted missing inputs");
	return true;
}

/* Gain four and DC 128 shift each observation by one 512-unit point step.
 * Equivalent physical fits must not retain the wrong coding history.
 * The later half still decodes independently, including a deliberately
 * wrong source bit and the payload following B1u. */
static bool test_translated_acquisition(bool damaged)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_pcm_frame_rx decoder;
	struct v92_upstream_model model = { .residual_acquiring = true };
	struct v92_upstream_data_line line = {
		.plan.observation = V92_PCM_FRAME_CONSTELLATION,
		.calibration = { .gain = 4., .minimum = 4., .maximum = 4. },
		.host.gain = 1., .polarity = 1,
	};
	CHECK(v92_cpd_direct_init(&cpd, G711_LAW_PCMU, 9, false) == V92_CPD_OK,
	      "translated acquisition mapping failed");
	cpd.constellation[0].count = 32;
	for (unsigned i = 0; i < 32; ++i)
		cpd.constellation[0].positive_linear[i] = (int16_t)((2 * i + 1) * 256);
	for (unsigned i = 0; i < V92_CPD_MODULUS_COUNT; ++i)
		cpd.modulus[i] = i % 4 == 3 ? 12 : 24;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) == V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
	      v92_pcm_frame_rx_init_constellation(&decoder, &mapping) == V92_PCM_FRAME_OK,
	      "translated acquisition initialization failed");
	unsigned errors = 0, payload_frames = 0;
	for (unsigned frame = 0; frame < V92_B1U_FRAMES + 20; ++frame) {
		uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame encoded;
		struct v92_pcm_frame_decision decision;
		if (frame < V92_B1U_FRAMES) memset(bits, 1, mapping.data_bits_per_frame);
		else payload(bits, mapping.data_bits_per_frame, frame - V92_B1U_FRAMES);
		if (damaged && frame == 40) bits[7] = 0;
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &encoded) == V92_PCM_FRAME_OK,
		      "translated acquisition source failed");
		for (unsigned i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			int32_t raw = encoded.u[i] / 4 + 128, correction;
			CHECK(v92_residual_sample(&line.residual, raw, 1, true, &correction),
			      "translated residual sample failed");
			line.raw[i] = raw;
			line.received[i] = line.wire_known[i] = true;
			line.samples[i] = (int16_t)((raw - correction) * line.calibration.gain);
		}
		CHECK(v92_pcm_frame_decode(&decoder, line.samples, frame < V92_B1U_FRAMES,
		      decoded, mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
		      "translated acquisition decode failed at %u", frame);
		v92_upstream_model_observe(&model, &decision, &line);
		v92_upstream_model_fit(&model, &line, &decoder);
		if (frame == V92_B1U_ACQUISITION_FRAMES - 1)
			CHECK(line.residual.active, "translated acquisition did not fit the channel");
		if (decision.bits_ready && frame >= V92_PCM_RX_DELAY_FRAMES + V92_B1U_ACQUISITION_FRAMES) {
			unsigned source = frame - V92_PCM_RX_DELAY_FRAMES;
			if (source < V92_B1U_FRAMES) {
				for (unsigned i = 0; i < mapping.data_bits_per_frame; ++i)
					errors += decoded[i] != 1;
			} else {
				payload(bits, mapping.data_bits_per_frame, source - V92_B1U_FRAMES);
				CHECK(!decision.ambiguous && !memcmp(decoded, bits, mapping.data_bits_per_frame),
				      "translated fit corrupted payload");
				payload_frames++;
			}
		}
		line.received_frames++;
	}
	CHECK(errors == (unsigned)damaged && payload_frames == 10,
	      "translated fit missed independent B1 or payload: %u/%u", errors, payload_frames);
	return true;
}

/* A single final codec after a known linear channel must not acquire an
 * unnecessary first-codec model during B1u. Exercise channel selection and
 * the E2u-to-B1u handoff before checking independent payload bits. */
static bool test_single_codec(enum g711_law law)
{
	enum { BITS = 54, PAYLOAD = 128,
		FRAMES = V92_B1U_FRAMES + PAYLOAD + V92_PCM_RX_DELAY_FRAMES, EXTRA = 6 };
	const int16_t lu = 1898;
	struct channel measured_line = { .gain = 1., .postcursor = .25, .law = law, .polarity = 1 };
	struct v92_upstream_channel measured = { 0 };
	uint32_t random = UINT32_C(0x2468ace);
	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_SYMBOLS; i++) {
		random ^= random << 13; random ^= random >> 17; random ^= random << 5;
		int16_t input = (int16_t)(8 * ((int)(random % 2048) - 1024));
		CHECK(v92_upstream_channel_sample(&measured, input, wire_sample(&measured_line, input)),
			"single-codec channel measurement failed");
	}
	struct v92_cpd_parameters cpd;
	struct v92_upstream_plan plan;
	struct v92_pcm_mapping mapping;
	CHECK(v92_cpd_direct_select(&cpd, law, 10, lu, false) == V92_CPD_OK &&
		v92_cpd_channel_design(&cpd, &measured, law, lu, &plan) &&
		v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK,
		"single-codec channel design failed");
	CHECK(mapping.data_bits_per_frame == BITS, "single-codec rate changed");
	struct v92_pcm_frame_tx encoder;
	CHECK(v92_pcm_frame_tx_init(&encoder, &mapping) == V92_PCM_FRAME_OK,
		"single-codec encoder failed");
	struct channel data_line = { .gain = 1., .postcursor = .25, .law = law, .polarity = 1 };
	uint8_t octets[(FRAMES + EXTRA) * V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t samples[(FRAMES + EXTRA) * V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t expected[PAYLOAD][BITS];
	random = UINT32_C(0x10203041) ^ UINT32_C(0xa17c001);
	for (unsigned f = 0; f < FRAMES + EXTRA; f++) {
		uint8_t bits[BITS];
		for (unsigned b = 0; b < BITS; b++) {
			if (f < V92_B1U_FRAMES) bits[b] = 1;
			else {
				random = random * UINT32_C(1664525) + UINT32_C(1013904223);
				bits[b] = (uint8_t)(random >> 31);
			}
		}
		if (f >= V92_B1U_FRAMES && f < V92_B1U_FRAMES + PAYLOAD)
			memcpy(expected[f - V92_B1U_FRAMES], bits, BITS);
		struct v92_pcm_frame frame;
		CHECK(v92_pcm_frame_encode(&encoder, bits, BITS, &frame) == V92_PCM_FRAME_OK,
			"single-codec encode failed");
		for (unsigned n = 0; n < V92_PCM_MAPPING_FRAME_SYMBOLS; n++) {
			int16_t sample;
			CHECK(v92_pcm_frame_scale(cpd.four_times_gain_q0_16, lu, frame.v[n], &sample) ==
				V92_PCM_FRAME_OK, "single-codec scale failed");
			unsigned i = f * V92_PCM_MAPPING_FRAME_SYMBOLS + n;
			samples[i] = wire_sample(&data_line, sample);
			octets[i] = data_line.code;
		}
	}
	struct channel control_line = { .gain = 1., .postcursor = .25, .law = law, .polarity = 1 };
	struct v92_phase4_record_tx control_tx;
	struct v92_e2u_rx control_rx;
	CHECK(prepare_e2u_at_level(&control_line, false, &control_tx, &control_rx,
		lu, lu, 0., V92_CONSTELLATION_4_POINT), "single-codec E2u preparation failed");
	unsigned input = 0;
	while (!v92_e2u_rx_is_complete(&control_rx)) {
		int16_t sample;
		if (!v92_phase4_record_tx_is_complete(&control_tx)) {
			CHECK(v92_phase4_record_tx_next(&control_tx, &sample) == V92_PHASE4_RECORD_TX_OK,
				"single-codec E2u TX failed");
			sample = wire_sample(&control_line, sample);
		} else {
			CHECK(input < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY,
				"single-codec E2u exceeded prefix");
			sample = samples[input++];
		}
		enum v92_e2u_rx_result result = v92_e2u_rx_sample(&control_rx, sample);
		CHECK(result == V92_E2U_RX_OK || result == V92_E2U_RX_COMPLETE,
			"single-codec E2u RX failed");
	}
	CHECK(input == V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY, "single-codec E2u delay changed");
	struct v92_upstream_data_rx receiver;
	CHECK(v92_upstream_data_rx_init(&receiver, &control_rx.signal, &mapping, &plan) ==
		V92_UPSTREAM_DATA_RX_OK, "single-codec handoff failed");
	unsigned frames = 0, checked = 0, errors = 0, mismatch = 0, ambiguity = 0;
	double error = 0.;
	while (frames < FRAMES) {
		CHECK(input < ARRAY_LENGTH(samples), "single-codec receiver exceeded source");
		struct v92_upstream_data_frame frame;
		enum v92_upstream_data_rx_result result = fixture_octet(&receiver,
			octets[input], samples[input], true, &frame);
		input++;
		CHECK(result == V92_UPSTREAM_DATA_RX_OK || result == V92_UPSTREAM_DATA_RX_FRAME,
			"single-codec data receive failed");
		if (result != V92_UPSTREAM_DATA_RX_FRAME) continue;
		unsigned f = frames++;
		if (f >= V92_B1U_ACQUISITION_FRAMES && f < V92_B1U_FRAMES) {
			double gain = receiver.line.host.gain;
			error += gain > 0. ? frame.squared_error / gain / gain : frame.squared_error;
		}
		if (f >= V92_B1U_ACQUISITION_FRAMES + V92_PCM_RX_DELAY_FRAMES &&
			f < V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			mismatch |= frame.b1u_mismatches;
			ambiguity += frame.ambiguous;
			CHECK(frame.bit_count == BITS, "single-codec B1u missing");
		}
		if (f >= V92_B1U_FRAMES + V92_PCM_RX_DELAY_FRAMES) {
			CHECK(frame.bit_count == BITS && !frame.b1u && !frame.ambiguous,
				"single-codec payload missing or ambiguous");
			for (unsigned b = 0; b < BITS; b++)
				errors += frame.bits[b] != expected[f - V92_B1U_FRAMES - V92_PCM_RX_DELAY_FRAMES][b];
			checked += BITS;
		}
	}
	CHECK(mismatch != V92_B1U_BOTH_MISMATCH && !ambiguity &&
		error <= V92_B1U_VALIDATION_SYMBOLS * (double)V92_DATA_MAX_RMS_ERROR * V92_DATA_MAX_RMS_ERROR,
		"single-codec B1u rejected");
	CHECK(checked == PAYLOAD * BITS && errors == 0,
		"single-codec law %u has %u payload errors / %u", law, errors, checked);
	return true;
}

/* A DC translation leaves the physical acquisition fit unchanged. Its
 * roundoff must not displace an independently known B1 coding history,
 * including when an earlier physical correction supplied the candidate. */
static bool test_translated_acquisition_seed(enum g711_law law,
	enum v92_cpd_trellis trellis, bool seeded)
{
	struct v92_cpd_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx tx;
	struct v92_upstream_data_rx rx = { 0 };
	int16_t points[V92_OBSERVATION_TRAINING];
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS], decoded[V92_PCM_MAPPING_MAX_BITS];

	CHECK(v92_cpd_direct_init(&cpd, law, 1u, false) == V92_CPD_OK, "translation CPd failed");
	cpd.trellis = trellis;
	CHECK(v92_pcm_mapping_init(&mapping, law, &cpd) == V92_PCM_MAPPING_OK &&
		  v92_pcm_frame_tx_init(&tx, &mapping) == V92_PCM_FRAME_OK &&
		  v92_pcm_frame_rx_init_constellation(&rx.decoder, &mapping) == V92_PCM_FRAME_OK,
		  "translation coding failed");
	memset(bits, 1, mapping.data_bits_per_frame);
	for (unsigned f = 0; f < V92_B1U_ACQUISITION_FRAMES; ++f) {
		struct v92_pcm_frame frame;
		struct v92_pcm_frame_decision decision;
		CHECK(v92_pcm_frame_encode(&tx, bits, mapping.data_bits_per_frame, &frame) ==
			  V92_PCM_FRAME_OK, "translation B1 encode failed");
		memcpy(points + V92_PCM_MAPPING_FRAME_SYMBOLS * f, frame.u, sizeof(frame.u));
		if (f + 1u < V92_B1U_ACQUISITION_FRAMES)
			CHECK(v92_pcm_frame_acquire_model(&rx.decoder, frame.u, decoded,
				  mapping.data_bits_per_frame, &decision) == V92_PCM_FRAME_OK,
				  "translation B1 acquire failed");
	}
	/* Stage the final acquisition transaction. The ordinary candidate's
	 * coordinates have a 512-unit DC alias and a disputed old payload bit.
	 * The known candidate has consumed the same first 23 complete frames. */
	rx.model.alternate = rx.decoder;
	for (unsigned i = 0; i < rx.decoder.path_count; ++i)
		rx.decoder.paths[i].pending[0][0] = 0;
	for (unsigned i = 0; i < V92_OBSERVATION_TRAINING; ++i) {
		rx.model.x[i] = points[i] + 512;
		rx.model.u[i] = points[i];
		rx.model.raw[i] = points[i] + 128;
	}
	/* Zero residual makes both scalar fits hit the same score floor. */
	rx.model.residual_acquiring = true;
	rx.line.received_frames = V92_B1U_ACQUISITION_FRAMES - 1u;
	rx.line.calibration.gain = 1.;
	rx.line.calibration.minimum = .9;
	rx.line.calibration.maximum = 1.1;
	rx.line.host.gain = 1.;
	if (seeded) {
		rx.model.seed.residual = (struct v92_residual){ .active = true, .offset = 128. };
		rx.model.seed.gain = 1.;
	}
	v92_upstream_model_fit(&rx.model, &rx.line, &rx.decoder);
	bool known = !seeded || v34_trellis_state_count(tx.state.trellis.selection) <= V92_PCM_RX_PATHS;
	CHECK(rx.line.residual.active && fabs(rx.line.calibration.gain - 1.) < 1e-10 &&
		  fabs(rx.line.residual.offset - (known ? 128. : -384.)) < 1e-8 &&
		  rx.decoder.paths[0].pending[0][0] == known &&
		  !rx.model.residual_acquiring && !rx.model.seed.residual.active,
		  "translated fit lost known coding: law=%u trellis=%u seeded=%u offset=%g bit=%u",
		  law, trellis, seeded, rx.line.residual.offset, rx.decoder.paths[0].pending[0][0]);
	return true;
}

int main(void)
{
	for (int polarity = -1; polarity <= 1; polarity += 2)
		if (!test_codec_model(polarity, false, 19, false, true, polarity < 0 ? 1 : 40)) return 1;
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (enum v92_cpd_trellis trellis = V92_CPD_TRELLIS_16_STATE;
			 trellis <= V92_CPD_TRELLIS_64_STATE; ++trellis)
			for (unsigned seeded = 0; seeded < 2; ++seeded)
				if (!test_translated_acquisition_seed(law, trellis, seeded))
					return 1;

	if (!test_residual_translation() || !test_translated_acquisition(false) ||
	    !test_translated_acquisition(true)) return 1;
	if (!test_affine_data()) return 1;
	for (unsigned prefix = 0; prefix < 2; prefix++)
		for (unsigned damaged = 0; damaged < 2; damaged++)
			for (int transient = -384; transient <= 384; transient += 384)
				if (!test_residual_acquisition(prefix, damaged, transient, false,
				    false, V92_CPD_TRELLIS_16_STATE)) return 1;
	/* One small acquisition disturbance exhausts the unconstrained 38,667
	 * mapping at frame seven. The known candidate must preserve later data
	 * and must still expose a deliberately wrong held-out training bit. */
	for (unsigned damaged = 0; damaged < 2; damaged++)
		if (!test_residual_acquisition(1, damaged, 128, true,
		    false, V92_CPD_TRELLIS_16_STATE)) return 1;
	/* A rate change can retain a physical seed while a short disturbance
	 * exhausts the primary decoder. Known B1u acquisition remains eligible
	 * with a complete 16/32-state search; the wrong held-out bit stays visible. */
	for (enum v92_cpd_trellis code = V92_CPD_TRELLIS_16_STATE; code <= V92_CPD_TRELLIS_32_STATE; code++)
		for (unsigned damaged = 0; damaged < 2; damaged++)
			if (!test_residual_acquisition(1, damaged, 128, true, true, code)) return 1;
	for (enum v92_cpd_trellis code = V92_CPD_TRELLIS_16_STATE; code <= V92_CPD_TRELLIS_32_STATE; code++)
		for (unsigned state = 0; state < (16u << code); state++)
			for (unsigned prefix = 0; prefix < 2; prefix++)
				for (unsigned damaged = 0; damaged < 2; damaged++)
					if (!test_acquisition_history(code, state, prefix, damaged)) return 1;
	if (!test_residual_transition() || !test_residual_inverse()) return 1;
	if (!test_data_initialization()) return 1;
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned fault = 0; fault < 2; fault++)
				if (!test_scalar_model(law, polarity, fault)) return 1;
	/* Minimal 48k alphabets must preserve payload after real B1u level
	 * refinement, not merely separate symbols at a nominal training LU. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		if (!test_quantized_level_tracking(law, 19u, false, 9461, true, 0u, 0u) ||
		    !test_quantized_level_tracking(law, 19u, false, 4091, true, 0u, 16u) ||
		    !test_quantized_level_tracking(law, 19u, false, 5471, true, 0u, 32u) ||
		    !test_quantized_level_tracking(law, 19u, false, 7777, true, 0u, 32u) ||
		    !test_quantized_level_tracking(law, 19u, false, 7040, true, 0u, 32u))
			return 1;
	/* The 46 2/3k selector must survive level uncertainty, long idle, and
	 * bounded pre-codec noise, not just produce nominally distinct points. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		if (!test_quantized_level_tracking(law, 18u, false, 2019, true, 0u, 0u) ||
		    !test_quantized_level_tracking(law, 18u, false, 5471, true, 0u, 0u) ||
		    !test_quantized_level_tracking(law, 18u, false, 5471, true, 0u, 16u) ||
		    !test_quantized_level_tracking(law, 18u, false, 7040, true, 0u, 32u))
			return 1;
	/* A constellation that is distinct only at the decoded TRN1u level
	 * can become ambiguous at another level in the same quantizer cell.
	 * Exercise 44k through B1u, long idle and payload under both laws. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		if (!test_quantized_level_tracking(law, 16u, false, 5987, true, 0u, 0u) ||
		    !test_quantized_level_tracking(law, 16u, false, 7139, true, 0u, 0u))
			return 1;
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned fault = 0u; fault < 2u; ++fault)
				for (unsigned control = 0u; control < 2u; ++control)
					if (!test_direct_b1_validation(law, polarity, fault, control))
						return 1;
	if (!test_codec_model(1, false, 19u, false, false, false) || !test_codec_model(-1, false, 19u, false, false, false) ||
	    !test_codec_model(1, true, 19u, false, false, false) || !test_codec_model(-1, true, 19u, false, false, false))
		return 1;
	for (int polarity = -1; polarity <= 1; polarity += 2)
		if (!test_codec_model(polarity, false, 1u, true, false, false) ||
		    !test_codec_model(polarity, false, 4u, true, false, false) ||
		    !test_codec_model(polarity, false, 1u, true, true, false))
			return 1;
	fprintf(stderr, "[ RUN      ] retained PCM data-to-Ru control channel\n");
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned extended = 0; extended < 2; ++extended)
				for (unsigned compensated = 0; compensated < 2; ++compensated)
					for (unsigned constellation = V92_CONSTELLATION_4_POINT;
					     constellation <= V92_CONSTELLATION_8_POINT; constellation *= 2u)
						if (!test_rate_request_line(law, polarity, extended, compensated, constellation))
							return 1;
	fprintf(stderr, "[       OK ] retained PCM data-to-Ru control channel\n");
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned int extended = 0u; extended < 2u; ++extended)
				for (unsigned int fixture = 0u; fixture < 8u; ++fixture)
					if (!test_channel_plan((enum g711_law)law, polarity, extended,
							fixture % 4u == 1u, fixture % 4u == 2u,
							fixture % 4u == 3u, fixture >= 4u, 0, CHANNEL_OK))
						return 1;
	fprintf(stderr, "[ RUN      ] PCM upstream acquisition seeds and changed channels\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (int seed = -384; seed <= 128; seed += 512)
				for (enum channel_fault fault = CHANNEL_OK; fault <= CHANNEL_LOSS; fault++)
					if (!run_channel_plan(law, polarity, false, false, false,
					    false, true, 128, fault, &seed)) return 1;
	fprintf(stderr, "[       OK ] PCM upstream acquisition seeds and changed channels\n");
	fprintf(stderr, "[ RUN      ] bounded PCM upstream offset acquisition\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned conditioned = 0; conditioned < 2; ++conditioned)
				for (int sign = -1; sign <= 1; sign += 2)
					for (enum channel_fault fault = CHANNEL_OK; fault <= CHANNEL_EXCESSIVE_OFFSET; ++fault)
						if (!test_channel_plan(law, polarity, false, true, conditioned,
						      false, true, sign * (fault == CHANNEL_EXCESSIVE_OFFSET ? 768 : 48), fault))
							return 1;
	fprintf(stderr, "[       OK ] bounded PCM upstream offset acquisition\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; law++)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned conditioned = 0; conditioned < 2; conditioned++)
				if (!test_channel_plan(law, polarity, false, false, conditioned,
				    false, true, 128, CHANNEL_RESIDUAL)) return 1;
	if (!test_quantized_level_tracking(G711_LAW_PCMU, 19u, false, TEST_LU, false, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 19u, false, TEST_LU, false, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 13u, false, TEST_LU, false, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 13u, false, TEST_LU, false, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 8u, true, TEST_LU, false, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 19u, false, 7164, true, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMA, 19u, false, 7040, true, 0u, 0u))
		return 1;
	fprintf(stderr, "[ RUN      ] PCM upstream codec-silence burst recovery\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		if (!test_quantized_level_tracking(law, 19u, false, TEST_LU, false, 80u, 0u))
			return 1;
	fprintf(stderr, "[       OK ] PCM upstream codec-silence burst recovery\n");
	const struct {
		uint8_t rate;
		double gain;
		double postcursor;
	} cases[] = {
		{ 1u, 1.0, 0.0 }, { 8u, 1.0, 0.0 }, { 13u, 1.0, 0.0 },
		{ 1u, 0.75, 0.0 }, { 8u, 0.75, 0.0 }, { 13u, 0.75, 0.0 },
		/* Channel memory at the two lower rates; dense CPd channel
		 * selection is not established by these line tests. */
		{ 1u, 0.75, 0.10 }, { 8u, 0.75, 0.10 },
	};

	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned int extended = 0u; extended < 2u; ++extended)
				for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index)
					if (!run_stream((enum g711_law)law, polarity, extended,
							cases[index].rate, cases[index].gain,
							cases[index].postcursor, STREAM_DATA))
						return 1;
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int distorted = 0u; distorted < 2u; ++distorted)
			if (!run_stream((enum g711_law)law, -1, false, 8u,
					distorted ? 0.75 : 1.0, distorted ? 0.10 : 0.0, STREAM_RESET))
				return 1;
	/* Exercise both quantized-LU offsets and the normal training level
	 * through E2u, all B1u decisions, long idle and exact payload. */
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t rate = 15u; rate <= 17u; rate += 2u)
			if (!test_quantized_level_tracking(law, rate, false, 4092, true, 0u, 0u) ||
			    !test_quantized_level_tracking(law, rate, false, 7164, true, 0u, 0u))
				return 1;
	/* Later B1u evidence must recover LU values excluded by an early,
	 * slightly biased equalized frame, before freezing the estimate. */
	if (!test_quantized_level_tracking(G711_LAW_PCMU, 16u, false, 2019, true, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 17u, false, 4187, true, 0u, 0u) ||
	    !test_quantized_level_tracking(G711_LAW_PCMU, 17u, false, 7139, true, 0u, 0u))
		return 1;
	const uint8_t rm_rates[] = { 1u, 16u, 19u };
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned i = 0; i < ARRAY_LENGTH(rm_rates); ++i)
			if (!test_fast_control(law, rm_rates[i]) ||
			    !test_fast_finish(law, rm_rates[i], rm_rates[(i + 1) % ARRAY_LENGTH(rm_rates)], false) ||
			    !test_fast_finish(law, rm_rates[i], rm_rates[(i + 1) % ARRAY_LENGTH(rm_rates)], true)) return 1;
	for (unsigned law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (int polarity = -1; polarity <= 1; polarity += 2)
			for (unsigned i = 0u; i < ARRAY_LENGTH(rm_rates); ++i)
				if (!run_stream(law, polarity, false, rm_rates[i], 1.0, 0.0, STREAM_RM)) return 1;
	return test_single_codec(G711_LAW_PCMU) && test_single_codec(G711_LAW_PCMA) &&
		test_owned_boundaries() && test_rejection_and_failure() ? 0 : 1;
}
