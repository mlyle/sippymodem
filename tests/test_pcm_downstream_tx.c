// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>
#include <pcm_downstream_tx.h>

#include "test.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

enum {
	REFERENCE_MAX_SYMBOLS = 192,
	GPC_HISTORY_BITS = 23,
	GPC_TAP_DELAY = 18,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum v90_cp_kind kind,
	enum g711_law law, uint8_t redundancy, uint8_t lookahead)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.shaping_a1_q1_6 = 29,
		.shaping_a2_q1_6 = -13,
		.shaping_b1_q1_6 = 43,
		.shaping_b2_q1_6 = -5,
		.selected_rate_index = kind == V90_CP_TRAINING ? 4 : 1,
		.sign_redundancy = redundancy,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
	};
	uint8_t size = kind == V90_CP_TRAINING ? 4 : 8;

	for (uint8_t ordinal = 0; ordinal < size; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(12 + ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static bool make_width_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t frame_bits, uint8_t lookahead, bool parameter_exchange)
{
	struct v90_cp_parameters parameters = {
		.kind = parameter_exchange ? V90_CP_DATA : V90_CP_TRAINING,
		.law = law,
		.selected_rate_index = (uint8_t)(frame_bits -
			(parameter_exchange ? 20u : 8u)),
		.spectral_lookahead = lookahead,
		.constellation_count = V90_CP_MAX_CONSTELLATIONS,
	};
	uint8_t modulus_limit = parameter_exchange ? 36u : 24u;
	uint8_t sign_bits = frame_bits > modulus_limit + 3u ?
				    (uint8_t)(frame_bits - modulus_limit) : 3u;
	uint8_t modulus_bits = (uint8_t)(frame_bits - sign_bits);

	parameters.sign_redundancy =
		(uint8_t)(V90_CP_FRAME_INTERVALS - sign_bits);
	for (size_t interval = 0u;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t exponent = (uint8_t)(
			modulus_bits / V90_CP_FRAME_INTERVALS +
			(interval < modulus_bits % V90_CP_FRAME_INTERVALS));
		uint8_t count = (uint8_t)(1u << exponent);

		parameters.frame_constellation[interval] = (uint8_t)interval;
		for (uint8_t ordinal = 0u; ordinal < count; ++ordinal) {
			add_ucode(
				&parameters.transmit_constellations[interval],
				(uint8_t)(4u + interval *
					(parameter_exchange ? 8u : 20u) + ordinal));
		}
	}
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static uint8_t reference_scrambled_one(uint32_t *history)
{
	uint8_t feedback = (uint8_t)(
		((*history >> (GPC_TAP_DELAY - 1u)) ^
		 (*history >> (GPC_HISTORY_BITS - 1u))) & 1u);
	uint8_t output = 1u ^ feedback;

	*history = ((*history << 1) | output) &
		   ((UINT32_C(1) << GPC_HISTORY_BITS) - 1u);
	return output;
}

static bool append_output(
	const struct v90_pcm_mapping *mapping,
	const struct v90_pcm_frame_output *output,
	struct pcm_downstream_symbol *symbols, size_t *symbol_count)
{
	size_t shaping_length = mapping->sign_redundancy == 0 ?
				0 : V90_CP_FRAME_INTERVALS /
					    mapping->sign_redundancy;
	size_t rule_index = 0;
	enum v90_spectral_rule rule = V90_SPECTRAL_RULE_A;

	if (*symbol_count + output->symbol_count >
	    REFERENCE_MAX_SYMBOLS)
		return false;
	for (size_t index = 0; index < output->symbol_count; ++index) {
		struct pcm_downstream_symbol symbol = {
			.pcm = output->symbols[index],
		};

		if (mapping->sign_redundancy != 0) {
			bool starts = (*symbol_count + index) %
				      shaping_length == 0;

			if (starts) {
				if (rule_index >= output->rule_count)
					return false;
				rule = output->rules[rule_index++];
			}
			symbol.shaping_rule = rule;
			symbol.shaping_rule_valid = true;
			symbol.shaping_frame_start = starts;
		}
		symbols[*symbol_count + index] = symbol;
	}
	if (rule_index != output->rule_count)
		return false;
	*symbol_count += output->symbol_count;
	return true;
}

static bool make_reference(
	const struct v90_pcm_mapping *mapping, uint64_t frame_count,
	struct pcm_downstream_symbol *symbols, size_t *symbol_count)
{
	struct v90_pcm_frame_encoder encoder;
	uint32_t scrambler_history = 0;
	size_t produced = 0;

	if (frame_count > REFERENCE_MAX_SYMBOLS / V90_CP_FRAME_INTERVALS ||
	    v90_pcm_frame_encoder_init(&encoder, mapping) !=
		    V90_PCM_FRAME_ENCODER_OK)
		return false;
	for (uint64_t frame = 0; frame < frame_count; ++frame) {
		struct v90_pcm_frame_output output;
		uint8_t bits[V90_PCM_FRAME_MAX_BITS];
		enum v90_pcm_frame_encoder_result result;

		for (size_t bit = 0; bit < mapping->frame_bits; ++bit)
			bits[bit] = reference_scrambled_one(
				&scrambler_history);
		result = v90_pcm_frame_encoder_process(
			&encoder, bits, mapping->frame_bits, &output);
		if (result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY) {
			if (!append_output(
				    mapping, &output, symbols, &produced))
				return false;
		} else if (result != V90_PCM_FRAME_ENCODER_BUFFERING) {
			return false;
		}
	}
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_pcm_frame_encoder_result result =
			v90_pcm_frame_encoder_finish(&encoder, &output);

		if (result == V90_PCM_FRAME_ENCODER_COMPLETE)
			break;
		if (result != V90_PCM_FRAME_ENCODER_SYMBOLS_READY ||
		    !append_output(mapping, &output, symbols, &produced))
			return false;
	}
	*symbol_count = produced;
	return produced == frame_count * V90_CP_FRAME_INTERVALS;
}

static struct v92_cpd_base_parameters cpd_parameters(void)
{
	return (struct v92_cpd_base_parameters){
		.four_times_gain_q0_16 = UINT16_C(0x4321),
		.data_rate_number = 4u,
		.trellis = V92_CPD_TRELLIS_16_STATE,
		.acknowledges_cpu = true,
	};
}

static bool cpd_parameters_equal(
	const struct v92_cpd_parameters *left,
	const struct v92_cpd_base_parameters *right)
{
	return v92_cpd_parameters_are_valid(left) &&
	       v92_cpd_base_parameters_are_valid(right) &&
	       left->four_times_gain_q0_16 == right->four_times_gain_q0_16 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->trellis == right->trellis &&
	       left->extend_e2u == right->extend_e2u &&
	       left->acknowledges_cpu == right->acknowledges_cpu &&
	       !left->modulus_present && !left->filters_present &&
	       !left->constellations_present;
}

static bool symbols_equal(
	const struct pcm_downstream_symbol *left,
	const struct pcm_downstream_symbol *right)
{
	return left->pcm.ucode == right->pcm.ucode &&
	       left->pcm.positive == right->pcm.positive &&
	       left->pcm.octet == right->pcm.octet &&
	       left->pcm.sample == right->pcm.sample &&
	       left->shaping_rule == right->shaping_rule &&
	       left->shaping_rule_valid == right->shaping_rule_valid &&
	       left->shaping_frame_start == right->shaping_frame_start;
}

static bool test_generated_sequences(void)
{
	uint8_t first_ucodes[2][4][4][V90_CP_FRAME_INTERVALS] = { 0 };

	for (unsigned int law_index = 0; law_index < 2; ++law_index) {
		enum g711_law law = law_index == 0 ?
				    G711_LAW_PCMU : G711_LAW_PCMA;

		for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy) {
			for (uint8_t lookahead = 0; lookahead <= 3; ++lookahead) {
				struct v90_pcm_mapping mapping;
				struct pcm_downstream_tx transmitter;
				struct pcm_downstream_symbol actual[REFERENCE_MAX_SYMBOLS];
				struct pcm_downstream_symbol expected[REFERENCE_MAX_SYMBOLS];
				size_t actual_count = 0;
				size_t expected_count = 0;
				uint64_t accepted_at_finish;

				CHECK(make_mapping(
					      &mapping, V90_CP_TRAINING, law,
					      redundancy, lookahead) &&
				      pcm_downstream_tx_init(
					      &transmitter, PCM_PROTOCOL_V90,
					      &mapping) ==
					      PCM_DOWNSTREAM_TX_OK,
				      "sequence %u/%u/%u did not initialize",
				      law_index, redundancy, lookahead);
				while (actual_count < 13) {
					CHECK(pcm_downstream_tx_next(
						      &transmitter,
						      &actual[actual_count]) ==
						      PCM_DOWNSTREAM_TX_OK,
					      "sequence %u/%u/%u stopped early",
					      law_index, redundancy, lookahead);
					actual_count++;
				}
				accepted_at_finish = transmitter.accepted_frames;
				CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
					      PCM_DOWNSTREAM_TX_OK &&
				      pcm_downstream_tx_request_finish(&transmitter) ==
					      PCM_DOWNSTREAM_TX_OK,
				      "sequence %u/%u/%u finish was not idempotent",
				      law_index, redundancy, lookahead);
				while (!pcm_downstream_tx_is_complete(&transmitter)) {
					CHECK(actual_count < REFERENCE_MAX_SYMBOLS,
					      "sequence %u/%u/%u exceeded its bounded output",
					      law_index, redundancy, lookahead);
					enum pcm_downstream_tx_result result =
						pcm_downstream_tx_next(
							&transmitter,
							&actual[actual_count]);

					if (result == PCM_DOWNSTREAM_TX_COMPLETE)
						break;
					CHECK(result == PCM_DOWNSTREAM_TX_OK &&
					      actual_count < REFERENCE_MAX_SYMBOLS,
					      "sequence %u/%u/%u failed while draining",
					      law_index, redundancy, lookahead);
					actual_count++;
				}
				CHECK(make_reference(
					      &mapping, accepted_at_finish,
					      expected, &expected_count) &&
				      actual_count == expected_count &&
				      transmitter.accepted_frames ==
					      accepted_at_finish &&
				      transmitter.transmitted_symbols ==
					      actual_count &&
				      actual_count % V90_CP_FRAME_INTERVALS == 0,
				      "sequence %u/%u/%u lost an accepted frame",
				      law_index, redundancy, lookahead);
				for (size_t symbol = 0;
				     symbol < actual_count; ++symbol) {
					CHECK(symbols_equal(
						      &actual[symbol],
						      &expected[symbol]),
					      "sequence %u/%u/%u symbol %zu differed",
					      law_index, redundancy, lookahead,
					      symbol);
				}
				for (size_t symbol = 0;
				     symbol < V90_CP_FRAME_INTERVALS; ++symbol)
					first_ucodes[law_index][redundancy]
						[lookahead][symbol] =
						actual[symbol].pcm.ucode;
			}
			for (uint8_t lookahead = 1; lookahead <= 3; ++lookahead) {
				CHECK(memcmp(
					      first_ucodes[law_index][redundancy][0],
					      first_ucodes[law_index][redundancy]
						      [lookahead],
					      V90_CP_FRAME_INTERVALS) == 0,
				      "first TRN2d magnitudes changed with look-ahead %u/%u/%u",
				      law_index, redundancy, lookahead);
			}
		}
	}
	return true;
}

static bool test_sparse_training_wire_digests(void)
{
	static const struct {
		enum g711_law network_law;
		enum g711_law codec_law;
		uint8_t ucodes[4];
		uint64_t digest;
	} cases[] = {
		{ G711_LAW_PCMU, G711_LAW_PCMU, { 44, 66, 81, 87 },
		  UINT64_C(0x619ff550026905f6) },
		{ G711_LAW_PCMA, G711_LAW_PCMU, { 44, 66, 81, 87 },
		  UINT64_C(0x6b8f95687aa43116) },
		{ G711_LAW_PCMU, G711_LAW_PCMA, { 47, 69, 81, 87 },
		  UINT64_C(0xa21fb56665d29bab) },
		{ G711_LAW_PCMA, G711_LAW_PCMA, { 47, 69, 81, 87 },
		  UINT64_C(0xc0a473af56f2a2ab) },
	};
	/* Independently calculated from V.90 5.3-5.4 and 8.6.5, not from the
	 * native receiver or frame encoder. Start zeroed GPC (18,23), parse
	 * S=5/K=12, divide the modulus word by four six times, and use descending
	 * Ucodes. Table 4 differential signs feed the two-state shaper with one
	 * lookahead frame and F(z)=1/(1-z^-1); minimize accumulated squared
	 * Table 1 linear values. FNV-1a covers the first 2040 network octets.
	 * These sparse sets span companding chords; codec type must not change
	 * the negotiated network law. V.90 and V.92 share this training epoch. */
	for (size_t i = 0; i < ARRAY_LENGTH(cases); ++i) {
		struct v90_cp_parameters parameters = {
			.kind = V90_CP_TRAINING,
			.law = cases[i].codec_law,
			.selected_rate_index = 9,
			.sign_redundancy = 1,
			.spectral_lookahead = 1,
			.shaping_a1_q1_6 = 64,
			.constellation_count = 1,
		};
		struct v90_pcm_mapping mapping;

		for (size_t level = 0; level < ARRAY_LENGTH(cases[i].ucodes); ++level)
			add_ucode(&parameters.transmit_constellations[0],
				  cases[i].ucodes[level]);
		CHECK(v90_pcm_mapping_init_transmit(
			      &mapping, &parameters, cases[i].network_law) ==
			      V90_PCM_MAPPING_OK,
		      "sparse training mapping %zu failed", i);
		for (unsigned protocol = PCM_PROTOCOL_V90;
		     protocol <= PCM_PROTOCOL_V92; ++protocol) {
			struct pcm_downstream_tx transmitter;
			uint64_t digest = UINT64_C(14695981039346656037);

			CHECK(pcm_downstream_tx_init(&transmitter,
				      (enum pcm_protocol)protocol, &mapping) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "sparse training %zu protocol %u did not initialize",
			      i, protocol);
			for (unsigned symbol = 0; symbol < 2040; ++symbol) {
				struct pcm_downstream_symbol output;

				CHECK(pcm_downstream_tx_next(&transmitter, &output) ==
					      PCM_DOWNSTREAM_TX_OK,
				      "sparse training %zu protocol %u stopped at %u",
				      i, protocol, symbol);
				digest = (digest ^ output.pcm.octet) *
					 UINT64_C(1099511628211);
			}
			CHECK(digest == cases[i].digest,
			      "sparse training %zu protocol %u changed its wire sequence",
			      i, protocol);
		}
	}
	return true;
}

static bool run_v92_width(
	enum g711_law law, uint8_t frame_bits, uint8_t lookahead,
	bool parameter_exchange, bool cleardown)
{
	const struct v92_suvd_parameters initial_suvd = {
		.request_silence = !parameter_exchange && !cleardown && (frame_bits & 1u) != 0u,
	};
	const struct v92_suvd_parameters acknowledged_suvd = {
		.request_silence = !parameter_exchange && !cleardown && (frame_bits & 1u) != 0u,
		.acknowledges_cpu = true,
	};
	struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_rx receiver;
	size_t training_frames = 0u;
	size_t suvd_records = 0u;
	size_t cpd_records = 0u;
	size_t ed_records = 0u;
	size_t symbol_count = 0u;
	uint64_t cpd_first_frame = 0u;
	size_t maximum_symbols =
		V90_PHASE4_TRN2D_MINIMUM_SYMBOLS + 2048u;
	enum {
		WAIT_INITIAL_SUVD,
		WAIT_CPD,
		WAIT_ACKNOWLEDGED_SUVD,
		CONTROL_COMPLETE,
	} stage = WAIT_INITIAL_SUVD;
	bool cpd_pending = false;
	bool suvd_pending = false;

	if (cleardown) {
		cpd.data_rate_number = 0u;
		cpd.acknowledges_cpu = (frame_bits & 1u) != 0u;
	}
	CHECK(make_width_mapping(&mapping, law, frame_bits, lookahead,
				 parameter_exchange) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &initial_suvd) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &initial_suvd) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_is_v92_control_boundary(&transmitter) ==
		      parameter_exchange &&
	      transmitter.accepted_frames == 0u &&
	      transmitter.transmitted_symbols == 0u &&
	      transmitter.scrambler.history == 0u &&
	      receiver.stream.descrambler.history == 0u,
	      "V.92 width %u law %u fixture did not initialize",
	      frame_bits, law);
	while (stage != CONTROL_COMPLETE && symbol_count < maximum_symbols) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;
		struct pcm_downstream_event untouched;
		enum pcm_downstream_rx_result result;

		if (cpd_pending &&
		    pcm_downstream_tx_is_v92_control_boundary(&transmitter)) {
			CHECK(pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
				      PCM_DOWNSTREAM_TX_OK &&
			      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "V.92 width %u law %u did not switch to CPd",
			      frame_bits, law);
			cpd_pending = false;
			cpd_first_frame = transmitter.accepted_frames;
			if (cleardown) {
				struct pcm_downstream_tx before = transmitter;

				CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
					      PCM_DOWNSTREAM_TX_INVALID_STATE &&
				      memcmp(&before, &transmitter, sizeof(before)) == 0,
				      "unsent zero-rate CPd was discarded");
			}
		}
		if (suvd_pending &&
		    pcm_downstream_tx_is_v92_control_boundary(&transmitter)) {
			struct pcm_downstream_tx before = transmitter;

			CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
				      PCM_DOWNSTREAM_TX_INVALID_STATE &&
			      memcmp(&before, &transmitter, sizeof(before)) == 0,
			      "nonzero CPd was allowed to finish without Ed");
			CHECK(pcm_downstream_tx_start_suvd(
				      &transmitter, &acknowledged_suvd) ==
				      PCM_DOWNSTREAM_TX_OK &&
			      pcm_downstream_tx_start_suvd(
				      &transmitter, &acknowledged_suvd) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "V.92 width %u law %u did not return to SUVd",
			      frame_bits, law);
			suvd_pending = false;
		}

		CHECK(pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "V.92 width %u law %u transmitter stopped",
		      frame_bits, law);
		symbol_count++;
		memset(&event, 0xa5, sizeof(event));
		untouched = event;
		result = pcm_downstream_rx_receive(
			&receiver, symbol.pcm.octet, &event);
		if (result == PCM_DOWNSTREAM_RX_BUFFERING) {
			CHECK(memcmp(&event, &untouched, sizeof(event)) == 0,
			      "V.92 width %u law %u buffering changed event",
			      frame_bits, law);
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_TRN2D_FRAME) {
			CHECK(event.trn2d.bit_count == frame_bits &&
			      event.trn2d.training_bit_errors == 0u,
			      "V.92 width %u law %u corrupted TRN2d",
			      frame_bits, law);
			training_frames++;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_SUVD) {
			const struct v92_suvd_parameters *received =
				&event.control.record.suvd.parameters;

			CHECK(event.control.type == V92_CONTROL_EVENT_SUVD &&
			      event.control.record.suvd.frame_result == V92_SUVD_OK &&
			      received->request_silence ==
				      initial_suvd.request_silence,
			      "V.92 width %u law %u returned malformed SUVd",
			      frame_bits, law);
			suvd_records++;
			if (stage == WAIT_INITIAL_SUVD) {
				CHECK(!received->acknowledges_cpu,
				      "V.92 width %u law %u changed SUVd early",
				      frame_bits, law);
				cpd_pending = true;
				stage = WAIT_CPD;
			} else if (stage == WAIT_CPD) {
				CHECK(!received->acknowledges_cpu,
				      "V.92 width %u law %u changed SUVd early",
				      frame_bits, law);
			} else {
				CHECK(received->acknowledges_cpu,
				      "V.92 width %u law %u lost SUVd acknowledgement",
				      frame_bits, law);
				stage = CONTROL_COMPLETE;
			}
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_CPD) {
			CHECK((stage == WAIT_CPD ||
			       stage == WAIT_ACKNOWLEDGED_SUVD) &&
			      event.control.type == V92_CONTROL_EVENT_CPD &&
			      event.control.record.cpd.frame_result == V92_CPD_OK &&
			      cpd_parameters_equal(
				      &event.control.record.cpd.parameters, &cpd),
			      "V.92 width %u law %u returned malformed CPd",
			      frame_bits, law);
			cpd_records++;
			if (cleardown) {
				stage = CONTROL_COMPLETE;
				continue;
			}
			if (stage == WAIT_CPD) {
				suvd_pending = true;
				stage = WAIT_ACKNOWLEDGED_SUVD;
			}
			continue;
		}
		CHECK(false,
		      "V.92 width %u law %u receiver returned %s",
		      frame_bits, law,
		      pcm_downstream_rx_result_name(result));
	}
	CHECK(stage == CONTROL_COMPLETE && suvd_records >= (cleardown ? 1u : 2u) &&
	      cpd_records >= 1u && !cpd_pending && !suvd_pending &&
	      training_frames == (parameter_exchange ? 0u :
		      V90_PHASE4_TRN2D_MINIMUM_FRAMES) &&
	      transmitter.protocol == PCM_PROTOCOL_V92 &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
	      transmitter.v92_control_kind ==
		      (cleardown ? PCM_DOWNSTREAM_V92_CONTROL_CPD : PCM_DOWNSTREAM_V92_CONTROL_SUVD) &&
	      transmitter.control_configured &&
	      receiver.stage == PCM_DOWNSTREAM_RX_V92_CONTROL &&
	      receiver.trn2d_bit_errors == 0u &&
	      pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_rx_is_valid(&receiver),
	      "V.92 width %u law %u lost its continuous control epoch",
	      frame_bits, law);
	/* The owner decides when the 9.11 acknowledgement/timer permits exit.
	 * This component must then finish the current zero-rate CPd and drain
	 * every queued symbol, without Ed/B1d or another data epoch. */
	if (cleardown) {
		while (!pcm_downstream_tx_is_complete(&transmitter) &&
		       symbol_count < maximum_symbols) {
			struct pcm_downstream_tx before = transmitter;
			struct pcm_downstream_symbol symbol = { 0 }, untouched = symbol;
			struct pcm_downstream_event event;
			bool boundary = transmitter.control_bit_index == 0u;

			CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
				      (boundary ? PCM_DOWNSTREAM_TX_OK : PCM_DOWNSTREAM_TX_INVALID_STATE) &&
			      (boundary || memcmp(&before, &transmitter, sizeof(before)) == 0),
			      "zero-rate CPd finish lost its record boundary");
			if (boundary) {
				before.finish_requested = true;
				CHECK(memcmp(&before, &transmitter, sizeof(before)) == 0 &&
				      pcm_downstream_tx_request_finish(&transmitter) == PCM_DOWNSTREAM_TX_OK &&
				      !pcm_downstream_tx_is_v92_control_boundary(&transmitter) &&
				      pcm_downstream_tx_start_suvd(&transmitter, &acknowledged_suvd) ==
					      PCM_DOWNSTREAM_TX_INVALID_STATE &&
				      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
					      PCM_DOWNSTREAM_TX_INVALID_STATE &&
				      pcm_downstream_tx_request_ed(&transmitter) == PCM_DOWNSTREAM_TX_INVALID_STATE &&
				      memcmp(&before, &transmitter, sizeof(before)) == 0,
				      "zero-rate CPd finish reset coding or restarted transmission");
			}
			enum pcm_downstream_tx_result sent = pcm_downstream_tx_next(&transmitter, &symbol);

			if (sent == PCM_DOWNSTREAM_TX_COMPLETE) {
				CHECK(memcmp(&symbol, &untouched, sizeof(symbol)) == 0,
				      "completed CPd drain changed output");
				break;
			}
			CHECK(sent == PCM_DOWNSTREAM_TX_OK, "zero-rate CPd drain failed");
			symbol_count++;
			enum pcm_downstream_rx_result received = pcm_downstream_rx_receive(
				&receiver, symbol.pcm.octet, &event);

			if (received == PCM_DOWNSTREAM_RX_CPD) {
				CHECK(event.control.record.cpd.frame_result == V92_CPD_OK &&
				      cpd_parameters_equal(&event.control.record.cpd.parameters, &cpd),
				      "CPd drain corrupted its final record");
				cpd_records++;
			} else {
				CHECK(received == PCM_DOWNSTREAM_RX_BUFFERING,
				      "CPd drain emitted a non-cleardown record");
			}
		}
		CHECK(pcm_downstream_tx_is_complete(&transmitter) &&
		      transmitter.ed_frames == 0u && transmitter.queue_count == 0u &&
		      symbol_count == transmitter.accepted_frames * V90_CP_FRAME_INTERVALS &&
		      cpd_records * transmitter.control_sequence_bits ==
			      (transmitter.accepted_frames - cpd_first_frame) * frame_bits &&
		      pcm_downstream_tx_request_finish(&transmitter) == PCM_DOWNSTREAM_TX_COMPLETE,
		      "zero-rate CPd lost a record or spectral tail");
		return true;
	}
	CHECK(pcm_downstream_rx_expect_ed(&receiver) == PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "V.92 width %u law %u did not arm Ed", frame_bits, law);
	while (!pcm_downstream_tx_is_complete(&transmitter) &&
	       symbol_count < maximum_symbols) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;
		enum pcm_downstream_tx_result transmit_result =
			pcm_downstream_tx_next(&transmitter, &symbol);
		enum pcm_downstream_rx_result receive_result;

		if (transmit_result == PCM_DOWNSTREAM_TX_COMPLETE)
			break;
		CHECK(transmit_result == PCM_DOWNSTREAM_TX_OK,
		      "V.92 width %u law %u Ed transmitter stopped",
		      frame_bits, law);
		symbol_count++;
		receive_result = pcm_downstream_rx_receive(
			&receiver, symbol.pcm.octet, &event);
		if (receive_result == PCM_DOWNSTREAM_RX_ED) {
			CHECK(event.ed.bit_count ==
				      V90_PHASE4_ED_FRAMES * frame_bits &&
			      event.ed.bit_errors == 0u,
			      "V.92 width %u law %u returned corrupt Ed",
			      frame_bits, law);
			ed_records++;
		} else {
			CHECK(receive_result == PCM_DOWNSTREAM_RX_BUFFERING ||
			      receive_result == PCM_DOWNSTREAM_RX_SUVD,
			      "V.92 width %u law %u returned %s before Ed",
			      frame_bits, law,
			      pcm_downstream_rx_result_name(receive_result));
		}
	}
	CHECK(ed_records == 1u &&
	      pcm_downstream_tx_is_complete(&transmitter) &&
	      pcm_downstream_rx_is_complete(&receiver) &&
	      transmitter.protocol == PCM_PROTOCOL_V92 &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_ED &&
	      transmitter.ed_frames == V90_PHASE4_ED_FRAMES &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_COMPLETE,
	      "V.92 width %u law %u did not complete Ed exactly",
	      frame_bits, law);
	if (mapping.kind == V90_CP_DATA) {
		struct pcm_downstream_tx saved_tx = transmitter;
		struct pcm_downstream_rx saved_rx = receiver;
		CHECK(pcm_downstream_tx_resume_after_rt(&transmitter) == PCM_DOWNSTREAM_TX_INVALID_STATE &&
		      pcm_downstream_rx_resume_after_rt(&receiver, true) == PCM_DOWNSTREAM_RX_INVALID_STATE &&
		      memcmp(&transmitter, &saved_tx, sizeof(transmitter)) == 0 &&
		      memcmp(&receiver, &saved_rx, sizeof(receiver)) == 0,
		      "FPE's data-mapped Ed accepted Rt or changed on rejection");
	}
	return true;
}

static bool test_v92_sequences(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t lookahead = 0u; lookahead <= 3u; ++lookahead) {
			for (uint8_t frame_bits =
				     V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS;
			     frame_bits <=
				     V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MAX_BITS;
			     ++frame_bits) {
				CHECK(run_v92_width(
					      (enum g711_law)law, frame_bits,
					      lookahead, false, false) &&
				      run_v92_width(law, frame_bits, lookahead, false, true),
				      "V.92 downstream transmitter matrix stopped");
			}
		}
	}
	return true;
}

static bool test_v92_parameter_exchange(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t lookahead = 0u; lookahead <= 3u; ++lookahead) {
			for (uint8_t width = 21u; width <= 42u; ++width) {
				CHECK(run_v92_width(law, width, lookahead, true, false) &&
				      run_v92_width(law, width, lookahead, true, true),
				      "V.92 data-mode control stream failed");
			}
		}
	}
	return true;
}

static bool test_v92_parameter_exchange_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx tx;
	struct pcm_downstream_rx rx;
	struct pcm_downstream_tx saved_tx;
	struct pcm_downstream_rx saved_rx;

	CHECK(make_width_mapping(&mapping, G711_LAW_PCMU, 42u, 3u, true) &&
	      pcm_downstream_tx_init(&tx, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(&rx, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK,
	      "data-mode contract fixture did not initialize");
	saved_tx = tx;
	saved_rx = rx;
	CHECK(pcm_downstream_tx_init(&tx, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_PARAMETERS &&
	      pcm_downstream_rx_init(&rx, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_INVALID_PARAMETERS &&
	      memcmp(&tx, &saved_tx, sizeof(tx)) == 0 &&
	      memcmp(&rx, &saved_rx, sizeof(rx)) == 0,
	      "V.90 accepted a data mapping or changed rejected output");
	tx.stage = PCM_DOWNSTREAM_TX_TRN2D;
	rx.stage = PCM_DOWNSTREAM_RX_TRN2D;
	CHECK(!pcm_downstream_tx_is_valid(&tx) &&
	      !pcm_downstream_rx_is_valid(&rx),
	      "data-mode control accepted a TRN2d stage");
	tx = saved_tx;
	rx = saved_rx;
	tx.control_configured = false;
	rx.trn2d_bit_errors = 1u;
	CHECK(!pcm_downstream_tx_is_valid(&tx) &&
	      !pcm_downstream_rx_is_valid(&rx),
	      "data-mode control accepted absent control or training history");
	return true;
}

static bool advance_to_v92_position(
	struct pcm_downstream_tx *transmitter, bool boundary)
{
	for (size_t symbol_count = 0u; symbol_count < 4096u;
	     ++symbol_count) {
		struct pcm_downstream_symbol symbol;

		if (transmitter->stage == PCM_DOWNSTREAM_TX_V92_CONTROL &&
		    (transmitter->control_bit_index == 0u) == boundary)
			return true;
		if (pcm_downstream_tx_next(transmitter, &symbol) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
	}
	return false;
}

static void flip_sequence_bit(uint8_t *sequence, size_t bit)
{
	sequence[bit / 8u] ^=
		(uint8_t)(1u << (7u - bit % 8u));
}

static bool invalid_transmitter_is_rejected(
	struct pcm_downstream_tx corrupted)
{
	struct pcm_downstream_tx saved;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol saved_symbol;

	saved = corrupted;
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	return !pcm_downstream_tx_is_valid(&corrupted) &&
	       pcm_downstream_tx_next(&corrupted, &symbol) ==
		       PCM_DOWNSTREAM_TX_INVALID_STATE &&
	       memcmp(&corrupted, &saved, sizeof(corrupted)) == 0 &&
	       memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0;
}

static bool corrupted_cpd_is_rejected(
	const struct pcm_downstream_tx *valid, size_t corrupt_bit)
{
	struct pcm_downstream_tx corrupted = *valid;

	if (corrupt_bit >= sizeof(corrupted.control_sequence) * 8u)
		return false;
	flip_sequence_bit(corrupted.control_sequence, corrupt_bit);
	return invalid_transmitter_is_rejected(corrupted);
}

static bool test_v92_switch_contracts(void)
{
	const struct v92_suvd_parameters initial_suvd = { 0 };
	const struct v92_suvd_parameters acknowledged_suvd = {
		.acknowledges_cpu = true,
	};
	const struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v92_cpd_base_parameters different_cpd = cpd;
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_tx saved;
	struct pcm_downstream_tx corrupted;
	size_t first_unused_bit;

	different_cpd.four_times_gain_q0_16++;
	CHECK(make_width_mapping(&mapping, G711_LAW_PCMU, 17u, 3u, false) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &initial_suvd) == PCM_DOWNSTREAM_TX_OK &&
	      advance_to_v92_position(&transmitter, false),
	      "V.92 switch fixture did not reach a partial SUVd");
	saved = transmitter;
	CHECK(pcm_downstream_tx_start_suvd(
		      &transmitter, &initial_suvd) == PCM_DOWNSTREAM_TX_OK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &acknowledged_suvd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "V.92 transmitter replaced a partial SUVd");
	CHECK(advance_to_v92_position(&transmitter, true) &&
	      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      advance_to_v92_position(&transmitter, false),
	      "V.92 transmitter did not enter a partial CPd");
	saved = transmitter;
	CHECK(pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      pcm_downstream_tx_start_cpd(&transmitter, &different_cpd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &acknowledged_suvd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "V.92 transmitter replaced a partial CPd");
	CHECK(advance_to_v92_position(&transmitter, true) &&
	      pcm_downstream_tx_start_cpd(
		      &transmitter, &different_cpd) == PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_is_valid(&transmitter),
	      "V.92 transmitter did not replace CPd at a boundary");
	first_unused_bit =
		((transmitter.control_sequence_bits + 7u) / 8u) * 8u;
	CHECK(corrupted_cpd_is_rejected(&transmitter, 35u) &&
	      corrupted_cpd_is_rejected(
		      &transmitter, V92_CPD_FIXED_BITS + 1u) &&
	      corrupted_cpd_is_rejected(
		      &transmitter,
		      V92_CPD_FIXED_BITS + V92_CPD_TRAILER_BITS) &&
	      corrupted_cpd_is_rejected(
		      &transmitter, transmitter.control_sequence_bits) &&
	      corrupted_cpd_is_rejected(
		      &transmitter, first_unused_bit),
	      "V.92 transmitter accepted corrupted CPd storage");
	corrupted = transmitter;
	corrupted.control.cpd.four_times_gain_q0_16++;
	CHECK(invalid_transmitter_is_rejected(corrupted),
	      "V.92 transmitter accepted changed CPd gain");
	corrupted = transmitter;
	corrupted.control.cpd.data_rate_number++;
	CHECK(invalid_transmitter_is_rejected(corrupted),
	      "V.92 transmitter accepted changed CPd rate");
	corrupted = transmitter;
	corrupted.control.cpd.trellis = V92_CPD_TRELLIS_32_STATE;
	CHECK(invalid_transmitter_is_rejected(corrupted),
	      "V.92 transmitter accepted changed CPd trellis");
	corrupted = transmitter;
	corrupted.control.cpd.extend_e2u =
		!corrupted.control.cpd.extend_e2u;
	CHECK(invalid_transmitter_is_rejected(corrupted),
	      "V.92 transmitter accepted changed CPd E2U selection");
	corrupted = transmitter;
	corrupted.control.cpd.acknowledges_cpu =
		!corrupted.control.cpd.acknowledges_cpu;
	CHECK(invalid_transmitter_is_rejected(corrupted),
	      "V.92 transmitter accepted changed CPd acknowledgement");
	CHECK(pcm_downstream_tx_start_suvd(
		      &transmitter, &acknowledged_suvd) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "V.92 transmitter did not select acknowledging SUVd");
	saved = transmitter;
	CHECK(!transmitter.v92_control_record_accepted &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      advance_to_v92_position(&transmitter, false) &&
	      !transmitter.v92_control_record_accepted &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "V.92 transmitter skipped the acknowledging SUVd record");
	return true;
}

static bool source_record_fixture(
	struct pcm_downstream_tx *transmitter,
	struct pcm_downstream_source_record *record,
	uint8_t sequence[9])
{
	const struct v92_suvd_parameters suvd = { 0 };
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_symbol symbol;

	memset(sequence, 0xa5, 9u);
	*record = (struct pcm_downstream_source_record){
		.sequence = sequence,
		.sequence_capacity = 9u,
		.sequence_bits = 4u * 17u,
	};
	if (!make_width_mapping(
		    &mapping, G711_LAW_PCMU, 17u, 3u, false))
		return false;
	if (pcm_downstream_tx_init(
		    transmitter, PCM_PROTOCOL_V92, &mapping) !=
		    PCM_DOWNSTREAM_TX_OK ||
	    pcm_downstream_tx_start_suvd(transmitter, &suvd) !=
		    PCM_DOWNSTREAM_TX_OK ||
	    !advance_to_v92_position(transmitter, true))
		return false;
	while (transmitter->queue_count != 0u) {
		if (pcm_downstream_tx_next(transmitter, &symbol) !=
		    PCM_DOWNSTREAM_TX_OK)
			return false;
	}
	return pcm_downstream_tx_is_v92_control_boundary(transmitter);
}

static bool source_record_rejection_is_transactional(
	struct pcm_downstream_tx transmitter,
	struct pcm_downstream_source_record record,
	enum pcm_downstream_tx_result expected)
{
	struct pcm_downstream_tx before_transmitter = transmitter;
	struct pcm_downstream_source_record before_record = record;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol before_symbol;

	memset(&symbol, 0xa5, sizeof(symbol));
	before_symbol = symbol;
	return pcm_downstream_tx_next_source_record(
		       &transmitter, &record, &symbol) == expected &&
	       memcmp(&transmitter, &before_transmitter,
		      sizeof(transmitter)) == 0 &&
	       memcmp(&record, &before_record, sizeof(record)) == 0 &&
	       memcmp(&symbol, &before_symbol, sizeof(symbol)) == 0;
}

static bool test_source_record_contracts(void)
{
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_tx before_transmitter;
	struct pcm_downstream_source_record record;
	struct pcm_downstream_source_record before_record;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol before_symbol;
	uint8_t sequence[9];

	CHECK(source_record_fixture(&transmitter, &record, sequence),
	      "source-record fixture did not reach its boundary");
	{
		struct pcm_downstream_source_record invalid = record;

		invalid.bit_index = 1u;
		CHECK(source_record_rejection_is_transactional(
			      transmitter, invalid,
			      PCM_DOWNSTREAM_TX_INVALID_STATE),
		      "unaligned source cursor changed the transaction");
		invalid = record;
		invalid.sequence_capacity = 8u;
		CHECK(source_record_rejection_is_transactional(
			      transmitter, invalid,
			      PCM_DOWNSTREAM_TX_INVALID_STATE),
		      "short source capacity changed the transaction");
		invalid = record;
		invalid.bit_index = invalid.sequence_bits;
		CHECK(source_record_rejection_is_transactional(
			      transmitter, invalid,
			      PCM_DOWNSTREAM_TX_INVALID_STATE),
		      "unfinished terminal cursor changed the transaction");
		invalid.complete = true;
		CHECK(source_record_rejection_is_transactional(
			      transmitter, invalid,
			      PCM_DOWNSTREAM_TX_INVALID_STATE),
		      "complete source record was accepted again");
	}

	before_transmitter = transmitter;
	before_record = record;
	memset(&symbol, 0xa5, sizeof(symbol));
	before_symbol = symbol;
	CHECK(pcm_downstream_tx_next_source_record(
		      &transmitter, &record,
		      (struct pcm_downstream_symbol *)(void *)&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_next_source_record(
		      &transmitter,
		      (struct pcm_downstream_source_record *)(void *)&transmitter,
		      &symbol) == PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_next_source_record(
		      &transmitter, &record,
		      (struct pcm_downstream_symbol *)(void *)&record) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&record, &before_record, sizeof(record)) == 0 &&
	      memcmp(&symbol, &before_symbol, sizeof(symbol)) == 0,
	      "overlapping source-record owners changed state or output");

	record.sequence = (const uint8_t *)(const void *)&transmitter;
	record.sequence_capacity = sizeof(transmitter);
	before_transmitter = transmitter;
	before_record = record;
	CHECK(pcm_downstream_tx_next_source_record(
		      &transmitter, &record, &symbol) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&record, &before_record, sizeof(record)) == 0 &&
	      memcmp(&symbol, &before_symbol, sizeof(symbol)) == 0,
	      "source storage overlapping the line was accepted");

	CHECK(source_record_fixture(&transmitter, &record, sequence),
	      "source failure fixture did not reach its boundary");
	transmitter.accepted_frames =
		UINT64_MAX / V90_CP_FRAME_INTERVALS;
	transmitter.transmitted_symbols =
		transmitter.accepted_frames * V90_CP_FRAME_INTERVALS -
		transmitter.queue_count -
		(uint64_t)transmitter.encoder.shaper.pending_count *
			(V90_CP_FRAME_INTERVALS /
			 transmitter.encoder.mapping.sign_redundancy);
	before_transmitter = transmitter;
	before_record = record;
	memset(&symbol, 0xa5, sizeof(symbol));
	before_symbol = symbol;
	{
		enum pcm_downstream_tx_result result =
			pcm_downstream_tx_next_source_record(
				&transmitter, &record, &symbol);

		CHECK(pcm_downstream_tx_is_valid(&before_transmitter) &&
		      result == PCM_DOWNSTREAM_TX_OUT_OF_RANGE &&
		      memcmp(&transmitter, &before_transmitter,
			     sizeof(transmitter)) == 0 &&
		      memcmp(&record, &before_record, sizeof(record)) == 0 &&
		      memcmp(&symbol, &before_symbol, sizeof(symbol)) == 0,
		      "source failure %s was not transactional",
		      pcm_downstream_tx_result_name(result));
	}

	CHECK(source_record_fixture(&transmitter, &record, sequence),
	      "source completion fixture did not reach its boundary");
	for (size_t emitted = 0u; !record.complete && emitted < 128u;
	     ++emitted)
		CHECK(pcm_downstream_tx_next_source_record(
			      &transmitter, &record, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "source record stopped at output %zu", emitted);
	CHECK(record.complete && record.bit_index == record.sequence_bits &&
	      pcm_downstream_tx_is_valid(&transmitter),
	      "source record did not reach its exact terminal cursor");
	return true;
}

static bool test_empty_finish(void)
{
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol saved;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 2, 3) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "empty finish fixture did not initialize");
	memset(&symbol, 0xa5, sizeof(symbol));
	saved = symbol;
	CHECK(pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_COMPLETE &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_COMPLETE &&
	      memcmp(&symbol, &saved, sizeof(symbol)) == 0 &&
	      transmitter.accepted_frames == 0 &&
	      transmitter.transmitted_symbols == 0,
	      "empty finish generated or changed a symbol");
	return true;
}

static bool test_contracts(void)
{
	const struct v92_cpd_base_parameters cpd = cpd_parameters();
	struct v92_cpd_base_parameters invalid_cpd = cpd;
	const struct v92_suvd_parameters suvd = {
		.request_silence = true,
	};
	const struct v92_suvd_parameters different_suvd = {
		.acknowledges_cpu = true,
	};
	const struct v90_mp_record empty_mp = { 0 };
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping data_mapping;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_tx saved_transmitter;
	struct pcm_downstream_tx malformed;
	struct pcm_downstream_symbol symbol;
	struct pcm_downstream_symbol saved_symbol;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 1, 0) &&
	      make_mapping(
		      &data_mapping, V90_CP_DATA, G711_LAW_PCMU, 1, 0) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &data_mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_PARAMETERS &&
	      pcm_downstream_tx_init(NULL, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_init(
		      &transmitter, (enum pcm_protocol)99, &mapping) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT,
	      "initialization contract changed");
	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_suvd(&transmitter, &suvd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V92, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_mp(&transmitter, &empty_mp) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_suvd(&transmitter, &suvd) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      !pcm_downstream_tx_is_v92_control_boundary(&transmitter) &&
	      pcm_downstream_tx_start_cpd(&transmitter, NULL) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_start_cpd(NULL, &cpd) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_start_suvd(
		      &transmitter, &different_suvd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_cpd(&transmitter, &cpd) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE,
	      "protocol-specific control branch was not isolated");
	invalid_cpd.four_times_gain_q0_16 = 0u;
	saved_transmitter = transmitter;
	CHECK(pcm_downstream_tx_start_cpd(
		      &transmitter, &invalid_cpd) ==
		      PCM_DOWNSTREAM_TX_INVALID_PARAMETERS &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      !pcm_downstream_tx_is_v92_control_boundary(NULL),
	      "invalid CPd control was accepted");
	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &empty_mp) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "invalid MP fixture did not initialize");
	malformed = transmitter;
	malformed.control.mp.type = (enum v90_mp_record_type)99;
	malformed.cleardown_records = 1u;
	CHECK(invalid_transmitter_is_rejected(malformed) &&
	      !pcm_downstream_tx_mp_starts_next_frame(&malformed),
	      "invalid MP type reached cleardown or MP boundary");
	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "corruption fixture did not generate");
	transmitter.queue[0].pcm.octet ^= 1u;
	saved_transmitter = transmitter;
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	CHECK(!pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "corrupt queue was accepted or changed");

	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "numeric fixture did not initialize");
	transmitter.encoder.mapping.shaping_a2_q1_6 = 64;
	transmitter.encoder.shaper.shaping_a2_q1_6 = 64;
	transmitter.encoder.shaper.previous_v = DBL_MAX;
	saved_transmitter = transmitter;
	CHECK(pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_NUMERIC_FAILURE &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "numeric failure was not transactional");

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 0, 0) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "overflow fixture did not initialize");
	transmitter.accepted_frames =
		UINT64_MAX / V90_CP_FRAME_INTERVALS;
	transmitter.transmitted_symbols = transmitter.accepted_frames *
					  V90_CP_FRAME_INTERVALS;
	saved_transmitter = transmitter;
	CHECK(pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_next(&transmitter, &symbol) ==
		      PCM_DOWNSTREAM_TX_OUT_OF_RANGE &&
	      memcmp(&transmitter, &saved_transmitter,
		     sizeof(transmitter)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      !pcm_downstream_tx_is_valid(NULL) &&
	      strcmp(pcm_downstream_tx_result_name(PCM_DOWNSTREAM_TX_OK),
		     "ok") == 0 &&
	      strcmp(pcm_downstream_tx_result_name(
			     (enum pcm_downstream_tx_result)99),
		     "unknown") == 0,
	      "overflow or result-name contract changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

static bool verify_zero_rate_cleardown(unsigned int law, uint8_t lookahead,
	unsigned int replacement)
{
	const struct v90_mp_record mp = {
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 0u,
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_rate_capability_mask =
				V90_MP_RATE_CAPABILITY_MASK,
		},
	};

	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx tx;
	struct pcm_downstream_rx rx;
	struct v90_mp_record initial = mp;
	size_t sequence_bits = 0u;
	unsigned int received_records = 0u;
	unsigned int preceding_records = 0u;
	unsigned int symbols = 0u;
	bool completed = false;
	bool pending = false;
	bool replaced = replacement == 0u;

	if (replacement != 0u)
		initial.parameters.type0.maximum_upstream_rate_2400_bps_units = 12u;

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING,
		      (enum g711_law)law, 3u, lookahead) &&
	      pcm_downstream_tx_init(
		      &tx, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&tx, &initial) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(
		      &rx, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      v90_mp_sequence_size(
		      mp.type, mapping.frame_bits, &sequence_bits) ==
		      V90_MP_OK,
	      "cleardown fixture did not initialize");
	if (replacement == 0u)
		CHECK(pcm_downstream_tx_request_mp_prime(&tx) ==
			      PCM_DOWNSTREAM_TX_INVALID_STATE,
		      "cleardown entered data acknowledgement");
	else if (replacement == 2u)
		CHECK(pcm_downstream_tx_request_mp_prime(&tx) == PCM_DOWNSTREAM_TX_OK,
		      "MP-prime fixture did not start");
	/* A replacement follows initial conditioning and a complete MP; the
	 * two zero-rate records have their own bounded duration after that. */
	for (; symbols < V90_PHASE4_TRN2D_WAIT_CP_SYMBOLS + 4096u; ++symbols) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;
		enum pcm_downstream_tx_result sent;
		enum pcm_downstream_rx_result received;
		enum pcm_downstream_tx_stage target = replacement == 1u ?
			PCM_DOWNSTREAM_TX_MP : PCM_DOWNSTREAM_TX_MP_PRIME;

		if (!replaced && !pending && tx.stage == target && tx.control_bit_index != 0u) {
			struct pcm_downstream_tx before = tx;

			CHECK(pcm_downstream_tx_start_mp(&tx, &mp) == PCM_DOWNSTREAM_TX_INVALID_STATE &&
			      memcmp(&before, &tx, sizeof(tx)) == 0,
			      "mid-record cleardown changed the current MP");
			pending = true;
		}
		if (pending && tx.control_bit_index == 0u) {
			struct pcm_downstream_tx before = tx;

			CHECK(pcm_downstream_tx_start_mp(&tx, &mp) == PCM_DOWNSTREAM_TX_OK &&
			      memcmp(&before.encoder, &tx.encoder, sizeof(tx.encoder)) == 0 &&
			      memcmp(&before.scrambler, &tx.scrambler, sizeof(tx.scrambler)) == 0 &&
			      memcmp(before.queue, tx.queue, sizeof(tx.queue)) == 0 &&
			      before.queue_count == tx.queue_count &&
			      before.accepted_frames == tx.accepted_frames &&
			      before.transmitted_symbols == tx.transmitted_symbols,
			      "boundary cleardown reset or discarded encoded history");
			pending = false;
			replaced = true;
		}
		sent = pcm_downstream_tx_next(&tx, &symbol);

		if (sent == PCM_DOWNSTREAM_TX_COMPLETE) {
			completed = true;
			break;
		}
		CHECK(sent == PCM_DOWNSTREAM_TX_OK,
		      "cleardown transmitter failed");
		received = pcm_downstream_rx_receive(
			&rx, symbol.pcm.octet, &event);
		if ((received == PCM_DOWNSTREAM_RX_MP || received == PCM_DOWNSTREAM_RX_MP_PRIME) &&
		    event.mp.record.parameters.type0.maximum_upstream_rate_2400_bps_units != 0u) {
			CHECK(received_records == 0u &&
			      v90_mp_record_parameters_equal(&event.mp.record, &initial),
			      "preceding MP was damaged by cancellation");
			preceding_records++;
		} else if (received == PCM_DOWNSTREAM_RX_MP) {
			CHECK(v90_mp_record_equal(&event.mp.record, &mp),
			      "cleardown rate record changed");
			received_records++;
		} else {
			CHECK(received == PCM_DOWNSTREAM_RX_OK ||
			      received == PCM_DOWNSTREAM_RX_BUFFERING ||
			      received == PCM_DOWNSTREAM_RX_TRN2D_FRAME,
			      "cleardown emitted an unexpected control event");
		}
	}
	CHECK(completed && replaced && received_records == 2u &&
	      preceding_records == replacement &&
	      symbols == ((replacement == 1u ? V90_PHASE4_TRN2D_WAIT_CP_FRAMES :
			   V90_PHASE4_TRN2D_INITIAL_FRAMES) +
			  (2u + replacement) * sequence_bits / mapping.frame_bits) *
			 V90_CP_FRAME_INTERVALS &&
	      pcm_downstream_tx_is_complete(&tx) &&
	      tx.queue_count == 0u,
	      "cleardown truncated its records or spectral tail");
	return true;
}

static bool test_zero_rate_cleardown(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t lookahead = 0u; lookahead <= 3u; ++lookahead)
			for (unsigned int replacement = 0u; replacement <= 2u; ++replacement)
				CHECK(verify_zero_rate_cleardown(law, lookahead, replacement),
				      "law %u lookahead %u replacement %u cleardown failed",
				      law, lookahead, replacement);
	return true;
}

static bool verify_conditioning(enum g711_law law, uint8_t redundancy,
	uint8_t lookahead, unsigned int cp_arrival)
{
	const struct v90_mp_record mp = {
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12u,
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_rate_capability_mask = V90_MP_RATE_CAPABILITY_MASK,
		},
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_tx tx;
	struct v90_pcm_stream_rx rx;
	uint64_t expected_frames = cp_arrival == 1u ?
		V90_PHASE4_TRN2D_INITIAL_FRAMES : V90_PHASE4_TRN2D_WAIT_CP_FRAMES;
	bool requested = false;
	bool mp_started = false;

	CHECK(make_mapping(&mapping, V90_CP_TRAINING, law, redundancy, lookahead) &&
	      pcm_downstream_tx_init(&tx, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      v90_pcm_stream_rx_init(&rx, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_PCM_STREAM_RX_OK &&
	      pcm_downstream_tx_start_mp(&tx, &mp) == PCM_DOWNSTREAM_TX_OK,
	      "conditioning fixture did not initialize");
	CHECK(!pcm_downstream_tx_mp_starts_next_frame(NULL) &&
	      !pcm_downstream_tx_mp_starts_next_frame(&tx),
	      "MP boundary accepted an absent or unconditioned transmitter");
	for (unsigned int symbols = 0u; symbols < 16000u; ++symbols) {
		struct pcm_downstream_symbol symbol;

		/* CP can arrive before conditioning, or while delayed PCM output
		 * is queued after the shorter interval has already elapsed. */
		if (!requested && (cp_arrival == 1u ||
		    (cp_arrival == 2u && symbols > 9000u && tx.queue_count != 0u))) {
			struct pcm_downstream_tx before = tx;

			CHECK(pcm_downstream_tx_request_mp_prime(&tx) == PCM_DOWNSTREAM_TX_OK &&
			      memcmp(&before.encoder, &tx.encoder, sizeof(tx.encoder)) == 0 &&
			      memcmp(&before.scrambler, &tx.scrambler, sizeof(tx.scrambler)) == 0 &&
			      memcmp(before.queue, tx.queue, sizeof(tx.queue)) == 0 &&
			      before.queue_count == tx.queue_count &&
			      before.accepted_frames == tx.accepted_frames,
			      "CP discarded queued PCM or restarted conditioning");
			if (cp_arrival == 2u)
				expected_frames = tx.accepted_frames;
			requested = true;
		}
		CHECK(pcm_downstream_tx_mp_starts_next_frame(&tx) ==
		      (!mp_started && tx.accepted_frames >= expected_frames && tx.queue_count == 0u),
		      "MP boundary ignored conditioning or queued output");
		CHECK(pcm_downstream_tx_next(&tx, &symbol) == PCM_DOWNSTREAM_TX_OK,
		      "conditioning stopped at symbol %u", symbols);
		if (!mp_started && tx.stage == PCM_DOWNSTREAM_TX_MP) {
			CHECK(tx.accepted_frames == expected_frames + 1u &&
			      requested == (cp_arrival != 0u),
			      "CP arrival %u: MP began after %llu frames, expected %llu",
			      cp_arrival, (unsigned long long)(tx.accepted_frames - 1u),
			      (unsigned long long)expected_frames);
			mp_started = true;
		}
		/* Observe actual emitted codewords, not just the coder's stage:
		 * shaping look-ahead can still contain trailing TRN2d. MP's first
		 * zero source bit must arrive before the two-second wire deadline. */
		struct v90_pcm_stream_frame frame;
		enum v90_pcm_stream_rx_result result =
			v90_pcm_stream_rx_receive_octet(&rx, symbol.pcm.octet, &frame);
		CHECK(result == V90_PCM_STREAM_RX_BUFFERING ||
		      result == V90_PCM_STREAM_RX_FRAME_READY,
		      "conditioning wire decoder failed");
		if (result == V90_PCM_STREAM_RX_FRAME_READY)
			for (unsigned int bit = 0u; bit < frame.bit_count; ++bit)
				if (frame.bits[bit] == 0u) {
					CHECK(mp_started && rx.received_frames > expected_frames,
					      "wire MP truncated the training interval");
					return true;
				}
	}
	CHECK(false, "MP missed its two-second deadline");
}

static bool test_conditioning(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (uint8_t redundancy = 0u; redundancy <= 3u; ++redundancy)
			for (uint8_t lookahead = 0u; lookahead <= 3u; ++lookahead)
				for (unsigned int cp_arrival = 0u; cp_arrival <= 2u; ++cp_arrival)
					CHECK(verify_conditioning(law, redundancy, lookahead, cp_arrival),
					      "law %u redundancy %u lookahead %u CP arrival %u failed",
					      law, redundancy, lookahead, cp_arrival);
	return true;
}

int main(void)
{
	static const struct test_case tests[] = {
		{ "generated sequences", test_generated_sequences },
		{ "sparse training wire digests", test_sparse_training_wire_digests },
		{ "V.92 sequences", test_v92_sequences },
		{ "V.92 parameter exchange", test_v92_parameter_exchange },
		{ "V.92 parameter exchange contracts", test_v92_parameter_exchange_contracts },
		{ "V.92 switch contracts", test_v92_switch_contracts },
		{ "source-record contracts", test_source_record_contracts },
		{ "empty finish", test_empty_finish },
		{ "zero-rate cleardown", test_zero_rate_cleardown },
		{ "CP-dependent conditioning", test_conditioning },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
