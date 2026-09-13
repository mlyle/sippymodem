// SPDX-License-Identifier: BSD-3-Clause

#include <v92_control_rx.h>
#include <packed_bits.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t frame_bit(const uint8_t *frame, size_t bit)
{
	return (uint8_t)(
		(frame[bit / 8u] >> (7u - bit % 8u)) & 1u);
}

static void flip_bit(uint8_t *frame, size_t bit)
{
	frame[bit / 8u] ^= (uint8_t)(1u << (7u - bit % 8u));
}

static bool suvd_parameters_match(
	const struct v92_suvd_parameters *left,
	const struct v92_suvd_parameters *right)
{
	return left->request_silence == right->request_silence &&
	       left->acknowledges_cpu == right->acknowledges_cpu;
}

static struct v92_cpd_parameters cpd_selection(unsigned int sections)
{
	struct v92_cpd_parameters parameters = {
		.four_times_gain_q0_16 = UINT16_C(0x2345),
		.data_rate_number = 3u,
		.trellis = V92_CPD_TRELLIS_32_STATE,
		.extend_e2u = true,
		.acknowledges_cpu = true,
		.modulus_present = (sections & 1u) != 0u,
		.filters_present = (sections & 2u) != 0u,
		.constellations_present = (sections & 4u) != 0u,
	};

	if (parameters.modulus_present) {
		for (size_t index = 0u;
		     index < V92_CPD_MODULUS_COUNT; ++index)
			parameters.modulus[index] = (uint8_t)(index + 1u);
	}
	if (parameters.filters_present) {
		parameters.filter[V92_CPD_PRECODER_FEED_FORWARD].count = 1u;
		parameters.filter[V92_CPD_PRECODER_FEED_FORWARD]
			.coefficient[0] = INT16_MAX;
		parameters.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 1u;
		parameters.filter[V92_CPD_PREFILTER_FEED_FORWARD]
			.coefficient[0] = INT16_MIN;
	}
	if (parameters.constellations_present) {
		parameters.constellation[0].count = 3u;
		parameters.constellation[0].positive_linear[0] = 128;
		parameters.constellation[0].positive_linear[1] = 512;
		parameters.constellation[0].positive_linear[2] = 2048;
	}
	return parameters;
}

static bool cpd_parameters_match(
	const struct v92_cpd_parameters *left,
	const struct v92_cpd_parameters *right)
{
	if (left->four_times_gain_q0_16 != right->four_times_gain_q0_16 ||
	    left->data_rate_number != right->data_rate_number ||
	    left->trellis != right->trellis ||
	    left->extend_e2u != right->extend_e2u ||
	    left->acknowledges_cpu != right->acknowledges_cpu ||
	    left->modulus_present != right->modulus_present ||
	    left->filters_present != right->filters_present ||
	    left->constellations_present != right->constellations_present ||
	    memcmp(left->modulus, right->modulus, sizeof(left->modulus)) != 0 ||
	    memcmp(left->frame_constellation, right->frame_constellation,
		   sizeof(left->frame_constellation)) != 0)
		return false;
	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		if (left->filter[section].count != right->filter[section].count ||
		    memcmp(left->filter[section].coefficient,
			   right->filter[section].coefficient,
			   sizeof(left->filter[section].coefficient)) != 0)
			return false;
	}
	for (size_t set = 0u; set < V92_CPD_MAX_CONSTELLATIONS; ++set) {
		if (left->constellation[set].count !=
			    right->constellation[set].count ||
		    memcmp(left->constellation[set].positive_linear,
			   right->constellation[set].positive_linear,
			   sizeof(left->constellation[set].positive_linear)) != 0)
			return false;
	}
	return true;
}

static bool suvu_parameters_match(
	const struct v92_suvu_parameters *left,
	const struct v92_suvu_parameters *right)
{
	return left->prefilter_level_db_q2_2 ==
		       right->prefilter_level_db_q2_2 &&
	       left->wait_for_cpu == right->wait_for_cpu &&
	       left->request_silence == right->request_silence &&
	       left->acknowledges_cpd == right->acknowledges_cpd;
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpu_parameters cpu_selection(
	uint8_t count, bool codec_differs)
{
	struct v92_cpu_parameters parameters = {
		.law = G711_LAW_PCMU,
		.trn1d_ratio_q3_13 = UINT16_C(0x4567),
		.shaping_a1_q1_6 = -17,
		.shaping_a2_q1_6 = 23,
		.shaping_b1_q1_6 = -31,
		.shaping_b2_q1_6 = 47,
		.data_rate_number = 3u,
		.sign_redundancy = 2u,
		.spectral_lookahead_frames = 1u,
		.constellation_count = count,
		.acknowledges_cpd = true,
		.codec_constellations_differ = codec_differs,
	};

	for (uint8_t index = 0u; index < count; ++index) {
		add_ucode(&parameters.transmit_constellations[index], index);
		add_ucode(
			&parameters.transmit_constellations[index],
			(uint8_t)(32u + index));
		if (codec_differs) {
			add_ucode(
				&parameters.codec_constellations[index],
				(uint8_t)(96u - index));
		}
	}
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		parameters.frame_constellation[interval] =
			(uint8_t)(interval % count);
	}
	return parameters;
}

static bool cpu_parameters_match(
	const struct v92_cpu_parameters *left,
	const struct v92_cpu_parameters *right)
{
	return left->law == right->law &&
	       left->trn1d_ratio_q3_13 == right->trn1d_ratio_q3_13 &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->data_rate_number == right->data_rate_number &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead_frames ==
		       right->spectral_lookahead_frames &&
	       left->constellation_count == right->constellation_count &&
	       left->acknowledges_cpd == right->acknowledges_cpd &&
	       left->codec_constellations_differ ==
		       right->codec_constellations_differ &&
	       memcmp(
		       left->frame_constellation,
		       right->frame_constellation,
		       sizeof(left->frame_constellation)) == 0 &&
	       memcmp(
		       left->transmit_constellations,
		       right->transmit_constellations,
		       sizeof(left->transmit_constellations)) == 0 &&
	       memcmp(
		       left->codec_constellations,
		       right->codec_constellations,
		       sizeof(left->codec_constellations)) == 0;
}

static bool receive_bit(
	struct v92_control_rx *receiver, uint8_t bit,
	struct v92_control_event *event)
{
	CHECK(v92_control_rx_receive_bit(receiver, bit, event) ==
		      V92_CONTROL_RX_OK &&
	      v92_control_rx_is_valid(receiver),
	      "control receiver rejected bit %u", bit);
	return true;
}

static bool feed_frame(
	struct v92_control_rx *receiver,
	const uint8_t *frame, size_t first_bit, size_t bit_count,
	struct v92_control_event *last_event)
{
	struct v92_control_event observed = { 0 };

	for (size_t bit = first_bit; bit < bit_count; ++bit) {
		struct v92_control_event event;

		CHECK(receive_bit(receiver, frame_bit(frame, bit), &event),
		      "control frame stopped at bit %zu", bit);
		CHECK(!receiver->record_boundary ||
		      (event.type != V92_CONTROL_EVENT_NONE && bit + 1u == bit_count),
		      "control boundary appeared inside a record at bit %zu", bit);
		if (event.type == V92_CONTROL_EVENT_NONE)
			continue;
		CHECK(observed.type == V92_CONTROL_EVENT_NONE,
		      "control frame emitted multiple events");
		observed = event;
	}
	*last_event = observed;
	return true;
}

static bool test_all_frame_widths(void)
{
	for (uint8_t width = V92_STATUS_UPDATE_DOWNSTREAM_FRAME_MIN_BITS;
	     width <= V92_STATUS_UPDATE_DOWNSTREAM_DATA_MAX_BITS; ++width) {
		for (unsigned int flags = 0u; flags < 4u; ++flags) {
			struct v92_suvd_parameters parameters = {
				.request_silence = (flags & 1u) != 0u,
				.acknowledges_cpu = (flags & 2u) != 0u,
			};
			struct v92_control_rx receiver;
			struct v92_control_event event;
			uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
			size_t bit_count = 0u;

			CHECK(v92_suvd_encode(
				      &parameters, width, frame, sizeof(frame),
				      &bit_count) == V92_SUVD_OK &&
			      v92_control_rx_init_downstream(&receiver, width) ==
				      V92_CONTROL_RX_OK,
			      "SUVd width %u fixture did not initialize", width);
			for (size_t bit = 0u; bit < 47u; ++bit)
				CHECK(receive_bit(&receiver, 1u, &event) &&
				      event.type == V92_CONTROL_EVENT_NONE,
				      "TRN2d suffix emitted an event");
			CHECK(feed_frame(
				      &receiver, frame, 0u, bit_count, &event) &&
			      event.type == V92_CONTROL_EVENT_SUVD &&
			      event.record.suvd.frame_result == V92_SUVD_OK &&
			      suvd_parameters_match(
				      &event.record.suvd.parameters, &parameters) &&
			      receiver.stage == V92_CONTROL_RX_SEARCH_SYNC &&
			      receiver.sync_one_count == 0u,
			      "SUVd flags 0x%x width %u did not decode",
			      flags, width);
		}
	}
	return true;
}

static bool test_cpd_matrix(void)
{
	unsigned int cases = 0u;

	for (uint8_t width = V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
	     width <= V92_CPD_DOWNSTREAM_DATA_MAX_BITS; ++width) {
		for (unsigned int sections = 0u; sections < 8u; ++sections) {
			struct v92_cpd_parameters parameters =
				cpd_selection(sections);
			struct v92_control_rx receiver;
			struct v92_control_event event;
			uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
			size_t bit_count = 0u;

			CHECK(v92_cpd_encode(
				      &parameters, width, frame, sizeof(frame),
				      &bit_count) == V92_CPD_OK &&
			      v92_control_rx_init_downstream(&receiver, width) ==
				      V92_CONTROL_RX_OK,
			      "CPd sections 0x%x width %u did not initialize",
			      sections, width);
			for (size_t bit = 0u; bit < 47u; ++bit)
				CHECK(receive_bit(&receiver, 1u, &event) &&
				      event.type == V92_CONTROL_EVENT_NONE,
				      "TRN2d suffix emitted a CPd event");
			CHECK(feed_frame(
				      &receiver, frame, 0u, bit_count, &event) &&
			      event.type == V92_CONTROL_EVENT_CPD &&
			      event.record.cpd.frame_result == V92_CPD_OK &&
			      cpd_parameters_match(
				      &event.record.cpd.parameters, &parameters) &&
			      receiver.stage == V92_CONTROL_RX_SEARCH_SYNC &&
			      receiver.frame_bit_count == 0u &&
			      receiver.sync_one_count == 0u,
			      "CPd sections 0x%x width %u did not decode",
			      sections, width);
			cases++;
		}
	}
	CHECK(cases == 272u, "generated %u CPd receiver cases", cases);
	return true;
}

static bool test_upstream_records(void)
{
	for (unsigned int alignment = V92_CONTROL_UPSTREAM_4_POINT_BITS;
	     alignment <= V92_CONTROL_UPSTREAM_DATA_MAX_BITS; alignment += 2u) {
		struct v92_control_rx receiver;
		struct v92_control_event event;

		if (alignment != 24u && alignment < 36u)
			continue;
		CHECK(v92_control_rx_init_upstream(
			      &receiver, alignment) == V92_CONTROL_RX_OK &&
		      receiver.direction == V92_CONTROL_UPSTREAM &&
		      receiver.alignment_bits == alignment,
		      "upstream control receiver did not initialize");
		for (size_t bit = 0u; bit < 47u; ++bit) {
			CHECK(receive_bit(&receiver, 1u, &event) &&
			      event.type == V92_CONTROL_EVENT_NONE,
			      "TRN2u suffix emitted an event");
		}
		for (unsigned int flags = 0u; flags < 16u; ++flags) {
			struct v92_suvu_parameters parameters = {
				.prefilter_level_db_q2_2 = (int8_t)((int)flags - 8),
				.wait_for_cpu = (flags & 1u) != 0u,
				.request_silence = (flags & 2u) != 0u,
				.acknowledges_cpd = (flags & 4u) != 0u,
			};
			uint8_t frame[V92_SUVU_MAX_FRAME_BYTES];
			size_t frame_bits = 0u;

			CHECK(v92_suvu_encode(
				      &parameters, alignment, frame,
				      sizeof(frame), &frame_bits) == V92_SUVU_OK &&
			      feed_frame(
				      &receiver, frame, 0u, frame_bits, &event) &&
			      event.type == V92_CONTROL_EVENT_SUVU &&
			      event.record.suvu.frame_result == V92_SUVU_OK &&
			      suvu_parameters_match(
				      &event.record.suvu.parameters, &parameters),
			      "SUVu flags 0x%x alignment %u did not decode",
			      flags, alignment);
		}
		for (uint8_t count = 1u;
		     count <= V92_CPU_MAX_CONSTELLATIONS; ++count) {
			for (unsigned int differs = 0u; differs < 2u; ++differs) {
				struct v92_cpu_parameters parameters =
					cpu_selection(count, differs != 0u);
				uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
				size_t frame_bits = 0u;

				CHECK(v92_cpu_encode(
					      &parameters, alignment, frame,
					      sizeof(frame), &frame_bits) == V92_CPU_OK &&
				      feed_frame(
					      &receiver, frame, 0u, frame_bits,
					      &event) &&
				      event.type == V92_CONTROL_EVENT_CPU &&
				      event.record.cpu.frame_result == V92_CPU_OK &&
				      cpu_parameters_match(
					      &event.record.cpu.parameters,
					      &parameters),
				      "CPu shape %u/%u alignment %u did not decode",
				      count, differs,
				      alignment);
			}
		}
		for (unsigned int fields = 0u; fields < 46u; ++fields) {
			struct v92_cpus_parameters parameters = {
				.data_rate_number = (uint8_t)(fields / 2u),
				.acknowledges_cpd = (fields & 1u) != 0u,
			};
			uint8_t frame[V92_CPUS_MAX_FRAME_BYTES];
			size_t frame_bits = 0u;

			CHECK(v92_cpus_encode(&parameters, alignment, frame,
				      sizeof(frame), &frame_bits) == V92_CPU_OK &&
			      feed_frame(&receiver, frame, 0u, frame_bits, &event) &&
			      event.type == V92_CONTROL_EVENT_CPUS &&
			      event.record.cpus.frame_result == V92_CPU_OK &&
			      event.record.cpus.parameters.data_rate_number ==
				      parameters.data_rate_number &&
			      event.record.cpus.parameters.acknowledges_cpd ==
				      parameters.acknowledges_cpd,
			      "CPus fields %u alignment %u did not decode", fields, alignment);
		}
	}
	return true;
}

/* A truncated candidate can fail while consuming the next sync prefix.
 * The complete retry must survive that rejection, even without an idle gap. */
static bool check_truncated_record_recovery(
	const uint8_t *frame, size_t bit_count, uint8_t width,
	enum v92_control_direction direction,
	enum v92_control_event_type expected)
{
	for (size_t prefix = 0u; prefix < bit_count; ++prefix) {
		struct v92_control_rx receiver;
		struct v92_control_event event;
		unsigned int accepted = 0u;

		CHECK((direction == V92_CONTROL_DOWNSTREAM ?
			      v92_control_rx_init_downstream(&receiver, width) :
			      v92_control_rx_init_upstream(&receiver, width)) ==
			      V92_CONTROL_RX_OK &&
		      feed_frame(&receiver, frame, 0u, prefix, &event) &&
		      event.type == V92_CONTROL_EVENT_NONE,
		      "truncated record fixture failed");
		for (size_t bit = 0u; bit < bit_count; ++bit) {
			CHECK(receive_bit(&receiver, frame_bit(frame, bit), &event),
			      "replacement stopped at bit %zu", bit);
			switch (event.type) {
			case V92_CONTROL_EVENT_CPD:
			case V92_CONTROL_EVENT_SUVD:
			case V92_CONTROL_EVENT_CPU:
			case V92_CONTROL_EVENT_SUVU:
			case V92_CONTROL_EVENT_CPUS:
				CHECK(event.type == expected && bit + 1u == bit_count,
				      "type %u width %u prefix %zu accepted an old candidate",
				      expected, width, prefix);
				accepted++;
				break;
			default:
				break;
			}
		}
		CHECK(accepted == 1u &&
		      receiver.stage == V92_CONTROL_RX_SEARCH_SYNC &&
		      receiver.sync_one_count == 0u,
		      "type %u width %u prefix %zu lost the complete retry",
		      expected, width, prefix);
	}
	return true;
}

static bool test_truncated_record_recovery(void)
{
	const struct v92_cpd_parameters cpd = cpd_selection(0u);
	const struct v92_cpu_parameters cpu = cpu_selection(1u, false);
	const struct v92_suvd_parameters suvd = { .acknowledges_cpu = true };
	const struct v92_suvu_parameters suvu = {
		.wait_for_cpu = true, .acknowledges_cpd = true,
	};
	const struct v92_cpus_parameters cpus = {
		.data_rate_number = 3u, .acknowledges_cpd = true,
	};
	uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
	size_t bits = 0u;

	for (uint8_t width = V92_CPD_DOWNSTREAM_FRAME_MIN_BITS;
	     width <= V92_CPD_DOWNSTREAM_DATA_MAX_BITS; ++width) {
		CHECK(v92_cpd_encode(&cpd, width, frame, sizeof(frame), &bits) ==
			      V92_CPD_OK &&
		      check_truncated_record_recovery(frame, bits, width,
			      V92_CONTROL_DOWNSTREAM, V92_CONTROL_EVENT_CPD) &&
		      v92_suvd_encode(&suvd, width, frame, sizeof(frame), &bits) ==
			      V92_SUVD_OK &&
		      check_truncated_record_recovery(frame, bits, width,
			      V92_CONTROL_DOWNSTREAM, V92_CONTROL_EVENT_SUVD),
		      "downstream width %u lost a control retry", width);
	}
	for (uint8_t width = 24u; width <= 72u; width += 2u) {
		if (!v92_control_upstream_alignment_is_valid(width))
			continue;
		CHECK(v92_cpu_encode(&cpu, width, frame, sizeof(frame), &bits) ==
			      V92_CPU_OK &&
		      check_truncated_record_recovery(frame, bits, width,
			      V92_CONTROL_UPSTREAM, V92_CONTROL_EVENT_CPU) &&
		      v92_suvu_encode(&suvu, width, frame, sizeof(frame), &bits) ==
			      V92_SUVU_OK &&
		      check_truncated_record_recovery(frame, bits, width,
			      V92_CONTROL_UPSTREAM, V92_CONTROL_EVENT_SUVU) &&
		      v92_cpus_encode(&cpus, width, frame, sizeof(frame), &bits) ==
			      V92_CPU_OK &&
		      check_truncated_record_recovery(frame, bits, width,
			      V92_CONTROL_UPSTREAM, V92_CONTROL_EVENT_CPUS),
		      "upstream width %u lost a control retry", width);
	}
	return true;
}

static bool test_short_recovery(void)
{
	const struct v92_cpus_parameters parameters = {
		.data_rate_number = 22u, .acknowledges_cpd = true,
	};
	struct v92_cpu_parameters cpu = cpu_selection(1u, false);
	struct v92_control_rx receiver;
	struct v92_control_event event;
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
	uint8_t short_frame[V92_CPUS_MAX_FRAME_BYTES];
	size_t bits = 0u;
	size_t short_bits = 0u;

	CHECK(v92_control_rx_init_upstream(&receiver, 50u) == V92_CONTROL_RX_OK &&
	      v92_cpus_encode(&parameters, 50u, short_frame, sizeof(short_frame),
		      &short_bits) == V92_CPU_OK &&
	      v92_cpu_encode(&cpu, 50u, frame, sizeof(frame), &bits) == V92_CPU_OK,
	      "CPus recovery fixture failed");
	flip_bit(short_frame, 35u);
	CHECK(feed_frame(&receiver, short_frame, 0u, short_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPUS_REJECTED &&
	      event.record.cpus.frame_result == V92_CPU_CRC_MISMATCH,
	      "CPus corruption was not a typed CRC rejection");
	flip_bit(short_frame, 35u);
	CHECK(feed_frame(&receiver, short_frame, 0u, 31u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      feed_frame(&receiver, frame, 0u, bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPU &&
	      cpu_parameters_match(&event.record.cpu.parameters, &cpu) &&
	      feed_frame(&receiver, short_frame, 0u, short_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPUS,
	      "CPus truncation prevented recovery to long/short records");
	packed_msb_write_lsb_field(short_frame, 21u, 5u, 31u);
	CHECK(feed_frame(&receiver, short_frame, 0u, 34u, &event) &&
	      event.type == V92_CONTROL_EVENT_CPUS_REJECTED &&
	      event.record.cpus.frame_result == V92_CPU_INVALID_FRAME &&
	      feed_frame(&receiver, frame, 0u, bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPU,
	      "CPus invalid rate prevented stream recovery");
	return true;
}

static bool test_upstream_recovery_and_rejection(void)
{
	const unsigned int alignment =
		V92_CONTROL_UPSTREAM_8_POINT_BITS;
	const struct v92_suvu_parameters suvu = {
		.prefilter_level_db_q2_2 = -7,
		.wait_for_cpu = true,
		.request_silence = true,
		.acknowledges_cpd = true,
	};
	struct v92_cpu_parameters cpu = cpu_selection(2u, true);
	struct v92_control_rx receiver;
	struct v92_control_event event;
	uint8_t suvu_frame[V92_SUVU_FRAME_BYTES];
	uint8_t suvu_corrupt[V92_SUVU_FRAME_BYTES];
	uint8_t cpu_frame[V92_CPU_MAX_FRAME_BYTES];
	uint8_t cpu_corrupt[V92_CPU_MAX_FRAME_BYTES];
	size_t suvu_bits = 0u;
	size_t cpu_bits = 0u;

	CHECK(v92_control_rx_init_upstream(&receiver, alignment) ==
		      V92_CONTROL_RX_OK &&
	      v92_suvu_encode(
		      &suvu, alignment, suvu_frame, sizeof(suvu_frame),
		      &suvu_bits) == V92_SUVU_OK &&
	      v92_cpu_encode(
		      &cpu, 36u, cpu_frame, sizeof(cpu_frame), &cpu_bits) ==
		      V92_CPU_OK,
	      "upstream rejection fixtures did not initialize");
	memcpy(suvu_corrupt, suvu_frame, sizeof(suvu_corrupt));
	flip_bit(suvu_corrupt, 35u);
	CHECK(feed_frame(
		      &receiver, suvu_corrupt, 0u, suvu_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVU_REJECTED &&
	      receiver.record_boundary &&
	      event.record.suvu.frame_result == V92_SUVU_CRC_MISMATCH,
	      "corrupt SUVu did not emit a CRC rejection");
	memcpy(cpu_corrupt, cpu_frame, sizeof(cpu_corrupt));
	flip_bit(cpu_corrupt, 21u);
	CHECK(feed_frame(
		      &receiver, cpu_corrupt, 0u, cpu_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPU_REJECTED &&
	      receiver.record_boundary &&
	      event.record.cpu.frame_result == V92_CPU_CRC_MISMATCH,
	      "corrupt CPu did not emit a CRC rejection");
	memcpy(cpu_corrupt, cpu_frame, sizeof(cpu_corrupt));
	flip_bit(cpu_corrupt, 20u);
	CHECK(feed_frame(
		      &receiver, cpu_corrupt, 0u, V92_CPU_HEADER_BITS,
		      &event) &&
	      event.type == V92_CONTROL_EVENT_CPU_REJECTED &&
	      event.record.cpu.frame_result == V92_CPU_INVALID_FRAME &&
	      !receiver.record_boundary &&
	      feed_frame(
		      &receiver, cpu_frame, 0u, cpu_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPU &&
	      receiver.record_boundary &&
	      cpu_parameters_match(&event.record.cpu.parameters, &cpu),
	      "malformed CPu prevented upstream-stream recovery");
	return true;
}

static bool test_recovery_and_rejection(void)
{
	const struct v92_suvd_parameters parameters = {
		.request_silence = true,
		.acknowledges_cpu = true,
	};
	struct v92_control_rx receiver;
	struct v92_control_event event;
	struct v92_cpd_parameters valid_cpd = cpd_selection(7u);
	struct v92_cpd_parameters cpd;
	uint8_t frame[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t corrupt[V92_SUVD_MAX_FRAME_BYTES];
	uint8_t cpd_frame[V92_CPD_MAX_FRAME_BYTES];
	uint8_t cpd_corrupt[V92_CPD_MAX_FRAME_BYTES];
	size_t bit_count = 0u;
	size_t cpd_bits = 0u;
	size_t valid_cpd_bits;
	const uint8_t width = 17u;

	CHECK(v92_suvd_encode(
		      &parameters, width, frame, sizeof(frame), &bit_count) ==
		      V92_SUVD_OK &&
	      v92_control_rx_init_downstream(&receiver, width) ==
		      V92_CONTROL_RX_OK,
	      "SUVd recovery fixture did not initialize");
	memcpy(corrupt, frame, sizeof(corrupt));
	flip_bit(corrupt, 35u);
	CHECK(feed_frame(&receiver, corrupt, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD_REJECTED &&
	      event.record.suvd.frame_result == V92_SUVD_CRC_MISMATCH,
	      "corrupt SUVd did not emit a CRC rejection");
	CHECK(feed_frame(&receiver, frame, 0u, 31u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      feed_frame(&receiver, frame, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD &&
	      suvd_parameters_match(
		      &event.record.suvd.parameters, &parameters),
	      "newer SUVd prefix did not replace a partial candidate");
	for (size_t bit = 0u; bit < 17u; ++bit)
		CHECK(receive_bit(&receiver, 1u, &event),
		      "non-status prefix stopped at bit %zu", bit);
	CHECK(receive_bit(&receiver, 0u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      receive_bit(&receiver, 0u, &event) &&
	      event.type == V92_CONTROL_EVENT_NONE &&
	      feed_frame(&receiver, frame, 0u, bit_count, &event) &&
	      event.type == V92_CONTROL_EVENT_SUVD,
	      "partial CPd candidate prevented SUVd recovery");

	CHECK(v92_cpd_encode(
		      &valid_cpd, width, cpd_frame, sizeof(cpd_frame),
		      &cpd_bits) == V92_CPD_OK,
	      "CPd recovery fixture did not encode");
	valid_cpd_bits = cpd_bits;
	memcpy(cpd_corrupt, cpd_frame, sizeof(cpd_corrupt));
	flip_bit(cpd_corrupt, 22u);
	CHECK(feed_frame(
		      &receiver, cpd_corrupt, 0u, cpd_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPD_REJECTED &&
	      event.record.cpd.frame_result == V92_CPD_CRC_MISMATCH,
	      "corrupt CPd did not emit a CRC rejection");
	cpd = cpd_selection(2u);
	CHECK(v92_cpd_encode(
		      &cpd, width, cpd_corrupt, sizeof(cpd_corrupt),
		      &cpd_bits) == V92_CPD_OK,
	      "CPd bound fixture did not encode");
	packed_msb_write_lsb_field(
		cpd_corrupt, V92_CPD_FIXED_BITS + 1u, 9u,
		V92_CPD_MAX_SECTION_COEFFICIENTS + 1u);
	CHECK(feed_frame(
		      &receiver, cpd_corrupt, 0u, cpd_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPD_REJECTED &&
	      event.record.cpd.frame_result == V92_CPD_INVALID_FRAME &&
	      feed_frame(
		      &receiver, cpd_frame, 0u, valid_cpd_bits, &event) &&
	      event.type == V92_CONTROL_EVENT_CPD &&
	      cpd_parameters_match(
		      &event.record.cpd.parameters, &valid_cpd),
	      "malformed CPd prevented control-stream recovery");
	return true;
}

static bool test_contracts(void)
{
	struct v92_control_rx receiver;
	struct v92_control_rx before;
	struct v92_control_event event;
	struct v92_control_event event_before;
	static const char *const names[] = {
		"ok", "invalid argument", "invalid parameters",
		"invalid state", "internal error",
	};

	memset(&receiver, 0xa5, sizeof(receiver));
	before = receiver;
	CHECK(v92_control_rx_init_downstream(NULL, 17u) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_init_upstream(
		      NULL, V92_CONTROL_UPSTREAM_4_POINT_BITS) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_init_downstream(&receiver, 8u) ==
		      V92_CONTROL_RX_INVALID_PARAMETERS &&
	      v92_control_rx_init_downstream(&receiver, 43u) ==
		      V92_CONTROL_RX_INVALID_PARAMETERS &&
	      v92_control_rx_init_upstream(
		      &receiver, 6u) ==
		      V92_CONTROL_RX_INVALID_PARAMETERS &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "invalid control receiver initialization changed state");
	for (unsigned int width = 0u; width <= 73u; ++width) {
		if (width == 24u ||
		    (width >= 36u && width <= 72u && width % 2u == 0u))
			continue;
		CHECK(v92_control_rx_init_upstream(&receiver, width) ==
			      V92_CONTROL_RX_INVALID_PARAMETERS &&
		      memcmp(&receiver, &before, sizeof(receiver)) == 0,
		      "invalid upstream width %u changed receiver", width);
	}
	CHECK(v92_control_rx_init_downstream(&receiver, 17u) ==
		      V92_CONTROL_RX_OK,
	      "control receiver did not initialize");
	before = receiver;
	memset(&event, 0xa5, sizeof(event));
	event_before = event;
	CHECK(v92_control_rx_receive_bit(NULL, 0u, &event) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_receive_bit(&receiver, 0u, NULL) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      v92_control_rx_receive_bit(&receiver, 2u, &event) ==
		      V92_CONTROL_RX_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid control receiver call changed state or event");
	receiver.direction = (enum v92_control_direction)99;
	before = receiver;
	CHECK(!v92_control_rx_is_valid(NULL) &&
	      !v92_control_rx_is_valid(&receiver) &&
	      v92_control_rx_receive_bit(&receiver, 0u, &event) ==
		      V92_CONTROL_RX_INVALID_STATE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "invalid control receiver state was not transactional");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_control_rx_result_name(
				      (enum v92_control_rx_result)index),
			      names[index]) == 0,
		      "control receiver result name %zu changed", index);
	CHECK(strcmp(
		      v92_control_rx_result_name(
			      (enum v92_control_rx_result)-1),
		      "unknown") == 0,
	      "unknown control receiver result name changed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 control receiver\n");
	if (!test_all_frame_widths() || !test_cpd_matrix() ||
	    !test_upstream_records() ||
	    !test_truncated_record_recovery() ||
	    !test_short_recovery() ||
	    !test_upstream_recovery_and_rejection() ||
	    !test_recovery_and_rejection() ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 control receiver\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 control receiver\n");
	return 0;
}
