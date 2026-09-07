// SPDX-License-Identifier: BSD-3-Clause

#include <v90_downstream_data_handoff.h>
#include <v90_downstream_data_stream.h>
#include <v90_downstream_rrn_rx.h>
#include <pcm_downstream_rx.h>
#include <pcm_downstream_tx.h>

#include "test.h"

#include <string.h>

enum {
	FRAME_BITS = 17,
	MAXIMUM_STREAM_SYMBOLS = V90_PHASE4_TRN2D_INITIAL_SYMBOLS + 4096,
	DATA_FRAMES = 8,
	MAXIMUM_DATA_FRAMES = 16,
	MAXIMUM_DATA_SYMBOLS =
		(V90_PHASE4_B1D_FRAMES + MAXIMUM_DATA_FRAMES) *
		V90_CP_FRAME_INTERVALS,
};

struct data_source {
	uint8_t frames[MAXIMUM_DATA_FRAMES][V90_PCM_FRAME_MAX_BITS];
	size_t frame_count;
	size_t bit_count;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping_with_shaping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	enum v90_cp_kind kind, uint8_t sign_redundancy,
	uint8_t spectral_lookahead)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.shaping_a1_q1_6 = 29,
		.shaping_a2_q1_6 = -13,
		.shaping_b1_q1_6 = 43,
		.shaping_b2_q1_6 = -5,
		.selected_rate_index = kind == V90_CP_TRAINING ?
			FRAME_BITS - 8 : 1,
		.sign_redundancy = sign_redundancy,
		.spectral_lookahead = spectral_lookahead,
		.constellation_count = 1,
	};

	for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	enum v90_cp_kind kind)
{
	return make_mapping_with_shaping(mapping, law, kind, 2, 2);
}

static bool make_rd_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_DATA,
		.law = law,
		.shaping_a1_q1_6 = 29,
		.shaping_a2_q1_6 = -13,
		.shaping_b1_q1_6 = 43,
		.shaping_b2_q1_6 = -5,
		.selected_rate_index = 1,
		.sign_redundancy = 2,
		.spectral_lookahead = 2,
		.constellation_count = V90_CP_FRAME_INTERVALS,
		.codec_constellations_differ = true,
	};

	for (uint8_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		parameters.frame_constellation[interval] = interval;
		for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
			add_ucode(
				&parameters.transmit_constellations[interval],
				(uint8_t)(12u + interval % 3u +
					  14u * ordinal));
		for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
			add_ucode(
				&parameters.codec_constellations[interval],
				(uint8_t)(9u + 2u * (interval % 3u) +
					  14u * ordinal));
	}
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static struct v90_mp_record make_record(bool type1)
{
	if (!type1) {
		return (struct v90_mp_record){
			.type = V90_MP_RECORD_TYPE0,
			.parameters.type0 = {
				.maximum_upstream_rate_2400_bps_units = 12,
				.upstream_trellis = V34_TRELLIS_32_STATE,
				.upstream_nonlinear_encoding = true,
				.upstream_expanded_shaping = true,
				.upstream_rate_capability_mask = 0x1fffu,
			},
		};
	}
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_upstream_rate_2400_bps_units = 14,
				.upstream_trellis = V34_TRELLIS_64_STATE,
				.upstream_rate_capability_mask = 0x1555u,
			},
			.precoder_coefficients = {
				{ .real = 1, .imaginary = -1 },
				{ .real = 8192, .imaginary = -8192 },
				{ .real = INT16_MAX, .imaginary = -32767 },
			},
		},
	};
}

static void set_acknowledgment(
	struct v90_mp_record *record, bool acknowledged)
{
	if (record->type == V90_MP_RECORD_TYPE0)
		record->parameters.type0.acknowledges_cp = acknowledged;
	else
		record->parameters.type1.common.acknowledges_cp =
			acknowledged;
}

static bool records_match(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	uint8_t left_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	uint8_t right_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t left_bits = 0;
	size_t right_bits = 0;

	return v90_mp_encode(
		       left, FRAME_BITS, left_sequence,
		       sizeof(left_sequence), &left_bits) == V90_MP_OK &&
	       v90_mp_encode(
		       right, FRAME_BITS, right_sequence,
		       sizeof(right_sequence), &right_bits) == V90_MP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_sequence, right_sequence,
		      (left_bits + 7u) / 8u) == 0;
}

static bool check_training_frame(const struct pcm_trn2d_frame *frame)
{
	if (frame->bit_count != FRAME_BITS ||
	    frame->training_bit_errors != 0)
		return false;
	for (size_t bit = 0; bit < frame->bit_count; ++bit) {
		if (frame->bits[bit] != 1)
			return false;
	}
	return true;
}

static void make_data_bits(
	uint8_t *bits, size_t bit_count, size_t frame)
{
	for (size_t bit = 0; bit < bit_count; ++bit)
		bits[bit] = (uint8_t)(
			(frame * 13u + bit * 7u + (frame >> (bit % 3u))) & 1u);
}

static bool source_data_frame(
	void *context, uint8_t *bits, size_t bit_count)
{
	struct data_source *source = context;

	if (source == NULL || bits == NULL || bit_count != source->bit_count ||
	    source->frame_count >= MAXIMUM_DATA_FRAMES)
		return false;
	make_data_bits(bits, bit_count, source->frame_count + 1u);
	memcpy(source->frames[source->frame_count], bits, bit_count);
	source->frame_count++;
	return true;
}

static bool reject_data_frame(
	void *context, uint8_t *bits, size_t bit_count)
{
	(void)context;
	(void)bits;
	(void)bit_count;
	return false;
}

static bool leave_data_frame_invalid(
	void *context, uint8_t *bits, size_t bit_count)
{
	(void)context;
	(void)bits;
	(void)bit_count;
	return true;
}

static bool verify_data_stream(
	const struct pcm_downstream_tx *completed_transmit,
	const struct pcm_downstream_rx *completed_receive,
	const struct v90_pcm_mapping *data_mapping)
{
	struct v90_downstream_data_stream transmit;
	struct v90_downstream_data_rx receive;
	struct data_source source = {
		.bit_count = data_mapping->frame_bits,
	};
	size_t b1d_frames = 0;
	size_t data_frames = 0;
	size_t symbols = 0;

	CHECK(v90_downstream_data_stream_start_after_ed(
		      &transmit, completed_transmit, data_mapping) ==
		      V90_DOWNSTREAM_DATA_STREAM_OK &&
	      v90_downstream_data_rx_start_after_ed(
		      &receive, completed_receive, data_mapping) ==
		      V90_DOWNSTREAM_DATA_RX_OK &&
	      !v90_downstream_data_stream_is_online(&transmit),
	      "post-Ed character stream did not start at B1d");
	while (data_frames < DATA_FRAMES && symbols < MAXIMUM_DATA_SYMBOLS) {
		struct v90_pcm_symbol symbol;
		struct v90_pcm_stream_frame frame;
		size_t source_count = source.frame_count;
		enum v90_downstream_data_rx_result result;

		CHECK(v90_downstream_data_stream_next(
			      &transmit, source_data_frame, &source, &symbol) ==
			      V90_DOWNSTREAM_DATA_STREAM_OK &&
		      v90_pcm_symbol_is_valid(&symbol, data_mapping->law),
		      "data stream stopped at network symbol %zu", symbols);
		CHECK(source.frame_count - source_count <= 1,
		      "one network symbol acquired multiple data frames");
		symbols++;
		CHECK(v90_downstream_data_stream_is_online(&transmit) ==
		      (symbols >= V90_PHASE4_B1D_FRAMES *
				  V90_CP_FRAME_INTERVALS),
		      "B1d network boundary moved to symbol %zu", symbols);
		result = v90_downstream_data_rx_receive_octet(
			&receive, symbol.octet, &frame);
		if (result == V90_DOWNSTREAM_DATA_RX_BUFFERING)
			continue;
		if (result == V90_DOWNSTREAM_DATA_RX_B1D_FRAME ||
		    result == V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE) {
			for (size_t bit = 0; bit < frame.bit_count; ++bit)
				CHECK(frame.bits[bit] == 1,
				      "B1d frame %zu bit %zu changed",
				      b1d_frames, bit);
			b1d_frames++;
			CHECK((result ==
			       V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE) ==
			      (b1d_frames == V90_PHASE4_B1D_FRAMES),
			      "B1d receive boundary moved to frame %zu",
			      b1d_frames);
			continue;
		}
		CHECK(result == V90_DOWNSTREAM_DATA_RX_DATA_FRAME &&
		      data_frames < source.frame_count &&
		      frame.bit_count == data_mapping->frame_bits &&
		      memcmp(frame.bits, source.frames[data_frames],
			     frame.bit_count) == 0,
		      "data frame %zu changed: %s", data_frames,
		      v90_downstream_data_rx_result_name(result));
		data_frames++;
	}
	CHECK(symbols < MAXIMUM_DATA_SYMBOLS &&
	      b1d_frames == V90_PHASE4_B1D_FRAMES &&
	      data_frames == DATA_FRAMES &&
	      source.frame_count >= data_frames &&
	      source.frame_count <= MAXIMUM_DATA_FRAMES &&
	      receive.b1d_bit_errors == 0,
	      "data stream stopped at %zu B1d/%zu data frames from %zu sources",
	      b1d_frames, data_frames, source.frame_count);

	while (transmit.queue_count != 0) {
		struct v90_pcm_symbol discarded;

		CHECK(v90_downstream_data_stream_next(
			      &transmit, source_data_frame, &source,
			      &discarded) == V90_DOWNSTREAM_DATA_STREAM_OK,
		      "data stream queue did not drain");
	}
	{
		struct v90_downstream_data_stream saved = transmit;
		struct v90_pcm_symbol output;
		struct v90_pcm_symbol untouched;

		memset(&untouched, 0xa5, sizeof(untouched));
		output = untouched;
		CHECK(v90_downstream_data_stream_next(
			      &transmit, reject_data_frame, NULL, &output) ==
			      V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE &&
		      memcmp(&transmit, &saved, sizeof(transmit)) == 0 &&
		      memcmp(&output, &untouched, sizeof(output)) == 0 &&
		      v90_downstream_data_stream_next(
			      &transmit, leave_data_frame_invalid, NULL,
			      &output) ==
			      V90_DOWNSTREAM_DATA_STREAM_SOURCE_FAILURE &&
		      memcmp(&transmit, &saved, sizeof(transmit)) == 0 &&
		      memcmp(&output, &untouched, sizeof(output)) == 0,
		      "rejected frame source changed stream state or output");
		CHECK(v90_downstream_data_stream_next(
			      &transmit, NULL, NULL, &output) ==
			      V90_DOWNSTREAM_DATA_STREAM_OK &&
		      v90_pcm_symbol_is_valid(&output, data_mapping->law),
		      "idle frame source did not resume the stream");
	}
	{
		struct v90_pcm_symbol output;
		struct v90_pcm_symbol untouched;
		size_t accepted_sources = source.frame_count;
		size_t tail_symbols = 0;

		while (tail_symbols <=
		       V90_CP_FRAME_INTERVALS *
			       (V90_SPECTRAL_MAX_LOOKAHEAD + 2u)) {
			enum v90_downstream_data_stream_result result =
				v90_downstream_data_stream_finish(
					&transmit, &output);

			if (result == V90_DOWNSTREAM_DATA_STREAM_COMPLETE)
				break;
			CHECK(result == V90_DOWNSTREAM_DATA_STREAM_OK &&
			      v90_pcm_symbol_is_valid(
				      &output, data_mapping->law),
			      "data tail stopped after %zu symbols",
			      tail_symbols);
			tail_symbols++;
		}
		CHECK(source.frame_count == accepted_sources &&
		      transmit.queue_count == 0 &&
		      transmit.transmitted_symbols ==
			      transmit.accepted_frames *
				      V90_CP_FRAME_INTERVALS &&
		      transmit.transmitted_symbols %
				      V90_CP_FRAME_INTERVALS == 0 &&
		      v90_downstream_data_tx_is_complete(
			      &transmit.transmitter) &&
		      v90_downstream_data_stream_is_valid(&transmit),
		      "data tail did not end at its accepted frame boundary");
		memset(&untouched, 0xa5, sizeof(untouched));
		output = untouched;
		struct v90_downstream_data_stream saved = transmit;

		CHECK(v90_downstream_data_stream_finish(
			      &transmit, &output) ==
			      V90_DOWNSTREAM_DATA_STREAM_COMPLETE &&
		      v90_downstream_data_stream_next(
			      &transmit, NULL, NULL, &output) ==
			      V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE &&
		      memcmp(&transmit, &saved, sizeof(transmit)) == 0 &&
		      memcmp(&output, &untouched, sizeof(output)) == 0,
		      "completed data tail changed state or output");
	}
	return true;
}

static bool verify_rd_sequence(enum g711_law law)
{
	struct v90_pcm_mapping mapping;
	struct v90_rd_tx transmitter;
	struct v90_pcm_symbol symbol;

	CHECK(make_rd_mapping(&mapping, law) &&
	      v90_rd_tx_init(&transmitter, &mapping) == V90_RD_TX_OK &&
	      v90_rd_tx_is_valid(&transmitter) &&
	      !v90_rd_tx_is_complete(&transmitter),
	      "law %u Rd fixture did not initialize", law);
	for (size_t index = 0; index < V90_RD_SEQUENCE_SYMBOLS; ++index) {
		size_t interval = index % V90_CP_FRAME_INTERVALS;
		uint8_t expected_ucode = 0;
		bool expected_positive =
			interval < V90_CP_FRAME_INTERVALS / 2u;

		if (index >= V90_RD_SYMBOLS)
			expected_positive = !expected_positive;
		CHECK(v90_pcm_mapping_ucode(
			      &mapping, V90_PCM_MAPPING_TRANSMIT,
			      interval, 0, &expected_ucode) ==
			      V90_PCM_MAPPING_OK &&
		      v90_rd_tx_next(&transmitter, &symbol) ==
			      V90_RD_TX_OK &&
		      symbol.ucode == expected_ucode &&
		      symbol.positive == expected_positive &&
		      v90_pcm_symbol_is_valid(&symbol, law),
		      "law %u Rd symbol %zu changed", law, index);
	}
	CHECK(v90_rd_tx_is_complete(&transmitter) &&
	      transmitter.transmitted_symbols == V90_RD_SYMBOLS +
		      V90_RD_BAR_SYMBOLS,
	      "law %u Rd/Rd-bar length changed", law);
	{
		struct v90_rd_tx saved = transmitter;
		struct v90_pcm_symbol untouched;

		memset(&untouched, 0xa5, sizeof(untouched));
		symbol = untouched;
		CHECK(v90_rd_tx_next(&transmitter, &symbol) ==
			      V90_RD_TX_COMPLETE &&
		      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
		      memcmp(&symbol, &untouched, sizeof(symbol)) == 0,
		      "law %u completed Rd changed state or output", law);
	}
	return true;
}

static bool test_rd_sequence(void)
{
	CHECK(verify_rd_sequence(G711_LAW_PCMU),
	      "PCMU Rd sequence failed");
	CHECK(verify_rd_sequence(G711_LAW_PCMA),
	      "PCMA Rd sequence failed");
	return true;
}

static bool make_rrn_octet(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, uint8_t interval,
	bool inverted, bool bar, uint8_t *octet)
{
	uint8_t ucode;
	bool positive = interval < V90_CP_FRAME_INTERVALS / 2u;

	if (v90_pcm_mapping_ucode(
		    mapping, plane, interval, 0, &ucode) !=
	    V90_PCM_MAPPING_OK)
		return false;
	if (inverted)
		positive = !positive;
	if (bar)
		positive = !positive;
	return g711_ucode_octet(mapping->law, ucode, positive, octet);
}

static bool verify_rrn_detection(
	enum g711_law law, enum v90_pcm_mapping_plane plane,
	uint8_t offset, bool inverted)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_rrn_rx receiver;
	uint8_t octet;
	size_t rd_symbols = V90_RD_SYMBOLS - offset;

	CHECK(make_rd_mapping(&mapping, law) &&
	      v90_downstream_rrn_rx_init(&receiver, &mapping, plane) ==
		      V90_DOWNSTREAM_RRN_RX_OK &&
	      v90_downstream_rrn_rx_is_valid(&receiver) &&
	      !v90_downstream_rrn_rx_rd_detected(&receiver) &&
	      !v90_downstream_rrn_rx_is_complete(&receiver),
	      "law %u plane %u offset %u polarity %u RRN init failed",
	      law, plane, offset, inverted);
	for (size_t index = 0; index < rd_symbols; ++index) {
		uint8_t interval =
			(uint8_t)((offset + index) %
				  V90_CP_FRAME_INTERVALS);
		enum v90_downstream_rrn_rx_result expected =
			index + 1u == V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS ?
				V90_DOWNSTREAM_RRN_RX_RD_DETECTED :
				V90_DOWNSTREAM_RRN_RX_BUFFERING;

		CHECK(make_rrn_octet(
			      &mapping, plane, interval, inverted, false,
			      &octet) &&
		      v90_downstream_rrn_rx_receive(&receiver, octet) ==
			      expected,
		      "law %u plane %u offset %u polarity %u Rd %zu "
		      "failed",
		      law, plane, offset, inverted, index);
	}
	CHECK(v90_downstream_rrn_rx_rd_detected(&receiver) &&
	      !v90_downstream_rrn_rx_is_complete(&receiver),
	      "law %u plane %u offset %u polarity %u did not lock Rd",
	      law, plane, offset, inverted);
	for (size_t index = 0;
	     index < V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS; ++index) {
		enum v90_downstream_rrn_rx_result expected =
			index == 0 ? V90_DOWNSTREAM_RRN_RX_RD_BAR_STARTED :
			index + 1u == V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS ?
				V90_DOWNSTREAM_RRN_RX_RD_BAR_COMPLETE :
				V90_DOWNSTREAM_RRN_RX_BUFFERING;

		CHECK(make_rrn_octet(
			      &mapping, plane,
			      (uint8_t)(index % V90_CP_FRAME_INTERVALS),
			      inverted, true, &octet) &&
		      v90_downstream_rrn_rx_receive(&receiver, octet) ==
			      expected,
		      "law %u plane %u offset %u polarity %u Rd-bar %zu "
		      "failed",
		      law, plane, offset, inverted, index);
	}
	CHECK(v90_downstream_rrn_rx_is_complete(&receiver),
	      "law %u plane %u offset %u polarity %u did not complete",
	      law, plane, offset, inverted);
	{
		struct v90_downstream_rrn_rx saved = receiver;

		CHECK(v90_downstream_rrn_rx_receive(&receiver, octet) ==
			      V90_DOWNSTREAM_RRN_RX_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "completed RRN receiver consumed the TRN2d boundary");
	}
	return true;
}

static bool test_rrn_detection(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (enum v90_pcm_mapping_plane plane =
			     V90_PCM_MAPPING_TRANSMIT;
		     plane <= V90_PCM_MAPPING_CODEC; ++plane) {
			for (uint8_t offset = 0;
			     offset < V90_CP_FRAME_INTERVALS; ++offset) {
				CHECK(verify_rrn_detection(
					      law, plane, offset, false) &&
				      verify_rrn_detection(
					      law, plane, offset, true),
				      "RRN detector matrix failed");
			}
		}
	}
	return true;
}

static bool test_rrn_rejection(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_rrn_rx receiver;
	uint8_t octet;

	CHECK(make_rd_mapping(&mapping, G711_LAW_PCMU) &&
	      v90_downstream_rrn_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RRN_RX_OK,
	      "RRN rejection fixture did not initialize");
	for (size_t index = 0; index < 234u; ++index) {
		CHECK(make_rrn_octet(
			      &mapping, V90_PCM_MAPPING_TRANSMIT,
			      (uint8_t)(index % V90_CP_FRAME_INTERVALS),
			      false, false, &octet) &&
		      v90_downstream_rrn_rx_receive(&receiver, octet) ==
			      V90_DOWNSTREAM_RRN_RX_BUFFERING,
		      "short Rd failed at %zu", index);
	}
	for (size_t index = 0;
	     index < V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS; ++index) {
		CHECK(make_rrn_octet(
			      &mapping, V90_PCM_MAPPING_TRANSMIT,
			      (uint8_t)(index % V90_CP_FRAME_INTERVALS),
			      false, true, &octet) &&
		      v90_downstream_rrn_rx_receive(&receiver, octet) ==
			      V90_DOWNSTREAM_RRN_RX_BUFFERING,
		      "short Rd-bar failed at %zu", index);
	}
	CHECK(!v90_downstream_rrn_rx_rd_detected(&receiver),
	      "short Rd/Rd-bar sequence was accepted");
	CHECK(v90_downstream_rrn_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RRN_RX_OK,
	      "RRN mismatch fixture did not reset");
	for (size_t index = 0;
	     index < V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS; ++index) {
		enum v90_downstream_rrn_rx_result expected =
			index + 1u == V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS ?
				V90_DOWNSTREAM_RRN_RX_RD_DETECTED :
				V90_DOWNSTREAM_RRN_RX_BUFFERING;

		CHECK(make_rrn_octet(
			      &mapping, V90_PCM_MAPPING_TRANSMIT,
			      (uint8_t)(index % V90_CP_FRAME_INTERVALS),
			      false, false, &octet) &&
		      v90_downstream_rrn_rx_receive(&receiver, octet) ==
			      expected,
		      "RRN mismatch fixture stopped at %zu", index);
	}
	CHECK(make_rrn_octet(
		      &mapping, V90_PCM_MAPPING_TRANSMIT, 1,
		      false, true, &octet) &&
	      v90_downstream_rrn_rx_receive(&receiver, octet) ==
		      V90_DOWNSTREAM_RRN_RX_SEQUENCE_MISMATCH &&
	      v90_downstream_rrn_rx_is_valid(&receiver) &&
	      !v90_downstream_rrn_rx_rd_detected(&receiver),
	      "off-boundary Rd-bar transition was accepted");
	return true;
}

static bool test_rrn_contracts(void)
{
	struct v90_pcm_mapping training;
	struct v90_pcm_mapping data;
	struct v90_downstream_rrn_rx receiver;
	struct v90_downstream_rrn_rx saved;

	CHECK(make_mapping(
		      &training, G711_LAW_PCMU, V90_CP_TRAINING) &&
	      make_rd_mapping(&data, G711_LAW_PCMU),
	      "RRN contract mappings did not initialize");
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v90_downstream_rrn_rx_init(
		      NULL, &data, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT &&
	      v90_downstream_rrn_rx_init(
		      &receiver, NULL, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT &&
	      v90_downstream_rrn_rx_init(
		      &receiver, &training, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS &&
	      v90_downstream_rrn_rx_init(
		      &receiver, &data,
		      (enum v90_pcm_mapping_plane)2) ==
		      V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      v90_downstream_rrn_rx_receive(NULL, 0) ==
		      V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT &&
	      v90_downstream_rrn_rx_receive(&receiver, 0) ==
		      V90_DOWNSTREAM_RRN_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "RRN receiver accepted an invalid contract");
	return true;
}

static bool run_stream(
	enum g711_law law, bool type1, bool request_during_trn2d,
	uint8_t training_redundancy, uint8_t training_lookahead)
{
	struct v90_pcm_mapping mapping;
	struct v90_mp_record record = make_record(type1);
	struct v90_mp_record offered = record;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_rx receiver;
	size_t training_frames = 0;
	size_t mp_records = 0;
	size_t first_mp_symbol = 0;
	size_t prime_records = 0;
	size_t expected_prime_records = training_lookahead == 0 ? 2u : 3u;
	size_t symbols = 0;
	bool prime_requested = false;
	bool ed_requested = false;
	bool ed_received = false;
	bool refined = false;
	bool frozen_checked = false;

	CHECK(make_mapping_with_shaping(
		      &mapping, law, V90_CP_TRAINING,
		      training_redundancy, training_lookahead) &&
	      mapping.frame_bits == FRAME_BITS &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "stream fixture did not initialize");
	CHECK(v90_mp_record_limit_maximum_upstream_rate(&record, type1 ? 12u : 10u),
	      "refined MP did not select the lower rate");
	if (request_during_trn2d) {
		CHECK(pcm_downstream_tx_request_mp_prime(&transmitter) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "early MP prime request failed");
		prime_requested = true;
	}

	while (!pcm_downstream_tx_is_complete(&transmitter) &&
	       symbols < MAXIMUM_STREAM_SYMBOLS) {
		struct pcm_downstream_symbol symbol;
		struct pcm_downstream_event event;
		enum pcm_downstream_tx_result transmit_result;
		enum pcm_downstream_rx_result result;

		if (!refined && transmitter.accepted_frames >=
				V90_PHASE4_TRN2D_INITIAL_FRAMES) {
			CHECK(transmitter.stage == PCM_DOWNSTREAM_TX_TRN2D &&
			      pcm_downstream_tx_start_mp(&transmitter, &record) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "MP could not be refined after conditioning");
			refined = true;
		}
		transmit_result =
			pcm_downstream_tx_next(&transmitter, &symbol);
		if (transmit_result == PCM_DOWNSTREAM_TX_COMPLETE)
			break;
		CHECK(transmit_result == PCM_DOWNSTREAM_TX_OK,
		      "stream transmitter stopped at symbol %zu", symbols);
		if (!frozen_checked && transmitter.stage == PCM_DOWNSTREAM_TX_MP) {
			struct pcm_downstream_tx saved = transmitter;

			CHECK(refined &&
			      pcm_downstream_tx_start_mp(&transmitter, &offered) ==
				      PCM_DOWNSTREAM_TX_INVALID_STATE &&
			      memcmp(&transmitter, &saved, sizeof(saved)) == 0,
			      "MP changed after its first source frame entered the coder");
			frozen_checked = true;
		}
		symbols++;
		if (!prime_requested &&
		    transmitter.stage == PCM_DOWNSTREAM_TX_MP &&
		    transmitter.control_bit_index != 0) {
			CHECK(pcm_downstream_tx_request_mp_prime(&transmitter) ==
				      PCM_DOWNSTREAM_TX_OK &&
			      pcm_downstream_tx_request_mp_prime(&transmitter) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "mid-record MP prime request failed");
			prime_requested = true;
		}
		result = pcm_downstream_rx_receive(
			&receiver, symbol.pcm.octet, &event);
		if (result == PCM_DOWNSTREAM_RX_BUFFERING)
			continue;
		if (result == PCM_DOWNSTREAM_RX_TRN2D_FRAME) {
			CHECK(check_training_frame(&event.trn2d),
			      "TRN2d frame %zu changed", training_frames);
			training_frames++;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_MP) {
			CHECK(prime_records == 0 &&
			      records_match(&event.mp.record, &record),
			      "MP record %zu changed or followed MP prime",
			      mp_records);
			if (mp_records == 0)
				first_mp_symbol = symbols;
			mp_records++;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_ED) {
			CHECK(ed_requested && !ed_received &&
			      event.ed.bit_count ==
				      V90_PHASE4_ED_FRAMES * FRAME_BITS &&
			      event.ed.bit_errors == 0,
			      "stream Ed event changed");
			ed_received = true;
			continue;
		}
		CHECK(result == PCM_DOWNSTREAM_RX_MP_PRIME,
		      "stream receiver returned %s",
		      pcm_downstream_rx_result_name(result));
		struct v90_mp_record expected = record;

		set_acknowledgment(&expected, true);
		CHECK(mp_records >= 1 &&
		      records_match(&event.mp.record, &expected),
		      "MP prime record %zu changed", prime_records);
		prime_records++;
		if (prime_records == 2) {
			CHECK(pcm_downstream_rx_expect_ed(&receiver) ==
				      PCM_DOWNSTREAM_RX_OK &&
			      pcm_downstream_rx_expect_ed(&receiver) ==
				      PCM_DOWNSTREAM_RX_OK &&
			      pcm_downstream_tx_request_ed(&transmitter) ==
				      PCM_DOWNSTREAM_TX_OK &&
			      pcm_downstream_tx_request_ed(&transmitter) ==
				      PCM_DOWNSTREAM_TX_OK,
			      "Ed requests were not idempotent");
			ed_requested = true;
		}
	}
	CHECK(symbols < MAXIMUM_STREAM_SYMBOLS && refined && frozen_checked &&
	      prime_requested &&
	      ed_requested && ed_received &&
	      training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      first_mp_symbol >= V90_PHASE4_TRN2D_INITIAL_SYMBOLS &&
	      mp_records == 1 && prime_records == expected_prime_records &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_ED &&
	      transmitter.ed_frames == V90_PHASE4_ED_FRAMES &&
	      transmitter.transmitted_symbols == symbols &&
	      receiver.trn2d_bit_errors == 0 &&
	      pcm_downstream_tx_is_complete(&transmitter) &&
	      pcm_downstream_rx_is_complete(&receiver) &&
	      pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_rx_is_valid(&receiver),
	      "continuous Ed boundary changed at %zu symbols: training=%zu "
	      "MP=%zu MP'=%zu Ed=%d/%d stage=%u/%u",
	      symbols, training_frames, mp_records, prime_records,
	      ed_requested, ed_received, transmitter.stage, receiver.stage);
	struct pcm_downstream_tx completed = transmitter;
	struct pcm_downstream_symbol untouched;
	struct pcm_downstream_symbol saved;
	struct v90_pcm_mapping data_mapping;
	struct v90_pcm_mapping unshaped_data_mapping;
	struct v90_pcm_mapping other_law_mapping;
	struct v90_downstream_data_tx data_transmit;
	struct v90_downstream_data_rx data_receive;
	struct v90_downstream_data_tx expected_transmit;
	struct v90_downstream_data_rx expected_receive;
	struct v90_downstream_data_tx rejected_transmit;
	struct v90_downstream_data_rx rejected_receive;
	struct v90_downstream_data_stream rejected_stream;
	struct v90_downstream_data_stream saved_stream;
	struct pcm_downstream_tx incomplete_transmit = transmitter;
	struct pcm_downstream_rx incomplete_receive = receiver;

	memset(&untouched, 0xa5, sizeof(untouched));
	saved = untouched;
	CHECK(pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_COMPLETE &&
	      pcm_downstream_tx_next(&transmitter, &untouched) ==
		      PCM_DOWNSTREAM_TX_COMPLETE &&
	      memcmp(&transmitter, &completed, sizeof(transmitter)) == 0 &&
	      memcmp(&untouched, &saved, sizeof(untouched)) == 0,
	      "completed Ed transmitter changed state or output");
	incomplete_transmit.complete = false;
	incomplete_receive.stage = PCM_DOWNSTREAM_RX_ED_SEARCH;
	incomplete_receive.ed_frames--;
	memset(&rejected_transmit, 0xa5, sizeof(rejected_transmit));
	memset(&rejected_receive, 0x5a, sizeof(rejected_receive));
	memset(&rejected_stream, 0x3c, sizeof(rejected_stream));
	saved_stream = rejected_stream;
	CHECK(make_mapping(&data_mapping, law, V90_CP_DATA),
	      "post-Ed data mapping was invalid");
	CHECK(make_mapping_with_shaping(
		      &unshaped_data_mapping, law, V90_CP_DATA, 0, 0),
	      "post-Ed unshaped data mapping was invalid");
	CHECK(make_mapping(
		      &other_law_mapping,
		      law == G711_LAW_PCMU ? G711_LAW_PCMA : G711_LAW_PCMU,
		      V90_CP_DATA),
	      "post-Ed alternate-law mapping was invalid");
	CHECK(pcm_downstream_tx_is_valid(&incomplete_transmit) &&
	      !pcm_downstream_tx_is_complete(&incomplete_transmit),
	      "post-Ed incomplete transmitter was invalid");
	CHECK(pcm_downstream_rx_is_valid(&incomplete_receive) &&
	      !pcm_downstream_rx_is_complete(&incomplete_receive),
	      "post-Ed incomplete receiver was invalid");
	CHECK(v90_downstream_data_tx_start_after_ed(
		      &data_transmit, &transmitter, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      v90_downstream_data_rx_start_after_ed(
		      &data_receive, &receiver, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_RX_OK &&
	      v90_downstream_data_tx_init(
		      &expected_transmit, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      v90_downstream_data_rx_init(
		      &expected_receive, &data_mapping,
		      receiver.stream.decoder.decision_plane) ==
		      V90_DOWNSTREAM_DATA_RX_OK &&
	      memcmp(&data_transmit, &expected_transmit,
		     sizeof(data_transmit)) == 0 &&
	      memcmp(&data_receive, &expected_receive,
		     sizeof(data_receive)) == 0,
	      "Ed did not start a fresh B1d epoch");
	CHECK(verify_data_stream(&transmitter, &receiver, &data_mapping),
	      "Ed did not continue through one-character B1d/data");
	CHECK(verify_data_stream(
		      &transmitter, &receiver, &unshaped_data_mapping),
	      "unshaped B1d/data character stream failed");
	CHECK(v90_downstream_data_tx_start_after_ed(
		      NULL, &transmitter, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT &&
	      v90_downstream_data_tx_start_after_ed(
		      &rejected_transmit, &incomplete_transmit,
		      &data_mapping) == V90_DOWNSTREAM_DATA_TX_INVALID_STATE &&
	      v90_downstream_data_tx_start_after_ed(
		      &rejected_transmit, &transmitter,
		      &other_law_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS &&
	      v90_downstream_data_rx_start_after_ed(
		      NULL, &receiver, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT &&
	      v90_downstream_data_rx_start_after_ed(
		      &rejected_receive, &incomplete_receive,
		      &data_mapping) == V90_DOWNSTREAM_DATA_RX_INVALID_STATE &&
	      v90_downstream_data_rx_start_after_ed(
		      &rejected_receive, &receiver,
		      &other_law_mapping) ==
		      V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS &&
	      v90_downstream_data_stream_start_after_ed(
		      NULL, &transmitter, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT &&
	      v90_downstream_data_stream_start_after_ed(
		      &rejected_stream, &incomplete_transmit,
		      &data_mapping) ==
		      V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE &&
	      v90_downstream_data_stream_start_after_ed(
		      &rejected_stream, &transmitter,
		      &other_law_mapping) ==
		      V90_DOWNSTREAM_DATA_STREAM_INVALID_PARAMETERS &&
	      memcmp(&rejected_stream, &saved_stream,
		     sizeof(rejected_stream)) == 0,
	      "post-Ed handoff accepted an invalid boundary");
	for (size_t byte = 0; byte < sizeof(rejected_transmit); ++byte)
		CHECK(((const uint8_t *)&rejected_transmit)[byte] == 0xa5,
		      "rejected transmit handoff changed byte %zu", byte);
	for (size_t byte = 0; byte < sizeof(rejected_receive); ++byte)
		CHECK(((const uint8_t *)&rejected_receive)[byte] == 0x5a,
		      "rejected receive handoff changed byte %zu", byte);
	return true;
}

static bool test_continuous_streams(void)
{
	CHECK(run_stream(G711_LAW_PCMU, false, true, 2, 2),
	      "PCMU early-request stream failed");
	CHECK(run_stream(G711_LAW_PCMA, true, false, 2, 2),
	      "PCMA mid-record stream failed");
	CHECK(run_stream(G711_LAW_PCMU, false, false, 0, 0),
	      "PCMU unshaped CPt stream failed");
	CHECK(run_stream(G711_LAW_PCMA, true, true, 0, 0),
	      "PCMA unshaped CPt stream failed");
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping data_mapping;
	struct v90_mp_record record = make_record(false);
	struct v90_mp_record changed = record;
	struct v90_mp_record acknowledged = record;
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_tx saved;
	struct v90_downstream_data_stream stream;
	struct v90_downstream_data_stream saved_stream;
	struct v90_pcm_symbol symbol;
	struct v90_pcm_symbol saved_symbol;
	struct v90_rd_tx rd;
	struct v90_rd_tx saved_rd;

	CHECK(make_mapping(
		      &mapping, G711_LAW_PCMU, V90_CP_TRAINING) &&
	      make_mapping(&data_mapping, G711_LAW_PCMU, V90_CP_DATA) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "contract fixture did not initialize");
	memset(&stream, 0, sizeof(stream));
	CHECK(v90_downstream_data_tx_init(
		      &stream.transmitter, &data_mapping) ==
		      V90_DOWNSTREAM_DATA_TX_OK &&
	      v90_downstream_data_stream_is_valid(&stream),
	      "data-stream contract fixture did not initialize");
	saved_stream = stream;
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	CHECK(v90_downstream_data_stream_finish(NULL, &symbol) ==
		      V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT &&
	      v90_downstream_data_stream_finish(&stream, NULL) ==
		      V90_DOWNSTREAM_DATA_STREAM_INVALID_ARGUMENT &&
	      v90_downstream_data_stream_finish(&stream, &symbol) ==
		      V90_DOWNSTREAM_DATA_STREAM_INVALID_STATE &&
	      memcmp(&stream, &saved_stream, sizeof(stream)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "data tail accepted an invalid pre-data boundary");
	memset(&rd, 0xa5, sizeof(rd));
	saved_rd = rd;
	CHECK(v90_rd_tx_init(NULL, &data_mapping) ==
		      V90_RD_TX_INVALID_ARGUMENT &&
	      v90_rd_tx_init(&rd, NULL) == V90_RD_TX_INVALID_ARGUMENT &&
	      v90_rd_tx_init(&rd, &mapping) ==
		      V90_RD_TX_INVALID_PARAMETERS &&
	      memcmp(&rd, &saved_rd, sizeof(rd)) == 0 &&
	      v90_rd_tx_next(NULL, &symbol) ==
		      V90_RD_TX_INVALID_ARGUMENT &&
	      v90_rd_tx_next(&rd, NULL) ==
		      V90_RD_TX_INVALID_ARGUMENT,
	      "Rd accepted an invalid map or destination");
	saved = transmitter;
	set_acknowledgment(&acknowledged, true);
	CHECK(pcm_downstream_tx_start_mp(NULL, &record) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_start_mp(&transmitter, NULL) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_request_mp_prime(NULL) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_request_ed(NULL) ==
		      PCM_DOWNSTREAM_TX_INVALID_ARGUMENT &&
	      pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_mp(&transmitter, &acknowledged) ==
		      PCM_DOWNSTREAM_TX_INVALID_PARAMETERS &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "rejected MP setup changed state");

	CHECK(pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "valid MP setup failed");
	saved = transmitter;
	changed.parameters.type0.maximum_upstream_rate_2400_bps_units = 10;
	CHECK(pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &changed) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "configured MP transaction changed");
	CHECK(pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "valid MP prime request failed");
	saved = transmitter;
	CHECK(pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "repeated MP prime request changed state");
	CHECK(pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "early Ed request failed");
	saved = transmitter;
	CHECK(pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "repeated Ed request changed state");

	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_request_finish(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "finished setup fixture failed");
	saved = transmitter;
	CHECK(pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "MP started after finish request");

	CHECK(pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "corruption fixture did not initialize");
	transmitter.control_sequence[0] ^= 1u;
	saved = transmitter;
	CHECK(!pcm_downstream_tx_is_valid(&transmitter) &&
	      pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_INVALID_STATE &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "corrupt MP sequence was accepted or changed");
	return true;
}

static bool test_boundary_request(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_mp_record record = make_record(false);
	struct pcm_downstream_tx transmitter;
	struct pcm_downstream_symbol symbol;
	size_t symbols = 0;

	CHECK(make_mapping(
		      &mapping, G711_LAW_PCMU, V90_CP_TRAINING) &&
	      pcm_downstream_tx_init(
		      &transmitter, PCM_PROTOCOL_V90, &mapping) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      pcm_downstream_tx_start_mp(&transmitter, &record) ==
		      PCM_DOWNSTREAM_TX_OK,
	      "boundary fixture did not initialize");
	while ((transmitter.stage != PCM_DOWNSTREAM_TX_MP ||
		transmitter.control_bit_index != 0) &&
	       symbols < MAXIMUM_STREAM_SYMBOLS) {
		CHECK(pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "boundary fixture stopped at symbol %zu", symbols);
		symbols++;
	}
	CHECK(symbols < MAXIMUM_STREAM_SYMBOLS &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_MP &&
	      transmitter.control_bit_index == 0 &&
	      pcm_downstream_tx_request_mp_prime(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_MP_PRIME &&
	      transmitter.control_bit_index == 0 &&
	      pcm_downstream_tx_is_valid(&transmitter),
	      "record-boundary request did not switch immediately");
	while ((!transmitter.mp_prime_record_accepted ||
		transmitter.control_bit_index != 0) &&
	       symbols < MAXIMUM_STREAM_SYMBOLS) {
		CHECK(pcm_downstream_tx_next(&transmitter, &symbol) ==
			      PCM_DOWNSTREAM_TX_OK,
		      "MP-prime boundary fixture stopped at symbol %zu",
		      symbols);
		symbols++;
	}
	CHECK(symbols < MAXIMUM_STREAM_SYMBOLS &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_MP_PRIME &&
	      transmitter.mp_prime_record_accepted &&
	      transmitter.control_bit_index == 0 &&
	      pcm_downstream_tx_request_ed(&transmitter) ==
		      PCM_DOWNSTREAM_TX_OK &&
	      transmitter.stage == PCM_DOWNSTREAM_TX_ED &&
	      transmitter.ed_frames == 0 &&
	      pcm_downstream_tx_is_valid(&transmitter),
	      "record-boundary Ed request did not switch immediately");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous streams", test_continuous_streams },
		{ "Rd and Rd-bar sequence", test_rd_sequence },
		{ "Rd and Rd-bar detection", test_rrn_detection },
		{ "Rd and Rd-bar rejection", test_rrn_rejection },
		{ "Rd receiver contracts", test_rrn_contracts },
		{ "record-boundary request", test_boundary_request },
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
