// SPDX-License-Identifier: BSD-3-Clause

#include <v90_upstream_data.h>
#include <v90_upstream_mode.h>
#include <v90_phase3.h>

#include "test.h"

#include <string.h>

enum {
	NOMINAL_AMPLITUDE = 5000,
	SAMPLE_LIMIT = 6000,
};

static uint16_t rate_bit(uint8_t rate_2400_bps_units)
{
	return (uint16_t)(1u <<
		(rate_2400_bps_units -
		 V90_MP_MINIMUM_RATE_2400_BPS_UNITS));
}

static struct v34_phase3_profile profile(uint8_t projected_rate)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_ANSWERING_STATION,
		.projected_data_rate_2400_bps_units = projected_rate,
		.high_carrier = true,
	};
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters cp_parameters(
	enum v90_cp_kind kind, uint16_t upstream_rates, bool acknowledged)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = G711_LAW_PCMU,
		.upstream_rate_mask = upstream_rates,
		.trn1d_ratio_q3_13 = 0x2000,
		.shaping_a1_q1_6 = -12,
		.shaping_a2_q1_6 = 6,
		.shaping_b1_q1_6 = -3,
		.shaping_b2_q1_6 = 24,
		.selected_rate_index = 1,
		.constellation_count = 1,
		.acknowledges_peer = acknowledged,
	};

	add_ucode(&parameters.transmit_constellations[0], 36);
	add_ucode(&parameters.transmit_constellations[0], 72);
	return parameters;
}

static bool drive_cp_record(struct v90_phase4_cp_line_tx *transmitter)
{
	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t sample;
		enum v90_phase4_cp_line_tx_result result =
			v90_phase4_cp_line_tx_sample(transmitter, &sample);

		if (result == V90_PHASE4_CP_LINE_TX_COMPLETE)
			return true;
		if (result != V90_PHASE4_CP_LINE_TX_OK)
			return false;
	}
	return false;
}

static bool cp_line_fixture(
	struct v90_phase4_cp_line_tx *transmitter,
	uint8_t projected_rate, uint16_t upstream_rates)
{
	struct v34_phase3_profile selected = profile(projected_rate);
	struct v90_cp_parameters training = cp_parameters(
		V90_CP_TRAINING, upstream_rates, false);
	struct v34_training_line line;

	return v34_training_line_init(
		       &line, &selected, V90_PHASE3_SAMPLE_RATE,
		       NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	       v90_phase4_cp_line_tx_init(
		       transmitter, &line, V34_CONSTELLATION_4_POINT,
		       &training) == V90_PHASE4_CP_LINE_TX_OK &&
	       drive_cp_record(transmitter);
}

static bool completed_e_fixture(
	struct v90_phase4_e_line_tx *transmitter,
	uint8_t projected_rate, uint16_t upstream_rates)
{
	struct v90_phase4_cp_line_tx cp;
	struct v90_cp_parameters data = cp_parameters(
		V90_CP_DATA, upstream_rates, true);

	if (!cp_line_fixture(&cp, projected_rate, upstream_rates) ||
	    v90_phase4_cp_line_tx_start_next_record(&cp, &data) !=
		    V90_PHASE4_CP_LINE_TX_OK ||
	    !drive_cp_record(&cp) ||
	    v90_phase4_e_line_tx_init(transmitter, &cp) !=
		    V90_PHASE4_E_LINE_TX_OK)
		return false;
	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t sample;
		enum v90_phase4_e_line_tx_result result =
			v90_phase4_e_line_tx_sample(transmitter, &sample);

		if (result == V90_PHASE4_E_LINE_TX_COMPLETE)
			return true;
		if (result != V90_PHASE4_E_LINE_TX_OK)
			return false;
	}
	return false;
}

static struct v90_mp_record type1_mp(uint8_t maximum, uint16_t rates)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_upstream_rate_2400_bps_units = maximum,
				.upstream_trellis = V34_TRELLIS_64_STATE,
				.upstream_nonlinear_encoding = true,
				.upstream_expanded_shaping = true,
				.upstream_rate_capability_mask = rates,
			},
			.precoder_coefficients = {
				{ 1024, -512 },
				{ -768, 384 },
				{ 256, 640 },
			},
		},
	};
}

static struct v90_mp_record type0_mp(uint8_t maximum, uint16_t rates)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = maximum,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_rate_capability_mask = rates,
		},
	};
}

static bool streams_match_through_b1(
	struct v34_data_stream *left, struct v34_data_stream *right,
	size_t *sample_count)
{
	for (size_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t left_sample;
		int16_t right_sample;
		bool left_accepted;
		bool right_accepted;

		if (v34_data_stream_sample(
			    left, NULL, NULL, &left_sample, &left_accepted) !=
			    V34_DATA_STREAM_OK ||
		    v34_data_stream_sample(
			    right, NULL, NULL, &right_sample, &right_accepted) !=
			    V34_DATA_STREAM_OK ||
		    left_sample != right_sample ||
		    left_accepted != right_accepted ||
		    left->b1_active != right->b1_active)
			return false;
		*sample_count = index + 1u;
		if (!left->b1_active)
			return true;
	}
	return false;
}

static bool test_selected_type1_stream(void)
{
	uint16_t cp_rates = rate_bit(6) | rate_bit(8) |
			    rate_bit(10) | rate_bit(12);
	uint16_t mp_rates = rate_bit(8) | rate_bit(10) | rate_bit(12);
	struct v90_mp_record mp = type1_mp(12, mp_rates);
	struct v90_phase4_e_line_tx e;
	struct v90_phase4_e_line_tx saved_e;
	struct v34_data_stream actual;
	struct v34_data_stream expected;
	struct v34_data_stream_configuration configuration = { 0 };
	struct v34_data_direction_parameters direction;
	size_t samples = 0;

	CHECK(completed_e_fixture(&e, 10, cp_rates),
	      "acknowledged CP did not reach E");
	saved_e = e;
	configuration.profile = e.line.profile;
	configuration.sample_rate = e.line.carrier.channel.sample_rate;
	configuration.nominal_peak_amplitude = e.line.nominal_peak_amplitude;
	configuration.trellis_selection = V34_TRELLIS_64_STATE;
	configuration.nonlinear_parameter = V34_NONLINEAR_THETA_5_OVER_16;
	configuration.primary_data_rate_2400_bps_units = 10;
	configuration.expanded_shaping = true;
	memcpy(configuration.precoder_coefficients,
	       mp.parameters.type1.precoder_coefficients,
	       sizeof(configuration.precoder_coefficients));
	CHECK(v90_upstream_data_start_after_e(&actual, &e, &mp) ==
		      V90_UPSTREAM_DATA_OK &&
	      v90_upstream_mode_select(
		      &e.cp, &mp, &e.line.profile, &direction) ==
		      V90_UPSTREAM_MODE_OK &&
	      v34_data_stream_continue_b1(
		      &expected, &e.line, &configuration) ==
		      V34_DATA_STREAM_OK &&
	      actual.transmitter.points.schedule.geometry
			      .primary_data_rate_2400_bps_units == 10 &&
	      actual.transmitter.points.encoder.parameters.expanded_shaping &&
	      direction.primary_data_rate_2400_bps_units == 10 &&
	      direction.trellis_selection == V34_TRELLIS_64_STATE &&
	      direction.nonlinear_parameter ==
		      V34_NONLINEAR_THETA_5_OVER_16 &&
	      direction.expanded_shaping &&
	      actual.transmitter.points.encoder.convolutional_encoder
			      .selection == V34_TRELLIS_64_STATE &&
	      memcmp(actual.transmitter.points.encoder.precoder.coefficients,
		     mp.parameters.type1.precoder_coefficients,
		     sizeof(mp.parameters.type1.precoder_coefficients)) == 0 &&
	      memcmp(&e, &saved_e, sizeof(e)) == 0,
	      "Type 1 MP did not produce the selected V.34 stream");
	CHECK(streams_match_through_b1(&actual, &expected, &samples) &&
	      samples != 0 && v34_data_stream_is_valid(&actual),
	      "selected upstream B1 did not match its negotiated model");
	return true;
}

static bool test_rate_intersection(void)
{
	uint16_t cp_rates = rate_bit(4) | rate_bit(6) | rate_bit(8);
	struct v90_phase4_e_line_tx e;
	struct v34_data_stream stream;
	struct v90_mp_record mp = type0_mp(
		12, rate_bit(6) | rate_bit(8) | rate_bit(10));

	CHECK(completed_e_fixture(&e, 8, cp_rates) &&
	      v90_upstream_data_start_after_e(&stream, &e, &mp) ==
		      V90_UPSTREAM_DATA_OK &&
	      stream.transmitter.points.schedule.geometry
			      .primary_data_rate_2400_bps_units == 8 &&
	      stream.transmitter.points.encoder.convolutional_encoder
			      .selection == V34_TRELLIS_32_STATE &&
	      !stream.transmitter.points.encoder.parameters.expanded_shaping,
	      "profile cap did not select the highest common rate");
	mp.parameters.type0.maximum_upstream_rate_2400_bps_units = 6;
	CHECK(v90_upstream_data_start_after_e(&stream, &e, &mp) ==
		      V90_UPSTREAM_DATA_OK &&
	      stream.transmitter.points.schedule.geometry
			      .primary_data_rate_2400_bps_units == 6,
	      "MP maximum did not cap the common rate");
	return true;
}

static bool test_contracts(void)
{
	uint16_t cp_rates = rate_bit(6) | rate_bit(8);
	struct v90_phase4_e_line_tx complete;
	struct v90_phase4_e_line_tx incomplete;
	struct v90_phase4_cp_line_tx cp;
	struct v90_cp_parameters unacknowledged = cp_parameters(
		V90_CP_DATA, cp_rates, false);
	struct v90_mp_record mp = type0_mp(8, rate_bit(8));
	struct v90_mp_record malformed = mp;
	struct v34_data_stream untouched;
	struct v34_data_stream saved;
	struct v34_data_direction_parameters untouched_direction;
	struct v34_data_direction_parameters saved_direction;
	struct v34_phase3_profile wrong_role;
	struct v34_phase3_profile active_md;

	CHECK(completed_e_fixture(&complete, 8, cp_rates),
	      "contract fixture did not complete E");
	incomplete = complete;
	incomplete.sequence.emitted_bits -= 2u;
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	memset(&untouched_direction, 0xa5, sizeof(untouched_direction));
	saved_direction = untouched_direction;
	wrong_role = complete.line.profile;
	wrong_role.local_role = V34_CALLING_STATION;
	active_md = complete.line.profile;
	active_md.md_length_35_ms_units = 1;
	active_md.md_symbol_count = 105;
	malformed.parameters.type0.maximum_upstream_rate_2400_bps_units = 1;
	CHECK(v90_upstream_data_start_after_e(NULL, &complete, &mp) ==
		      V90_UPSTREAM_DATA_INVALID_ARGUMENT &&
	      v90_upstream_data_start_after_e(&untouched, NULL, &mp) ==
		      V90_UPSTREAM_DATA_INVALID_ARGUMENT &&
	      v90_upstream_data_start_after_e(
		      &untouched, &complete, NULL) ==
		      V90_UPSTREAM_DATA_INVALID_ARGUMENT &&
	      v90_upstream_data_start_after_e(
		      &untouched, &incomplete, &mp) ==
		      V90_UPSTREAM_DATA_INVALID_STATE &&
	      v90_upstream_data_start_after_e(
		      &untouched, &complete, &malformed) ==
		      V90_UPSTREAM_DATA_INVALID_PARAMETERS &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "rejected upstream initialization changed its destination");
	CHECK(v90_upstream_mode_select(
		      NULL, &mp, &complete.line.profile,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_ARGUMENT &&
	      v90_upstream_mode_select(
		      &complete.cp, NULL, &complete.line.profile,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_ARGUMENT &&
	      v90_upstream_mode_select(
		      &complete.cp, &mp, NULL,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_ARGUMENT &&
	      v90_upstream_mode_select(
		      &complete.cp, &mp, &complete.line.profile, NULL) ==
		      V90_UPSTREAM_MODE_INVALID_ARGUMENT &&
	      v90_upstream_mode_select(
		      &unacknowledged, &mp, &complete.line.profile,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_PARAMETERS &&
	      v90_upstream_mode_select(
		      &complete.cp, &malformed, &complete.line.profile,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_PARAMETERS &&
	      v90_upstream_mode_select(
		      &complete.cp, &mp, &wrong_role,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_PARAMETERS &&
	      v90_upstream_mode_select(
		      &complete.cp, &mp, &active_md,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_INVALID_PARAMETERS &&
	      memcmp(&untouched_direction, &saved_direction,
		     sizeof(untouched_direction)) == 0,
	      "rejected upstream selection changed its output");
	mp.parameters.type0.maximum_upstream_rate_2400_bps_units = 0;
	CHECK(v90_upstream_data_start_after_e(
		      &untouched, &complete, &mp) ==
		      V90_UPSTREAM_DATA_NO_COMMON_RATE &&
	      v90_upstream_mode_select(
		      &complete.cp, &mp, &complete.line.profile,
		      &untouched_direction) ==
		      V90_UPSTREAM_MODE_NO_COMMON_RATE &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "cleardown changed the upstream destination");
	mp = type0_mp(7, rate_bit(7));
	CHECK(v90_upstream_data_start_after_e(
		      &untouched, &complete, &mp) ==
		      V90_UPSTREAM_DATA_NO_COMMON_RATE &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "disjoint rate sets changed the upstream destination");
	CHECK(cp_line_fixture(&cp, 8, cp_rates) &&
	      v90_phase4_e_line_tx_init(&complete, &cp) ==
		      V90_PHASE4_E_LINE_TX_INVALID_STATE &&
	      v90_phase4_cp_line_tx_start_next_record(
		      &cp, &unacknowledged) == V90_PHASE4_CP_LINE_TX_OK &&
	      drive_cp_record(&cp) &&
	      v90_phase4_e_line_tx_init(&complete, &cp) ==
		      V90_PHASE4_E_LINE_TX_INVALID_STATE,
	      "E accepted CPt or an unacknowledged CP");
	CHECK(strcmp(v90_upstream_data_result_name(
		     V90_UPSTREAM_DATA_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v90_upstream_data_result_name(
		     (enum v90_upstream_data_result)99),
		     "unknown") == 0,
	      "upstream result names changed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] selected Type 1 stream\n");
	if (!test_selected_type1_stream())
		return 1;
	fprintf(stderr, "[       OK ] selected Type 1 stream\n");
	fprintf(stderr, "[ RUN      ] rate intersection\n");
	if (!test_rate_intersection())
		return 1;
	fprintf(stderr, "[       OK ] rate intersection\n");
	fprintf(stderr, "[ RUN      ] contracts\n");
	if (!test_contracts())
		return 1;
	fprintf(stderr, "[       OK ] contracts\n");
	return 0;
}
