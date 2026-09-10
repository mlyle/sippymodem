// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_frame.h"

#include "integer_math.h"
#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum {
	V92_PCM_TRELLIS_INTERVAL_SYMBOLS = 4,
	V92_PCM_GAIN_SHIFT = 18,
};

static bool bits_are_valid(const uint8_t *bits, size_t bit_count)
{
	if (bits == NULL)
		return false;
	/* Each byte is a bit. Check whole words without alignment or aliasing
	 * assumptions; every byte's upper seven bits must be clear. */
	while (bit_count >= sizeof(uint64_t)) {
		uint64_t word;

		memcpy(&word, bits, sizeof(word));
		if (word & UINT64_C(0xfefefefefefefefe))
			return false;
		bits += sizeof(word);
		bit_count -= sizeof(word);
	}
	for (size_t bit = 0u; bit < bit_count; ++bit) {
		if (bits[bit] > 1u)
			return false;
	}
	return true;
}

static bool trellis_selection(
	enum v92_cpd_trellis selected,
	enum v34_trellis_selection *trellis)
{
	if (trellis == NULL)
		return false;
	switch (selected) {
	case V92_CPD_TRELLIS_16_STATE:
		*trellis = V34_TRELLIS_16_STATE;
		return true;
	case V92_CPD_TRELLIS_32_STATE:
		*trellis = V34_TRELLIS_32_STATE;
		return true;
	case V92_CPD_TRELLIS_64_STATE:
		*trellis = V34_TRELLIS_64_STATE;
		return true;
	}
	return false;
}

static bool state_is_valid(
	const struct v92_pcm_frame_tx_state *state,
	enum v92_cpd_trellis selected)
{
	enum v34_trellis_selection expected;

	return state != NULL && state->differential <= 1u &&
	       v34_scrambler_is_valid(&state->scrambler) &&
	       state->scrambler.polynomial == V34_POLYNOMIAL_GPA &&
	       trellis_selection(selected, &expected) &&
	       v34_convolutional_encoder_is_valid(&state->trellis) &&
	       state->trellis.selection == expected;
}

static bool state_init(
	struct v92_pcm_frame_tx_state *state,
	enum v92_cpd_trellis selected)
{
	enum v34_trellis_selection trellis;

	if (state == NULL || !trellis_selection(selected, &trellis))
		return false;
	memset(state, 0, sizeof(*state));
	return v34_scrambler_init(
		       &state->scrambler, V34_POLYNOMIAL_GPA) ==
		       V34_FRAMING_OK &&
	       v34_convolutional_encoder_init(
		       &state->trellis, trellis) == V34_TRELLIS_OK;
}

bool v92_pcm_frame_tx_is_valid(
	const struct v92_pcm_frame_tx *transmitter)
{
	return transmitter != NULL &&
	       v92_pcm_mapping_is_valid(&transmitter->mapping) &&
	       state_is_valid(
		       &transmitter->state,
		       transmitter->mapping.cpd.trellis);
}

enum v92_pcm_frame_result v92_pcm_frame_tx_init(
	struct v92_pcm_frame_tx *transmitter,
	const struct v92_pcm_mapping *mapping)
{
	struct v92_pcm_frame_tx initialized;

	if (transmitter == NULL || mapping == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	memset(&initialized, 0, sizeof(initialized));
	initialized.mapping = *mapping;
	if (!state_init(&initialized.state, mapping->cpd.trellis))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	*transmitter = initialized;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_tx_reset_b1u(
	struct v92_pcm_frame_tx *transmitter)
{
	struct v92_pcm_frame_tx_state reset;

	if (transmitter == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(&transmitter->mapping) ||
	    !state_init(&reset, transmitter->mapping.cpd.trellis))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	transmitter->state = reset;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_tx_reset_control(
	struct v92_pcm_frame_tx *transmitter)
{
	if (transmitter == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_tx_is_valid(transmitter))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	transmitter->state.scrambler.history = 0u;
	transmitter->state.differential = 0u;
	return V92_PCM_FRAME_OK;
}

static int positive_modulo(int value, unsigned int modulus)
{
	int reduced = value % (int)modulus;

	return reduced < 0 ? reduced + (int)modulus : reduced;
}

static size_t x_history_count(const struct v92_pcm_mapping *mapping)
{
	size_t count =
		mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK].count;
	size_t feed_forward =
		mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD].count;

	if (feed_forward > 1u && count < feed_forward - 1u)
		count = feed_forward - 1u;
	return count;
}

static const int32_t *u_history(
	const struct v92_pcm_frame_filter_state *filter)
{
	return filter->history;
}

static const int32_t *x_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	return filter->history +
	       mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD].count;
}

static const int32_t *v_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	return x_history(mapping, filter) + x_history_count(mapping);
}

/* A validated path fixes the fourth-symbol coset before point enumeration.
 * Other symbols use every index; the fourth uses eta = offset + 2*k. */
static int equivalence_offset(size_t symbol,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], uint8_t y0)
{
	if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS != 3u)
		return 0;
	size_t base = symbol - 3u;

	return positive_modulo(selected[base] + selected[base + 1u] +
		selected[base + 2u] + y0, 2u);
}

static int64_t precoder_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEEDBACK];
	const int32_t *u_values = u_history(filter);
	const int32_t *x_values = x_history(mapping, filter);
	int64_t accumulator = 0;

	for (size_t tap = 0u; tap < feed_forward->count; ++tap)
		accumulator += (int64_t)u_values[tap] *
			       feed_forward->coefficient[tap];
	for (size_t tap = 0u; tap < feedback->count; ++tap)
		accumulator += (int64_t)x_values[tap] *
			       feedback->coefficient[tap] * 2;
	return accumulator;
}

static int64_t prefilter_history(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];
	const int32_t *x_values = x_history(mapping, filter);
	const int32_t *v_values = v_history(mapping, filter);
	int64_t accumulator = 0;

	for (size_t tap = 1u; tap < feed_forward->count; ++tap)
		accumulator += (int64_t)x_values[tap - 1u] *
			       feed_forward->coefficient[tap];
	for (size_t tap = 0u; tap < feedback->count; ++tap)
		accumulator += (int64_t)v_values[tap] *
			       feedback->coefficient[tap] * 2;
	return accumulator;
}

/* A path's past inputs are fixed across candidate points. Retain the full
 * accumulator: rounding the history first would change half-way cases when
 * the current input changes its sign, and can lose cancellation at clipping. */
static int32_t precoder_output(int64_t history, int32_t u)
{
	return round_shift_s64_to_s32_saturating(
		history + (int64_t)u * INT64_C(32768), 15u);
}

static int32_t prefilter_output(
	const struct v92_pcm_mapping *mapping, int64_t history, int32_t x)
{
	const struct v92_cpd_filter *feed_forward =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEED_FORWARD];
	const struct v92_cpd_filter *feedback =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];

	if (feed_forward->count == 0u && feedback->count == 0u)
		return x;
	int64_t accumulator = history;
	if (feed_forward->count != 0u)
		accumulator += (int64_t)x * feed_forward->coefficient[0];
	return round_shift_s64_to_s32_saturating(accumulator, 15u);
}

static void push_history(int32_t *history, size_t count, int32_t value)
{
	if (count == 0u)
		return;
	if (count > 1u)
		memmove(
			history + 1u, history,
			(count - 1u) * sizeof(history[0]));
	history[0] = value;
}

static void filter_step(
	const struct v92_pcm_mapping *mapping,
	struct v92_pcm_frame_filter_state *filter, int32_t u,
	int32_t *x, int32_t *v)
{
	const struct v92_cpd_filter *z1 =
		&mapping->cpd.filter[V92_CPD_PRECODER_FEED_FORWARD];
	const struct v92_cpd_filter *p2 =
		&mapping->cpd.filter[V92_CPD_PREFILTER_FEEDBACK];
	size_t x_count = x_history_count(mapping);
	int32_t *u_values = filter->history;
	int32_t *x_values = u_values + z1->count;
	int32_t *v_values = x_values + x_count;

	*x = precoder_output(precoder_history(mapping, filter), u);
	*v = prefilter_output(mapping, prefilter_history(mapping, filter), *x);
	push_history(u_values, z1->count, u);
	push_history(x_values, x_count, *x);
	push_history(v_values, p2->count, *v);
}

static bool select_point(
	const struct v92_pcm_mapping *mapping,
	const struct v92_pcm_frame_filter_state *filter,
	size_t symbol, uint8_t expected_digit,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], uint8_t y0,
	int16_t *eta_out, int16_t *u_out)
{
	const struct v92_cpd_constellation *constellation;
	int64_t best_score = 0;
	int16_t best_eta = 0;
	int16_t best_u = 0;
	bool found = false;
	int64_t history = precoder_history(mapping, filter);
	uint8_t constellation_index =
		mapping->cpd.frame_constellation[
			symbol % V92_PCM_MAPPING_PHASES];

	constellation = &mapping->cpd.constellation[constellation_index];
	unsigned stride = symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u ? 2u : 1u;
	unsigned step = stride * mapping->cpd.modulus[symbol];
	int first = -(int)constellation->count;
	int offset = equivalence_offset(symbol, selected, y0) + (int)(stride * expected_digit);

	first += positive_modulo(offset - first, step);
	for (int eta = first; eta < (int)constellation->count; eta += (int)step) {
		uint8_t ordinal;
		int16_t positive;
		int16_t u;
		int32_t x;
		int64_t score;

		ordinal = (uint8_t)(eta >= 0 ? eta : -eta - 1);
		positive = constellation->positive_linear[ordinal];
		u = eta >= 0 ? positive : (int16_t)-positive;
		x = precoder_output(history, u);
		score = x < 0 ? -(int64_t)x : (int64_t)x;
		if (!found || score < best_score) {
			found = true;
			best_score = score;
			best_eta = (int16_t)eta;
			best_u = u;
		}
	}
	if (!found)
		return false;
	*eta_out = best_eta;
	*u_out = best_u;
	return true;
}

static bool advance_trellis(
	struct v34_convolutional_encoder *trellis,
	const int16_t selected[V92_PCM_MAPPING_FRAME_SYMBOLS], size_t last)
{
	struct v34_grid_point first = {
		.in_phase = (int16_t)(2 * selected[last - 3u] + 1),
		.quadrature = (int16_t)(2 * selected[last - 2u] + 1),
	};
	struct v34_grid_point second = {
		.in_phase = (int16_t)(2 * selected[last - 1u] + 1),
		.quadrature = (int16_t)(2 * selected[last] + 1),
	};
	struct v34_convolutional_input input;
	uint8_t first_subset;
	uint8_t second_subset;

	return v34_grid_subset_label(&first, &first_subset) ==
		       V34_TRELLIS_OK &&
	       v34_grid_subset_label(&second, &second_subset) ==
		       V34_TRELLIS_OK &&
	       v34_convolutional_input_from_subset_labels(
		       first_subset, second_subset, &input) ==
		       V34_TRELLIS_OK &&
	       v34_convolutional_encoder_advance(trellis, &input) ==
		       V34_TRELLIS_OK;
}

static int16_t scale_sample(
	const struct v92_pcm_mapping *mapping, int16_t upstream_level,
	int32_t v)
{
	return clamp_s16(round_shift_s64_to_s32_saturating(
		(int64_t)v * mapping->cpd.four_times_gain_q0_16 *
			upstream_level, V92_PCM_GAIN_SHIFT));
}

static int16_t quantized_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ?
		g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static uint64_t level_error(
	const struct v92_pcm_mapping *mapping, int16_t level,
	const struct v92_pcm_frame *frame, const int16_t *samples)
{
	uint64_t error = 0u;

	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int64_t residual = (int64_t)samples[symbol] - quantized_sample(
			mapping->law, scale_sample(mapping, level, frame->v[symbol]));

		error += (uint64_t)(residual * residual);
	}
	return error;
}

static bool same_level_waveform(
	const struct v92_pcm_mapping *mapping, const struct v92_pcm_frame *frame,
	int16_t first, int16_t second)
{
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		if (quantized_sample(mapping->law, scale_sample(mapping, first, frame->v[symbol])) !=
		    quantized_sample(mapping->law, scale_sample(mapping, second, frame->v[symbol])))
			return false;
	}
	return true;
}

enum v92_pcm_frame_result v92_pcm_frame_rx_refine_level(
	struct v92_pcm_frame_rx *receiver, const struct v92_pcm_frame *frame,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS],
	int16_t minimum_level, int16_t maximum_level)
{
	if (receiver == NULL || frame == NULL || samples == NULL ||
	    memory_regions_overlap(receiver, sizeof(*receiver), frame, sizeof(*frame)) ||
	    memory_regions_overlap(receiver, sizeof(*receiver), samples,
				   V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*samples)))
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_rx_is_valid(receiver) ||
	    receiver->observation != V92_PCM_FRAME_FILTER_OUTPUT || minimum_level <= 0 ||
	    minimum_level > receiver->upstream_level || maximum_level < receiver->upstream_level)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	int16_t best = receiver->upstream_level;
	uint64_t best_error = level_error(&receiver->mapping, best, frame, samples);
	int distance = 1;

	/* Quantization makes the objective piecewise constant. Visit closest
	 * integer levels first rather than moving arbitrarily across a plateau. */
	while (best_error != 0u &&
	       (receiver->upstream_level - distance >= minimum_level ||
		receiver->upstream_level + distance <= maximum_level)) {
		for (int direction = -1; direction <= 1; direction += 2) {
			int candidate = receiver->upstream_level + direction * distance;

			if (candidate < minimum_level || candidate > maximum_level)
				continue;
			uint64_t error = level_error(&receiver->mapping,
				(int16_t)candidate, frame, samples);

			if (error < best_error) {
				best_error = error;
				best = (int16_t)candidate;
			}
		}
		distance++;
	}
	int16_t low = best;
	int16_t high = best;

	while (low > minimum_level &&
	       same_level_waveform(&receiver->mapping, frame, best, (int16_t)(low - 1)))
		low--;
	while (high < maximum_level &&
	       same_level_waveform(&receiver->mapping, frame, best, (int16_t)(high + 1)))
		high++;
	receiver->upstream_level = best;
	receiver->minimum_level = low;
	receiver->maximum_level = high;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_encode(
	struct v92_pcm_frame_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame *frame)
{
	struct v92_pcm_frame_tx_state next;
	struct v92_pcm_frame encoded = { 0 };
	uint8_t scrambled[V92_PCM_MAPPING_MAX_BITS];

	if (transmitter == NULL || frame == NULL || bits == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_tx_is_valid(transmitter))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (bit_count != transmitter->mapping.data_bits_per_frame ||
	    !bits_are_valid(bits, bit_count))
		return V92_PCM_FRAME_OUT_OF_RANGE;
	next = transmitter->state;
	for (size_t bit = 0u; bit < bit_count; ++bit) {
		if (v34_scramble_group(
			    &next.scrambler, bits[bit], 1u,
			    &scrambled[bit]) != V34_FRAMING_OK)
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	if (v92_pcm_mapping_encode(
		    &transmitter->mapping, scrambled, bit_count,
		    &next.differential, encoded.digit,
		    &encoded.frame_sign) != V92_PCM_MAPPING_OK)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		uint8_t y0;

		if (v34_convolutional_encoder_output(
			    &next.trellis, &y0) != V34_TRELLIS_OK ||
		    !select_point(
			    &transmitter->mapping, &next.filter, symbol,
			    encoded.digit[symbol], encoded.signed_index, y0,
			    &encoded.signed_index[symbol], &encoded.u[symbol]))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
		encoded.y0[symbol] = y0;
		filter_step(
			&transmitter->mapping, &next.filter,
			encoded.u[symbol], &encoded.x[symbol],
			&encoded.v[symbol]);
		if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u &&
		    !advance_trellis(
			    &next.trellis, encoded.signed_index, symbol))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	transmitter->state = next;
	*frame = encoded;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_scale(
	const struct v92_pcm_mapping *mapping, int16_t upstream_level,
	int32_t prefilter_output, int16_t *sample)
{
	if (mapping == NULL || sample == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (upstream_level <= 0)
		return V92_PCM_FRAME_OUT_OF_RANGE;
	*sample = scale_sample(mapping, upstream_level, prefilter_output);
	return V92_PCM_FRAME_OK;
}

static bool noise_is_valid(const struct v92_pcm_frame_noise *noise)
{
	if (noise->frames > V92_B1U_FRAMES ||
	    !isfinite(noise->relative_variance) || noise->relative_variance < 0.0 ||
	    noise->relative_variance > 65535.0 ||
	    (noise->frames < V92_B1U_FRAMES && noise->relative_variance != 0.0))
		return false;
	unsigned symbols = noise->frames > V92_B1U_FRAMES / 2u && noise->frames < V92_B1U_FRAMES ?
		(noise->frames - V92_B1U_FRAMES / 2u) * V92_PCM_MAPPING_FRAME_SYMBOLS : 0u;
	const double moments[] = { noise->signal_energy, noise->signal_fourth,
		noise->error_energy, noise->error_signal_energy };
	const double bounds[] = { 32768.0 * 32768.0, 32768.0 * 32768.0 * 32768.0 * 32768.0,
		65535.0 * 65535.0, 65535.0 * 65535.0 * 32768.0 * 32768.0 };

	for (size_t i = 0; i < sizeof(moments) / sizeof(moments[0]); ++i)
		if (!isfinite(moments[i]) || moments[i] < 0.0 || moments[i] > symbols * bounds[i])
			return false;
	return true;
}

bool v92_pcm_frame_rx_is_valid(const struct v92_pcm_frame_rx *receiver)
{
	enum v34_trellis_selection selected;
	bool normalized = false;

	if (receiver == NULL || !v92_pcm_mapping_is_valid(&receiver->mapping) ||
	    receiver->path_count == 0u ||
	    receiver->path_count > V92_PCM_RX_PATHS ||
	    receiver->pending_frames > V92_PCM_RX_DELAY_FRAMES ||
	    !trellis_selection(receiver->mapping.cpd.trellis, &selected))
		return false;
	switch (receiver->observation) {
	case V92_PCM_FRAME_FILTER_OUTPUT:
		if (!memory_region_is_zero(&receiver->noise, sizeof(receiver->noise)) ||
		    receiver->minimum_level <= 0 ||
		    receiver->minimum_level > receiver->upstream_level ||
		    receiver->maximum_level < receiver->upstream_level)
			return false;
		break;
	case V92_PCM_FRAME_CONSTELLATION:
		if (!noise_is_valid(&receiver->noise) ||
		    receiver->upstream_level != 0 || receiver->minimum_level != 0 ||
		    receiver->maximum_level != 0)
			return false;
		break;
	default:
		return false;
	}
	for (size_t index = 0u; index < receiver->path_count; ++index) {
		const struct v92_pcm_frame_rx_path *path = &receiver->paths[index];

		normalized |= path->metric == 0u;
		if (path->differential > 1u ||
		    !v34_scrambler_is_valid(&path->descrambler) ||
		    path->descrambler.polynomial != V34_POLYNOMIAL_GPA ||
		    !v34_convolutional_encoder_is_valid(&path->trellis) ||
		    path->trellis.selection != selected)
			return false;
		if (path->ambiguous_frames >> receiver->pending_frames)
			return false;
		for (size_t frame = 0u; frame < V92_PCM_RX_DELAY_FRAMES; ++frame) {
			size_t used = frame < receiver->pending_frames ?
				receiver->mapping.data_bits_per_frame : 0u;

			if (!bits_are_valid(path->pending[frame], used) ||
			    !memory_region_is_zero(path->pending[frame] + used,
				V92_PCM_MAPPING_MAX_BITS - used))
				return false;
		}
	}
	return normalized;
}

static enum v92_pcm_frame_result initialize_receiver(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level,
	enum v92_pcm_frame_observation observation)
{
	struct v92_pcm_frame_rx initialized = { 0 };
	enum v34_trellis_selection selected;

	if (receiver == NULL || mapping == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_mapping_is_valid(mapping) ||
	    (observation == V92_PCM_FRAME_FILTER_OUTPUT && upstream_level <= 0) ||
	    !trellis_selection(mapping->cpd.trellis, &selected))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	initialized.mapping = *mapping;
	initialized.observation = observation;
	initialized.upstream_level = upstream_level;
	initialized.minimum_level = upstream_level;
	initialized.maximum_level = upstream_level;
	initialized.path_count = 1u;
	if (v34_scrambler_init(&initialized.paths[0].descrambler,
			       V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK ||
	    v34_convolutional_encoder_init(&initialized.paths[0].trellis, selected) !=
		    V34_TRELLIS_OK)
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	*receiver = initialized;
	return V92_PCM_FRAME_OK;
}

enum v92_pcm_frame_result v92_pcm_frame_rx_init(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level)
{
	return initialize_receiver(receiver, mapping, upstream_level, V92_PCM_FRAME_FILTER_OUTPUT);
}

enum v92_pcm_frame_result v92_pcm_frame_rx_init_constellation(
	struct v92_pcm_frame_rx *receiver, const struct v92_pcm_mapping *mapping)
{
	return initialize_receiver(receiver, mapping, 0, V92_PCM_FRAME_CONSTELLATION);
}

enum v92_pcm_frame_result v92_pcm_frame_rx_reset_control(
	struct v92_pcm_frame_rx *receiver)
{
	if (receiver == NULL)
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_rx_is_valid(receiver))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	for (size_t index = 0u; index < receiver->path_count; ++index) {
		receiver->paths[index].descrambler.history = 0u;
		receiver->paths[index].differential = 0u;
	}
	return V92_PCM_FRAME_OK;
}

struct frame_path {
	struct v92_pcm_frame_rx_path state;
	struct v92_pcm_frame frame;
	uint64_t frame_metric;
	uint8_t output_bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t known_digits[V92_PCM_MAPPING_FRAME_SYMBOLS];
	bool output_ambiguous;
};

struct frame_candidate {
	uint64_t metric;
	uint64_t frame_metric;
	int16_t eta;
	uint8_t path;
	uint8_t digit;
};

/* Max-heap of the best bounded candidates. Equal metrics do not replace
 * retained candidates; the path/point enumeration makes ties deterministic. */
static void candidate_push(struct frame_candidate *candidates, size_t count)
{
	size_t child = count - 1u;

	while (child != 0u) {
		size_t parent = (child - 1u) / 2u;
		struct frame_candidate swap;

		if (candidates[parent].metric >= candidates[child].metric)
			break;
		swap = candidates[parent];
		candidates[parent] = candidates[child];
		candidates[child] = swap;
		child = parent;
	}
}

static void candidate_replace(
	struct frame_candidate *candidates, size_t count,
	const struct frame_candidate *replacement)
{
	size_t parent = 0u;

	candidates[0] = *replacement;
	for (;;) {
		size_t left = 2u * parent + 1u;
		size_t right = left + 1u;
		size_t larger;
		struct frame_candidate swap;

		if (left >= count)
			break;
		larger = right < count &&
			candidates[right].metric > candidates[left].metric ?
				right : left;
		if (candidates[parent].metric >= candidates[larger].metric)
			break;
		swap = candidates[parent];
		candidates[parent] = candidates[larger];
		candidates[larger] = swap;
		parent = larger;
	}
}

static int16_t signed_point(
	const struct v92_cpd_constellation *constellation, int eta)
{
	return eta >= 0 ? constellation->positive_linear[eta] :
		(int16_t)-constellation->positive_linear[-eta - 1];
}

static int16_t level_sample(const struct v92_pcm_frame_rx *receiver, int16_t level, int32_t v)
{
	return quantized_sample(receiver->mapping.law, scale_sample(&receiver->mapping, level, v));
}

static int16_t predicted_sample(
	const struct v92_pcm_frame_rx *receiver, int32_t v, int16_t observed, bool b1u)
{
	if (b1u || receiver->minimum_level == receiver->maximum_level)
		return level_sample(receiver, receiver->upstream_level, v);
	int low = receiver->minimum_level;
	int high = receiver->maximum_level;
	int16_t low_sample = level_sample(receiver, (int16_t)low, v);
	int16_t high_sample = level_sample(receiver, (int16_t)high, v);

	if (low_sample == high_sample || (v >= 0 ? observed <= low_sample : observed >= low_sample))
		return low_sample;
	if (v >= 0 ? observed >= high_sample : observed <= high_sample)
		return high_sample;
	/* Quantized predictions are monotone in LU. Find the two reachable
	 * codewords surrounding the observation, not an interpolated level
	 * which the transmitter cannot produce. */
	while (high - low > 1) {
		int middle = low + (high - low) / 2;
		int16_t sample = level_sample(receiver, (int16_t)middle, v);

		if (v >= 0 ? sample < observed : sample > observed) {
			low = middle;
			low_sample = sample;
		} else {
			high = middle;
			high_sample = sample;
		}
	}
	int low_error = observed - low_sample;
	int high_error = observed - high_sample;

	return (int64_t)low_error * low_error <= (int64_t)high_error * high_error ?
		low_sample : high_sample;
}

static int16_t predicted_observation(
	const struct v92_pcm_frame_rx *receiver, int16_t u, int32_t v,
	int16_t observed, bool b1u)
{
	return receiver->observation == V92_PCM_FRAME_CONSTELLATION ? u :
		predicted_sample(receiver, v, observed, b1u);
}

static bool b1u_digits(
	const struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_frame_rx_path *path,
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	struct v34_scrambler scrambler = path->descrambler;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS];
	uint8_t differential = path->differential;
	uint8_t sign;

	for (size_t bit = 0u; bit < receiver->mapping.data_bits_per_frame; ++bit) {
		if (v34_scramble_group(&scrambler, 1u, 1u, &bits[bit]) !=
		    V34_FRAMING_OK)
			return false;
	}
	return v92_pcm_mapping_encode(
		&receiver->mapping, bits, receiver->mapping.data_bits_per_frame,
		&differential, digits, &sign) == V92_PCM_MAPPING_OK;
}

static void train_noise(struct v92_pcm_frame_noise *noise,
	const int16_t *samples, const int16_t *expected)
{
	if (noise->frames == V92_B1U_FRAMES)
		return;
	/* Allow initial B1u gain acquisition to settle. Use only the remaining
	 * training interval; the caller qualifies its decoded bits. Payload and
	 * subsequent control exchanges must not update this estimate. */
	if (noise->frames >= V92_B1U_ACQUISITION_FRAMES) {
		for (size_t i = 0; i < V92_PCM_MAPPING_FRAME_SYMBOLS; ++i) {
			double signal = expected[i], error = samples[i] - signal;
			double square = signal * signal, residual = error * error;

			noise->signal_energy += square;
			noise->signal_fourth += square * square;
			noise->error_energy += residual;
			noise->error_signal_energy += residual * square;
		}
	}
	if (++noise->frames != V92_B1U_FRAMES)
		return;
	double count = V92_B1U_SYMBOLS / 2u;
	double determinant = count * noise->signal_fourth - noise->signal_energy * noise->signal_energy;
	double slope = determinant > 0.0 ?
		(count * noise->error_signal_energy - noise->error_energy * noise->signal_energy) / determinant : 0.0;
	double average_error = noise->error_energy / count;
	/* Do not extrapolate near-zero variance at low amplitudes from this
	 * short fit: retain the average B1u residual as a conservative noise
	 * floor. Constant-amplitude or amplitude-independent noise keeps equal
	 * weights. One sample and Q16 bound numerical precision, not quality. */
	double ratio = slope > 0.0 ? slope / (average_error > 1.0 ? average_error : 1.0) : 0.0;

	if (ratio > 65535.0)
		ratio = 65535.0;

	*noise = (struct v92_pcm_frame_noise){
		.frames = V92_B1U_FRAMES, .relative_variance = ratio,
	};
}

enum v92_pcm_frame_result v92_pcm_frame_decode(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], bool b1u,
	uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame_decision *decision)
{
	struct frame_path banks[2][V92_PCM_RX_PATHS];
	struct frame_path *paths = banks[0];
	struct frame_path *next = banks[1];
	uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
	size_t path_count;
	size_t best = V92_PCM_RX_PATHS;
	const struct memory_region regions[] = {
		{ receiver, sizeof(*receiver) },
		{ samples, V92_PCM_MAPPING_FRAME_SYMBOLS * sizeof(*samples) },
		{ bits, bit_count },
		{ decision, sizeof(*decision) },
	};

	if (receiver == NULL || samples == NULL || bits == NULL ||
	    decision == NULL || memory_region_set_overlaps(regions, 4u))
		return V92_PCM_FRAME_INVALID_ARGUMENT;
	if (!v92_pcm_frame_rx_is_valid(receiver))
		return V92_PCM_FRAME_INVALID_PARAMETERS;
	if (bit_count != receiver->mapping.data_bits_per_frame)
		return V92_PCM_FRAME_OUT_OF_RANGE;
	path_count = receiver->path_count;
	/* Acquire with known bits, then test the settled raw-coordinate path
	 * using ordinary decisions. Constraining all 48 frames to ones would
	 * conceal a channel that cannot recover data at the selected rate. */
	bool known = b1u && (receiver->observation != V92_PCM_FRAME_CONSTELLATION ||
		receiver->noise.frames < V92_B1U_ACQUISITION_FRAMES);

	for (size_t index = 0u; index < path_count; ++index) {
		paths[index] = (struct frame_path){ .state = receiver->paths[index] };
		if (known && !b1u_digits(receiver, &paths[index].state,
				      paths[index].known_digits))
			return V92_PCM_FRAME_INVALID_PARAMETERS;
	}
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		uint8_t set = receiver->mapping.cpd.frame_constellation[
			symbol % V92_PCM_MAPPING_PHASES];
		const struct v92_cpd_constellation *constellation =
			&receiver->mapping.cpd.constellation[set];
		unsigned modulus = receiver->mapping.cpd.modulus[symbol];
		unsigned stride = symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u ? 2u : 1u;
		struct frame_candidate candidates[V92_PCM_RX_PATHS];
		size_t count = 0u;
		uint64_t weight = 1u;

		if (receiver->observation == V92_PCM_FRAME_CONSTELLATION) {
			/* Companding noise grows with amplitude. Estimate reliability
			 * from the observation, independently of each candidate point.
			 * Compute once per symbol, outside the bounded path search. */
			double variance = 1.0 + receiver->noise.relative_variance *
				(double)samples[symbol] * samples[symbol];

			weight = (uint64_t)(65536.0 / variance);
			if (weight == 0u)
				weight = 1u;
		}

		for (size_t path = 0u; path < path_count; ++path) {
			uint8_t y0;
			int64_t precoder = 0, prefilter = 0;

			if (receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT) {
				precoder = precoder_history(&receiver->mapping, &paths[path].state.filter);
				prefilter = prefilter_history(&receiver->mapping, &paths[path].state.filter);
			}

			if (v34_convolutional_encoder_output(
					&paths[path].state.trellis, &y0) !=
			    V34_TRELLIS_OK)
				return V92_PCM_FRAME_INVALID_PARAMETERS;
			int offset = equivalence_offset(symbol, paths[path].frame.signed_index, y0);
			unsigned step = known ? stride * modulus : stride;
			int residue = offset + (known ? (int)(stride * paths[path].known_digits[symbol]) : 0);
			int first = -(int)constellation->count;

			first += positive_modulo(residue - first, step);
			unsigned next_digit = (unsigned)positive_modulo((first - offset) / (int)stride, modulus);

			/* Keep the original ascending eta order, including metric ties.
			 * Known B1u visits one residue class; data cycles all digits. */
			for (int eta = first; eta < (int)constellation->count; eta += (int)step) {
				uint8_t digit = (uint8_t)next_digit;
				int64_t error;
				struct frame_candidate candidate;

				next_digit += !known;
				if (next_digit == modulus)
					next_digit = 0u;
				int16_t u = signed_point(constellation, eta);
				int32_t v = 0;

				if (receiver->observation == V92_PCM_FRAME_FILTER_OUTPUT) {
					int32_t x = precoder_output(precoder, u);

					v = prefilter_output(&receiver->mapping, prefilter, x);
				}

				error = (int64_t)samples[symbol] -
					predicted_observation(receiver, u, v, samples[symbol], b1u);
				candidate = (struct frame_candidate){
					.metric = add_u64_saturating(paths[path].state.metric,
						(uint64_t)(error * error) * weight),
					.frame_metric = paths[path].frame_metric +
						(uint64_t)(error * error),
					.eta = (int16_t)eta, .path = (uint8_t)path,
					.digit = digit,
				};
				if (count < V92_PCM_RX_PATHS) {
					candidates[count++] = candidate;
					candidate_push(candidates, count);
				} else if (candidate.metric < candidates[0].metric) {
					candidate_replace(candidates, count, &candidate);
				}
			}
		}
		if (count == 0u)
			return V92_PCM_FRAME_NO_PATH;
		for (size_t index = 0u; index < count; ++index) {
			const struct frame_candidate *candidate = &candidates[index];
			struct v92_pcm_frame *frame = &next[index].frame;

			next[index] = paths[candidate->path];
			next[index].frame_metric = candidate->frame_metric;
			next[index].state.metric = candidate->metric;
			frame->signed_index[symbol] = candidate->eta;
			frame->digit[symbol] = candidate->digit;
			frame->u[symbol] = signed_point(constellation, candidate->eta);
			if (v34_convolutional_encoder_output(&next[index].state.trellis,
							    &frame->y0[symbol]) !=
			    V34_TRELLIS_OK)
				return V92_PCM_FRAME_INVALID_PARAMETERS;
			filter_step(&receiver->mapping, &next[index].state.filter,
				    frame->u[symbol], &frame->x[symbol], &frame->v[symbol]);
			if (symbol % V92_PCM_TRELLIS_INTERVAL_SYMBOLS == 3u &&
			    !advance_trellis(&next[index].state.trellis,
					     frame->signed_index, symbol))
				return V92_PCM_FRAME_INVALID_PARAMETERS;
		}
		struct frame_path *swap = paths;

		paths = next;
		next = swap;
		path_count = count;
	}
	/* A mixed-radix constellation may have spare words. Reject those paths
	 * before selecting a survivor, rather than returning an invalid frame. */
	size_t valid = 0u;

	for (size_t index = 0u; index < path_count; ++index) {
		uint8_t candidate_bits[V92_PCM_MAPPING_MAX_BITS];
		struct v92_pcm_frame_rx_path *state = &paths[index].state;

		if (v92_pcm_mapping_decode(&receiver->mapping, paths[index].frame.digit,
					   &state->differential, candidate_bits,
					   bit_count) != V92_PCM_MAPPING_OK)
			continue;
		for (size_t bit = 0u; bit < bit_count; ++bit) {
			if (v34_descramble_group(&state->descrambler, candidate_bits[bit],
						1u, &candidate_bits[bit]) != V34_FRAMING_OK)
				return V92_PCM_FRAME_INVALID_PARAMETERS;
		}
		paths[index].frame.frame_sign = state->differential;
		memcpy(paths[index].output_bits, state->pending[0], bit_count);
		paths[index].output_ambiguous = (state->ambiguous_frames & 1u) != 0u;
		if (receiver->pending_frames == V92_PCM_RX_DELAY_FRAMES) {
			state->ambiguous_frames >>= 1u;
			memmove(state->pending, state->pending + 1,
				(V92_PCM_RX_DELAY_FRAMES - 1u) * sizeof(state->pending[0]));
			memcpy(state->pending[V92_PCM_RX_DELAY_FRAMES - 1u],
				candidate_bits, bit_count);
		} else {
			memcpy(state->pending[receiver->pending_frames],
				candidate_bits, bit_count);
		}
		/* Merge histories with identical future behavior. Different old
		 * payload decisions are resolved by their accumulated metric. */
		size_t survivor;

		for (survivor = 0u; survivor < valid; ++survivor) {
			const struct v92_pcm_frame_rx_path *prior = &paths[survivor].state;

			if (state->differential == prior->differential &&
			    state->descrambler.history == prior->descrambler.history &&
			    state->trellis.state == prior->trellis.state &&
			    memcmp(&state->filter, &prior->filter, sizeof(state->filter)) == 0)
				break;
		}
		if (survivor == valid)
			paths[valid++] = paths[index];
		else if (state->metric < paths[survivor].state.metric)
			paths[survivor] = paths[index];
		else if (state->metric == paths[survivor].state.metric) {
			struct frame_path *prior = &paths[survivor];

			prior->output_ambiguous |= paths[index].output_ambiguous ||
				memcmp(prior->output_bits, paths[index].output_bits, bit_count) != 0;
			prior->state.ambiguous_frames |= state->ambiguous_frames;
			for (size_t frame = 0u; frame < V92_PCM_RX_DELAY_FRAMES; ++frame) {
				if (memcmp(prior->state.pending[frame], state->pending[frame],
					   bit_count) != 0)
					prior->state.ambiguous_frames |= (uint16_t)(1u << frame);
			}
		}
	}
	for (size_t index = 0u; index < valid; ++index) {
		if (best == V92_PCM_RX_PATHS ||
		    paths[index].state.metric < paths[best].state.metric)
			best = index;
	}
	if (best == V92_PCM_RX_PATHS)
		return V92_PCM_FRAME_NO_PATH;
	memcpy(decoded, paths[best].output_bits, bit_count);
	bool ambiguous = paths[best].output_ambiguous;

	for (size_t index = 0u; index < valid; ++index) {
		if (paths[index].state.metric == paths[best].state.metric)
			ambiguous |= paths[index].output_ambiguous ||
				memcmp(decoded, paths[index].output_bits, bit_count) != 0;
	}
	*decision = (struct v92_pcm_frame_decision){
		.frame = paths[best].frame,
		.squared_error = paths[best].frame_metric,
		.bits_ready = receiver->pending_frames == V92_PCM_RX_DELAY_FRAMES,
		.ambiguous = receiver->pending_frames == V92_PCM_RX_DELAY_FRAMES && ambiguous,
	};
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
		decision->predicted_samples[symbol] = predicted_observation(receiver,
			decision->frame.u[symbol], decision->frame.v[symbol], samples[symbol], b1u);
	if (decision->bits_ready)
		memcpy(bits, decoded, bit_count);
	else
		receiver->pending_frames++;
	uint64_t minimum_metric = paths[best].state.metric;

	for (size_t index = 0u; index < valid; ++index) {
		receiver->paths[index] = paths[index].state;
		receiver->paths[index].metric -= minimum_metric;
	}
	memset(receiver->paths + valid, 0,
		(V92_PCM_RX_PATHS - valid) * sizeof(receiver->paths[0]));
	receiver->path_count = (uint8_t)valid;
	if (b1u && receiver->observation == V92_PCM_FRAME_CONSTELLATION)
		train_noise(&receiver->noise, samples, decision->predicted_samples);
	return V92_PCM_FRAME_OK;
}

const char *v92_pcm_frame_result_name(
	enum v92_pcm_frame_result result)
{
	switch (result) {
	case V92_PCM_FRAME_OK:
		return "ok";
	case V92_PCM_FRAME_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_FRAME_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PCM_FRAME_OUT_OF_RANGE:
		return "out of range";
	case V92_PCM_FRAME_NO_PATH:
		return "no path";
	}
	return "unknown";
}
