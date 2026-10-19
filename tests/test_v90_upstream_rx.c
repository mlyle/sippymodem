// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase4_cp_line_rx.h>
#include <v90_phase4_e_line_tx.h>
#include <v90_jd.h>
#include "v90_upstream_fixture.h"
#include <v90_upstream_rx.h>

#include "test.h"
#include "v34_state_fixture.h"
#include "v90_phase4_line_fixture.h"

#include <string.h>

enum { SAMPLE_LIMIT = 70000 };

static void receiver_snapshot(struct v90_upstream_rx *copy,
	const struct v90_upstream_rx *receiver)
{
	memcpy(copy, receiver, sizeof(*copy));
	v34_snapshot_geometry(&copy->data_mapping.schedule.geometry);
	v34_snapshot_geometry(&copy->calibration.geometry);
	v34_snapshot_geometry(&copy->calibration.schedule.geometry);
	v34_snapshot_parameters(&copy->data_mapping.encoder.parameters);
	v34_snapshot_parameters(&copy->data_mapping.normalizer.parameters);
	v34_snapshot_parameters(&copy->calibration.parameters);
	v34_snapshot_parameters(&copy->calibration.encoder.parameters);
	if (copy->stage == V90_UPSTREAM_RX_STAGE_B1 ||
	    copy->stage == V90_UPSTREAM_RX_STAGE_COMPLETE) {
		v34_snapshot_geometry(&copy->state.data.geometry);
		v34_snapshot_data_channel_receiver(
			&copy->state.data.decisions.data_receiver);
	}
}

static bool receiver_states_equal(const struct v90_upstream_rx *left,
	const struct v90_upstream_rx *right)
{
	struct v90_upstream_rx a, b;
	receiver_snapshot(&a, left);
	receiver_snapshot(&b, right);
	return memcmp(&a, &b, sizeof(a)) == 0;
}

static uint16_t rate_bit(uint8_t rate_2400_bps_units)
{
	return (uint16_t)(1u <<
		(rate_2400_bps_units -
		 V90_MP_MINIMUM_RATE_2400_BPS_UNITS));
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters cp_parameters(
	enum v90_cp_kind kind, enum g711_law law, bool acknowledged,
	uint8_t rate_2400_bps_units)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = rate_bit(rate_2400_bps_units),
		.trn1d_ratio_q3_13 = 0x2000,
		.shaping_a1_q1_6 = -12,
		.shaping_a2_q1_6 = 6,
		.shaping_b1_q1_6 = -3,
		.shaping_b2_q1_6 = 24,
		.selected_rate_index = kind == V90_CP_TRAINING ? 13 : 1,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
		.acknowledges_peer = acknowledged,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v90_mp_record local_mp(uint8_t rate_2400_bps_units)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units =
				rate_2400_bps_units,
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_rate_capability_mask =
				rate_bit(rate_2400_bps_units),
		},
	};
}

static bool line_profile(
	enum v34_symbol_rate symbol_rate, uint8_t rate_2400_bps_units,
	struct v34_phase3_profile *profile)
{
	const struct v34_info1_probe_result probe = {
		.projected_data_rate_2400_bps_units = rate_2400_bps_units,
		.high_carrier = true,
	};

	return v34_phase3_profile_init(
		       profile, V34_ANSWERING_STATION, symbol_rate, &probe,
		       0, 0) == V34_PHASE3_PROFILE_OK;
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool check_cleardown(
	enum g711_law law, enum v34_constellation_size constellation,
	enum v90_cp_kind kind)
{
	struct v90_cp_parameters training = cp_parameters(V90_CP_TRAINING, law, false, 6u);
	struct v90_cp_parameters cleardown = training;
	struct v90_mp_record mp = local_mp(6u);
	struct v90_pcm_mapping mapping;
	struct v34_phase3_profile profile;
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx transmitter;
	struct v90_phase4_cp_line_rx cp_receiver;
	struct v90_upstream_rx receiver;
	bool cleardown_started = false;

	cleardown.kind = kind;
	cleardown.selected_rate_index = 0u;
	cleardown.upstream_rate_mask = 0u;
	cleardown.acknowledges_peer = true;
	cleardown.request_silence = true;
	memset(cleardown.transmit_constellations, 0,
	       sizeof(cleardown.transmit_constellations));
	CHECK(line_profile(V34_SYMBOL_RATE_3000, 6u, &profile) &&
	      v90_phase4_line_fixture_init_for_profile(
		      &profile, law, constellation, &training, &transmitter, &phase3) &&
	      v90_phase4_cp_line_rx_init(&cp_receiver, &phase3, constellation) ==
		      V90_PHASE4_CP_LINE_RX_OK &&
	      v90_pcm_mapping_init_transmit(&mapping, &training, law) == V90_PCM_MAPPING_OK &&
	      v90_upstream_rx_init(&receiver, &cp_receiver, &mapping, &mp, 1u) ==
		      V90_UPSTREAM_RX_OK,
	      "cleardown line did not initialize");
	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		struct v90_cp_parameters event;
		struct v90_cp_parameters before;
		int16_t sample;
		enum v90_upstream_rx_result result;

		if (v90_phase4_cp_line_tx_record_is_complete(&transmitter)) {
			CHECK(v90_phase4_cp_line_tx_start_next_record(&transmitter, &cleardown) ==
			      V90_PHASE4_CP_LINE_TX_OK, "zero-rate CP did not start");
			cleardown_started = true;
		}
		CHECK(v90_phase4_cp_line_tx_sample(&transmitter, &sample) ==
		      V90_PHASE4_CP_LINE_TX_OK, "cleardown source stopped");
		memset(&event, 0xa5, sizeof(event));
		memcpy(&before, &event, sizeof(before));
		result = v90_upstream_rx_sample(&receiver, quantize(law, sample), &event);
		CHECK((result == V90_UPSTREAM_RX_OK || result == V90_UPSTREAM_RX_CLEARDOWN) &&
		      memcmp(&event, &before, sizeof(event)) == 0 &&
		      v90_upstream_rx_is_valid(&receiver) && !receiver.failed &&
		      !receiver.data_cp_received,
		      "zero-rate CP became a data selection/control request: %s",
		      v90_upstream_rx_result_name(result));
		if (result == V90_UPSTREAM_RX_CLEARDOWN)
			return cleardown_started;
	}
	CHECK(false, "zero-rate CP was not recognized");
	return false;
}

static bool check_cps_requests(enum g711_law law, unsigned int final_case)
{
	struct v90_cp_parameters training = cp_parameters(V90_CP_TRAINING, law, false, 6u);
	struct v90_cp_parameters data = cp_parameters(V90_CP_DATA, law, false, 6u);
	struct v90_cp_parameters cps = data;
	struct v90_mp_record mp = local_mp(6u);
	struct v90_pcm_mapping mapping;
	struct v34_phase3_profile profile;
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_rx cp;
	struct v90_upstream_rx rx;
	unsigned int requests = 0u;
	unsigned int scr_samples = 0u;
	bool scr_complete = false;

	cps.request_silence = true;
	cps.selected_rate_index = V90_CP_RATE_INDEX_MAX;
	cps.upstream_rate_mask = 0u;
	/* CPs masks are unused; retain active masks only for the invalid-data case. */
	if (final_case != 2u)
		memset(cps.transmit_constellations, 0, sizeof(cps.transmit_constellations));
	/* The invalid-data case advertises its rate so mapping admission, rather
	 * than the disabled-rate filter, must reject it. */
	CHECK(v90_cp_parameters_are_valid(&cps) &&
	      v90_pcm_mapping_init_transmit(&mapping, &cps, law) != V90_PCM_MAPPING_OK &&
	      line_profile(V34_SYMBOL_RATE_3000, 6u, &profile) &&
	      v90_phase4_line_fixture_init_for_profile(&profile, law,
		      V34_CONSTELLATION_4_POINT, &training, &tx, &phase3) &&
	      v90_phase4_cp_line_rx_init(&cp, &phase3, V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_OK &&
	      v90_pcm_mapping_init_transmit(&mapping, &training, law) == V90_PCM_MAPPING_OK &&
	      v90_upstream_rx_init(&rx, &cp, &mapping, &mp,
		      final_case == 2u ? V90_JD_RATE_CAPABILITY_MASK : 1u) ==
		      V90_UPSTREAM_RX_OK,
	      "CPs admission fixture failed");
	if (final_case == 1u)
		data.selected_rate_index = 0u;
	else if (final_case == 2u) {
		data = cps;
		data.request_silence = false;
	}
	for (size_t clock = 0; clock < SAMPLE_LIMIT; ++clock) {
		struct v90_cp_parameters event;
		int16_t sample;
		bool sample_ready = false;
		enum v90_upstream_rx_result result;

		if (requests == 2u && !scr_complete &&
		    v90_phase4_cp_line_tx_record_is_complete(&tx)) {
			enum v90_phase4_cp_line_tx_result scr_result =
				v90_phase4_cp_line_tx_scr_sample(&tx, scr_samples >= 128u, &sample);

			CHECK(scr_result == V90_PHASE4_CP_LINE_TX_OK ||
			      scr_result == V90_PHASE4_CP_LINE_TX_COMPLETE, "CPs SCR stopped");
			scr_complete = scr_result == V90_PHASE4_CP_LINE_TX_COMPLETE;
			sample_ready = !scr_complete;
			if (sample_ready)
				scr_samples++;
		}
		if (!sample_ready && v90_phase4_cp_line_tx_record_is_complete(&tx)) {
			cps.acknowledges_peer = requests != 0u;
			CHECK(v90_phase4_cp_line_tx_start_next_record(&tx,
				      requests < 2u ? &cps : &data) == V90_PHASE4_CP_LINE_TX_OK,
			      "CPs repetition failed");
		}
		CHECK(sample_ready ||
		      v90_phase4_cp_line_tx_sample(&tx, &sample) == V90_PHASE4_CP_LINE_TX_OK,
		      "CPs line stopped");
		result = v90_upstream_rx_sample(&rx, quantize(law, sample), &event);
		if (result == V90_UPSTREAM_RX_OK)
			continue;
		CHECK(v90_upstream_rx_is_valid(&rx) && rx.failed ==
			      (requests == 2u && final_case == 2u),
		      "CPs owner failed: %s", v90_upstream_rx_result_name(result));
		CHECK(result == (requests == 0u ? V90_UPSTREAM_RX_REQUEST_MP_PRIME :
			requests == 1u ? V90_UPSTREAM_RX_REQUEST_SILENCE :
			final_case == 1u ? V90_UPSTREAM_RX_CLEARDOWN :
			final_case == 2u ? V90_UPSTREAM_RX_INVALID_PARAMETERS : V90_UPSTREAM_RX_REQUEST_RT),
		      "CPs request %u became %s", requests, v90_upstream_rx_result_name(result));
		CHECK(rx.data_cp_received == (requests == 2u && final_case == 0u),
		      "CPs installed a data selection or zero rate became data");
		if (++requests == 3u) {
			CHECK(scr_complete && scr_samples >= 128u,
			      "CPs did not exercise pre-DATA SCR");
			return true;
		}
	}
	CHECK(false, "CPs control did not complete");
	return false;
}

struct entry_payload {
	size_t bits;
};

static uint8_t entry_bit(size_t offset)
{
	uint32_t mixed = (uint32_t)offset + UINT32_C(0x94d049bb);

	mixed ^= mixed >> 16;
	mixed *= UINT32_C(0x7feb352d);
	mixed ^= mixed >> 15;
	return (uint8_t)(mixed & 1u);
}

static bool entry_source(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *bits, uint8_t *auxiliary)
{
	struct entry_payload *payload = context;

	for (unsigned int bit = 0; bit < request->primary_bits; ++bit)
		bits[bit] = entry_bit(payload->bits++);
	if (auxiliary != NULL)
		*auxiliary = 0;
	return true;
}

static bool entry_payload_received(
	struct v34_data_stream *transmitter,
	struct v34_data_line_receiver *receiver,
	enum g711_law law, struct entry_payload *payload)
{
	size_t received_bits = 0u;

	for (size_t clock = 0; clock < SAMPLE_LIMIT && received_bits < 4096u; ++clock) {
		int16_t sample;
		bool accepted;
		struct v34_survivor_decision current;
		struct v34_received_mapping_frame frame;
		enum v34_data_receive_result result;

		CHECK(v34_data_stream_sample(transmitter, entry_source, payload,
			&sample, &accepted) == V34_DATA_STREAM_OK, "post-B1 source failed");
		result = v34_data_line_receiver_sample(receiver, quantize(law, sample),
			&current, &frame);
		CHECK(result == V34_DATA_RECEIVE_OK || result == V34_DATA_RECEIVE_INTERVAL_READY ||
			result == V34_DATA_RECEIVE_MAPPING_FRAME_READY, "post-B1 receiver failed: %u", result);
		if (result != V34_DATA_RECEIVE_MAPPING_FRAME_READY)
			continue;
		for (unsigned int bit = 0; bit < frame.primary_bit_count; ++bit, ++received_bits) {
			/* Account for the specified pending DTE mark bits at B1 exit,
			 * without searching past or discarding any received payload. */
			uint8_t expected = received_bits < transmitter->primary_delay_bits ? 1u :
				entry_bit(received_bits - transmitter->primary_delay_bits);

			CHECK(frame.primary_bits[bit] == expected,
			      "post-B1 payload changed at bit %zu", received_bits);
		}
	}
	CHECK(received_bits >= 4096u, "post-B1 data timed out");
	return true;
}

static bool run_line(
	enum g711_law law, enum v34_constellation_size constellation,
	enum v34_symbol_rate symbol_rate, uint8_t rate_2400_bps_units,
	bool acknowledged_first, bool different_codec, bool damage_prime,
	bool unusable_unadvertised)
{
	enum g711_law codec_law = different_codec ?
		(law == G711_LAW_PCMU ? G711_LAW_PCMA : G711_LAW_PCMU) : law;
	struct v90_cp_parameters training =
		cp_parameters(
			V90_CP_TRAINING, codec_law, false,
			rate_2400_bps_units);
	struct v90_cp_parameters data =
		cp_parameters(
			V90_CP_DATA, codec_law, false,
			rate_2400_bps_units);
	struct v90_cp_parameters unadvertised = data;
	struct v90_cp_parameters late_training = training;
	struct v90_cp_parameters prime = data;
	struct v90_cp_parameters expected_data;
	struct v90_mp_record mp = local_mp(rate_2400_bps_units);
	struct v90_pcm_mapping training_mapping;
	struct v34_phase3_profile profile;
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx cp_tx;
	struct v90_phase4_cp_line_rx cp_rx;
	struct v90_phase4_e_line_tx e_tx;
	struct v34_data_stream data_tx;
	struct v90_upstream_rx receiver;
	struct v34_data_line_receiver data_handoff;
	bool data_started = false;
	bool unadvertised_started = false;
	bool late_training_started = false;
	bool prime_started = false;
	bool cp_finished = false;
	bool e_started = false;
	bool b1_started = false;
	bool mp_prime_requested = false;
	bool deferred_mp_selected = false;
	unsigned int preparation_samples = 0;
	bool ed_requested = false;
	bool e_received = false;
	bool b1_received = false;
	bool prime_damaged = false;
	bool scr_complete = false;
	unsigned int scr_samples = 0u;
	struct entry_payload payload = { 0 };
	uint16_t b1_previous_error_bits = 0u;
	bool b1_frame_seen = false;

	data.acknowledges_peer = acknowledged_first;
	prime.acknowledges_peer = true;
	expected_data = data;
	expected_data.acknowledges_peer = false;
	/* Exercise usable and impossible mappings at a disabled rate. Neither
	 * CRC-valid record may become a data selection or fail negotiation. */
	unadvertised.selected_rate_index = unusable_unadvertised ?
		V90_CP_RATE_INDEX_MAX : 2u;
	if (unusable_unadvertised) {
		struct v90_pcm_mapping unusable;

		CHECK(v90_cp_parameters_are_valid(&unadvertised) &&
		      v90_pcm_mapping_init_transmit(&unusable, &unadvertised, law) ==
			      V90_PCM_MAPPING_INVALID_PARAMETERS,
		      "unadvertised CP must have an unusable data mapping");
	}
	/* A peer can finish training with a different, still CPt-marked
	 * constellation before sending its actual data CP. Keep the original
	 * training map and wait for an explicit data selection. */
	add_ucode(&late_training.transmit_constellations[0], 25u);
	CHECK(line_profile(symbol_rate, rate_2400_bps_units, &profile) &&
	      v90_phase4_line_fixture_init_for_profile(
		      &profile, law, constellation, &training, &cp_tx,
		      &phase3),
	      "line fixture did not initialize law=%d constellation=%d "
	      "symbol_rate=%d rate=%u",
	      law, constellation, symbol_rate, rate_2400_bps_units);
	CHECK(v90_phase4_cp_line_rx_init(
		      &cp_rx, &phase3, constellation) ==
		      V90_PHASE4_CP_LINE_RX_OK,
	      "CP receiver did not initialize law=%d constellation=%d",
	      law, constellation);
	CHECK(v90_pcm_mapping_init_transmit(&training_mapping, &training, law) ==
		      V90_PCM_MAPPING_OK,
	      "training mapping did not initialize law=%d", law);
	CHECK(v90_upstream_rx_init(
		      &receiver, &cp_rx, &training_mapping, &mp,
		      UINT32_C(1)) ==
		      V90_UPSTREAM_RX_OK,
	      "upstream owner did not initialize law=%d constellation=%d",
	      law, constellation);

	receiver.mp_selection_pending = true;
	for (size_t clock = 0;
	     clock < SAMPLE_LIMIT && !v90_upstream_rx_is_complete(&receiver);
	     ++clock) {
		int16_t sample = 0;
		bool sample_ready = false;

		CHECK(v90_pcm_mapping_equal(&receiver.training_mapping, &training_mapping),
		      "late CPt replaced the active training mapping");

		if (!scr_complete && v90_phase4_cp_line_tx_record_is_complete(&cp_tx)) {
			enum v90_phase4_cp_line_tx_result result =
				v90_phase4_cp_line_tx_scr_sample(&cp_tx, scr_samples >= 128u, &sample);

			CHECK(result == V90_PHASE4_CP_LINE_TX_OK || result == V90_PHASE4_CP_LINE_TX_COMPLETE,
			      "SCR before DATA CP stopped");
			scr_complete = result == V90_PHASE4_CP_LINE_TX_COMPLETE;
			sample_ready = !scr_complete;
			if (sample_ready)
				scr_samples++;
		}
		if (!sample_ready && !e_started &&
		    v90_phase4_cp_line_tx_record_is_complete(&cp_tx)) {
			if (!late_training_started) {
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, &late_training) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "late CPt did not start");
				late_training_started = true;
			} else if (!unadvertised_started) {
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, &unadvertised) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "unadvertised CP did not start at clock %zu",
				      clock);
				unadvertised_started = true;
			} else if (!data_started) {
				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, &data) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "data CP did not start at clock %zu", clock);
				data_started = true;
			} else if (!prime_started) {
				const struct v90_cp_parameters *next =
					mp_prime_requested ? &prime : &data;

				CHECK(v90_phase4_cp_line_tx_start_next_record(
					      &cp_tx, next) ==
					      V90_PHASE4_CP_LINE_TX_OK,
				      "CP repetition failed at clock %zu", clock);
				prime_started = mp_prime_requested;
			} else {
				cp_finished = true;
			}
		}
		if (!sample_ready && !e_started) {
			enum v90_phase4_cp_line_tx_result result =
				v90_phase4_cp_line_tx_sample(&cp_tx, &sample);

			CHECK(result == V90_PHASE4_CP_LINE_TX_OK ||
			      (cp_finished &&
			       result == V90_PHASE4_CP_LINE_TX_COMPLETE),
			      "CP source stopped at clock %zu: %s", clock,
			      v90_phase4_cp_line_tx_result_name(result));
			if (result == V90_PHASE4_CP_LINE_TX_OK) {
				sample_ready = true;
			} else {
				CHECK(v90_phase4_e_line_tx_init(&e_tx, &cp_tx) ==
					      V90_PHASE4_E_LINE_TX_OK,
				      "CP prime did not hand off to E");
				e_started = true;
			}
		}
		if (!sample_ready && e_started && !b1_started) {
			enum v90_phase4_e_line_tx_result result =
				v90_phase4_e_line_tx_sample(&e_tx, &sample);

			CHECK(result == V90_PHASE4_E_LINE_TX_OK ||
			      result == V90_PHASE4_E_LINE_TX_COMPLETE,
			      "E source stopped at clock %zu: %s", clock,
			      v90_phase4_e_line_tx_result_name(result));
			if (result == V90_PHASE4_E_LINE_TX_OK) {
				sample_ready = true;
			} else {
				CHECK(v90_upstream_data_start_after_e(
					      &data_tx, &e_tx, &mp) ==
					      V90_UPSTREAM_DATA_OK,
				      "E did not hand off to upstream B1");
				b1_started = true;
			}
		}
		if (!sample_ready && b1_started) {
			bool accepted;

			CHECK(v34_data_stream_sample(
				      &data_tx, entry_source, &payload, &sample, &accepted) ==
				      V34_DATA_STREAM_OK,
			      "B1 source stopped at clock %zu", clock);
			(void)accepted;
			sample_ready = true;
		}
		CHECK(sample_ready, "line produced no sample at clock %zu", clock);
		/* Damage only the CP-prime waveform, not the transmitter state.
		 * Its remaining tail, E and B1 still use the continuous scrambler. */
		if (damage_prime && prime_started && !e_started &&
		    cp_tx.symbols.bit_index >= 64u &&
		    cp_tx.symbols.bit_index < 128u) {
			sample = 0;
			prime_damaged = true;
		}
		{
			struct v90_cp_parameters event;
			struct v90_cp_parameters saved;
			enum v90_upstream_rx_result result;

			memset(&event, 0xa5, sizeof(event));
			saved = event;
			result = v90_upstream_rx_sample(
				&receiver, quantize(law, sample), &event);
			CHECK(result == V90_UPSTREAM_RX_OK ||
			      result == V90_UPSTREAM_RX_REQUEST_MP_PRIME ||
			      result == V90_UPSTREAM_RX_REQUEST_ED ||
			      result == V90_UPSTREAM_RX_E_COMPLETE ||
			      result == V90_UPSTREAM_RX_B1_COMPLETE,
			      "upstream owner stopped at clock %zu: %s "
			      "symbol_rate=%d rate=%u",
			      clock, v90_upstream_rx_result_name(result),
			      symbol_rate, rate_2400_bps_units);
			if (receiver.data_cp_received && !receiver.mapping_prepared) {
				CHECK(!mp_prime_requested && !ed_requested,
				      "upstream acknowledged before mapping preparation");
				preparation_samples++;
				if (preparation_samples == 160u) {
					struct v90_upstream_rx saved = receiver;

					CHECK(receiver.mp_selection_pending &&
					      receiver.calibration.frame == 0 &&
					      v90_upstream_rx_select_mp(&receiver, NULL) ==
						      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
					      receiver_states_equal(&receiver, &saved) &&
					      v90_upstream_rx_select_mp(&receiver, &mp) ==
						      V90_UPSTREAM_RX_OK &&
					      !receiver.mp_selection_pending,
					      "MP selection did not release calibration");
					saved = receiver;
					CHECK(v90_upstream_rx_select_mp(&receiver, &mp) ==
						      V90_UPSTREAM_RX_INVALID_STATE &&
					      receiver_states_equal(&receiver, &saved),
					      "final MP selection was mutable");
					deferred_mp_selected = true;
				}
			}
			if (receiver.stage == V90_UPSTREAM_RX_STAGE_B1) {
				const struct v34_data_receiver *b1 =
					&receiver.state.data.decisions.data_receiver.data_receiver;

				b1_previous_error_bits = b1->b1_error_bits;
				b1_frame_seen = b1->b1_received_bits != 0u;
			}
			if (result == V90_UPSTREAM_RX_OK) {
				CHECK(memcmp(&event, &saved, sizeof(event)) == 0,
				      "quiet sample changed its event");
			} else if (result ==
				   V90_UPSTREAM_RX_REQUEST_MP_PRIME) {
				CHECK(!mp_prime_requested && !ed_requested &&
				      receiver.mapping_prepared && deferred_mp_selected && preparation_samples >= 320u &&
				      v90_cp_parameters_are_equal(&event, &expected_data),
				      "first data CP event changed");
				mp_prime_requested = true;
			} else if (result == V90_UPSTREAM_RX_REQUEST_ED) {
				CHECK(!damage_prime && mp_prime_requested && !ed_requested &&
				      v90_cp_parameters_are_equal(&event, &prime),
				      "CP-prime event changed or arrived early");
				ed_requested = true;
			} else if (result == V90_UPSTREAM_RX_E_COMPLETE) {
				CHECK(memcmp(&event, &saved, sizeof(event)) == 0,
				      "E changed the CP event output");
				e_received = true;
			} else {
				b1_received = true;
			}
		}
	}

	{
		enum v90_upstream_rx_result handoff_result =
			v90_upstream_rx_handoff(&receiver, &data_handoff);
		bool handoff_valid = handoff_result == V90_UPSTREAM_RX_OK &&
			v34_data_line_receiver_is_valid(&data_handoff);

		CHECK(unadvertised_started && data_started && prime_started &&
		      scr_complete && scr_samples >= 128u && cp_finished && e_started &&
		      b1_started && mp_prime_requested &&
		      (damage_prime ? prime_damaged && !ed_requested : ed_requested) &&
		      e_received && b1_received &&
		      v90_upstream_rx_is_complete(&receiver) &&
		      !v90_upstream_rx_has_failed(&receiver) && handoff_valid &&
		      !v34_data_line_receiver_is_receiving_b1(&data_handoff) &&
		      data_handoff.geometry
			      .primary_data_rate_2400_bps_units ==
			      rate_2400_bps_units &&
		      v34_line_receiver_matches_profile(
			      &data_handoff.line,
			      &data_handoff.transmitting_profile) &&
		      data_handoff.line.equalizer.feedback_mode ==
			      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
		      b1_frame_seen &&
		      data_handoff.decisions.data_receiver.data_receiver
			      .b1_error_bits == b1_previous_error_bits,
		      "upstream entry at rate %u stopped: source=%d/%d/%d/%d/%d/%d "
		      "events=%d/%d/%d/%d stage=%d failed=%d handoff=%s "
		      "valid=%d rate=%u errors=%u",
		      rate_2400_bps_units,
		      unadvertised_started, data_started, prime_started,
		      cp_finished, e_started,
		      b1_started, mp_prime_requested, ed_requested, e_received,
		      b1_received,
		      receiver.stage, v90_upstream_rx_has_failed(&receiver),
		      v90_upstream_rx_result_name(handoff_result), handoff_valid,
		      handoff_valid ?
			      data_handoff.geometry
				      .primary_data_rate_2400_bps_units : 0,
		      handoff_valid ?
			      (unsigned int)data_handoff.decisions
				      .data_receiver.data_receiver.b1_error_bits : 0);
	}
	/* B1 trains the dense data constellation. Require convergence by its
	 * last mapping frame above, then verify actual online bits below;
	 * early training errors are not failed payload or evidence of success. */
	CHECK(entry_payload_received(&data_tx, &data_handoff, law, &payload),
	      "upstream payload did not survive B1 at rate %u law %u",
	      rate_2400_bps_units, law);
	CHECK(!damage_prime ||
	      (receiver.control_symbols.descrambler.history == cp_tx.symbols.scrambler.history &&
	       receiver.control_symbols.previous_clockwise_quarter_turns ==
		       cp_tx.symbols.previous_clockwise_quarter_turns),
	      "E changed the CP history retained for renegotiation");
	{
		struct v90_upstream_rx saved_receiver = receiver;
		struct v90_cp_parameters event;
		struct v90_cp_parameters saved_event;

		memset(&event, 0xa5, sizeof(event));
		saved_event = event;
		CHECK(v90_upstream_rx_sample(&receiver, 1234, &event) ==
			      V90_UPSTREAM_RX_COMPLETE &&
		      receiver_states_equal(&receiver, &saved_receiver) &&
		      memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "completed owner consumed another sample");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_cp_parameters training =
		cp_parameters(V90_CP_TRAINING, G711_LAW_PCMU, false, 4);
	struct v90_mp_record mp = local_mp(4);
	struct v90_mp_record acknowledged = mp;
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping wrong_mapping;
	struct v90_phase3_return_line_handoff phase3;
	struct v90_phase4_cp_line_tx cp_tx;
	struct v90_phase4_cp_line_rx cp_rx;
	struct v90_upstream_rx receiver;
	struct v90_upstream_rx untouched;
	struct v90_upstream_rx saved;
	struct v34_data_line_receiver data;
	struct v34_data_line_receiver saved_data;

	acknowledged.parameters.type0.acknowledges_cp = true;
	CHECK(v90_phase4_line_fixture_init(
		      G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      &training, &cp_tx, &phase3) &&
	      v90_phase4_cp_line_rx_init(
		      &cp_rx, &phase3, V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_OK &&
	      v90_pcm_mapping_init(&mapping, &training) ==
		      V90_PCM_MAPPING_OK,
	      "contract fixture did not initialize");
	wrong_mapping = mapping;
	wrong_mapping.kind = V90_CP_DATA;
	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(v90_upstream_rx_init(
		      NULL, &cp_rx, &mapping, &mp, UINT32_C(1)) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, NULL, &mapping, &mp, UINT32_C(1)) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, NULL, &mp, UINT32_C(1)) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &mapping, NULL, UINT32_C(1)) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &wrong_mapping, &mp,
		      UINT32_C(1)) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &mapping, &acknowledged,
		      UINT32_C(1)) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &mapping, &mp, 0) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_init(
		      &untouched, &cp_rx, &mapping, &mp,
		      UINT32_C(1) << V90_JD_RATE_CAPABILITY_BITS) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(v90_upstream_rx_init(
		      &receiver, &cp_rx, &mapping, &mp, UINT32_C(1)) ==
		      V90_UPSTREAM_RX_OK,
	      "valid contract owner did not initialize");
	memset(&data, 0xa5, sizeof(data));
	saved_data = data;
	CHECK(v90_upstream_rx_handoff(NULL, &data) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_handoff(&receiver, NULL) ==
		      V90_UPSTREAM_RX_INVALID_ARGUMENT &&
	      v90_upstream_rx_handoff(&receiver, &data) ==
		      V90_UPSTREAM_RX_INVALID_STATE &&
	      memcmp(&data, &saved_data, sizeof(data)) == 0 &&
	      strcmp(v90_upstream_rx_result_name(
		     V90_UPSTREAM_RX_REQUEST_MP_PRIME),
		     "request MP prime") == 0 &&
	      strcmp(v90_upstream_rx_result_name(
		     (enum v90_upstream_rx_result)99),
		     "unknown") == 0,
	      "owner contracts or result names changed");
	/* Exhausting the equalizer epoch fails after timing and input history
	 * have advanced. Keep that consumed state and both failure latches;
	 * an owner rollback must not silently make the CP child usable again. */
	receiver.state.cp.line.equalizer.epoch = UINT64_MAX;
	CHECK(v90_upstream_rx_is_valid(&receiver),
	      "exhausted-epoch fixture is invalid");
	for (unsigned int index = 0; index < 16; ++index) {
		struct v90_cp_parameters event;
		struct v90_cp_parameters saved_event;
		enum v90_upstream_rx_result result;
		int16_t sample;

		memset(&event, 0xa5, sizeof(event));
		saved_event = event;
		saved = receiver;
		CHECK(v90_phase4_cp_line_tx_sample(&cp_tx, &sample) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "failure fixture lost its CP input");
		result = v90_upstream_rx_sample(
			&receiver, quantize(G711_LAW_PCMU, sample), &event);
		CHECK(memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "numeric receive failure published a control event");
		if (result == V90_UPSTREAM_RX_NUMERIC_FAILURE) {
			CHECK(v90_upstream_rx_has_failed(&receiver) &&
			      receiver.state.cp.failed &&
			      !receiver.state.cp.line.equalizer.decision_pending &&
			      receiver.state.cp.line.equalizer.epoch == UINT64_MAX &&
			      memcmp(&receiver.state.cp.line.timing,
				     &saved.state.cp.line.timing,
				     sizeof(receiver.state.cp.line.timing)) != 0,
			      "CP failure lost consumed history or its failure latch");
			saved = receiver;
			CHECK(v90_upstream_rx_sample(&receiver, sample, &event) ==
				      V90_UPSTREAM_RX_FAILED &&
			      receiver_states_equal(&receiver, &saved) &&
			      memcmp(&event, &saved_event, sizeof(event)) == 0,
			      "failed CP owner consumed another sample");
			return true;
		}
		CHECK(result == V90_UPSTREAM_RX_OK,
		      "exhausted CP epoch produced an unexpected result");
	}
	CHECK(false, "CP receiver did not reach its exhausted epoch");
	return false;
}

int main(void)
{
	static const struct {
		enum g711_law law;
		enum v34_constellation_size constellation;
	} paths[] = {
		{ G711_LAW_PCMU, V34_CONSTELLATION_4_POINT },
		{ G711_LAW_PCMA, V34_CONSTELLATION_16_POINT },
	};
	static const struct {
		enum v34_symbol_rate symbol_rate;
		uint8_t minimum_rate_2400_bps_units;
		uint8_t maximum_rate_2400_bps_units;
	} profiles[] = {
		{ V34_SYMBOL_RATE_3000, V90_MP_MINIMUM_RATE_2400_BPS_UNITS, 9 },
		{ V34_SYMBOL_RATE_3200, V90_MP_MINIMUM_RATE_2400_BPS_UNITS, 9 },
		{ V34_SYMBOL_RATE_3429, V90_MP_MINIMUM_RATE_2400_BPS_UNITS, 10 },
		{ V34_SYMBOL_RATE_3000, 10, 12 },
		{ V34_SYMBOL_RATE_3200, 10, 13 },
		{ V34_SYMBOL_RATE_3429, 11, 14 },
	};

	fprintf(stderr, "[ RUN      ] generated upstream entry\n");
	for (size_t path = 0; path < ARRAY_LENGTH(paths); ++path)
		for (size_t profile = 0;
		     profile < ARRAY_LENGTH(profiles); ++profile)
			for (uint8_t rate = profiles[profile]
						     .minimum_rate_2400_bps_units;
			     rate <= profiles[profile]
					     .maximum_rate_2400_bps_units;
			     ++rate)
				if (!run_line(
					    paths[path].law,
					    paths[path].constellation,
					    profiles[profile].symbol_rate,
					    rate, false, false, false, false))
					return 1;
	fprintf(stderr, "[       OK ] generated upstream entry\n");
	fprintf(stderr, "[ RUN      ] CP-prime-first upstream entry\n");
	if (!run_line(
		    G711_LAW_PCMA, V34_CONSTELLATION_16_POINT,
		    V34_SYMBOL_RATE_3429, 14, true, false, false, false))
		return 1;
	fprintf(stderr, "[       OK ] CP-prime-first upstream entry\n");
	fprintf(stderr, "[ RUN      ] differing peer codec through CP/E/B1\n");
	if (!run_line(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      V34_SYMBOL_RATE_3000, 6, false, true, false, false) ||
	    !run_line(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT,
		      V34_SYMBOL_RATE_3429, 14, true, true, false, false))
		return 1;
	fprintf(stderr, "[       OK ] differing peer codec through CP/E/B1\n");
	fprintf(stderr, "[ RUN      ] E after damaged CP prime\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int mode = 0u; mode < 2u; ++mode)
			if (!run_line(law, mode == 0u ? V34_CONSTELLATION_4_POINT :
				      V34_CONSTELLATION_16_POINT,
				      V34_SYMBOL_RATE_3000, 6, false, false, true, false))
				return 1;
	fprintf(stderr, "[       OK ] E after damaged CP prime\n");
	fprintf(stderr, "[ RUN      ] disabled CP with unusable data mapping\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int mode = 0u; mode < 2u; ++mode)
			if (!run_line(law, mode == 0u ? V34_CONSTELLATION_4_POINT :
				      V34_CONSTELLATION_16_POINT,
				      V34_SYMBOL_RATE_3000, 6, false, false, false, true))
				return 1;
	fprintf(stderr, "[       OK ] disabled CP with unusable data mapping\n");
	fprintf(stderr, "[ RUN      ] zero-rate CP/CPt cleardown\n");
	for (size_t path = 0; path < ARRAY_LENGTH(paths); ++path)
		if (!check_cleardown(paths[path].law, paths[path].constellation, V90_CP_DATA) ||
		    !check_cleardown(paths[path].law, paths[path].constellation, V90_CP_TRAINING))
			return 1;
	fprintf(stderr, "[       OK ] zero-rate CP/CPt cleardown\n");
	fprintf(stderr, "[ RUN      ] CPs control bypasses data admission\n");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned int final_case = 0u; final_case < 3u; ++final_case)
			if (!check_cps_requests(law, final_case))
				return 1;
	fprintf(stderr, "[       OK ] CPs control bypasses data admission\n");
	fprintf(stderr, "[ RUN      ] contracts\n");
	if (!test_contracts())
		return 1;
	fprintf(stderr, "[       OK ] contracts\n");
	return 0;
}
