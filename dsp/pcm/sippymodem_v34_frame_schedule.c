// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_frame_schedule.h"

#include "sippymodem_v34_framing.h"

#include <stddef.h>

struct symbol_rate_geometry {
	uint8_t data_frames_per_superframe;
	uint8_t mapping_frames_per_data_frame;
	uint8_t minimum_primary_rate_2400_bps_units;
	uint8_t maximum_primary_rate_2400_bps_units;
};

static bool symbol_rate_geometry(
	enum sippymodem_v34_symbol_rate symbol_rate,
	struct symbol_rate_geometry *geometry)
{
	switch (symbol_rate) {
	case SIPPYMODEM_V34_SYMBOL_RATE_2400:
		*geometry = (struct symbol_rate_geometry){ 7, 12, 1, 9 };
		return true;
	case SIPPYMODEM_V34_SYMBOL_RATE_2743:
		*geometry = (struct symbol_rate_geometry){ 8, 12, 2, 11 };
		return true;
	case SIPPYMODEM_V34_SYMBOL_RATE_2800:
		*geometry = (struct symbol_rate_geometry){ 7, 14, 2, 11 };
		return true;
	case SIPPYMODEM_V34_SYMBOL_RATE_3000:
		*geometry = (struct symbol_rate_geometry){ 7, 15, 2, 12 };
		return true;
	case SIPPYMODEM_V34_SYMBOL_RATE_3200:
		*geometry = (struct symbol_rate_geometry){ 7, 16, 2, 13 };
		return true;
	case SIPPYMODEM_V34_SYMBOL_RATE_3429:
		*geometry = (struct symbol_rate_geometry){ 8, 15, 2, 14 };
		return true;
	case SIPPYMODEM_V34_SYMBOL_RATE_COUNT:
		break;
	}
	return false;
}

static enum sippymodem_v34_frame_schedule_result derive_geometry(
	enum sippymodem_v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units,
	bool auxiliary_channel_enabled,
	struct sippymodem_v34_frame_geometry *geometry)
{
	struct symbol_rate_geometry rate;
	uint16_t primary_bits_per_frame;
	uint16_t data_bits_per_frame;
	uint8_t auxiliary_bits_per_frame;
	uint8_t high_mapping_frame_bits;
	uint8_t high_mapping_frame_count;

	if (!symbol_rate_geometry(symbol_rate, &rate) ||
	    primary_data_rate_2400_bps_units == 0 ||
	    primary_data_rate_2400_bps_units >
		    SIPPYMODEM_V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	if (primary_data_rate_2400_bps_units <
		    rate.minimum_primary_rate_2400_bps_units ||
	    primary_data_rate_2400_bps_units >
		    rate.maximum_primary_rate_2400_bps_units)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION;

	/* R * 0.28 / J is 96 bits per 2400 bit/s when J=7, and 84
	 * bits when J=8. The optional 200 bit/s channel contributes 8 or 7. */
	primary_bits_per_frame =
		(uint16_t)primary_data_rate_2400_bps_units *
		(rate.data_frames_per_superframe == 7u ? 96u : 84u);
	auxiliary_bits_per_frame = auxiliary_channel_enabled ?
		(rate.data_frames_per_superframe == 7u ? 8u : 7u) :
		0u;
	data_bits_per_frame =
		primary_bits_per_frame + auxiliary_bits_per_frame;
	high_mapping_frame_bits = (uint8_t)(
		(data_bits_per_frame + rate.mapping_frames_per_data_frame - 1u) /
		rate.mapping_frames_per_data_frame);
	high_mapping_frame_count = (uint8_t)(
		data_bits_per_frame -
		(uint16_t)(high_mapping_frame_bits - 1u) *
			rate.mapping_frames_per_data_frame);
	*geometry = (struct sippymodem_v34_frame_geometry){
		.symbol_rate = symbol_rate,
		.data_bits_per_frame = data_bits_per_frame,
		.primary_data_rate_2400_bps_units =
			primary_data_rate_2400_bps_units,
		.data_frames_per_superframe =
			rate.data_frames_per_superframe,
		.mapping_frames_per_data_frame =
			rate.mapping_frames_per_data_frame,
		.high_mapping_frame_bits = high_mapping_frame_bits,
		.high_mapping_frame_count = high_mapping_frame_count,
		.auxiliary_bits_per_frame = auxiliary_bits_per_frame,
		.auxiliary_channel_enabled = auxiliary_channel_enabled,
	};
	return SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

static bool geometries_are_equal(
	const struct sippymodem_v34_frame_geometry *left,
	const struct sippymodem_v34_frame_geometry *right)
{
	return left->symbol_rate == right->symbol_rate &&
	       left->data_bits_per_frame == right->data_bits_per_frame &&
	       left->primary_data_rate_2400_bps_units ==
		       right->primary_data_rate_2400_bps_units &&
	       left->data_frames_per_superframe ==
		       right->data_frames_per_superframe &&
	       left->mapping_frames_per_data_frame ==
		       right->mapping_frames_per_data_frame &&
	       left->high_mapping_frame_bits ==
		       right->high_mapping_frame_bits &&
	       left->high_mapping_frame_count ==
		       right->high_mapping_frame_count &&
	       left->auxiliary_bits_per_frame ==
		       right->auxiliary_bits_per_frame &&
	       left->auxiliary_channel_enabled ==
		       right->auxiliary_channel_enabled;
}

enum sippymodem_v34_frame_schedule_result
sippymodem_v34_frame_geometry_init(
	struct sippymodem_v34_frame_geometry *geometry,
	enum sippymodem_v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units,
	bool auxiliary_channel_enabled)
{
	struct sippymodem_v34_frame_geometry derived;
	enum sippymodem_v34_frame_schedule_result result;

	if (geometry == NULL)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	result = derive_geometry(
		symbol_rate, primary_data_rate_2400_bps_units,
		auxiliary_channel_enabled, &derived);
	if (result != SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return result;
	*geometry = derived;
	return SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

bool sippymodem_v34_frame_geometry_is_valid(
	const struct sippymodem_v34_frame_geometry *geometry)
{
	struct sippymodem_v34_frame_geometry derived;

	return geometry != NULL &&
	       derive_geometry(
		       geometry->symbol_rate,
		       geometry->primary_data_rate_2400_bps_units,
		       geometry->auxiliary_channel_enabled, &derived) ==
		       SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	       geometries_are_equal(geometry, &derived);
}

enum sippymodem_v34_frame_schedule_result
sippymodem_v34_frame_schedule_init(
	struct sippymodem_v34_frame_schedule *schedule,
	const struct sippymodem_v34_frame_geometry *geometry,
	uint8_t data_frame_in_superframe)
{
	struct sippymodem_v34_frame_schedule initialized;

	if (schedule == NULL ||
	    !sippymodem_v34_frame_geometry_is_valid(geometry) ||
	    data_frame_in_superframe >= geometry->data_frames_per_superframe)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	initialized = (struct sippymodem_v34_frame_schedule){
		.geometry = *geometry,
		.data_frame_in_superframe = data_frame_in_superframe,
	};
	*schedule = initialized;
	return SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

enum sippymodem_v34_frame_schedule_result
sippymodem_v34_b1_frame_schedule_init(
	struct sippymodem_v34_frame_schedule *schedule,
	const struct sippymodem_v34_frame_geometry *geometry)
{
	if (!sippymodem_v34_frame_geometry_is_valid(geometry))
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	return sippymodem_v34_frame_schedule_init(
		schedule, geometry,
		(uint8_t)(geometry->data_frames_per_superframe - 1u));
}

bool sippymodem_v34_frame_schedule_is_valid(
	const struct sippymodem_v34_frame_schedule *schedule)
{
	return schedule != NULL &&
	       sippymodem_v34_frame_geometry_is_valid(&schedule->geometry) &&
	       schedule->data_frame_in_superframe <
		       schedule->geometry.data_frames_per_superframe &&
	       schedule->next_mapping_frame <=
		       schedule->geometry.mapping_frames_per_data_frame;
}

bool sippymodem_v34_frame_schedule_is_complete(
	const struct sippymodem_v34_frame_schedule *schedule)
{
	return sippymodem_v34_frame_schedule_is_valid(schedule) &&
	       schedule->next_mapping_frame ==
		       schedule->geometry.mapping_frames_per_data_frame;
}

static bool period_crosses(
	uint8_t index, uint8_t events, uint8_t period)
{
	return (uint16_t)(index + 1u) * events / period !=
	       (uint16_t)index * events / period;
}

static enum sippymodem_v34_frame_schedule_result inversion_mask(
	const struct sippymodem_v34_frame_schedule *schedule, uint8_t *mask)
{
	enum sippymodem_v34_inversion_pattern pattern =
		schedule->geometry.data_frames_per_superframe == 7u ?
			SIPPYMODEM_V34_INVERSION_J7 :
			SIPPYMODEM_V34_INVERSION_J8;
	uint8_t generated = 0;

	for (uint8_t interval = 0;
	     interval < SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME;
	     ++interval) {
		uint8_t half;
		uint8_t inversion;
		uint16_t m =
			(uint16_t)schedule->next_mapping_frame *
				SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME +
			interval;

		if (m == 0)
			half = 0;
		else if (m ==
			 (uint16_t)schedule->geometry
				 .mapping_frames_per_data_frame *
				 2u)
			half = 1;
		else
			continue;
		if (sippymodem_v34_superframe_inversion_bit(
			    pattern,
			    (unsigned int)schedule->data_frame_in_superframe * 2u +
				    half,
			    &inversion) != SIPPYMODEM_V34_FRAMING_OK)
			return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
		if (inversion != 0)
			generated |= (uint8_t)(1u << interval);
	}
	*mask = generated;
	return SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

enum sippymodem_v34_frame_schedule_result
sippymodem_v34_frame_schedule_next(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_mapping_frame *mapping_frame)
{
	struct sippymodem_v34_frame_schedule next;
	struct sippymodem_v34_mapping_frame generated;
	bool high_frame;
	bool auxiliary;

	if (!sippymodem_v34_frame_schedule_is_valid(schedule) ||
	    mapping_frame == NULL)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	if (schedule->next_mapping_frame ==
	    schedule->geometry.mapping_frames_per_data_frame)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_FRAME_COMPLETE;
	next = *schedule;
	high_frame = period_crosses(
		next.next_mapping_frame,
		next.geometry.high_mapping_frame_count,
		next.geometry.mapping_frames_per_data_frame);
	auxiliary = next.geometry.auxiliary_channel_enabled &&
		period_crosses(
			next.next_mapping_frame,
			next.geometry.auxiliary_bits_per_frame,
			next.geometry.mapping_frames_per_data_frame);
	generated = (struct sippymodem_v34_mapping_frame){
		.index = next.next_mapping_frame,
		.data_bits = (uint8_t)(
			next.geometry.high_mapping_frame_bits - !high_frame),
		.high_frame = high_frame,
		.carries_auxiliary_bit = auxiliary,
	};
	generated.primary_bits =
		(uint8_t)(generated.data_bits - auxiliary);
	if (inversion_mask(&next, &generated.inversion_4d_mask) !=
	    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	next.next_mapping_frame++;
	generated.completes_data_frame =
		next.next_mapping_frame ==
		next.geometry.mapping_frames_per_data_frame;
	if (!sippymodem_v34_frame_schedule_is_valid(&next))
		return SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT;
	*schedule = next;
	*mapping_frame = generated;
	return SIPPYMODEM_V34_FRAME_SCHEDULE_OK;
}

const char *sippymodem_v34_frame_schedule_result_name(
	enum sippymodem_v34_frame_schedule_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_FRAME_SCHEDULE_OK:
		return "ok";
	case SIPPYMODEM_V34_FRAME_SCHEDULE_FRAME_COMPLETE:
		return "frame complete";
	case SIPPYMODEM_V34_FRAME_SCHEDULE_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION:
		return "unsupported selection";
	}
	return "unknown";
}
