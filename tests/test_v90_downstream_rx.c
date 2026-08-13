// SPDX-License-Identifier: BSD-3-Clause

#include <v90_downstream_rx.h>

#include <v34_framing.h>
#include <v90_pcm_frame_encoder.h>
#include <v90_pcm_symbol.h>

#include "test.h"

#include <string.h>

enum {
	FRAME_BITS = 17,
	EXTRA_TRN2D_FRAMES = 5,
	MAX_EVENTS = 12,
};

struct observed_event {
	enum v90_downstream_rx_result result;
	struct v90_mp_event mp;
	struct v90_downstream_ed_event ed;
};

struct observer {
	struct observed_event events[MAX_EVENTS];
	size_t event_count;
	size_t training_frames;
};

struct source {
	struct v90_pcm_frame_encoder encoder;
	struct v34_scrambler scrambler;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t redundancy, uint8_t lookahead)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.upstream_rate_mask = 0x1555u,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = 29,
		.shaping_a2_q1_6 = -13,
		.shaping_b1_q1_6 = 43,
		.shaping_b2_q1_6 = -5,
		.selected_rate_index = FRAME_BITS - 8,
		.sign_redundancy = redundancy,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
	};

	for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static struct v90_mp_record type0_record(bool acknowledged)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.acknowledges_cp = acknowledged,
			.upstream_rate_capability_mask = 0x1fffu,
		},
	};
}

static struct v90_mp_record type1_record(bool acknowledged)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE1,
		.parameters.type1 = {
			.common = {
				.maximum_upstream_rate_2400_bps_units = 14,
				.upstream_trellis = V34_TRELLIS_64_STATE,
				.acknowledges_cp = acknowledged,
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

static uint8_t packed_bit(const uint8_t *sequence, size_t bit_index)
{
	return (uint8_t)((sequence[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static bool records_match(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	uint8_t left_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t right_sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t left_bits;
	size_t right_bits;

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

static bool observe_output(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_frame_output *output,
	struct observer *observer)
{
	for (size_t symbol = 0; symbol < output->symbol_count; ++symbol) {
		struct v90_downstream_event event;
		struct v90_downstream_event untouched;
		enum v90_downstream_rx_result result;

		memset(&event, 0xa5, sizeof(event));
		untouched = event;
		result = v90_downstream_rx_receive(
			receiver, output->symbols[symbol].octet, &event);
		if (result == V90_DOWNSTREAM_RX_BUFFERING) {
			CHECK(memcmp(&event, &untouched, sizeof(event)) == 0,
			      "buffering changed the downstream event");
			continue;
		}
		if (result == V90_DOWNSTREAM_RX_TRN2D_FRAME) {
			CHECK(event.trn2d.bit_count == FRAME_BITS &&
			      event.trn2d.training_bit_errors == 0,
			      "TRN2d frame did not decode cleanly");
			for (size_t bit = 0;
			     bit < event.trn2d.bit_count; ++bit) {
				CHECK(event.trn2d.bits[bit] == 1,
				      "TRN2d bit %zu was not one", bit);
			}
			observer->training_frames++;
			continue;
		}
		CHECK(result == V90_DOWNSTREAM_RX_MP ||
		      result == V90_DOWNSTREAM_RX_MP_PRIME ||
		      result == V90_DOWNSTREAM_RX_MP_REJECTED ||
		      result == V90_DOWNSTREAM_RX_ED ||
		      result == V90_DOWNSTREAM_RX_ED_REJECTED,
		      "downstream receiver returned %s",
		      v90_downstream_rx_result_name(result));
		CHECK(observer->event_count < MAX_EVENTS,
		      "downstream receiver emitted too many records");
		observer->events[observer->event_count++] =
			(struct observed_event){
				.result = result,
				.mp = event.mp,
				.ed = event.ed,
			};
	}
	return true;
}

static bool source_init(
	struct source *source, const struct v90_pcm_mapping *mapping)
{
	struct source initialized = { 0 };

	if (v90_pcm_frame_encoder_init(
		    &initialized.encoder, mapping) !=
		    V90_PCM_FRAME_ENCODER_OK ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPC) !=
		    V34_FRAMING_OK)
		return false;
	*source = initialized;
	return true;
}

static bool push_frame(
	struct source *source, const uint8_t bits[FRAME_BITS],
	struct v90_downstream_rx *receiver, struct observer *observer)
{
	struct v90_pcm_frame_output output;
	uint8_t scrambled[FRAME_BITS];
	enum v90_pcm_frame_encoder_result result;

	for (size_t bit = 0; bit < FRAME_BITS; ++bit) {
		if (v34_scramble_group(
			    &source->scrambler, bits[bit], 1,
			    &scrambled[bit]) != V34_FRAMING_OK)
			return false;
	}
	result = v90_pcm_frame_encoder_process(
		&source->encoder, scrambled, FRAME_BITS, &output);
	if (result == V90_PCM_FRAME_ENCODER_BUFFERING)
		return true;
	CHECK(result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY,
	      "source encoder returned %s",
	      v90_pcm_frame_encoder_result_name(result));
	return observe_output(receiver, &output, observer);
}

static bool finish_source(
	struct source *source, struct v90_downstream_rx *receiver,
	struct observer *observer)
{
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_pcm_frame_encoder_result result =
			v90_pcm_frame_encoder_finish(&source->encoder, &output);

		if (result == V90_PCM_FRAME_ENCODER_COMPLETE)
			return true;
		CHECK(result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY,
		      "source finish returned %s",
		      v90_pcm_frame_encoder_result_name(result));
		CHECK(observe_output(receiver, &output, observer),
		      "source finish output was rejected");
	}
}

static bool push_trn2d(
	struct source *source, size_t frame_count,
	struct v90_downstream_rx *receiver, struct observer *observer)
{
	uint8_t ones[FRAME_BITS];

	memset(ones, 1, sizeof(ones));
	for (size_t frame = 0; frame < frame_count; ++frame) {
		CHECK(push_frame(source, ones, receiver, observer),
		      "TRN2d source stopped at frame %zu", frame);
	}
	return true;
}

static bool push_sequence(
	struct source *source, const uint8_t *sequence, size_t bit_count,
	struct v90_downstream_rx *receiver, struct observer *observer)
{
	uint8_t frame[FRAME_BITS];

	CHECK(bit_count % FRAME_BITS == 0,
	      "MP sequence was not mapping-frame aligned");
	for (size_t first = 0; first < bit_count; first += FRAME_BITS) {
		for (size_t bit = 0; bit < FRAME_BITS; ++bit)
			frame[bit] = packed_bit(sequence, first + bit);
		CHECK(push_frame(source, frame, receiver, observer),
		      "MP source stopped at bit %zu", first);
	}
	return true;
}

static bool encode_record(
	const struct v90_mp_record *record,
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES], size_t *bit_count)
{
	CHECK(v90_mp_encode(
		      record, FRAME_BITS, sequence,
		      V90_MP_MAXIMUM_SEQUENCE_BYTES, bit_count) == V90_MP_OK,
	      "MP source record did not encode");
	return true;
}

static bool run_continuous_stream(enum g711_law law)
{
	struct v90_mp_record mp = type0_record(false);
	struct v90_mp_record mp_prime = type1_record(true);
	struct v90_pcm_mapping mapping;
	struct v90_downstream_rx receiver;
	struct source source;
	struct observer observer = { 0 };
	uint8_t corrupt[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t valid_mp[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t valid_prime[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	size_t mp_bits;
	size_t prime_bits;

	CHECK(make_mapping(&mapping, law, 2, 2) &&
	      mapping.frame_bits == FRAME_BITS &&
	      source_init(&source, &mapping) &&
	      v90_downstream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_OK &&
	      v90_downstream_rx_is_fresh(&receiver),
	      "continuous downstream fixture did not initialize");
	CHECK(push_trn2d(
		      &source,
		      V90_PHASE4_TRN2D_MINIMUM_SYMBOLS /
			      V90_CP_FRAME_INTERVALS,
		      &receiver, &observer) &&
	      push_trn2d(
		      &source, EXTRA_TRN2D_FRAMES,
		      &receiver, &observer),
	      "continuous TRN2d stream stopped");
	CHECK(observer.training_frames ==
		      V90_PHASE4_TRN2D_MINIMUM_SYMBOLS /
			      V90_CP_FRAME_INTERVALS &&
	      observer.event_count == 0 &&
	      receiver.stage == V90_DOWNSTREAM_RX_MP_SEARCH,
	      "TRN2d/MP boundary moved");

	CHECK(encode_record(&mp, valid_mp, &mp_bits) &&
	      encode_record(&mp_prime, valid_prime, &prime_bits),
	      "continuous MP records did not encode");
	memcpy(corrupt, valid_mp, sizeof(corrupt));
	corrupt[24u / 8u] ^=
		(uint8_t)(1u << (7u - 24u % 8u));
	CHECK(push_sequence(
		      &source, corrupt, mp_bits, &receiver, &observer) &&
	      push_sequence(
		      &source, valid_mp, mp_bits, &receiver, &observer) &&
	      push_sequence(
		      &source, valid_prime, prime_bits,
		      &receiver, &observer) &&
	      push_sequence(
		      &source, valid_prime, prime_bits,
		      &receiver, &observer),
	      "continuous MP stream stopped");

	CHECK(observer.event_count >= 3 &&
	      observer.events[0].result ==
		      V90_DOWNSTREAM_RX_MP_REJECTED &&
	      observer.events[0].mp.type == V90_MP_EVENT_REJECTED &&
	      observer.events[0].mp.frame_result == V90_MP_CRC_MISMATCH &&
	      observer.events[1].result == V90_DOWNSTREAM_RX_MP &&
	      observer.events[1].mp.type == V90_MP_EVENT_TYPE0 &&
	      records_match(&observer.events[1].mp.record, &mp) &&
	      observer.events[2].result == V90_DOWNSTREAM_RX_MP_PRIME &&
	      observer.events[2].mp.type == V90_MP_EVENT_TYPE1 &&
	      records_match(
		      &observer.events[2].mp.record, &mp_prime),
	      "continuous MP event order changed");
	CHECK(receiver.trn2d_bit_errors == 0 &&
	      receiver.stream.received_symbols >=
		      V90_PHASE4_TRN2D_MINIMUM_SYMBOLS +
			      EXTRA_TRN2D_FRAMES *
				      V90_CP_FRAME_INTERVALS +
			      mp_bits / FRAME_BITS *
				      V90_CP_FRAME_INTERVALS &&
	      v90_downstream_rx_is_valid(&receiver) &&
	      v90_pcm_frame_encoder_is_valid(&source.encoder) &&
	      v34_scrambler_is_valid(&source.scrambler),
	      "continuous stream lost coding state or accounting");
	return true;
}

static bool test_continuous_stream(void)
{
	CHECK(run_continuous_stream(G711_LAW_PCMU),
	      "PCMU downstream stream failed");
	CHECK(run_continuous_stream(G711_LAW_PCMA),
	      "PCMA downstream stream failed");
	return true;
}

static bool test_trn2d_error_accounting(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_downstream_rx receiver;
	struct source source;
	struct v90_pcm_frame_output output;
	struct v90_downstream_event event;
	uint8_t ones[FRAME_BITS];
	uint8_t scrambled[FRAME_BITS];

	memset(ones, 1, sizeof(ones));
	CHECK(make_mapping(&mapping, G711_LAW_PCMA, 0, 0) &&
	      source_init(&source, &mapping) &&
	      v90_downstream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_OK,
	      "TRN2d error fixture did not initialize");
	for (size_t bit = 0; bit < FRAME_BITS; ++bit) {
		CHECK(v34_scramble_group(
			      &source.scrambler, ones[bit], 1,
			      &scrambled[bit]) == V34_FRAMING_OK,
		      "TRN2d error source did not scramble bit %zu", bit);
	}
	CHECK(v90_pcm_frame_encoder_process(
		      &source.encoder, scrambled, FRAME_BITS, &output) ==
		      V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	      output.symbol_count == V90_CP_FRAME_INTERVALS,
	      "TRN2d error source did not emit one frame");
	for (size_t symbol = 0; symbol < output.symbol_count; ++symbol) {
		struct v90_pcm_symbol altered = output.symbols[symbol];
		enum v90_downstream_rx_result result;

		if (symbol == 0) {
			CHECK(v90_pcm_symbol_make(
				      &altered, mapping.law, altered.ucode,
				      !altered.positive),
			      "TRN2d error symbol did not invert");
		}
		result = v90_downstream_rx_receive(
			&receiver, altered.octet, &event);
		CHECK(result == (symbol + 1u == output.symbol_count ?
					 V90_DOWNSTREAM_RX_TRN2D_FRAME :
					 V90_DOWNSTREAM_RX_BUFFERING),
		      "TRN2d error symbol %zu returned %s", symbol,
		      v90_downstream_rx_result_name(result));
	}
	CHECK(event.trn2d.training_bit_errors > 0 &&
	      receiver.trn2d_bit_errors ==
		      event.trn2d.training_bit_errors &&
	      receiver.stage == V90_DOWNSTREAM_RX_TRN2D &&
	      v90_downstream_rx_is_valid(&receiver),
	      "TRN2d error accounting escaped the Phase 4 owner");
	return true;
}

static bool run_ed_transition(
	enum g711_law law, bool arm_before_mp, bool corrupt)
{
	struct v90_mp_record mp_prime = arm_before_mp ?
					     type0_record(true) :
					     type1_record(true);
	struct v90_pcm_mapping mapping;
	struct v90_downstream_rx receiver;
	struct source source;
	struct observer observer = { 0 };
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t ed[FRAME_BITS] = { 0 };
	size_t sequence_bits;
	size_t expected_symbols;

	CHECK(make_mapping(&mapping, law, 2, 2) &&
	      source_init(&source, &mapping) &&
	      v90_downstream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_OK &&
	      push_trn2d(
		      &source, V90_PHASE4_TRN2D_MINIMUM_FRAMES +
			       mapping.spectral_lookahead,
		      &receiver, &observer) &&
	      encode_record(&mp_prime, sequence, &sequence_bits),
	      "Ed fixture did not reach MP search");
	if (arm_before_mp) {
		CHECK(v90_downstream_rx_expect_ed(&receiver) ==
			      V90_DOWNSTREAM_RX_OK &&
		      v90_downstream_rx_expect_ed(&receiver) ==
			      V90_DOWNSTREAM_RX_OK,
		      "early Ed request was not idempotent");
	}
	CHECK(push_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer) &&
	      push_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer),
	      "repeated MP-prime stream stopped");
	if (!arm_before_mp) {
		CHECK(v90_downstream_rx_expect_ed(&receiver) ==
			      V90_DOWNSTREAM_RX_OK &&
		      v90_downstream_rx_expect_ed(&receiver) ==
			      V90_DOWNSTREAM_RX_OK,
		      "mid-record Ed request was not idempotent");
	}
	if (corrupt)
		ed[7] = 1;
	CHECK(push_frame(&source, ed, &receiver, &observer) &&
	      push_frame(&source, (uint8_t[FRAME_BITS]){ 0 },
			 &receiver, &observer) &&
	      finish_source(&source, &receiver, &observer),
	      "Ed stream did not drain");

	expected_symbols = V90_PHASE4_TRN2D_MINIMUM_SYMBOLS +
			   mapping.spectral_lookahead *
				   V90_CP_FRAME_INTERVALS +
			   2u * sequence_bits / FRAME_BITS *
				   V90_CP_FRAME_INTERVALS +
			   V90_PHASE4_ED_FRAMES *
				   V90_CP_FRAME_INTERVALS;
	CHECK(observer.training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      observer.event_count == 3 &&
	      observer.events[0].result == V90_DOWNSTREAM_RX_MP_PRIME &&
	      observer.events[1].result == V90_DOWNSTREAM_RX_MP_PRIME &&
	      records_match(&observer.events[0].mp.record, &mp_prime) &&
	      records_match(&observer.events[1].mp.record, &mp_prime) &&
	      observer.events[2].result ==
		      (corrupt ? V90_DOWNSTREAM_RX_ED_REJECTED :
				 V90_DOWNSTREAM_RX_ED) &&
	      observer.events[2].ed.bit_count ==
		      V90_PHASE4_ED_FRAMES * FRAME_BITS &&
	      observer.events[2].ed.bit_errors == (corrupt ? 1u : 0u) &&
	      receiver.stream.received_symbols == expected_symbols &&
	      receiver.trn2d_bit_errors == 0 &&
	      v90_downstream_rx_is_valid(&receiver) &&
	      v90_downstream_rx_is_complete(&receiver) == !corrupt &&
	      v90_downstream_rx_has_failed(&receiver) == corrupt,
	      "Ed boundary, event, or accounting changed");
	struct v90_downstream_rx saved = receiver;
	struct v90_downstream_event terminal_event;
	struct v90_downstream_event terminal_saved;

	memset(&terminal_event, 0xa5, sizeof(terminal_event));
	terminal_saved = terminal_event;
	CHECK(v90_downstream_rx_expect_ed(&receiver) ==
		      V90_DOWNSTREAM_RX_INVALID_STATE &&
	      v90_downstream_rx_receive(
		      &receiver, 0, &terminal_event) ==
		      V90_DOWNSTREAM_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&terminal_event, &terminal_saved,
		     sizeof(terminal_event)) == 0,
	      "terminal Ed receiver accepted more input");
	return true;
}

static bool test_ed_transition(void)
{
	CHECK(run_ed_transition(G711_LAW_PCMU, true, false),
	      "PCMU early Ed transition failed");
	CHECK(run_ed_transition(G711_LAW_PCMA, false, false),
	      "PCMA mid-record Ed transition failed");
	CHECK(run_ed_transition(G711_LAW_PCMU, true, true),
	      "corrupt Ed transition failed");
	return true;
}

static bool test_ed_boundary_request(void)
{
	struct v90_mp_record mp_prime = type0_record(true);
	struct v90_pcm_mapping mapping;
	struct v90_downstream_rx receiver;
	struct source source;
	struct observer observer = { 0 };
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t zeros[FRAME_BITS] = { 0 };
	size_t sequence_bits;

	CHECK(make_mapping(&mapping, G711_LAW_PCMA, 0, 0) &&
	      source_init(&source, &mapping) &&
	      v90_downstream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_OK &&
	      push_trn2d(
		      &source, V90_PHASE4_TRN2D_MINIMUM_FRAMES,
		      &receiver, &observer) &&
	      encode_record(&mp_prime, sequence, &sequence_bits) &&
	      push_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer) &&
	      observer.event_count == 1 && receiver.mp_prime_boundary,
	      "exact MP-prime boundary was not retained");
	CHECK(v90_downstream_rx_expect_ed(&receiver) ==
		      V90_DOWNSTREAM_RX_OK &&
	      receiver.stage == V90_DOWNSTREAM_RX_ED_SEARCH &&
	      !receiver.mp_prime_boundary &&
	      push_frame(&source, zeros, &receiver, &observer) &&
	      push_frame(&source, zeros, &receiver, &observer) &&
	      observer.event_count == 2 &&
	      observer.events[1].result == V90_DOWNSTREAM_RX_ED &&
	      v90_downstream_rx_is_complete(&receiver),
	      "boundary Ed request did not complete exactly two frames");
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping data_mapping;
	struct v90_cp_parameters data = {
		.kind = V90_CP_DATA,
		.law = G711_LAW_PCMU,
		.selected_rate_index = 1,
		.constellation_count = 1,
	};
	struct v90_downstream_rx receiver;
	struct v90_downstream_rx saved;
	struct v90_downstream_rx untouched;
	struct v90_downstream_rx untouched_saved;
	struct v90_downstream_event event;
	struct v90_downstream_event event_saved;
	struct v90_pcm_symbol invalid;

	for (uint8_t ordinal = 0; ordinal < 8; ++ordinal)
		add_ucode(
			&data.transmit_constellations[0],
			(uint8_t)(20u + ordinal));
	CHECK(make_mapping(&mapping, G711_LAW_PCMU, 0, 0) &&
	      v90_pcm_mapping_init(&data_mapping, &data) ==
		      V90_PCM_MAPPING_OK,
	      "contract mappings did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_downstream_rx_init(
		      NULL, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      v90_downstream_rx_init(
		      &untouched, NULL, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      v90_downstream_rx_init(
		      &untouched, &mapping,
		      (enum v90_pcm_mapping_plane)2) ==
		      V90_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      v90_downstream_rx_init(
		      &untouched, &data_mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_INVALID_PARAMETERS &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(v90_downstream_rx_init(
		      &receiver, &mapping, V90_PCM_MAPPING_TRANSMIT) ==
		      V90_DOWNSTREAM_RX_OK &&
	      v90_downstream_rx_is_valid(&receiver) &&
	      v90_downstream_rx_is_fresh(&receiver) &&
	      !v90_downstream_rx_is_valid(NULL) &&
	      !v90_downstream_rx_is_fresh(NULL),
	      "valid receiver did not initialize");
	CHECK(v90_downstream_rx_expect_ed(NULL) ==
		      V90_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      v90_downstream_rx_expect_ed(&receiver) ==
		      V90_DOWNSTREAM_RX_INVALID_STATE &&
	      !v90_downstream_rx_is_complete(&receiver) &&
	      !v90_downstream_rx_has_failed(&receiver),
	      "fresh receiver accepted Ed or reported terminal state");
	saved = receiver;
	memset(&event, 0xa5, sizeof(event));
	event_saved = event;
	CHECK(v90_downstream_rx_receive(NULL, 0, &event) ==
		      V90_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      v90_downstream_rx_receive(&receiver, 0, NULL) ==
		      V90_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_saved, sizeof(event)) == 0,
	      "invalid receive call changed state or output");
	CHECK(v90_pcm_symbol_make(
		      &invalid, mapping.law, 1, false),
	      "invalid mapping symbol did not initialize");
	CHECK(v90_downstream_rx_receive(
		      &receiver, invalid.octet, &event) ==
		      V90_DOWNSTREAM_RX_INVALID_SYMBOL &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_saved, sizeof(event)) == 0,
	      "invalid mapping symbol changed state or output");
	receiver.stage = (enum v90_downstream_rx_stage)99;
	CHECK(!v90_downstream_rx_is_valid(&receiver) &&
	      v90_downstream_rx_receive(
		      &receiver, invalid.octet, &event) ==
		      V90_DOWNSTREAM_RX_INVALID_STATE,
	      "receiver accepted an unknown stage");
	receiver = saved;
	receiver.stream.descrambler.history = 1;
	CHECK(v90_downstream_rx_is_valid(&receiver) &&
	      !v90_downstream_rx_is_fresh(&receiver),
	      "fresh predicate ignored used coding history");
	CHECK(strcmp(v90_downstream_rx_result_name(
		     V90_DOWNSTREAM_RX_MP_PRIME),
		     "MP prime") == 0 &&
	      strcmp(v90_downstream_rx_result_name(
		     (enum v90_downstream_rx_result)99),
		     "unknown") == 0,
	      "downstream result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous TRN2d and MP", test_continuous_stream },
		{ "TRN2d error accounting", test_trn2d_error_accounting },
		{ "Ed transition", test_ed_transition },
		{ "Ed boundary request", test_ed_boundary_request },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
