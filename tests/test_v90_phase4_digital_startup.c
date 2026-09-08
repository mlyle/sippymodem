// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_analog_station.h>
#include <v90_phase4_digital_startup.h>
#include <v90_digital_path.h>

#include <g711.h>

#include "test.h"
#include "v90_phase4_station_fixture.h"

#include <limits.h>
#include <string.h>

enum {
	TRAINING_UCODE = 91,
	CHARACTER_LIMIT = V90_PHASE4_TRN2D_WAIT_CP_SYMBOLS + 4096,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters training_parameters(enum g711_law law)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.upstream_rate_mask = 0x1555u,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = -16,
		.shaping_a2_q1_6 = 8,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = 31,
		.selected_rate_index = 13,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = 0x15555u,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation =
			V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static struct v90_mp_record mp_parameters(bool acknowledges_cp)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.acknowledges_cp = acknowledges_cp,
			.upstream_rate_capability_mask = 0x1555u,
		},
	};
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_encode_pcma(sample) : g711_encode_pcmu(sample);
}

static int16_t decode_octet(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(octet) : g711_decode_pcmu(octet);
}

static bool prepare_stations(
	enum g711_law law, uint8_t projected_rate,
	const struct v90_cp_parameters *training,
	struct v90_phase4_analog_station *analog,
	struct v90_phase4_digital_station *digital)
{
	struct v90_jd_parameters jd = jd_parameters();
	struct v90_phase3_return_line_handoff receive;

	if (!v90_phase4_station_fixture_init(
		    law, projected_rate, &jd, TRAINING_UCODE, training,
		    analog, &receive))
		return false;
	return v90_phase3_return_line_handoff_is_valid(&receive) &&
	       v90_phase4_digital_station_init(
		       digital, &receive, &jd, law,
		       TRAINING_UCODE) ==
		       V90_PHASE4_DIGITAL_STATION_OK;
}

static bool next_cp_octet(
	enum g711_law law, struct v90_phase4_analog_station *analog,
	const struct v90_cp_parameters *training,
	struct v90_phase4_cp_line_tx *continued, bool *handed_off,
	uint8_t *octet)
{
	int16_t sample;

	if (!*handed_off &&
	    !v90_phase4_analog_station_transmit_complete(analog)) {
		CHECK(v90_phase4_analog_station_transmit(analog, &sample) ==
			      V90_PHASE4_ANALOG_STATION_OK,
		      "initial CPt transmitter stopped");
		*octet = encode_sample(law, sample);
		return true;
	}
	if (!*handed_off) {
		CHECK(v90_phase4_analog_station_transmit_handoff(
			      analog, continued) ==
			      V90_PHASE4_ANALOG_STATION_OK,
		      "CPt transmitter did not hand off");
		*handed_off = true;
	}
	if (v90_phase4_cp_line_tx_record_is_complete(continued))
		CHECK(v90_phase4_cp_line_tx_start_next_record(
			      continued, training) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "continued CPt record did not start");
	CHECK(v90_phase4_cp_line_tx_sample(continued, &sample) ==
		      V90_PHASE4_CP_LINE_TX_OK,
	      "continued CPt transmitter stopped");
	*octet = encode_sample(law, sample);
	return true;
}

static bool check_rejected_cpt_continuation(
	const struct v90_phase4_digital_startup *startup,
	const struct v90_cp_parameters *training, int16_t sample)
{
	struct v90_phase4_digital_startup trial = *startup;
	struct v90_cp_receiver *records =
		&trial.state.continuing.receive.symbols.records;
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	size_t bits;

	CHECK(v90_cp_encode(training, frame, sizeof(frame), &bits) == V90_CP_OK &&
	      v90_cp_receiver_init(records) == V90_CP_OK,
	      "repeated CPt did not encode");
	/* Leave four bits for the live line decoder and corrupt an already
	 * buffered CRC bit. No possible suffix can repair the candidate. */
	for (size_t bit = 0u; bit < bits - 4u; ++bit) {
		struct v90_cp_receive_event event;

		CHECK(v90_cp_receiver_receive_bit(records,
			      (frame[bit / 8u] >> (7u - bit % 8u)) & 1u,
			      &event) == V90_CP_OK &&
		      event.type == V90_CP_RECEIVE_NO_EVENT,
		      "repeated CPt rejected before its suffix");
	}
	records->frame[(bits - 8u) / 8u] ^=
		(uint8_t)(1u << (7u - (bits - 8u) % 8u));
	for (size_t clock = 0u; clock < 16u; ++clock) {
		struct v90_phase4_cp_line_rx expected = trial.state.continuing.receive;
		struct v90_cp_receive_event event;
		enum v90_phase4_cp_line_rx_result received =
			v90_phase4_cp_line_rx_sample(&expected, sample, &event);
		uint8_t output;

		CHECK(v90_phase4_digital_startup_sample(&trial, sample, &output) ==
			      V90_PHASE4_STARTUP_OK && !trial.failed &&
		      trial.stage == V90_PHASE4_STARTUP_CONTINUING,
		      "rejected CPt revoked an accepted mapping");
		if (received != V90_PHASE4_CP_LINE_RX_RECORD_READY)
			continue;
		CHECK(event.type == V90_CP_RECEIVE_REJECTED &&
		      v90_phase4_cp_line_rx_continue(&expected) == V90_PHASE4_CP_LINE_RX_OK &&
		      memcmp(&expected, &trial.state.continuing.receive, sizeof(expected)) == 0 &&
		      v90_pcm_mapping_equal(&startup->state.continuing.station.training_mapping,
					    &trial.state.continuing.station.training_mapping),
		      "rejected CPt reset receive state or changed the mapping");
		return true;
	}
	CHECK(false, "repeated CPt candidate did not reach its CRC");
	return false;
}

static bool run_exchange(
	enum g711_law law, uint8_t projected_rate,
	uint16_t peer_rate_mask, int16_t decision_metric,
	uint8_t expected_maximum)
{
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_mp_record mp = mp_parameters(false);
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station digital_station;
	struct v90_phase4_digital_startup startup;
	struct v90_phase4_digital_handoff handoff;
	struct v90_phase4_cp_line_tx continued_cp;
	size_t continued_octets = 0;
	uint32_t expected_carrier_phase = 0;
	uint8_t to_analog;
	uint8_t to_digital;
	bool cp_handed_off = false;
	bool tracking_continuation = false;
	bool complete = false;

	training.upstream_rate_mask = peer_rate_mask;
	CHECK(prepare_stations(
		      law, projected_rate, &training,
		      &analog_station, &digital_station),
	      "law %u digital fixture did not initialize",
	      (unsigned int)law);
	{
		int16_t sample;

		CHECK(v90_phase4_analog_station_transmit(
			      &analog_station, &sample) ==
			      V90_PHASE4_ANALOG_STATION_OK &&
		      v90_phase4_digital_station_transmit(
			      &digital_station, &to_analog) ==
			      V90_PHASE4_DIGITAL_STATION_OK,
		      "law %u first characters were unavailable",
		      (unsigned int)law);
		to_digital = encode_sample(law, sample);
	}
	CHECK(v90_phase4_digital_startup_init(
		      &startup, &digital_station, &mp) ==
		      V90_PHASE4_STARTUP_OK,
	      "law %u digital startup did not initialize",
	      (unsigned int)law);

	for (size_t clock = 0; clock < CHARACTER_LIMIT; ++clock) {
		enum v90_phase4_startup_stage before = startup.stage;
		uint8_t next_to_analog;
		uint8_t next_to_digital;

		/* Ri-era quality must not freeze the final MP rate. */
		if (before == V90_PHASE4_STARTUP_INITIAL && decision_metric >= 0)
			startup.state.initial.receive.line.equalizer.quality =
				(struct v34_adaptive_equalizer_quality){
					.decision_metric = INT16_MAX,
				};
		if (before == V90_PHASE4_STARTUP_CONTINUING) {
			const struct v34_carrier_demodulator *carrier =
				&startup.state.continuing.receive.line.timing.carrier;

			if (!tracking_continuation) {
				CHECK(check_rejected_cpt_continuation(
					      &startup, &training, decode_octet(law, to_digital)),
				      "rejected CPt continuation failed");
				expected_carrier_phase =
					carrier->phase;
				tracking_continuation = true;
			}
			expected_carrier_phase +=
				carrier->phase_step;
			continued_octets++;
		}
		{
			enum v90_phase4_startup_result result =
				v90_phase4_digital_startup_sample(
					&startup,
					decode_octet(law, to_digital),
					&next_to_analog);

			CHECK(result == V90_PHASE4_STARTUP_OK,
			      "law %u digital startup stopped at %zu: %s",
			      (unsigned int)law, clock,
			      v90_phase4_startup_result_name(result));
		}
		if (!v90_phase4_analog_station_receive_complete(
			    &analog_station))
			CHECK(v90_phase4_analog_station_receive(
				      &analog_station, to_analog) ==
				      V90_PHASE4_ANALOG_STATION_OK,
			      "law %u Ri receive stopped at %zu",
			      (unsigned int)law, clock);
		CHECK(next_cp_octet(
			      law, &analog_station, &training,
			      &continued_cp, &cp_handed_off,
			      &next_to_digital),
		      "law %u CP source stopped at %zu",
		      (unsigned int)law, clock);
		to_analog = next_to_analog;
		to_digital = next_to_digital;
		if (v90_phase4_digital_startup_is_complete(&startup)) {
			if (!v90_phase4_analog_station_receive_complete(
				    &analog_station))
				CHECK(v90_phase4_analog_station_receive(
					      &analog_station, to_analog) ==
					      V90_PHASE4_ANALOG_STATION_OK,
				      "law %u final Ri-bar was not accepted",
				      (unsigned int)law);
			complete = true;
			break;
		}
	}
	CHECK(startup.initial_mp.parameters.type0
		      .maximum_upstream_rate_2400_bps_units ==
		      projected_rate &&
	      startup.initial_mp.parameters.type0
		      .upstream_rate_capability_mask == 0x1555u,
	      "law %u metric %d selected rate %u instead of %u or changed mask",
	      (unsigned int)law, decision_metric,
	      startup.initial_mp.parameters.type0
		      .maximum_upstream_rate_2400_bps_units,
	      projected_rate);
	CHECK(complete && tracking_continuation && continued_octets > 0 &&
	      v90_phase4_digital_startup_handoff(
		      &startup, &handoff) == V90_PHASE4_STARTUP_OK &&
	      handoff.receive.line.timing.carrier.phase ==
		      expected_carrier_phase &&
	      v90_phase4_digital_transmit_handoff_is_valid(
		      &handoff.transmit) &&
	      v90_phase4_cp_line_rx_is_valid(&handoff.receive) &&
	      !v90_phase4_cp_line_rx_has_pending_record(&handoff.receive),
	      "law %u digital startup lost a character: %zu",
	      (unsigned int)law, continued_octets);
	CHECK(handoff.peer_upstream_rate_mask == peer_rate_mask,
	      "CPt upstream capabilities were lost in the handoff");
	{
		struct v90_phase4_digital_startup mismatched = startup;

		mismatched.initial_mp.parameters.type0
			.maximum_upstream_rate_2400_bps_units++;
		CHECK(v90_mp_record_is_valid(&mismatched.initial_mp) &&
		      !v90_phase4_digital_startup_is_valid(&mismatched),
		      "law %u mismatched MP handoff was accepted",
		      (unsigned int)law);
	}
	{
		struct v90_phase4_digital_startup saved = startup;
		uint8_t output = 0xa5;

		CHECK(v90_phase4_digital_startup_sample(
			      &startup, decode_octet(law, to_digital), &output) ==
			      V90_PHASE4_STARTUP_DONE &&
		      output == 0xa5 &&
		      memcmp(&startup, &saved, sizeof(startup)) == 0,
		      "law %u completed startup consumed a character",
		      (unsigned int)law);
	}
	if (decision_metric >= 0) {
		struct v90_digital_path path;
		uint8_t output;
		bool selected = false;

		CHECK(v90_digital_path_init(
			      &path, &handoff, V90_JD_RATE_CAPABILITY_MASK,
			      V34_CONSTELLATION_4_POINT) == V90_DIGITAL_PATH_OK,
		      "quality-selection path did not initialize");
		for (size_t sample = 0; sample < CHARACTER_LIMIT; ++sample) {
			struct pcm_downstream_tx *transmit = &path.downstream.training;
			bool final_window = transmit->accepted_frames >=
				V90_PHASE4_TRN2D_WAIT_CP_FRAMES - 10u;

			path.upstream.startup.state.cp.line.equalizer.quality =
				(struct v34_adaptive_equalizer_quality){
					.decision_metric = 0,
					.recent_decision_metric = final_window ?
						decision_metric : 0,
					.recent_symbol_count =
						V34_ADAPTIVE_EQUALIZER_RECENT_SYMBOLS,
				};
			CHECK(path.local_mp.parameters.type0
				      .maximum_upstream_rate_2400_bps_units ==
				      projected_rate &&
			      v90_digital_path_transmit(&path, NULL, NULL, &output) ==
				      V90_DIGITAL_PATH_OK,
			      "MP was selected before the conditioning interval");
			if (transmit->stage == PCM_DOWNSTREAM_TX_MP) {
				selected = true;
				break;
			}
		}
		CHECK(selected && path.local_mp.parameters.type0
			      .maximum_upstream_rate_2400_bps_units ==
			      expected_maximum &&
		      path.local_mp.parameters.type0.upstream_rate_capability_mask ==
			      0x1555u &&
		      v90_mp_record_equal(&path.local_mp,
			      &path.downstream.training.control.mp) &&
		      v90_mp_record_equal(&path.local_mp,
			      &path.upstream.startup.local_mp),
		      "law %u metric %d selected %u instead of %u after TRN2d",
		      (unsigned int)law, decision_metric,
		      path.local_mp.parameters.type0.maximum_upstream_rate_2400_bps_units,
		      expected_maximum);
		/* Later measurements cannot change a partly encoded MP. */
		path.upstream.startup.state.cp.line.equalizer.quality =
			(struct v34_adaptive_equalizer_quality){
				.decision_metric = INT16_MAX,
			};
		for (size_t sample = 0; sample < 256u; ++sample)
			CHECK(v90_digital_path_transmit(&path, NULL, NULL, &output) ==
				      V90_DIGITAL_PATH_OK &&
			      path.local_mp.parameters.type0
				      .maximum_upstream_rate_2400_bps_units ==
				      expected_maximum,
			      "quality changed an MP after its first source frame");
	}
	return true;
}

static bool run_unusable_cpt_selection(
	enum g711_law law, uint8_t local_maximum,
	uint16_t local_mask, uint16_t peer_mask)
{
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_mp_record mp = mp_parameters(false);
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station digital_station;
	struct v90_phase4_digital_startup startup;
	int16_t sample;
	uint8_t output;

	training.upstream_rate_mask = peer_mask;
	mp.parameters.type0.maximum_upstream_rate_2400_bps_units = local_maximum;
	mp.parameters.type0.upstream_rate_capability_mask = local_mask;
	CHECK(prepare_stations(
		      law, 12u, &training, &analog_station, &digital_station) &&
	      v90_phase4_digital_startup_init(
		      &startup, &digital_station, &mp) ==
		      V90_PHASE4_STARTUP_OK,
	      "law %u disjoint CPt fixture did not initialize",
	      (unsigned int)law);

	for (size_t clock = 0; clock < CHARACTER_LIMIT; ++clock) {
		CHECK(v90_phase4_analog_station_transmit(
			      &analog_station, &sample) ==
			      V90_PHASE4_ANALOG_STATION_OK,
		      "law %u disjoint CPt source stopped at %zu",
		      (unsigned int)law, clock);
		CHECK(v90_phase4_digital_startup_sample(
			      &startup, sample, &output) ==
			      V90_PHASE4_STARTUP_OK,
		      "law %u digital modem rejected disjoint CPt at %zu",
		      (unsigned int)law, clock);
		if (startup.stage == V90_PHASE4_STARTUP_INITIAL)
			continue;
		CHECK(startup.stage == V90_PHASE4_STARTUP_CONTINUING &&
		      !startup.failed &&
		      v90_mp_record_equal(&startup.initial_mp, &mp),
		      "law %u unusable CPt changed the local MP",
		      (unsigned int)law);
		return true;
	}
	CHECK(false, "law %u disjoint CPt was not decoded",
	      (unsigned int)law);
	return false;
}

static bool run_precoder_selection(
	enum g711_law law,
	const struct v34_precoder_recommendation *precoder,
	uint8_t maximum_rate, bool configure_type1)
{
	struct v90_cp_parameters training = training_parameters(law);
	struct v90_mp_record mp = mp_parameters(false);
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station digital_station;
	struct v90_phase4_digital_startup startup;
	uint8_t expected_maximum = maximum_rate;
	int16_t sample;
	uint8_t output;

	mp.parameters.type0.maximum_upstream_rate_2400_bps_units =
		maximum_rate;
	if (configure_type1) {
		struct v90_mp_parameters common = mp.parameters.type0;

		mp = (struct v90_mp_record){
			.type = V90_MP_RECORD_TYPE1,
			.parameters.type1.common = common,
		};
		memcpy(mp.parameters.type1.precoder_coefficients,
		       precoder->coefficients,
		       sizeof(precoder->coefficients));
	}

	CHECK(prepare_stations(
		      law, maximum_rate, &training,
		      &analog_station, &digital_station),
	      "law %u precoder fixture did not initialize",
	      (unsigned int)law);
	digital_station.precoder = *precoder;
	CHECK(v90_phase4_digital_station_is_valid(&digital_station) &&
	      v90_phase4_digital_startup_init(
		      &startup, &digital_station, &mp) ==
		      V90_PHASE4_STARTUP_OK,
	      "law %u precoder startup did not initialize",
	      (unsigned int)law);
	for (size_t clock = 0; clock < CHARACTER_LIMIT; ++clock) {
		CHECK(v90_phase4_analog_station_transmit(
			      &analog_station, &sample) ==
			      V90_PHASE4_ANALOG_STATION_OK &&
		      v90_phase4_digital_startup_sample(
			      &startup, sample, &output) ==
			      V90_PHASE4_STARTUP_OK,
		      "law %u precoder exchange stopped at %zu",
		      (unsigned int)law, clock);
		if (startup.stage == V90_PHASE4_STARTUP_INITIAL)
			continue;
		CHECK(startup.stage == V90_PHASE4_STARTUP_CONTINUING,
		      "law %u precoder startup did not continue",
		      (unsigned int)law);
		if (configure_type1) {
			CHECK(startup.initial_mp.type == V90_MP_RECORD_TYPE1 &&
			      memcmp(startup.initial_mp.parameters.type1
					     .precoder_coefficients,
				     precoder->coefficients,
				     sizeof(precoder->coefficients)) == 0 &&
			      startup.initial_mp.parameters.type1.common
					      .maximum_upstream_rate_2400_bps_units ==
					      expected_maximum &&
			      startup.initial_mp.parameters.type1.common
					      .upstream_rate_capability_mask ==
				      0x1555u,
			      "law %u did not select the expected Type-1 MP",
			      (unsigned int)law);
		} else {
			mp.parameters.type0.maximum_upstream_rate_2400_bps_units =
				expected_maximum;
			CHECK(v90_mp_record_equal(&startup.initial_mp, &mp),
			      "law %u automatically published a trained Type-1 MP",
			      (unsigned int)law);
		}
		return true;
	}
	CHECK(false, "law %u precoder CPt was not decoded",
	      (unsigned int)law);
	return false;
}

static bool test_generated_exchange(void)
{
	static const struct v34_precoder_recommendation learned_precoder = {
		.coefficients = {
			{ 31, 17 }, { 1, 15 }, { 25, -10 },
		},
		.enabled = true,
	};
	static const struct v34_precoder_recommendation excessive_precoder = {
		.coefficients = {
			{ 1530, 139 }, { 15, -261 }, { -301, 125 },
		},
		.enabled = true,
	};

	CHECK(run_exchange(G711_LAW_PCMU, 12u, 0x1555u, -1, 12u) &&
	      run_exchange(G711_LAW_PCMA, 12u, 0x1555u, -1, 12u) &&
	      run_exchange(G711_LAW_PCMU, 8u, 0x1555u, -1, 8u) &&
	      run_exchange(G711_LAW_PCMA, 8u, 0x1555u, -1, 8u) &&
	      run_exchange(G711_LAW_PCMU, 12u, 0x0015u, 0, 6u) &&
	      run_exchange(G711_LAW_PCMA, 12u, 0x0015u, 0, 6u) &&
	      run_precoder_selection(
		      G711_LAW_PCMU, &learned_precoder, 12u, false) &&
	      run_precoder_selection(
		      G711_LAW_PCMA, &learned_precoder, 12u, false) &&
	      run_precoder_selection(
		      G711_LAW_PCMU, &learned_precoder, 14u, false) &&
	      run_precoder_selection(
		      G711_LAW_PCMA, &learned_precoder, 14u, false) &&
	      run_precoder_selection(
		      G711_LAW_PCMU, &excessive_precoder, 14u, false) &&
	      run_precoder_selection(
		      G711_LAW_PCMA, &excessive_precoder, 14u, false) &&
	      run_precoder_selection(
		      G711_LAW_PCMU, &learned_precoder, 12u, true) &&
	      run_precoder_selection(
		      G711_LAW_PCMA, &learned_precoder, 12u, true),
	      "generated digital startup exchanges failed");
	return true;
}

static bool test_receive_quality_selection(void)
{
	static const struct {
		uint16_t peer_mask;
		int16_t metric;
		uint8_t maximum;
	} cases[] = {
		/* At 3000 baud the shared 28.8-kbit/s quality threshold is 58.
		 * Local rates are sparse: 4.8, 9.6, 14.4, 19.2, 24, 28.8, 33.6. */
		{ 0x1555u, 57, 12u },
		{ 0x1555u, 58, 10u },
		{ 0x1555u, 179, 8u },
		{ 0x1555u, 918, 6u },
		{ 0x1555u, INT16_MAX, 2u },
		/* Poor quality cannot enable a rate absent from either modem. */
		{ 0x0014u, INT16_MAX, 4u },
		{ 0x0400u, INT16_MAX, 12u },
	};
	static const enum g711_law laws[] = { G711_LAW_PCMU, G711_LAW_PCMA };

	for (size_t law = 0; law < ARRAY_LENGTH(laws); ++law)
		for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
			CHECK(run_exchange(
				      laws[law], 12u, cases[index].peer_mask,
				      cases[index].metric, cases[index].maximum),
			      "law %u quality selection case %zu failed",
			      (unsigned int)laws[law], index);
	for (size_t law = 0; law < ARRAY_LENGTH(laws); ++law)
		CHECK(
			/* Disjoint capabilities, or common 31.2 at unsupported
			 * 3000 baud, must not invalidate the local MP. */
			run_unusable_cpt_selection(laws[law], 13u, 0x0800u, 0x03ffu) &&
			run_unusable_cpt_selection(laws[law], 13u, 0x0800u, 0x0800u) &&
			/* Nor enable an above-ceiling rate or undo cleardown. */
			run_unusable_cpt_selection(laws[law], 12u, 0x1555u, 0x1000u) &&
			run_unusable_cpt_selection(laws[law], 0u, 0x1555u, 0x1555u),
			"law %u no-rate selection changed MP semantics",
			(unsigned int)laws[law]);
	return true;
}

static bool test_contracts(void)
{
	struct v90_mp_record mp = mp_parameters(false);
	struct v90_mp_record acknowledged = mp_parameters(true);
	struct v90_phase4_analog_station analog_station;
	struct v90_phase4_digital_station digital_station;
	struct v90_phase4_digital_startup startup;
	struct v90_phase4_digital_startup saved;
	struct v90_phase4_digital_startup failed;
	struct v90_phase4_digital_handoff handoff;
	struct v90_cp_parameters training =
		training_parameters(G711_LAW_PCMU);
	uint8_t output = 0xa5;

	CHECK(prepare_stations(
		      G711_LAW_PCMU, 8u, &training,
		      &analog_station, &digital_station),
	      "contract fixture did not initialize");
	memset(&startup, 0xa5, sizeof(startup));
	saved = startup;
	CHECK(v90_phase4_digital_startup_init(
		      NULL, &digital_station, &mp) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_digital_startup_init(
		      &startup, NULL, &mp) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_digital_startup_init(
		      &startup, &digital_station, NULL) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_digital_startup_init(
		      &startup, &digital_station, &acknowledged) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      memcmp(&startup, &saved, sizeof(startup)) == 0,
	      "invalid initialization changed the destination");
	CHECK(v90_phase4_digital_startup_init(
		      &startup, &digital_station, &mp) ==
		      V90_PHASE4_STARTUP_OK,
	      "valid startup did not initialize");
	saved = startup;
	CHECK(v90_phase4_digital_startup_sample(
		      &startup, 0, (uint8_t *)&startup) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_digital_startup_sample(
		      &startup, 0, NULL) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      v90_phase4_digital_startup_handoff(
		      &startup, &handoff) ==
		      V90_PHASE4_STARTUP_INVALID_STATE &&
	      v90_phase4_digital_startup_handoff(
		      &startup, NULL) ==
		      V90_PHASE4_STARTUP_INVALID_ARGUMENT &&
	      memcmp(&startup, &saved, sizeof(startup)) == 0,
	      "invalid active operations changed the startup");
	failed = startup;
	failed.failed = true;
	CHECK(v90_phase4_digital_startup_is_valid(&failed) &&
	      v90_phase4_digital_startup_sample(
		      &failed, 0, &output) == V90_PHASE4_STARTUP_FAILED &&
	      output == 0xa5 &&
	      !v90_phase4_digital_startup_is_valid(NULL) &&
	      !v90_phase4_digital_startup_has_failed(NULL) &&
	      !v90_phase4_digital_startup_is_complete(NULL),
	      "failed startup consumed another character");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated PCMU and PCMA continuation",
		  test_generated_exchange },
		{ "receive quality selects the common upstream rate",
		  test_receive_quality_selection },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
