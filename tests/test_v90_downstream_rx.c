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
	struct v90_cp_constellation *constellation, uint8_t ucode)
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
		      result == V90_DOWNSTREAM_RX_MP_REJECTED,
		      "downstream receiver returned %s",
		      v90_downstream_rx_result_name(result));
		CHECK(observer->event_count < MAX_EVENTS,
		      "downstream receiver emitted too many records");
		observer->events[observer->event_count++] =
			(struct observed_event){
				.result = result,
				.mp = event.mp,
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
		      V90_TRN2D_MINIMUM_SYMBOLS /
			      V90_CP_FRAME_INTERVALS,
		      &receiver, &observer) &&
	      push_trn2d(
		      &source, EXTRA_TRN2D_FRAMES,
		      &receiver, &observer),
	      "continuous TRN2d stream stopped");
	CHECK(observer.training_frames ==
		      V90_TRN2D_MINIMUM_SYMBOLS /
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
	CHECK(receiver.stream.training_bit_errors == 0 &&
	      receiver.stream.received_symbols >=
		      V90_TRN2D_MINIMUM_SYMBOLS +
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
	receiver.stage = (enum v90_downstream_rx_stage)2;
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
