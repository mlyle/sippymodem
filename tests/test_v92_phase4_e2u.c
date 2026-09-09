// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_downstream_rx.h>
#include <pcm_downstream_tx.h>
#include <qam_equalizer.h>
#include <v90_pcm_mapping.h>
#include <v92_control_rx.h>
#include <v92_cpd_direct.h>
#include <v92_phase4_e2u.h>
#include <v92_suvu.h>
#include <v92_upstream_data_tx.h>
#include <v92_upstream_data_rx.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 6000,
	DOWNSTREAM_PREPARE_LIMIT = 10000,
};

enum upstream_tail {
	NO_TAIL,
	SUVU_TAIL,
	CPU_TAIL,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		g711_decode_pcma(g711_encode_pcma(sample)) :
		g711_decode_pcmu(g711_encode_pcmu(sample));
}

static struct v92_cpu_parameters cpu_parameters(enum g711_law law)
{
	struct v92_cpu_parameters cpu = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x4200),
		.shaping_a1_q1_6 = 7,
		.shaping_a2_q1_6 = -5,
		.shaping_b1_q1_6 = 3,
		.shaping_b2_q1_6 = -2,
		.data_rate_number = 1,
		.sign_redundancy = 3,
		.spectral_lookahead_frames = 2,
		.constellation_count = 2,
		.acknowledges_cpd = true,
	};

	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval)
		cpu.frame_constellation[interval] = (uint8_t)(interval & 1u);
	for (uint8_t ordinal = 0u; ordinal < 8u; ++ordinal) {
		add_ucode(&cpu.transmit_constellations[0],
			  (uint8_t)(16u + ordinal));
		add_ucode(&cpu.transmit_constellations[1],
			  (uint8_t)(64u + ordinal));
	}
	return cpu;
}

static bool training_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t lookahead)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.shaping_a1_q1_6 = 5,
		.shaping_a2_q1_6 = -3,
		.shaping_b1_q1_6 = 2,
		.shaping_b2_q1_6 = -1,
		.selected_rate_index = 4,
		.sign_redundancy = lookahead == 0u ? 0u : 1u,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
	};

	for (uint8_t ordinal = 0u; ordinal < 4u; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static bool prepare_downstream(
	enum g711_law law, uint8_t lookahead,
	const struct v92_cpd_base_parameters *cpd,
	struct pcm_downstream_tx *transmit,
	struct pcm_downstream_rx *receive, bool finish_ed,
	bool stop_at_receive_boundary)
{
	const struct v92_suvd_parameters initial_suvd = { 0 };
	const struct v92_suvd_parameters acknowledged_suvd = {
		.acknowledges_cpu = true,
	};
	struct v90_pcm_mapping mapping;
	bool cpd_pending = false;
	bool suvd_pending = false;
	bool acknowledged = false;
	bool ed_requested = false;

	if (!training_mapping(&mapping, law, lookahead) ||
	    pcm_downstream_tx_init(
		    transmit, PCM_PROTOCOL_V92, &mapping) !=
		    PCM_DOWNSTREAM_TX_OK ||
	    pcm_downstream_tx_start_suvd(transmit, &initial_suvd) !=
		    PCM_DOWNSTREAM_TX_OK ||
	    pcm_downstream_rx_init(
		    receive, PCM_PROTOCOL_V92, &mapping,
		    V90_PCM_MAPPING_TRANSMIT) != PCM_DOWNSTREAM_RX_OK)
		return false;
	for (size_t sample = 0u; sample < DOWNSTREAM_PREPARE_LIMIT; ++sample) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;
		enum pcm_downstream_rx_result receive_result;

		if (cpd_pending &&
		    pcm_downstream_tx_is_v92_control_boundary(transmit)) {
			if (pcm_downstream_tx_start_cpd(transmit, cpd) !=
			    PCM_DOWNSTREAM_TX_OK)
				return false;
			cpd_pending = false;
		}
		if (suvd_pending &&
		    pcm_downstream_tx_is_v92_control_boundary(transmit)) {
			if (pcm_downstream_tx_start_suvd(
				    transmit, &acknowledged_suvd) !=
			    PCM_DOWNSTREAM_TX_OK)
				return false;
			suvd_pending = false;
		}
		if (!finish_ed && acknowledged &&
		    pcm_downstream_tx_is_v92_control_boundary(transmit))
			return true;
		enum pcm_downstream_tx_result transmit_result =
			pcm_downstream_tx_next(transmit, &symbol);

		if (transmit_result == PCM_DOWNSTREAM_TX_COMPLETE)
			return finish_ed && ed_requested &&
			       pcm_downstream_rx_is_complete(receive);
		if (transmit_result != PCM_DOWNSTREAM_TX_OK)
			return false;
		receive_result = pcm_downstream_rx_receive(
			receive, symbol.pcm.octet, &event);
		if (receive_result != PCM_DOWNSTREAM_RX_BUFFERING &&
		    receive_result != PCM_DOWNSTREAM_RX_TRN2D_FRAME &&
		    receive_result != PCM_DOWNSTREAM_RX_SUVD &&
		    receive_result != PCM_DOWNSTREAM_RX_CPD &&
		    receive_result != PCM_DOWNSTREAM_RX_ED)
			return false;
		if (receive_result == PCM_DOWNSTREAM_RX_CPD) {
			if (event.control.type != V92_CONTROL_EVENT_CPD ||
			    event.control.record.cpd.frame_result != V92_CPD_OK)
				return false;
			suvd_pending = true;
		} else if (receive_result == PCM_DOWNSTREAM_RX_SUVD) {
			if (event.control.type != V92_CONTROL_EVENT_SUVD ||
			    event.control.record.suvd.frame_result != V92_SUVD_OK)
				return false;
			if (event.control.record.suvd.parameters.acknowledges_cpu)
				acknowledged = true;
			else if (!cpd_pending && !suvd_pending)
				cpd_pending = true;
		}
		if (!finish_ed && stop_at_receive_boundary && acknowledged)
			return true;
		if (finish_ed && acknowledged && !ed_requested) {
			if (pcm_downstream_rx_expect_ed(receive) !=
				    PCM_DOWNSTREAM_RX_OK ||
			    pcm_downstream_tx_request_ed(transmit) !=
				    PCM_DOWNSTREAM_TX_OK)
				return false;
			ed_requested = true;
		}
	}
	return false;
}

static bool start_suvu_record(
	struct v92_phase4_record_tx *record,
	const struct v92_phase4_modulator *transmit, bool first);

static bool upstream_line(
	enum g711_law law, struct v92_phase4_record_tx *transmit,
	struct v92_upstream_control_line_rx *receive)
{
	struct v92_phase4_modulator modulator;
	struct qam_point reference = { .in_phase = TEST_LU };
	struct qam_point output;
	struct v92_pcm_phase4_trn2u_line_handoff handoff = {
		.input_samples = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
		.training_symbols = V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS,
		.law = law,
		.polarity = 1,
	};
	int16_t target;
	bool ready = false;

	if (v92_phase4_modulator_init(
		    &modulator, V92_CONSTELLATION_4_POINT, 0u, TEST_LU) !=
		    V92_PHASE4_MODULATOR_OK ||
	    v92_phase4_demodulator_init(
		    &handoff.demodulator, V92_CONSTELLATION_4_POINT,
		    0u, TEST_LU) != V92_PHASE4_MODULATOR_OK ||
	    qam_equalizer_init_geometry(
		    &handoff.equalizer, &reference, &reference,
		    V92_PCM_PHASE4_TRN2U_LINE_EQUALIZER_TAPS,
		    V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY) !=
		    QAM_EQUALIZER_OK)
		return false;
	/* Keep the required causal geometry while modelling an identity line. */
	handoff.equalizer.coefficients[
		V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY] =
		(struct qam_point){ 0 };
	handoff.equalizer.coefficients[0] = (struct qam_point){
		.in_phase = 1.0,
	};
	for (size_t index = 0u;
	     index <= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index) {
		if (qam_equalizer_push(
			    &handoff.equalizer, &reference,
			    &output, &ready) != QAM_EQUALIZER_OK)
			return false;
	}
	if (!ready || qam_equalizer_adapt(
			      &handoff.equalizer, &output, 0.0) !=
			      QAM_EQUALIZER_OK ||
	    v92_phase4_modulator_level(
		    V92_CONSTELLATION_4_POINT, TEST_LU, 0u, 0u,
		    &target) != V92_PHASE4_MODULATOR_OK)
		return false;
	target = quantize(law, target);
	for (size_t index = 0u;
	     index < V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY; ++index)
		handoff.pending_training_targets[index] = target;
	if (!v92_pcm_phase4_trn2u_line_handoff_is_valid(&handoff) ||
	    v92_upstream_control_line_rx_init(receive, &handoff) !=
		    V92_UPSTREAM_CONTROL_LINE_RX_OK)
		return false;
	receive->signal.training_tail_remaining = 0u;
	/* A real completed SUVu supplies both sides of the control boundary. */
	if (!start_suvu_record(transmit, &modulator, true))
		return false;
	while (!v92_phase4_record_tx_is_complete(transmit)) {
		struct v92_control_event event;
		int16_t sample;
		enum v92_upstream_control_line_rx_result result;

		if (v92_phase4_record_tx_next(transmit, &sample) !=
		    V92_PHASE4_RECORD_TX_OK)
			return false;
		result = v92_upstream_control_line_rx_sample(
			receive, quantize(law, sample), &event);
		if (result != V92_UPSTREAM_CONTROL_LINE_RX_OK &&
		    (result != V92_UPSTREAM_CONTROL_LINE_RX_EVENT ||
		     event.type != V92_CONTROL_EVENT_SUVU))
			return false;
	}
	return v92_upstream_control_line_rx_is_valid(receive);
}

struct digital_fixture {
	struct v92_phase4_e2u owner;
	struct v92_phase4_modulator transmit;
	struct v92_cpu_parameters cpu;
};

static bool prepare_digital_fixture(
	enum g711_law law, bool extended, struct digital_fixture *fixture)
{
	struct v92_cpd_parameters full_cpd;
	struct v92_cpd_base_parameters cpd;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct v92_upstream_control_line_rx upstream_rx;
	struct v92_phase4_digital_handoff handoff;
	struct v92_phase4_record_tx control;

	fixture->cpu = cpu_parameters(law);
	if (v92_cpd_direct_init(
		    &full_cpd, law, 12u, extended) != V92_CPD_OK)
		return false;
	cpd = (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = full_cpd.four_times_gain_q0_16,
		.data_rate_number = full_cpd.data_rate_number,
		.trellis = full_cpd.trellis,
		.extend_e2u = full_cpd.extend_e2u,
	};
	if (!prepare_downstream(
		    law, 3u, &cpd, &downstream_tx, &downstream_rx,
		    false, false) ||
	    !upstream_line(law, &control, &upstream_rx))
		return false;
	fixture->transmit = control.modulator;
	handoff = (struct v92_phase4_digital_handoff){
		.receive = upstream_rx,
		.cpu = fixture->cpu,
		.upstream_plan = { .gain = 1.25, .delay = 60u, .observation = V92_PCM_FRAME_CONSTELLATION },
	};
	if (!v92_phase4_ed_tx_init(&handoff.transmit, &downstream_tx, NULL))
		return false;
	if (v92_pcm_mapping_init(&handoff.mapping, law, &full_cpd) !=
	    V92_PCM_MAPPING_OK)
		return false;
	return v92_phase4_e2u_init_digital(&fixture->owner, &handoff) ==
		       V92_PHASE4_E2U_OK;
}

static bool test_fresh_analog_ed_arming(enum g711_law law)
{
	struct v92_cpd_parameters full_cpd;
	struct v92_cpd_base_parameters cpd;
	struct v92_pcm_mapping mapping;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct v92_phase4_record_tx upstream_tx;
	struct v92_upstream_control_line_rx unused_rx;
	struct v92_phase4_analog_handoff handoff;
	struct v92_phase4_e2u owner;

	CHECK(v92_cpd_direct_init(
		      &full_cpd, law, 12u, false) == V92_CPD_OK &&
	      v92_pcm_mapping_init(&mapping, law, &full_cpd) ==
		      V92_PCM_MAPPING_OK,
	      "law %u fresh-boundary mapping did not initialize",
	      (unsigned int)law);
	cpd = (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = full_cpd.four_times_gain_q0_16,
		.data_rate_number = full_cpd.data_rate_number,
		.trellis = full_cpd.trellis,
		.extend_e2u = full_cpd.extend_e2u,
	};
	CHECK(prepare_downstream(
		      law, 3u, &cpd, &downstream_tx, &downstream_rx,
		      false, true) &&
	      upstream_line(law, &upstream_tx, &unused_rx),
	      "law %u fresh control boundary did not prepare",
	      (unsigned int)law);
	handoff = (struct v92_phase4_analog_handoff){
		.transmit = upstream_tx,
		.receive = downstream_rx,
		.cpu = cpu_parameters(law),
		.mapping = mapping,
	};
	CHECK(v92_phase4_analog_handoff_is_valid(&handoff) &&
	      v92_phase4_e2u_init_analog(&owner, &handoff) ==
		      V92_PHASE4_E2U_OK &&
	      owner.state.analog.downstream.training.ed_requested &&
	      owner.state.analog.downstream.training.stage ==
		      PCM_DOWNSTREAM_RX_ED_SEARCH,
	      "law %u fresh control boundary did not arm Ed",
	      (unsigned int)law);
	return true;
}

static bool start_suvu_record(
	struct v92_phase4_record_tx *record,
	const struct v92_phase4_modulator *transmit, bool first)
{
	const struct v92_suvu_parameters parameters = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
		.acknowledges_cpd = true,
	};
	uint8_t frame[V92_SUVU_FRAME_BYTES];
	size_t frame_bits = 0u;

	if (v92_suvu_encode(
		    &parameters, v92_phase4_modulator_bits_per_symbol(
			    transmit->constellation) * V92_SUVU_SYMBOLS_PER_FRAME,
		    frame, sizeof(frame),
		    &frame_bits) != V92_SUVU_OK)
		return false;
	return first ?
		v92_phase4_record_tx_init(
			record, transmit, frame, sizeof(frame), frame_bits) ==
				V92_PHASE4_RECORD_TX_OK :
		v92_phase4_record_tx_start_next(
			record, frame, sizeof(frame), frame_bits) ==
				V92_PHASE4_RECORD_TX_OK;
}

static bool start_cpu_record(
	struct v92_phase4_record_tx *record,
	const struct v92_phase4_modulator *transmit,
	const struct v92_cpu_parameters *parameters, bool first)
{
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		transmit->constellation);

	if (v92_cpu_encode(
		    parameters, bits * V92_CPU_SYMBOLS_PER_FRAME,
		    frame, sizeof(frame), &frame_bits) !=
	    V92_CPU_OK)
		return false;
	return first ?
		v92_phase4_record_tx_init(
			record, transmit, frame, sizeof(frame), frame_bits) ==
				V92_PHASE4_RECORD_TX_OK :
		v92_phase4_record_tx_start_next(
			record, frame, sizeof(frame), frame_bits) ==
				V92_PHASE4_RECORD_TX_OK;
}

static bool feed_control_record(
	struct v92_phase4_e2u *owner,
	struct v92_phase4_record_tx *record)
{
	while (!v92_phase4_record_tx_is_complete(record)) {
		int16_t raw;
		uint8_t octet;

		if (v92_phase4_record_tx_next(record, &raw) !=
			    V92_PHASE4_RECORD_TX_OK ||
		    v92_phase4_e2u_next(
			    owner, 0u,
			    quantize(owner->state.digital.cpu.law, raw),
			    &octet) !=
			    V92_PHASE4_E2U_OK)
			return false;
	}
	return !v92_phase4_e2u_is_complete(owner);
}

static bool run_rearmed_crossing(
	enum g711_law law, bool extended, unsigned int extra_records)
{
	struct digital_fixture fixture;
	struct v92_phase4_record_tx control;
	struct v92_phase4_record_tx e2u;
	struct v92_phase4_modulator modulator;
	size_t symbols = extended ? V92_PHASE4_E2U_EXTENDED_SYMBOLS :
				    V92_PHASE4_E2U_SYMBOLS;

	CHECK(extra_records != 0u &&
	      prepare_digital_fixture(law, extended, &fixture),
	      "law %u extra %u digital fixture did not initialize",
	      (unsigned int)law, extra_records);
	struct v92_upstream_plan plan = fixture.owner.state.digital.upstream_plan;

	CHECK(plan.gain == 1.25 && plan.delay == 60u && plan.observation == V92_PCM_FRAME_CONSTELLATION,
	      "E2u initialization lost selected receive coordinates");
	for (unsigned int record_index = 0u;
	     record_index < extra_records; ++record_index) {
		bool cpu_record = extra_records > 1u && record_index == 1u;
		bool started = cpu_record ?
			start_cpu_record(
				&control, &fixture.transmit, &fixture.cpu,
				record_index == 0u) :
			start_suvu_record(
				&control, &fixture.transmit,
				record_index == 0u);

		CHECK(started && feed_control_record(&fixture.owner, &control),
		      "law %u extra %u control record %u stopped",
		      (unsigned int)law, extra_records, record_index);
		CHECK(v92_phase4_record_tx_handoff(&control, &fixture.transmit) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "law %u extra %u control handoff stopped",
		      (unsigned int)law, extra_records);
	}
	CHECK(v92_phase4_record_tx_handoff(&control, &modulator) ==
		      V92_PHASE4_RECORD_TX_OK &&
	      v92_phase4_record_tx_init_e2u(
		      &e2u, &modulator, extended) == V92_PHASE4_RECORD_TX_OK,
	      "law %u extra %u E2u did not initialize",
	      (unsigned int)law, extra_records);
	for (size_t symbol = 0u; symbol < symbols; ++symbol) {
		int16_t raw;
		uint8_t octet;
		enum v92_phase4_e2u_result result;

		CHECK(v92_phase4_record_tx_next(&e2u, &raw) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "law %u extra %u E2u generator stopped",
		      (unsigned int)law, extra_records);
		result = v92_phase4_e2u_next(
			&fixture.owner, 0u, quantize(law, raw), &octet);
		CHECK(result == (symbol + 1u == symbols ?
				 V92_PHASE4_E2U_COMPLETE : V92_PHASE4_E2U_OK),
		      "law %u extra %u E2u stopped at %zu: %s",
		      (unsigned int)law, extra_records, symbol,
		      v92_phase4_e2u_result_name(result));
	}
	struct v92_phase4_e2u_handoff handoff;

	CHECK(v92_phase4_e2u_handoff(&fixture.owner, &handoff) == V92_PHASE4_E2U_OK &&
	      memcmp(&handoff.state.digital.upstream_plan, &plan, sizeof(plan)) == 0,
	      "repeated controls or E2u changed receive plan");
	handoff.state.digital.upstream_plan.delay = 0u;
	CHECK(!v92_phase4_e2u_handoff_is_valid(&handoff), "invalid plan crossed E2u handoff");
	return true;
}

static bool run_changed_cpu_rejection(enum g711_law law)
{
	struct digital_fixture fixture;
	struct v92_phase4_record_tx record;
	struct v92_cpu_parameters changed;
	bool rejected = false;

	CHECK(prepare_digital_fixture(law, false, &fixture),
	      "law %u changed-CPu fixture did not initialize",
	      (unsigned int)law);
	changed = fixture.cpu;
	changed.shaping_a1_q1_6++;
	CHECK(start_cpu_record(
		      &record, &fixture.transmit, &changed, true),
	      "law %u changed CPu did not start", (unsigned int)law);
	while (!v92_phase4_record_tx_is_complete(&record)) {
		struct v92_phase4_e2u before = fixture.owner;
		int16_t raw;
		uint8_t octet = 0xa5;
		enum v92_phase4_e2u_result result;

		CHECK(v92_phase4_record_tx_next(&record, &raw) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "law %u changed CPu generator stopped",
		      (unsigned int)law);
		result = v92_phase4_e2u_next(
			&fixture.owner, 0u, quantize(law, raw), &octet);
		if (result == V92_PHASE4_E2U_FAILED) {
			rejected = true;
			CHECK(octet == 0xa5 && fixture.owner.failed &&
			      memcmp(&fixture.owner.state, &before.state,
			             sizeof(before.state)) == 0,
			      "law %u changed CPu failure was not transactional",
			      (unsigned int)law);
			break;
		}
		CHECK(result == V92_PHASE4_E2U_OK,
		      "law %u changed CPu stopped early: %s",
		      (unsigned int)law, v92_phase4_e2u_result_name(result));
	}
	CHECK(rejected,
	      "law %u changed CPu selection was accepted",
	      (unsigned int)law);
	return true;
}

static bool run_crossing(
	enum g711_law law, bool extended, bool ed_complete, bool pending_cpd,
	enum upstream_tail upstream_tail)
{
	struct v92_cpd_parameters full_cpd;
	struct v92_cpd_base_parameters cpd;
	struct v92_cpu_parameters cpu = cpu_parameters(law);
	struct v92_pcm_mapping upstream_mapping;
	struct pcm_downstream_tx downstream_tx;
	struct pcm_downstream_rx downstream_rx;
	struct pcm_downstream_tx completed_tx;
	struct pcm_downstream_rx completed_rx;
	struct v92_phase4_record_tx upstream_tx;
	struct v92_upstream_control_line_rx upstream_rx;
	struct v92_phase4_analog_handoff analog_handoff;
	struct v92_phase4_digital_handoff digital_handoff;
	struct v92_phase4_record_tx reference;
	struct v92_phase4_e2u analog;
	struct v92_phase4_e2u digital;
	struct v92_phase4_e2u_handoff analog_done;
	struct v92_phase4_e2u_handoff digital_done;
	struct v92_phase4_e2u saved;
	struct v92_phase4_cpd_tx cpd_tail;
	uint8_t lookahead = ed_complete ? 0u : 3u;
	bool mid_control_record = extended && !ed_complete && !pending_cpd;
	size_t symbols = extended ? V92_PHASE4_E2U_EXTENDED_SYMBOLS :
				    V92_PHASE4_E2U_SYMBOLS;
	size_t tail_symbols = 0u;

	CHECK(v92_cpd_direct_init(
		      &full_cpd, law, 12u, extended) == V92_CPD_OK,
	      "crossing CPd did not initialize");
	full_cpd.acknowledges_cpu = pending_cpd;
	/* A non-default selected filter must survive E2u without regeneration. */
	if (extended)
		full_cpd.filter[V92_CPD_PRECODER_FEED_FORWARD] =
			(struct v92_cpd_filter){ .count = 1u, .coefficient = { 4096 } };
	CHECK(v92_pcm_mapping_init(
		      &upstream_mapping, law, &full_cpd) ==
		      V92_PCM_MAPPING_OK,
	      "law %u extended %u CPd mapping did not initialize",
	      (unsigned int)law, extended);
	cpd = (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = full_cpd.four_times_gain_q0_16,
		.data_rate_number = full_cpd.data_rate_number,
		.trellis = full_cpd.trellis,
		.extend_e2u = full_cpd.extend_e2u,
	};
	CHECK(prepare_downstream(
		      law, lookahead, &cpd,
		      &downstream_tx, &downstream_rx, false, false) &&
	      upstream_line(law, &upstream_tx, &upstream_rx),
	      "law %u extended %u lookahead %u crossing did not prepare",
	      (unsigned int)law, extended, lookahead);
	if (mid_control_record) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;

		CHECK(pcm_downstream_tx_next(&downstream_tx, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK &&
		      pcm_downstream_rx_receive(
			      &downstream_rx, symbol.pcm.octet, &event) ==
			      PCM_DOWNSTREAM_RX_BUFFERING,
		      "law %u mid-record handoff did not advance",
		      (unsigned int)law);
	}
	if (pending_cpd) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;

		CHECK(v92_phase4_cpd_tx_init(&cpd_tail, &downstream_tx, &full_cpd),
		      "CPd tail did not initialize");
		/* Hand off after CPd has begun, with its coder queue still live. */
		for (unsigned int index = 0u; index < 12u; ++index)
			CHECK(v92_phase4_cpd_tx_next(&cpd_tail, &downstream_tx, &symbol) &&
			      pcm_downstream_rx_receive(&downstream_rx, symbol.pcm.octet, &event) ==
				      PCM_DOWNSTREAM_RX_BUFFERING,
			      "CPd tail did not start cleanly");
		CHECK(cpd_tail.bit_index && !cpd_tail.complete,
		      "CPd tail fixture did not leave a partial record");
	}
	if (ed_complete)
		CHECK(prepare_downstream(
			      law, lookahead, &cpd,
			      &completed_tx, &completed_rx, true, false),
		      "law %u completed Ed fixture did not prepare",
		      (unsigned int)law);
	if (upstream_tail != NO_TAIL) {
		CHECK(upstream_tail == SUVU_TAIL ?
		      start_suvu_record(&upstream_tx, &upstream_tx.modulator, false) :
		      start_cpu_record(&upstream_tx, &upstream_tx.modulator, &cpu, false),
		      "upstream tail did not initialize");
		for (size_t index = 0u; index < 6u; ++index) {
			struct v92_control_event event;
			int16_t sample;

			CHECK(v92_phase4_record_tx_next(&upstream_tx, &sample) ==
				      V92_PHASE4_RECORD_TX_OK &&
			      v92_upstream_control_line_rx_sample(&upstream_rx,
				      quantize(law, sample), &event) ==
				      V92_UPSTREAM_CONTROL_LINE_RX_OK,
			      "upstream tail prefix did not decode");
		}
		tail_symbols = (upstream_tx.frame_bit_count - upstream_tx.bit_index) /
			v92_phase4_modulator_bits_per_symbol(upstream_tx.modulator.constellation);
	}
	analog_handoff = (struct v92_phase4_analog_handoff){
		.transmit = upstream_tx,
		.receive = ed_complete ? completed_rx : downstream_rx,
		.cpu = cpu,
		.mapping = upstream_mapping,
	};
	digital_handoff = (struct v92_phase4_digital_handoff){
		.receive = upstream_rx,
		.cpu = cpu,
		.mapping = upstream_mapping,
	};
	CHECK(v92_phase4_ed_tx_init(&digital_handoff.transmit, &downstream_tx,
		      pending_cpd ? &cpd_tail : NULL),
	      "crossing Ed tail did not initialize");
	CHECK(v92_phase4_analog_handoff_is_valid(&analog_handoff),
	      "law %u extended %u analogue handoff was invalid",
	      (unsigned int)law, extended);
	CHECK(v92_phase4_digital_handoff_is_valid(&digital_handoff),
	      "law %u extended %u digital handoff was invalid",
	      (unsigned int)law, extended);
	CHECK(v92_phase4_e2u_init_analog(&analog, &analog_handoff) ==
		      V92_PHASE4_E2U_OK,
	      "law %u extended %u analogue owner did not initialize",
	      (unsigned int)law, extended);
	CHECK(v92_phase4_e2u_init_digital(&digital, &digital_handoff) ==
		      V92_PHASE4_E2U_OK,
	      "law %u extended %u digital owner did not initialize",
	      (unsigned int)law, extended);
	reference = upstream_tx;
	CHECK(tail_symbols != 0u || v92_phase4_record_tx_init_e2u(
		      &reference, &upstream_tx.modulator, extended) == V92_PHASE4_RECORD_TX_OK,
	      "law %u extended %u reference did not initialize",
	      (unsigned int)law, extended);
	if (mid_control_record)
		CHECK(!analog.state.analog.downstream.training.ed_requested,
		      "law %u mid-record control handoff armed Ed",
		      (unsigned int)law);
	if (ed_complete) {
		digital.state.digital.downstream.training.transmit = completed_tx;
		CHECK(v92_phase4_e2u_is_valid(&analog) &&
		      v92_phase4_e2u_is_valid(&digital),
		      "law %u completed Ed invalidated owners",
		      (unsigned int)law);
	}
	saved = analog;
	CHECK(v92_phase4_e2u_next(
		      &analog, 0u, 0, (uint8_t *)&analog) ==
		      V92_PHASE4_E2U_INVALID_ARGUMENT &&
	      memcmp(&analog, &saved, sizeof(analog)) == 0,
	      "law %u overlapping output changed the owner",
	      (unsigned int)law);
	for (size_t index = 0u; index < tail_symbols + symbols; ++index) {
		int16_t raw;
		uint8_t downstream_octet = 0xa5;
		uint8_t upstream_octet = 0xa5;
		enum v92_phase4_e2u_result digital_result;
		enum v92_phase4_e2u_result analog_result;

		CHECK(v92_phase4_record_tx_next(&reference, &raw) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "reference E2u stopped at %zu", index);
		digital_result = v92_phase4_e2u_next(
			&digital, 0u, quantize(law, raw), &downstream_octet);
		analog_result = v92_phase4_e2u_next(
			&analog, downstream_octet, 0, &upstream_octet);
		CHECK(digital_result ==
			      (index + 1u == tail_symbols + symbols ?
				       V92_PHASE4_E2U_COMPLETE :
				       V92_PHASE4_E2U_OK) &&
		      analog_result == digital_result &&
		      upstream_octet ==
			      (law == G711_LAW_PCMA ?
				       g711_encode_pcma(raw) :
				       g711_encode_pcmu(raw)),
		      "law %u extended %u lookahead %u crossing stopped at %zu: %s/%s",
		      (unsigned int)law, extended, lookahead, index,
		      v92_phase4_e2u_result_name(analog_result),
		      v92_phase4_e2u_result_name(digital_result));
		if (tail_symbols != 0u && index + 1u == tail_symbols) {
			struct v92_phase4_modulator modulator = reference.modulator;

			CHECK(!v92_phase4_e2u_is_complete(&analog) &&
			      !analog.state.analog.finishing_control &&
			      v92_phase4_record_tx_init_e2u(&reference, &modulator, extended) ==
				      V92_PHASE4_RECORD_TX_OK,
			      "finishing control truncated or consumed E2u");
		}
	}
	CHECK(v92_phase4_e2u_is_complete(&analog) &&
	      v92_phase4_e2u_is_complete(&digital) &&
	      v92_phase4_e2u_handoff(&analog, &analog_done) ==
		      V92_PHASE4_E2U_OK &&
	      v92_phase4_e2u_handoff(&digital, &digital_done) ==
		      V92_PHASE4_E2U_OK &&
	      v92_phase4_e2u_handoff_is_valid(&analog_done) &&
	      v92_phase4_e2u_handoff_is_valid(&digital_done) &&
	      v92_pcm_mapping_equal(&digital_done.state.digital.mapping,
				    &analog_done.state.analog.mapping) &&
	      v92_pcm_mapping_equal(&digital_done.state.digital.mapping,
				    &upstream_mapping),
	      "law %u extended %u lookahead %u handoff did not match",
	      (unsigned int)law, extended, lookahead);
	struct v92_phase4_e2u_handoff rejected = digital_done;

	rejected.state.digital.mapping.law ^= 1u;
	CHECK(!v92_phase4_e2u_handoff_is_valid(&rejected),
	      "E2u exported a mapping for a different PCM law");
	/* B1u begins on the very next media interval, using the accepted CPd
	 * and trained LU but fresh data-coding histories for either E2u length. */
	struct v92_upstream_data_tx upstream;
	struct v92_upstream_data_rx upstream_receive;
	struct v92_upstream_data_frame received_frame;
	struct v92_pcm_frame_tx encoder;
	struct v92_pcm_frame b1u;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	const struct v92_phase4_e2u_analog_handoff *handoff =
		&analog_done.state.analog;

	memset(ones, 1, handoff->mapping.data_bits_per_frame);
	CHECK(v92_upstream_data_tx_init(
		      &upstream, &handoff->mapping, handoff->transmit.lu) ==
		      V92_UPSTREAM_DATA_TX_OK &&
	      v92_upstream_data_rx_init(&upstream_receive,
		      &digital_done.state.digital.receive,
		      &digital_done.state.digital.mapping,
		      &digital_done.state.digital.upstream_plan) ==
		      V92_UPSTREAM_DATA_RX_OK &&
	      v92_pcm_frame_tx_init(&encoder, &handoff->mapping) ==
		      V92_PCM_FRAME_OK &&
	      v92_pcm_frame_encode(
		      &encoder, ones, handoff->mapping.data_bits_per_frame,
		      &b1u) == V92_PCM_FRAME_OK, "B1u handoff failed");
	for (size_t symbol = 0u; symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t actual;
		int16_t expected;

		CHECK(v92_upstream_data_tx_next(&upstream, NULL, NULL, &actual) ==
			      V92_UPSTREAM_DATA_TX_OK &&
		      v92_pcm_frame_scale(
			      &handoff->mapping, handoff->transmit.lu,
			      b1u.v[symbol], &expected) == V92_PCM_FRAME_OK &&
		      actual == expected,
		      "law %u extended %u first B1u frame changed at %zu",
		      (unsigned int)law, extended, symbol);
		CHECK(v92_upstream_data_rx_sample(&upstream_receive,
			      quantize(law, actual), &received_frame) ==
			      (symbol + 1u == V92_PCM_MAPPING_FRAME_SYMBOLS ?
				       V92_UPSTREAM_DATA_RX_FRAME : V92_UPSTREAM_DATA_RX_OK),
		      "first duplex B1u frame did not decode");
	}
	CHECK(received_frame.squared_error == 0u && received_frame.bit_count == 0u &&
	      upstream_receive.line.received_frames == 1u,
	      "E2u to B1u changed frame alignment or emitted payload early");
	if (ed_complete) {
		CHECK(digital_done.state.digital.downstream_stage ==
			      V92_PHASE4_E2U_DOWNSTREAM_DATA &&
		      digital_done.state.digital.downstream.data
			      .transmitted_symbols == tail_symbols + symbols &&
		      analog_done.state.analog.downstream_stage ==
			      V92_PHASE4_E2U_DOWNSTREAM_DATA &&
		      analog_done.state.analog.downstream.data.stream.received_symbols ==
			      tail_symbols + symbols,
		      "early Ed stages %u/%u preserved %zu B1d intervals",
		      (unsigned int)analog_done.state.analog.downstream_stage,
		      (unsigned int)digital_done.state.digital.downstream_stage,
		      digital_done.state.digital.downstream.data
			      .transmitted_symbols);
	} else {
		CHECK(analog_done.state.analog.downstream_stage ==
			      V92_PHASE4_E2U_DOWNSTREAM_TRAINING,
		      "delayed Ed did not remain in progress");
	}
	if (pending_cpd) {
		struct v92_phase4_e2u_digital_handoff *digital_tail = &digital_done.state.digital;
		struct v92_phase4_e2u_analog_handoff *analog_tail = &analog_done.state.analog;
		struct v92_phase4_ed_tx *tail = &digital_tail->downstream.training;
		uint64_t cpd_end_frame = tail->transmit.accepted_frames +
			(tail->cpd.sequence_bits - tail->cpd.bit_index) / tail->cpd.frame_bits;

		CHECK(digital_tail->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_TRAINING &&
		      tail->cpd_pending && !tail->transmit.ed_requested,
		      "E2u completion discarded the pending CPd");
		for (size_t index = 0u; index < DOWNSTREAM_PREPARE_LIMIT &&
		     analog_tail->downstream_stage != V92_PHASE4_E2U_DOWNSTREAM_DATA; ++index) {
			struct pcm_downstream_symbol symbol;
			struct v90_pcm_stream_frame frame;
			bool finishing_cpd = tail->cpd_pending;

			CHECK(v92_phase4_ed_tx_next(tail, &symbol) == PCM_DOWNSTREAM_TX_OK,
			      "post-E2u CPd/Ed tail stopped");
			if (finishing_cpd && !tail->cpd_pending)
				CHECK(tail->transmit.stage == PCM_DOWNSTREAM_TX_ED &&
				      tail->transmit.accepted_frames == cpd_end_frame,
				      "CPd was truncated or an extra SUVd preceded Ed");
			CHECK(v92_downstream_receive_owned(&analog_tail->downstream,
				      &analog_tail->downstream_stage, &analog_tail->cpu,
				      &analog_tail->mapping, symbol.pcm.octet, &frame) ==
				      V90_DOWNSTREAM_DATA_RX_BUFFERING,
			      "peer could not decode the complete CPd and Ed tail");
		}
		CHECK(analog_tail->downstream_stage == V92_PHASE4_E2U_DOWNSTREAM_DATA &&
		      v92_pcm_mapping_equal(&analog_tail->mapping, &upstream_mapping),
		      "post-E2u tail failed to reach data with the selected mapping");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_phase4_e2u owner;
	struct v92_phase4_e2u saved_owner;
	struct v92_phase4_e2u_handoff handoff;
	struct v92_phase4_e2u_handoff saved_handoff;
	uint8_t output = 0xa5;

	memset(&owner, 0xa5, sizeof(owner));
	saved_owner = owner;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v92_phase4_e2u_init_analog(NULL, NULL) ==
		      V92_PHASE4_E2U_INVALID_ARGUMENT &&
	      v92_phase4_e2u_init_digital(NULL, NULL) ==
		      V92_PHASE4_E2U_INVALID_ARGUMENT &&
	      v92_phase4_e2u_next(NULL, 0u, 0, &output) ==
		      V92_PHASE4_E2U_INVALID_ARGUMENT &&
	      v92_phase4_e2u_next(&owner, 0u, 0, &output) ==
		      V92_PHASE4_E2U_INVALID_STATE &&
	      v92_phase4_e2u_handoff(&owner, &handoff) ==
		      V92_PHASE4_E2U_INVALID_STATE &&
	      memcmp(&owner, &saved_owner, sizeof(owner)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0 &&
	      output == 0xa5 && !v92_phase4_e2u_is_valid(NULL) &&
	      !v92_phase4_e2u_handoff_is_valid(NULL),
	      "rejected owner calls changed state or output");
	return true;
}

int main(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		if (!test_fresh_analog_ed_arming((enum g711_law)law) ||
		    !run_crossing((enum g711_law)law, false, false, false, NO_TAIL) ||
		    !run_crossing((enum g711_law)law, true, false, false, NO_TAIL) ||
		    !run_crossing((enum g711_law)law, false, true, false, NO_TAIL) ||
		    !run_crossing((enum g711_law)law, true, true, false, NO_TAIL) ||
		    !run_crossing((enum g711_law)law, false, false, true, NO_TAIL) ||
		    !run_crossing((enum g711_law)law, true, false, true, NO_TAIL) ||
		    !run_crossing((enum g711_law)law, false, true, false, SUVU_TAIL) ||
		    !run_crossing((enum g711_law)law, true, true, false, CPU_TAIL) ||
		    !run_rearmed_crossing(
			    (enum g711_law)law, false, 1u) ||
		    !run_rearmed_crossing(
			    (enum g711_law)law, true, 3u) ||
		    !run_changed_cpu_rejection((enum g711_law)law))
			return 1;
	}
	return test_contracts() ? 0 : 1;
}
