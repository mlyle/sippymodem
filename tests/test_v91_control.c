// SPDX-License-Identifier: BSD-3-Clause
#include "v91_control.h"
#include "pcm_control_frame.h"
#include "test.h"
#include <string.h>

static bool info_equal(const struct v91_info *left, const struct v91_info *right)
{
	return left->law == right->law &&
	       left->maximum_power == right->maximum_power &&
	       left->custom_dil == right->custom_dil &&
	       left->control_channel == right->control_channel &&
	       left->acknowledges_peer == right->acknowledges_peer &&
	       left->receiver_power == right->receiver_power &&
	       left->transparent == right->transparent &&
	       left->require_transparent == right->require_transparent;
}

static bool test_info(void)
{
	for (unsigned law = 0; law < 2; ++law) {
		struct v91_info p = { .law = law, .custom_dil = true,
			.maximum_power = 17, .acknowledges_peer = true }, q;
		uint8_t frame[V91_INFO_BYTES];
		CHECK(v91_info_encode(&p, frame) && v91_info_decode(frame, &q) &&
		      q.law == p.law && q.custom_dil && q.acknowledges_peer &&
		      q.maximum_power == 17, "INFO fields differ");
		CHECK(frame[0] == 0xf7 && (frame[1] & 0xf0) == 0x20 &&
		      packed_msb_bit(frame, 26) && packed_msb_bit(frame, 28) &&
		      packed_msb_bit(frame, 33) && packed_msb_bit(frame, 37),
		      "INFO wire positions differ from Table 6");
		struct v91_info saved = q;
		for (unsigned bit = 0; bit < V91_INFO_BITS; ++bit) {
			frame[bit / 8] ^= 0x80u >> (bit % 8);
			CHECK(!v91_info_decode(frame, &q) && info_equal(&q, &saved),
			      "corrupt INFO bit %u accepted", bit);
			frame[bit / 8] ^= 0x80u >> (bit % 8);
		}
	}
	return true;
}

static bool test_cp(void)
{
	for (unsigned count = 1; count <= 6; ++count) {
		struct v91_cp p = { .count = count, .rate = 28, .acknowledges_peer = true }, q;
		uint8_t frame[V91_CP_MAX_BYTES];
		memset(p.constellations, 0xff, count * sizeof(p.constellations[0]));
		for (unsigned i = 0; i < 6; ++i) p.frame_constellation[i] = i % count;
		size_t bits = v91_cp_encode(&p, frame);
		CHECK(bits != 0 && bits % 6 == 0 &&
		      bits == v91_cp_length(frame, 136) && v91_cp_decode(frame, bits, &q) &&
		      memcmp(&p, &q, sizeof(p)) == 0, "CP differs at %u rows", count);
		CHECK(packed_msb_bit(frame, 19) && packed_msb_bit(frame, 33) &&
		      packed_msb_read_lsb_field(frame, 20, 5) == 28,
		      "CP wire positions differ from Table 4");
		struct v91_cp saved = q;
		for (unsigned bit = 0; bit < bits; ++bit) {
			frame[bit / 8] ^= 0x80u >> (bit % 8);
			CHECK(!v91_cp_decode(frame, bits, &q) && memcmp(&q, &saved, sizeof(q)) == 0,
			      "corrupt CP bit %u accepted", bit);
			frame[bit / 8] ^= 0x80u >> (bit % 8);
		}
		/* Table 4 distinguishes fixed compatibility bits from reserved
		 * fields. Recompute the CRC so it cannot hide either contract. */
		static const unsigned fixed[] = { 19, 30, 31, 32, 128 };
		static const unsigned reserved[][2] = {
			{ 25, 29 }, { 35, 50 }, { 52, 67 }, { 69, 84 },
			{ 86, 101 }, { 129, 135 },
		};
		size_t payload = pcm_control_frame_payload_end(count, false);
		for (unsigned bit = 18; bit < 136; ++bit) {
			bool is_fixed = false, is_reserved = false;
			for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
				is_fixed |= bit == fixed[i];
			for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
				is_reserved |= bit >= reserved[i][0] && bit <= reserved[i][1];
			if (!is_fixed && !is_reserved)
				continue;
			frame[bit / 8] ^= 0x80u >> (bit % 8);
			packed_msb_write_msb_field(frame, payload + 1, 16,
				pcm_control_frame_crc(frame, payload));
			CHECK(v91_cp_length(frame, 136) == (is_fixed ? 0 : bits) &&
			      v91_cp_decode(frame, bits, &q) == !is_fixed &&
			      memcmp(&q, &saved, sizeof(q)) == 0,
			      "CP header bit %u, rows %u violated fixed/reserved contract", bit, count);
			frame[bit / 8] ^= 0x80u >> (bit % 8);
			packed_msb_write_msb_field(frame, payload + 1, 16,
				pcm_control_frame_crc(frame, payload));
		}
		p.rate = 28;
		p.constellations[0].ucode_mask[0] = 0;
		CHECK(v91_cp_encode(&p, frame) == 0, "insufficient constellation capacity accepted");
		memset(p.constellations[0].ucode_mask, 0xff, sizeof(p.constellations[0].ucode_mask));
		p.rate = 29;
		CHECK(v91_cp_encode(&p, frame) == 0, "out-of-range rate accepted");
	}
	return true;
}
static bool test_info_receiver(void)
{
	struct v91_info_receiver receiver;
	struct v91_info sent = { .law = G711_LAW_PCMA, .custom_dil = true }, received;
	uint8_t frame[V91_INFO_BYTES], sign = 0;
	CHECK(v91_info_encode(&sent, frame), "INFO encode failed");
	v91_info_receiver_init(&receiver);
	for (unsigned record = 0; record < 4; ++record) {
		if (record == 2)
			frame[3] ^= 1; /* CRC-covered corruption breaks adjacency. */
		for (unsigned bit = 0; bit < V91_INFO_BITS; ++bit) {
			sign ^= packed_msb_bit(frame, bit);
			bool ready = v91_info_receiver_sample(&receiver, sign, &received);
			CHECK(ready == (bit == V91_INFO_BITS - 1 && record != 2),
			      "INFO acquisition at record %u bit %u", record, bit);
		}
		if (record == 2)
			frame[3] ^= 1;
		CHECK(receiver.consecutive == (record == 1 ? 2 : record == 2 ? 0 : 1),
		      "invalid retrain qualification count");
	}
	v91_info_receiver_init(&receiver);
	CHECK(receiver.position == 0 && receiver.consecutive == 0,
	      "reset retained acquisition history");
	return true;
}

static bool test_control(void)
{
	for (unsigned flags = 0; flags < 4; ++flags) {
		uint8_t frame[12];
		bool retrain, cleardown;
		CHECK(v91_control_encode(frame, flags & 1, flags & 2) &&
		      v91_control_decode(frame, &retrain, &cleardown) &&
		      retrain == !!(flags & 1) && cleardown == !!(flags & 2),
		      "control flags differ");
		for (unsigned bit = 0; bit < 96; ++bit) {
			frame[bit / 8] ^= 0x80u >> (bit % 8);
			CHECK(!v91_control_decode(frame, &retrain, &cleardown),
			      "corrupt control bit %u accepted", bit);
			frame[bit / 8] ^= 0x80u >> (bit % 8);
		}
	}
	return true;
}
int main(void)
{
	return test_info() && test_cp() && test_control() && test_info_receiver() ? 0 : 1;
}
