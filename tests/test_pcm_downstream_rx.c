// SPDX-License-Identifier: BSD-3-Clause

#include <pcm_downstream_rx.h>

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
	enum pcm_downstream_rx_result result;
	struct v90_mp_event mp;
	struct pcm_downstream_ed_event ed;
};

struct observer {
	struct observed_event events[MAX_EVENTS];
	size_t event_count;
	size_t training_frames;
};

struct v92_observer {
	struct v92_suvd_parameters last_parameters;
	struct v92_cpd_parameters last_cpd;
	size_t training_frames;
	size_t records;
	size_t cpd_records;
	size_t invalid_frames;
	size_t crc_mismatches;
	size_t cpd_invalid_frames;
	size_t cpd_crc_mismatches;
	size_t ed_events;
	size_t ed_bit_errors;
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

static bool make_width_mapping(
	struct v90_pcm_mapping *mapping, enum g711_law law,
	uint8_t frame_bits)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_TRAINING,
		.law = law,
		.selected_rate_index = (uint8_t)(frame_bits - 8u),
		.constellation_count = V90_CP_MAX_CONSTELLATIONS,
	};
	uint8_t sign_bits = frame_bits > 27u ?
				    (uint8_t)(frame_bits - 24u) : 3u;
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
				(uint8_t)(4u + interval * 20u + ordinal));
		}
	}
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
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_frame_output *output,
	struct observer *observer)
{
	for (size_t symbol = 0; symbol < output->symbol_count; ++symbol) {
		struct pcm_downstream_event event;
		struct pcm_downstream_event untouched;
		enum pcm_downstream_rx_result result;

		memset(&event, 0xa5, sizeof(event));
		untouched = event;
		result = pcm_downstream_rx_receive(
			receiver, output->symbols[symbol].octet, &event);
		if (result == PCM_DOWNSTREAM_RX_BUFFERING) {
			CHECK(memcmp(&event, &untouched, sizeof(event)) == 0,
			      "buffering changed the downstream event");
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_TRN2D_FRAME) {
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
		CHECK(result == PCM_DOWNSTREAM_RX_MP ||
		      result == PCM_DOWNSTREAM_RX_MP_PRIME ||
		      result == PCM_DOWNSTREAM_RX_MP_REJECTED ||
		      result == PCM_DOWNSTREAM_RX_ED ||
		      result == PCM_DOWNSTREAM_RX_ED_REJECTED,
		      "downstream receiver returned %s",
		      pcm_downstream_rx_result_name(result));
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

static bool observe_v92_output(
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_frame_output *output,
	struct v92_observer *observer)
{
	for (size_t symbol = 0u; symbol < output->symbol_count; ++symbol) {
		struct pcm_downstream_event event;
		struct pcm_downstream_event untouched;
		enum pcm_downstream_rx_result result;

		memset(&event, 0xa5, sizeof(event));
		untouched = event;
		result = pcm_downstream_rx_receive(
			receiver, output->symbols[symbol].octet, &event);
		if (result == PCM_DOWNSTREAM_RX_BUFFERING) {
			CHECK(memcmp(&event, &untouched, sizeof(event)) == 0,
			      "V.92 buffering changed the downstream event");
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_TRN2D_FRAME) {
			CHECK(event.trn2d.bit_count ==
				      receiver->stream.decoder.mapping.frame_bits &&
			      event.trn2d.training_bit_errors == 0u,
			      "V.92 TRN2d frame did not decode cleanly");
			observer->training_frames++;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_SUVD) {
			CHECK(event.control.type == V92_CONTROL_EVENT_SUVD &&
			      event.control.record.suvd.frame_result == V92_SUVD_OK,
			      "V.92 SUVd result and event disagreed");
			observer->last_parameters =
				event.control.record.suvd.parameters;
			observer->records++;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_CPD) {
			CHECK(event.control.type == V92_CONTROL_EVENT_CPD &&
			      event.control.record.cpd.frame_result == V92_CPD_OK,
			      "V.92 CPd result and event disagreed");
			observer->last_cpd = event.control.record.cpd.parameters;
			observer->cpd_records++;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_ED ||
		    result == PCM_DOWNSTREAM_RX_ED_REJECTED) {
			observer->ed_events++;
			observer->ed_bit_errors += event.ed.bit_errors;
			continue;
		}
		if (result == PCM_DOWNSTREAM_RX_SUVD_REJECTED) {
			enum v92_suvd_result rejected =
				event.control.record.suvd.frame_result;

			CHECK(event.control.type ==
				      V92_CONTROL_EVENT_SUVD_REJECTED,
			      "V.92 SUVd result and event disagreed");
			if (rejected == V92_SUVD_INVALID_FRAME)
				observer->invalid_frames++;
			else if (rejected == V92_SUVD_CRC_MISMATCH)
				observer->crc_mismatches++;
			else
				CHECK(false, "V.92 SUVd rejection had result %s",
				      v92_suvd_result_name(rejected));
			continue;
		}
		CHECK(result == PCM_DOWNSTREAM_RX_CPD_REJECTED &&
		      event.control.type == V92_CONTROL_EVENT_CPD_REJECTED,
		      "V.92 downstream receiver returned %s",
		      pcm_downstream_rx_result_name(result));
		if (event.control.record.cpd.frame_result == V92_CPD_INVALID_FRAME)
			observer->cpd_invalid_frames++;
		else if (event.control.record.cpd.frame_result ==
			 V92_CPD_CRC_MISMATCH)
			observer->cpd_crc_mismatches++;
		else
			CHECK(false, "V.92 CPd rejection had result %s",
			      v92_cpd_result_name(
				      event.control.record.cpd.frame_result));
	}
	return true;
}

static bool push_v92_frame(
	struct source *source, const uint8_t *bits,
	struct pcm_downstream_rx *receiver,
	struct v92_observer *observer)
{
	struct v90_pcm_frame_output output;
	uint8_t scrambled[V90_PCM_FRAME_MAX_BITS];
	size_t frame_bits = source->encoder.mapping.frame_bits;
	enum v90_pcm_frame_encoder_result result;

	for (size_t bit = 0u; bit < frame_bits; ++bit) {
		if (v34_scramble_group(
			    &source->scrambler, bits[bit], 1u,
			    &scrambled[bit]) != V34_FRAMING_OK)
			return false;
	}
	result = v90_pcm_frame_encoder_process(
		&source->encoder, scrambled, frame_bits, &output);
	if (result == V90_PCM_FRAME_ENCODER_BUFFERING)
		return true;
	CHECK(result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY,
	      "V.92 source encoder returned %s",
	      v90_pcm_frame_encoder_result_name(result));
	return observe_v92_output(receiver, &output, observer);
}

static bool finish_v92_source(
	struct source *source, struct pcm_downstream_rx *receiver,
	struct v92_observer *observer)
{
	for (;;) {
		struct v90_pcm_frame_output output;
		enum v90_pcm_frame_encoder_result result =
			v90_pcm_frame_encoder_finish(&source->encoder, &output);

		if (result == V90_PCM_FRAME_ENCODER_COMPLETE)
			return true;
		CHECK(result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
		      observe_v92_output(receiver, &output, observer),
		      "V.92 source finish failed");
	}
}

static bool push_v92_trn2d(
	struct source *source, size_t frame_count,
	struct pcm_downstream_rx *receiver,
	struct v92_observer *observer)
{
	uint8_t ones[V90_PCM_FRAME_MAX_BITS];

	memset(ones, 1, sizeof(ones));
	for (size_t frame = 0u; frame < frame_count; ++frame) {
		CHECK(push_v92_frame(source, ones, receiver, observer),
		      "V.92 TRN2d source stopped at frame %zu", frame);
	}
	return true;
}

static bool push_v92_sequence(
	struct source *source, const uint8_t *sequence, size_t bit_count,
	struct pcm_downstream_rx *receiver,
	struct v92_observer *observer)
{
	uint8_t frame[V90_PCM_FRAME_MAX_BITS];
	size_t frame_bits = source->encoder.mapping.frame_bits;

	CHECK(bit_count % frame_bits == 0u,
	      "V.92 sequence was not mapping-frame aligned");
	for (size_t first = 0u; first < bit_count; first += frame_bits) {
		for (size_t bit = 0u; bit < frame_bits; ++bit)
			frame[bit] = packed_bit(sequence, first + bit);
		CHECK(push_v92_frame(source, frame, receiver, observer),
		      "V.92 source stopped at bit %zu", first);
	}
	return true;
}

static bool push_misaligned_v92_sequence(
	struct source *source, const uint8_t *sequence, size_t bit_count,
	struct pcm_downstream_rx *receiver,
	struct v92_observer *observer)
{
	uint8_t shifted[V92_SUVD_MAX_FRAME_BITS + V90_PCM_FRAME_MAX_BITS];
	size_t frame_bits = source->encoder.mapping.frame_bits;
	size_t shifted_bits = bit_count + 1u;
	size_t padded_bits =
		(shifted_bits + frame_bits - 1u) / frame_bits * frame_bits;

	memset(shifted, 0, sizeof(shifted));
	for (size_t bit = 0u; bit < bit_count; ++bit)
		shifted[bit + 1u] = packed_bit(sequence, bit);
	for (size_t first = 0u; first < padded_bits; first += frame_bits) {
		CHECK(push_v92_frame(
			      source, shifted + first, receiver, observer),
		      "misaligned V.92 source stopped at bit %zu", first);
	}
	return true;
}

static bool push_frame(
	struct source *source, const uint8_t bits[FRAME_BITS],
	struct pcm_downstream_rx *receiver, struct observer *observer)
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
	struct source *source, struct pcm_downstream_rx *receiver,
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
	struct pcm_downstream_rx *receiver, struct observer *observer)
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
	struct pcm_downstream_rx *receiver, struct observer *observer)
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
	struct pcm_downstream_rx receiver;
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
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_rx_is_fresh(&receiver),
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
	      receiver.stage == PCM_DOWNSTREAM_RX_MP_SEARCH,
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
		      PCM_DOWNSTREAM_RX_MP_REJECTED &&
	      observer.events[0].mp.type == V90_MP_EVENT_REJECTED &&
	      observer.events[0].mp.frame_result == V90_MP_CRC_MISMATCH &&
	      observer.events[1].result == PCM_DOWNSTREAM_RX_MP &&
	      observer.events[1].mp.type == V90_MP_EVENT_TYPE0 &&
	      records_match(&observer.events[1].mp.record, &mp) &&
	      observer.events[2].result == PCM_DOWNSTREAM_RX_MP_PRIME &&
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
	      pcm_downstream_rx_is_valid(&receiver) &&
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
	struct pcm_downstream_rx receiver;
	struct source source;
	struct v90_pcm_frame_output output;
	struct pcm_downstream_event event;
	uint8_t ones[FRAME_BITS];
	uint8_t scrambled[FRAME_BITS];

	memset(ones, 1, sizeof(ones));
	CHECK(make_mapping(&mapping, G711_LAW_PCMA, 0, 0) &&
	      source_init(&source, &mapping) &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK,
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
		enum pcm_downstream_rx_result result;

		if (symbol == 0) {
			CHECK(v90_pcm_symbol_make(
				      &altered, mapping.law, altered.ucode,
				      !altered.positive),
			      "TRN2d error symbol did not invert");
		}
		result = pcm_downstream_rx_receive(
			&receiver, altered.octet, &event);
		CHECK(result == (symbol + 1u == output.symbol_count ?
					 PCM_DOWNSTREAM_RX_TRN2D_FRAME :
					 PCM_DOWNSTREAM_RX_BUFFERING),
		      "TRN2d error symbol %zu returned %s", symbol,
		      pcm_downstream_rx_result_name(result));
	}
	CHECK(event.trn2d.training_bit_errors > 0 &&
	      receiver.trn2d_bit_errors ==
		      event.trn2d.training_bit_errors &&
	      receiver.stage == PCM_DOWNSTREAM_RX_TRN2D &&
	      pcm_downstream_rx_is_valid(&receiver),
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
	struct pcm_downstream_rx receiver;
	struct source source;
	struct observer observer = { 0 };
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t ed[FRAME_BITS] = { 0 };
	size_t sequence_bits;
	size_t expected_symbols;

	CHECK(make_mapping(&mapping, law, 2, 2) &&
	      source_init(&source, &mapping) &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      push_trn2d(
		      &source, V90_PHASE4_TRN2D_MINIMUM_FRAMES +
			       mapping.spectral_lookahead,
		      &receiver, &observer) &&
	      encode_record(&mp_prime, sequence, &sequence_bits),
	      "Ed fixture did not reach MP search");
	if (arm_before_mp) {
		CHECK(pcm_downstream_rx_expect_ed(&receiver) ==
			      PCM_DOWNSTREAM_RX_OK &&
		      pcm_downstream_rx_expect_ed(&receiver) ==
			      PCM_DOWNSTREAM_RX_OK,
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
		CHECK(pcm_downstream_rx_expect_ed(&receiver) ==
			      PCM_DOWNSTREAM_RX_OK &&
		      pcm_downstream_rx_expect_ed(&receiver) ==
			      PCM_DOWNSTREAM_RX_OK,
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
	      observer.events[0].result == PCM_DOWNSTREAM_RX_MP_PRIME &&
	      observer.events[1].result == PCM_DOWNSTREAM_RX_MP_PRIME &&
	      records_match(&observer.events[0].mp.record, &mp_prime) &&
	      records_match(&observer.events[1].mp.record, &mp_prime) &&
	      observer.events[2].result ==
		      (corrupt ? PCM_DOWNSTREAM_RX_ED_REJECTED :
				 PCM_DOWNSTREAM_RX_ED) &&
	      observer.events[2].ed.bit_count ==
		      V90_PHASE4_ED_FRAMES * FRAME_BITS &&
	      observer.events[2].ed.bit_errors == (corrupt ? 1u : 0u) &&
	      receiver.stream.received_symbols == expected_symbols &&
	      receiver.trn2d_bit_errors == 0 &&
	      pcm_downstream_rx_is_valid(&receiver) &&
	      pcm_downstream_rx_is_complete(&receiver) == !corrupt &&
	      pcm_downstream_rx_has_failed(&receiver) == corrupt,
	      "Ed boundary, event, or accounting changed");
	struct pcm_downstream_rx saved = receiver;
	struct pcm_downstream_event terminal_event;
	struct pcm_downstream_event terminal_saved;

	memset(&terminal_event, 0xa5, sizeof(terminal_event));
	terminal_saved = terminal_event;
	CHECK(pcm_downstream_rx_expect_ed(&receiver) ==
		      PCM_DOWNSTREAM_RX_INVALID_STATE &&
	      pcm_downstream_rx_receive(
		      &receiver, 0, &terminal_event) ==
		      PCM_DOWNSTREAM_RX_INVALID_STATE &&
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
	struct pcm_downstream_rx receiver;
	struct source source;
	struct observer observer = { 0 };
	uint8_t sequence[V90_MP_MAXIMUM_SEQUENCE_BYTES];
	uint8_t zeros[FRAME_BITS] = { 0 };
	size_t sequence_bits;

	CHECK(make_mapping(&mapping, G711_LAW_PCMA, 0, 0) &&
	      source_init(&source, &mapping) &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      push_trn2d(
		      &source, V90_PHASE4_TRN2D_MINIMUM_FRAMES,
		      &receiver, &observer) &&
	      encode_record(&mp_prime, sequence, &sequence_bits) &&
	      push_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer) &&
	      observer.event_count == 1 && receiver.mp_prime_boundary,
	      "exact MP-prime boundary was not retained");
	CHECK(pcm_downstream_rx_expect_ed(&receiver) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      receiver.stage == PCM_DOWNSTREAM_RX_ED_SEARCH &&
	      !receiver.mp_prime_boundary &&
	      push_frame(&source, zeros, &receiver, &observer) &&
	      push_frame(&source, zeros, &receiver, &observer) &&
	      observer.event_count == 2 &&
	      observer.events[1].result == PCM_DOWNSTREAM_RX_ED &&
	      pcm_downstream_rx_is_complete(&receiver),
	      "boundary Ed request did not complete exactly two frames");
	return true;
}

static bool run_v92_buffered_prefix_ed(enum g711_law law)
{
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_rx armed;
	struct pcm_downstream_rx unarmed;
	struct source armed_source;
	struct source unarmed_source;
	struct v92_observer armed_observer = { 0 };
	struct v92_observer unarmed_observer = { 0 };
	uint8_t prefix[FRAME_BITS] = { 0 };
	uint8_t zeros[FRAME_BITS] = { 0 };

	for (size_t bit = FRAME_BITS - 8u; bit < FRAME_BITS; ++bit)
		prefix[bit] = 1u;
	CHECK(make_mapping(&mapping, law, 0u, 0u) &&
	      source_init(&armed_source, &mapping) &&
	      pcm_downstream_rx_init(
		      &armed, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      push_v92_trn2d(
		      &armed_source, V90_PHASE4_TRN2D_MINIMUM_FRAMES,
		      &armed, &armed_observer) &&
	      armed.stage == PCM_DOWNSTREAM_RX_V92_CONTROL,
	      "V.92 buffered-prefix fixture did not reach control");
	unarmed = armed;
	unarmed_source = armed_source;

	CHECK(pcm_downstream_rx_expect_ed(&armed) == PCM_DOWNSTREAM_RX_OK &&
	      push_v92_frame(
		      &armed_source, prefix, &armed, &armed_observer) &&
	      armed.stage == PCM_DOWNSTREAM_RX_V92_CONTROL &&
	      armed.ed_requested && armed.ed_frames == 0u &&
	      armed.control.v92.stage == V92_CONTROL_RX_SEARCH_SYNC &&
	      armed.control.v92.sync_one_count == 8u,
	      "armed V.92 Ed search did not retain a buffered prefix");
	CHECK(push_v92_frame(
		      &armed_source, zeros, &armed, &armed_observer) &&
	      armed.stage == PCM_DOWNSTREAM_RX_ED_SEARCH &&
	      armed.ed_requested && armed.ed_frames == 1u &&
	      armed.control.v92.stage == V92_CONTROL_RX_SEARCH_SYNC &&
	      armed.control.v92.sync_one_count == 0u &&
	      push_v92_frame(
		      &armed_source, zeros, &armed, &armed_observer) &&
	      armed_observer.ed_events == 1u &&
	      armed_observer.ed_bit_errors == 0u &&
	      pcm_downstream_rx_is_complete(&armed),
	      "armed V.92 receiver did not resume Ed after the prefix cleared");

	CHECK(push_v92_frame(
		      &unarmed_source, prefix, &unarmed,
		      &unarmed_observer) &&
	      unarmed.stage == PCM_DOWNSTREAM_RX_V92_CONTROL &&
	      !unarmed.ed_requested &&
	      unarmed.control.v92.sync_one_count == 8u &&
	      push_v92_frame(
		      &unarmed_source, zeros, &unarmed,
		      &unarmed_observer) &&
	      push_v92_frame(
		      &unarmed_source, zeros, &unarmed,
		      &unarmed_observer) &&
	      unarmed.stage == PCM_DOWNSTREAM_RX_V92_CONTROL &&
	      !unarmed.ed_requested && unarmed.ed_frames == 0u &&
	      unarmed.control.v92.stage == V92_CONTROL_RX_SEARCH_SYNC &&
	      unarmed.control.v92.sync_one_count == 0u &&
	      unarmed_observer.ed_events == 0u &&
	      pcm_downstream_rx_is_valid(&unarmed),
	      "unarmed V.92 control stream was mistaken for Ed");
	return true;
}

static bool test_v92_buffered_prefix_ed(void)
{
	CHECK(run_v92_buffered_prefix_ed(G711_LAW_PCMU),
	      "PCMU buffered-prefix Ed recovery failed");
	CHECK(run_v92_buffered_prefix_ed(G711_LAW_PCMA),
	      "PCMA buffered-prefix Ed recovery failed");
	return true;
}

static bool run_v92_width(enum g711_law law, uint8_t frame_bits)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = (frame_bits & 1u) != 0u,
		.acknowledges_cpu = law == G711_LAW_PCMA,
	};
	const struct v92_cpd_parameters cpd = {
		.four_times_gain_q0_16 = UINT16_C(0x2345),
		.data_rate_number = (uint8_t)((frame_bits - 9u) % 20u),
		.trellis = V92_CPD_TRELLIS_32_STATE,
		.extend_e2u = true,
		.acknowledges_cpu = law == G711_LAW_PCMU,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_rx receiver;
	struct source source;
	struct v92_observer observer = { 0 };
	uint8_t sequence[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t cpd_sequence[V92_CPD_MAX_FRAME_BYTES];
	uint8_t zeros[V90_PCM_FRAME_MAX_BITS] = { 0 };
	size_t sequence_bits = 0u;
	size_t cpd_bits = 0u;
	size_t expected_frames;

	CHECK(make_width_mapping(&mapping, law, frame_bits) &&
	      mapping.frame_bits == frame_bits &&
	      source_init(&source, &mapping) &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      receiver.protocol == PCM_PROTOCOL_V92 &&
	      pcm_downstream_rx_is_fresh(&receiver),
	      "V.92 width %u law %u fixture did not initialize",
	      frame_bits, law);
	CHECK(push_v92_trn2d(
		      &source,
		      V90_PHASE4_TRN2D_MINIMUM_FRAMES +
			      EXTRA_TRN2D_FRAMES,
		      &receiver, &observer) &&
	      v92_suvd_encode(
		      &parameters, frame_bits, sequence, sizeof(sequence),
		      &sequence_bits) == V92_SUVD_OK &&
	      push_v92_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer) &&
	      v92_cpd_encode(
		      &cpd, frame_bits, cpd_sequence,
		      sizeof(cpd_sequence), &cpd_bits) == V92_CPD_OK &&
	      push_v92_sequence(
		      &source, cpd_sequence, cpd_bits,
		      &receiver, &observer) &&
	      observer.cpd_records == 1u &&
	      pcm_downstream_rx_expect_ed(&receiver) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_rx_expect_ed(&receiver) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      push_v92_sequence(
		      &source, cpd_sequence, cpd_bits,
		      &receiver, &observer) &&
	      push_v92_frame(&source, zeros, &receiver, &observer) &&
	      push_v92_frame(&source, zeros, &receiver, &observer) &&
	      finish_v92_source(&source, &receiver, &observer),
	      "V.92 width %u law %u line stream stopped", frame_bits, law);
	expected_frames = V90_PHASE4_TRN2D_MINIMUM_FRAMES +
			  EXTRA_TRN2D_FRAMES +
			  (sequence_bits + 2u * cpd_bits) / frame_bits +
			  V90_PHASE4_ED_FRAMES;
	CHECK(observer.training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      observer.records == 1u && observer.invalid_frames == 0u &&
	      observer.crc_mismatches == 0u &&
	      observer.cpd_records == 2u &&
	      observer.cpd_invalid_frames == 0u &&
	      observer.cpd_crc_mismatches == 0u &&
	      observer.last_parameters.request_silence ==
		      parameters.request_silence &&
	      observer.last_parameters.acknowledges_cpu ==
		      parameters.acknowledges_cpu &&
	      observer.last_cpd.four_times_gain_q0_16 ==
		      cpd.four_times_gain_q0_16 &&
	      observer.last_cpd.data_rate_number == cpd.data_rate_number &&
	      observer.last_cpd.trellis == cpd.trellis &&
	      observer.last_cpd.extend_e2u == cpd.extend_e2u &&
	      observer.last_cpd.acknowledges_cpu == cpd.acknowledges_cpu &&
	      !observer.last_cpd.modulus_present &&
	      !observer.last_cpd.filters_present &&
	      !observer.last_cpd.constellations_present &&
	      observer.ed_events == 1u && observer.ed_bit_errors == 0u &&
	      receiver.stage == PCM_DOWNSTREAM_RX_COMPLETE &&
	      receiver.stream.received_frames == expected_frames &&
	      receiver.stream.received_symbols ==
		      expected_frames * V90_CP_FRAME_INTERVALS &&
	      receiver.trn2d_bit_errors == 0u &&
	      pcm_downstream_rx_is_valid(&receiver) &&
	      pcm_downstream_rx_is_complete(&receiver),
	      "V.92 width %u law %u status result changed", frame_bits, law);
	return true;
}

static bool test_v92_control_widths(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t frame_bits =
			     V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS;
		     frame_bits <=
			     V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MAX_BITS;
		     ++frame_bits) {
			CHECK(run_v92_width((enum g711_law)law, frame_bits),
			      "V.92 downstream width matrix stopped");
		}
	}
	return true;
}

static bool test_v92_control_recovery(void)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = true,
		.acknowledges_cpu = true,
	};
	const struct v92_cpd_parameters cpd = {
		.four_times_gain_q0_16 = UINT16_C(0x4567),
		.data_rate_number = 7u,
		.trellis = V92_CPD_TRELLIS_64_STATE,
		.extend_e2u = true,
		.acknowledges_cpu = true,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_downstream_rx receiver;
	struct source source;
	struct v92_observer observer = { 0 };
	uint8_t sequence[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t corrupt[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t cpd_sequence[V92_CPD_MAX_FRAME_BYTES];
	uint8_t cpd_corrupt[V92_CPD_MAX_FRAME_BYTES];
	size_t sequence_bits = 0u;
	size_t cpd_bits = 0u;

	CHECK(make_mapping(&mapping, G711_LAW_PCMA, 2u, 2u) &&
	      source_init(&source, &mapping) &&
	      pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V92, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) == PCM_DOWNSTREAM_RX_OK &&
	      v92_suvd_encode(
		      &parameters, FRAME_BITS, sequence, sizeof(sequence),
		      &sequence_bits) == V92_SUVD_OK &&
	      v92_cpd_encode(
		      &cpd, FRAME_BITS, cpd_sequence,
		      sizeof(cpd_sequence), &cpd_bits) == V92_CPD_OK,
	      "V.92 recovery fixture did not initialize");
	memcpy(corrupt, sequence, sizeof(corrupt));
	corrupt[35u / 8u] ^= (uint8_t)(1u << (7u - 35u % 8u));
	memcpy(cpd_corrupt, cpd_sequence, sizeof(cpd_corrupt));
	cpd_corrupt[35u / 8u] ^= (uint8_t)(1u << (7u - 35u % 8u));
	CHECK(push_v92_trn2d(
		      &source,
		      V90_PHASE4_TRN2D_MINIMUM_FRAMES +
			      EXTRA_TRN2D_FRAMES,
		      &receiver, &observer) &&
	      push_misaligned_v92_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer) &&
	      push_v92_sequence(
		      &source, corrupt, sequence_bits,
		      &receiver, &observer) &&
	      push_v92_sequence(
		      &source, sequence, sequence_bits,
		      &receiver, &observer) &&
	      push_v92_sequence(
		      &source, cpd_corrupt, cpd_bits,
		      &receiver, &observer) &&
	      push_v92_sequence(
		      &source, cpd_sequence, cpd_bits,
		      &receiver, &observer) &&
	      finish_v92_source(&source, &receiver, &observer),
	      "V.92 recovery stream stopped");
	CHECK(observer.training_frames == V90_PHASE4_TRN2D_MINIMUM_FRAMES &&
	      observer.invalid_frames == 1u &&
	      observer.crc_mismatches == 1u && observer.records == 1u &&
	      observer.cpd_invalid_frames == 0u &&
	      observer.cpd_crc_mismatches == 1u &&
	      observer.cpd_records == 1u &&
	      observer.last_parameters.request_silence &&
	      observer.last_parameters.acknowledges_cpu &&
	      receiver.stage == PCM_DOWNSTREAM_RX_V92_CONTROL &&
	      receiver.trn2d_bit_errors == 0u &&
	      pcm_downstream_rx_is_valid(&receiver),
	      "V.92 receiver did not recover on a mapping-frame boundary");
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
	struct pcm_downstream_rx receiver;
	struct pcm_downstream_rx saved;
	struct pcm_downstream_rx untouched;
	struct pcm_downstream_rx untouched_saved;
	struct pcm_downstream_event event;
	struct pcm_downstream_event event_saved;
	struct v90_pcm_symbol noisy;

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
	CHECK(pcm_downstream_rx_init(
		      NULL, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      pcm_downstream_rx_init(
		      &untouched, (enum pcm_protocol)99, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      pcm_downstream_rx_init(
		      &untouched, PCM_PROTOCOL_V90, NULL,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      pcm_downstream_rx_init(
		      &untouched, PCM_PROTOCOL_V90, &mapping,
		      (enum v90_pcm_mapping_plane)2) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      pcm_downstream_rx_init(
		      &untouched, PCM_PROTOCOL_V90, &data_mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_INVALID_PARAMETERS &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(pcm_downstream_rx_init(
		      &receiver, PCM_PROTOCOL_V90, &mapping,
		      V90_PCM_MAPPING_TRANSMIT) ==
		      PCM_DOWNSTREAM_RX_OK &&
	      pcm_downstream_rx_is_valid(&receiver) &&
	      pcm_downstream_rx_is_fresh(&receiver) &&
	      !pcm_downstream_rx_is_valid(NULL) &&
	      !pcm_downstream_rx_is_fresh(NULL),
	      "valid receiver did not initialize");
	CHECK(pcm_downstream_rx_expect_ed(NULL) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      pcm_downstream_rx_expect_ed(&receiver) ==
		      PCM_DOWNSTREAM_RX_INVALID_STATE &&
	      !pcm_downstream_rx_is_complete(&receiver) &&
	      !pcm_downstream_rx_has_failed(&receiver),
	      "fresh receiver accepted Ed or reported terminal state");
	saved = receiver;
	memset(&event, 0xa5, sizeof(event));
	event_saved = event;
	CHECK(pcm_downstream_rx_receive(NULL, 0, &event) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      pcm_downstream_rx_receive(&receiver, 0, NULL) ==
		      PCM_DOWNSTREAM_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_saved, sizeof(event)) == 0,
	      "invalid receive call changed state or output");
	CHECK(v90_pcm_symbol_make(
		      &noisy, mapping.law, 1, false),
	      "off-constellation symbol did not initialize");
	CHECK(pcm_downstream_rx_receive(
		      &receiver, noisy.octet, &event) ==
		      PCM_DOWNSTREAM_RX_BUFFERING &&
	      receiver.stream.received_symbols == 1u &&
	      !pcm_downstream_rx_is_fresh(&receiver) &&
	      memcmp(&event, &event_saved, sizeof(event)) == 0,
	      "off-constellation sample lost its interval or produced an event");
	receiver.stage = (enum pcm_downstream_rx_stage)99;
	CHECK(!pcm_downstream_rx_is_valid(&receiver) &&
	      pcm_downstream_rx_receive(
		      &receiver, noisy.octet, &event) ==
		      PCM_DOWNSTREAM_RX_INVALID_STATE,
	      "receiver accepted an unknown stage");
	receiver = saved;
	receiver.stream.descrambler.history = 1;
	CHECK(pcm_downstream_rx_is_valid(&receiver) &&
	      !pcm_downstream_rx_is_fresh(&receiver),
	      "fresh predicate ignored used coding history");
	CHECK(strcmp(pcm_downstream_rx_result_name(
		     PCM_DOWNSTREAM_RX_MP_PRIME),
		     "MP prime") == 0 &&
	      strcmp(pcm_downstream_rx_result_name(
		     PCM_DOWNSTREAM_RX_SUVD),
		     "SUVd") == 0 &&
	      strcmp(pcm_downstream_rx_result_name(
		     PCM_DOWNSTREAM_RX_SUVD_REJECTED),
		     "SUVd rejected") == 0 &&
	      strcmp(pcm_downstream_rx_result_name(
		     PCM_DOWNSTREAM_RX_CPD),
		     "CPd") == 0 &&
	      strcmp(pcm_downstream_rx_result_name(
		     PCM_DOWNSTREAM_RX_CPD_REJECTED),
		     "CPd rejected") == 0 &&
	      strcmp(pcm_downstream_rx_result_name(
		     (enum pcm_downstream_rx_result)99),
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
		{ "V.92 buffered-prefix Ed", test_v92_buffered_prefix_ed },
		{ "V.92 control width matrix", test_v92_control_widths },
		{ "V.92 control recovery", test_v92_control_recovery },
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
