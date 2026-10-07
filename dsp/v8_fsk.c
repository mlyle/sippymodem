// SPDX-License-Identifier: BSD-3-Clause

#include "v8_fsk.h"

#include "fixed_phase.h"
#include "fir.h"
#include "integer_math.h"
#include "memory_region.h"
#include "v21.h"

enum {
	V8_FSK_SAMPLE_RATE = 8000,
	V8_FSK_BIT_RATE = 300,
	V8_FSK_PHASE_UNITS = 8192,
	V8_FSK_PHASE_MASK = V8_FSK_PHASE_UNITS - 1,
	V8_FSK_PHASE_SHIFT = 5,
	V8_FSK_TRANSMIT_GAIN = 0x3ead,
	V8_FSK_HISTORY_SEED_INDEX = 33,
};

/* V.21 transmit pulse shapers, ordered newest sample first. */
static const int16_t V8_FSK_LOW_FILTER[V8_FSK_TRANSMIT_FILTER_TAPS] = {
	-2, -11, 19, 31, 19, 0, 12, 58, 66, -38, -211, -279, -120,
	138, 224, 70, -19, 294, 825, 836, -200, -1773, -2491, -1339,
	1134, 3062, 2863, 702, -1660, -2489, -1529, 39, 903, 749,
	213, -27, 102, 229, 100, -160, -283, -184, -12, 71, 50, 7,
	2, 22, 31, 13, -12,
};

static const int16_t V8_FSK_HIGH_FILTER[V8_FSK_TRANSMIT_FILTER_TAPS] = {
	-2, -14, 25, 37, -22, -58, 0, 27, -7, 77, 126, -159, -337,
	91, 421, 59, -135, 86, -404, -875, 643, 2141, -44, -3127,
	-1288, 3081, 2539, -1969, -2854, 558, 2096, 271, -907, -264,
	71, -151, 140, 417, -5, -348, -96, 138, 54, -5, 30, -12,
	-59, -10, 41, 17, -15,
};

_Static_assert(
	(unsigned int)V8_FSK_HISTORY_SEED_INDEX <
		(unsigned int)V8_FSK_TRANSMIT_FILTER_TAPS,
	"V.8 transmit history seed left the filter");

static uint16_t phase_step(unsigned int frequency_hz)
{
	uint32_t scaled = frequency_hz * V8_FSK_PHASE_UNITS;

	return (uint16_t)((scaled + V8_FSK_SAMPLE_RATE / 2u) /
			  V8_FSK_SAMPLE_RATE);
}

static bool role_channels(
	enum v8_station_role role,
	const struct fsk_channel **transmit,
	const struct fsk_channel **receive)
{
	if (role == V8_CALLING_STATION) {
		*transmit = &v21_channel_1;
		*receive = &v21_channel_2;
		return true;
	}
	if (role == V8_ANSWERING_STATION) {
		*transmit = &v21_channel_2;
		*receive = &v21_channel_1;
		return true;
	}
	return false;
}

static bool channels_equal(
	const struct fsk_channel *left,
	const struct fsk_channel *right)
{
	return left->sample_rate == right->sample_rate &&
	       left->symbol_rate_numerator == right->symbol_rate_numerator &&
	       left->symbol_rate_denominator == right->symbol_rate_denominator &&
	       left->mark_frequency_hz == right->mark_frequency_hz &&
	       left->space_frequency_hz == right->space_frequency_hz &&
	       left->detector_window == right->detector_window &&
	       left->metric_smoothing_shift == right->metric_smoothing_shift;
}

static const int16_t *transmit_filter(enum v8_station_role role)
{
	if (role == V8_CALLING_STATION)
		return V8_FSK_LOW_FILTER;
	if (role == V8_ANSWERING_STATION)
		return V8_FSK_HIGH_FILTER;
	return NULL;
}

static bool transmitter_is_valid(
	const struct v8_fsk_transmitter *transmitter,
	const struct fsk_channel *channel)
{
	return transmitter != NULL && channel != NULL &&
	       transmitter->carrier_phase < V8_FSK_PHASE_UNITS &&
	       transmitter->bit_progress < V8_FSK_SAMPLE_RATE &&
	       transmitter->mark_phase_step ==
		       phase_step(channel->mark_frequency_hz) &&
	       transmitter->space_phase_step ==
		       phase_step(channel->space_frequency_hz) &&
	       transmitter->gain == V8_FSK_TRANSMIT_GAIN &&
	       transmitter->held_bit <= 1;
}

static bool fsk_is_configured(const struct v8_fsk *fsk)
{
	const struct fsk_channel *transmit;
	const struct fsk_channel *receive;

	return fsk != NULL && role_channels(fsk->role, &transmit, &receive) &&
	       transmitter_is_valid(&fsk->transmitter, transmit) &&
	       v8_line_conditioner_is_valid(&fsk->line) &&
	       fsk->line.role == fsk->role &&
	       channels_equal(&fsk->receiver.channel, receive);
}

enum v8_fsk_result v8_fsk_init(
	struct v8_fsk *fsk,
	enum v8_station_role role)
{
	const struct fsk_channel *transmit;
	const struct fsk_channel *receive;
	struct v8_fsk initialized = { 0 };

	if (fsk == NULL || !role_channels(role, &transmit, &receive))
		return V8_FSK_INVALID_ARGUMENT;
	initialized.transmitter = (struct v8_fsk_transmitter){
		.mark_phase_step = phase_step(transmit->mark_frequency_hz),
		.space_phase_step = phase_step(transmit->space_frequency_hz),
		.gain = V8_FSK_TRANSMIT_GAIN,
	};
	initialized.transmitter
		.filter_history[V8_FSK_HISTORY_SEED_INDEX] = 0x1000;
	if (v8_line_conditioner_init(&initialized.line, role) !=
		    V8_LINE_CONDITIONER_OK ||
	    fsk_receiver_init(&initialized.receiver, receive) !=
		    FSK_OK)
		return V8_FSK_INVALID_ARGUMENT;
	initialized.role = role;
	*fsk = initialized;
	return V8_FSK_OK;
}

enum v8_fsk_result v8_fsk_reset(
	struct v8_fsk *fsk)
{
	if (fsk == NULL ||
	    (fsk->role != V8_CALLING_STATION &&
	     fsk->role != V8_ANSWERING_STATION))
		return V8_FSK_INVALID_ARGUMENT;
	return v8_fsk_init(fsk, fsk->role);
}

bool v8_fsk_can_accept_bit(
	const struct v8_fsk *fsk)
{
	return fsk_is_configured(fsk) &&
	       !fsk->transmitter.holding_bit;
}

static int16_t filter_sample(
	struct v8_fsk_transmitter *transmitter,
	const int16_t coefficients[V8_FSK_TRANSMIT_FILTER_TAPS],
	int16_t sample)
{
	return round_shift_s32_word_to_s16(fir_shift_s16(
		transmitter->filter_history, coefficients,
		V8_FSK_TRANSMIT_FILTER_TAPS, sample), 16);
}

enum v8_fsk_result v8_fsk_transmit_sample(
	struct v8_fsk *fsk, uint8_t bit, int16_t *sample,
	bool *bit_consumed)
{
	struct v8_fsk_transmitter *transmitter;
	uint16_t step;
	uint32_t bit_progress;
	int16_t generated;
	bool consumed = false;

	if (!fsk_is_configured(fsk) || bit > 1 || sample == NULL ||
	    bit_consumed == NULL ||
	    memory_regions_overlap(
		    fsk, sizeof(*fsk), sample, sizeof(*sample)) ||
	    memory_regions_overlap(
		    fsk, sizeof(*fsk), bit_consumed, sizeof(*bit_consumed)) ||
	    memory_regions_overlap(
		    sample, sizeof(*sample), bit_consumed,
		    sizeof(*bit_consumed)))
		return V8_FSK_INVALID_ARGUMENT;
	transmitter = &fsk->transmitter;
	if (!transmitter->holding_bit) {
		transmitter->held_bit = bit;
		transmitter->holding_bit = true;
	}
	step = transmitter->held_bit ? transmitter->mark_phase_step :
		transmitter->space_phase_step;
	transmitter->carrier_phase = (uint16_t)(
		(transmitter->carrier_phase + step) & V8_FSK_PHASE_MASK);
	generated = multiply_shift_s16(
		fixed_phase_cosine_u8_q14(
			(uint8_t)(transmitter->carrier_phase >> V8_FSK_PHASE_SHIFT)),
		transmitter->gain, 14);
	generated = filter_sample(
		transmitter, transmit_filter(fsk->role), generated);
	bit_progress = (uint32_t)transmitter->bit_progress + V8_FSK_BIT_RATE;
	if (bit_progress >= V8_FSK_SAMPLE_RATE) {
		bit_progress -= V8_FSK_SAMPLE_RATE;
		transmitter->holding_bit = false;
		consumed = true;
	}
	transmitter->bit_progress = (uint16_t)bit_progress;
	*sample = generated;
	*bit_consumed = consumed;
	return V8_FSK_OK;
}

enum v8_fsk_result v8_fsk_filter_transmit_sample(
	struct v8_fsk *fsk, int16_t unfiltered, int16_t *sample)
{
	if (!fsk_is_configured(fsk) || sample == NULL ||
	    memory_regions_overlap(
		    fsk, sizeof(*fsk), sample, sizeof(*sample)))
		return V8_FSK_INVALID_ARGUMENT;
	*sample = filter_sample(
		&fsk->transmitter, transmit_filter(fsk->role), unfiltered);
	return V8_FSK_OK;
}

static bool receive_block(struct fsk_receiver *receiver,
	const struct v8_line_conditioned_block *block,
	uint8_t *bit, bool *ready)
{
	*bit = 0;
	*ready = false;
	for (size_t index = 0; index < V8_LINE_BLOCK_SAMPLES; ++index) {
		uint8_t candidate;
		bool candidate_ready;

		if (fsk_receiver_sample(receiver, block->samples[index],
			&candidate, &candidate_ready) != FSK_OK ||
		    (*ready && candidate_ready))
			return false;
		if (candidate_ready) {
			*bit = candidate;
			*ready = true;
		}
	}
	return true;
}

enum v8_fsk_result v8_fsk_receive_block(
	struct v8_fsk *fsk, const struct v8_line_conditioned_block *block,
	uint8_t *bit, bool *bit_ready)
{
	struct fsk_receiver next;
	uint8_t received;
	bool ready;

	if (!fsk_is_configured(fsk) || block == NULL || bit == NULL ||
	    bit_ready == NULL ||
	    memory_regions_overlap(fsk, sizeof(*fsk), block, sizeof(*block)) ||
	    memory_regions_overlap(fsk, sizeof(*fsk), bit, sizeof(*bit)) ||
	    memory_regions_overlap(fsk, sizeof(*fsk), bit_ready, sizeof(*bit_ready)) ||
	    memory_regions_overlap(block, sizeof(*block), bit, sizeof(*bit)) ||
	    memory_regions_overlap(block, sizeof(*block), bit_ready, sizeof(*bit_ready)) ||
	    memory_regions_overlap(bit, sizeof(*bit), bit_ready, sizeof(*bit_ready)))
		return V8_FSK_INVALID_ARGUMENT;

	next = fsk->receiver;
	if (!receive_block(&next, block, &received, &ready))
		return V8_FSK_INVALID_ARGUMENT;
	fsk->receiver = next;
	*bit = received;
	*bit_ready = ready;
	return V8_FSK_OK;
}

enum v8_fsk_result v8_fsk_receive_sample(
	struct v8_fsk *fsk, int16_t sample, uint8_t *bit,
	bool *bit_ready)
{
	struct v8_line_conditioner next_line;
	struct v8_line_conditioned_block block;
	uint8_t received = 0;
	bool block_ready;
	bool ready = false;

	if (!fsk_is_configured(fsk) || bit == NULL || bit_ready == NULL ||
	    memory_regions_overlap(fsk, sizeof(*fsk), bit, sizeof(*bit)) ||
	    memory_regions_overlap(
		    fsk, sizeof(*fsk), bit_ready, sizeof(*bit_ready)) ||
	    memory_regions_overlap(
		    bit, sizeof(*bit), bit_ready, sizeof(*bit_ready)))
		return V8_FSK_INVALID_ARGUMENT;
	next_line = fsk->line;
	if (v8_line_conditioner_process_sample(
		    &next_line, sample, &block, &block_ready) !=
	    V8_LINE_CONDITIONER_OK)
		return V8_FSK_INVALID_ARGUMENT;
	if (block_ready && v8_fsk_receive_block(
		    fsk, &block, &received, &ready) != V8_FSK_OK)
		return V8_FSK_INVALID_ARGUMENT;
	fsk->line = next_line;
	*bit = received;
	*bit_ready = ready;
	return V8_FSK_OK;
}
