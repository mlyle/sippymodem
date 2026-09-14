// SPDX-License-Identifier: BSD-3-Clause

#include <v92_qc1.h>
#include <v8_fsk.h>
#include <v8_framing.h>
#include <g711.h>

#include "test.h"

#include <stdlib.h>
#include <string.h>

static struct v92_qc1_message message_at(unsigned index)
{
	const bool digital = index >= 64;
	const unsigned values = digital ? 4 : 16;
	const unsigned local = digital ? index - 64 : index;
	struct v92_qc1_message message = {
		.sender = digital ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM,
		.acknowledgment = (local / values & 1u) != 0,
		.lapm = (local / values & 2u) != 0,
	};

	if (digital)
		message.signal.anspcm_level = local % values;
	else
		message.signal.qts_index = local % values;
	return message;
}

static bool same_message(const struct v92_qc1_message *a,
	const struct v92_qc1_message *b)
{
	return a->sender == b->sender &&
		a->acknowledgment == b->acknowledgment && a->lapm == b->lapm &&
		(a->sender == V90_ANALOG_MODEM ?
		 a->signal.qts_index == b->signal.qts_index :
		 a->signal.anspcm_level == b->signal.anspcm_level);
}

/* Independent chronological transcription of V.92 Tables 2/4/11/13. */
static void wire_bits(const struct v92_qc1_message *message, uint8_t bits[70])
{
	const char *prefix = "11111111110101010101";

	for (size_t i = 0; i < 20; ++i)
		bits[i] = prefix[i] - '0';
	memset(bits + 20, 0, 10);
	bits[21] = message->sender == V90_DIGITAL_MODEM;
	bits[22] = message->acknowledgment;
	bits[23] = message->lapm;
	bits[29] = 1;
	if (message->sender == V90_ANALOG_MODEM) {
		unsigned index = message->signal.qts_index;

		bits[24] = index >= 8;
		index %= 8;
		bits[26] = index >= 4;
		index %= 4;
		bits[27] = index >= 2;
		bits[28] = index % 2;
	} else {
		bits[27] = message->signal.anspcm_level >= 2;
		bits[28] = message->signal.anspcm_level % 2;
	}
	memcpy(bits + 30, bits, 30);
	memset(bits + 60, 1, 10);
}

static bool feed(struct v92_qc1_receiver *receiver, const uint8_t *bits,
	size_t count, const struct v92_qc1_message *expected)
{
	unsigned events = 0;

	for (size_t i = 0; i < count; ++i) {
		struct v92_qc1_message message = {
			.sender = (enum v90_modem_role)91,
		};
		const struct v92_qc1_message saved = message;
		bool ready = true;

		CHECK(v92_qc1_receive_bit(receiver, bits[i], &message, &ready),
		      "receive bit %zu", i);
		if (ready) {
			CHECK(expected && same_message(&message, expected) &&
			      i == count - 1, "unexpected message at bit %zu", i);
			++events;
		} else {
			CHECK(memcmp(&message, &saved, sizeof(message)) == 0,
			      "incomplete reception modified message");
		}
	}
	CHECK(events == (expected ? 1u : 0u), "message count %u", events);
	return true;
}

static bool check_wire(const struct v92_qc1_message *message)
{
	const size_t count = message->acknowledgment ? 70 : 60;
	uint8_t bits[70];

	wire_bits(message, bits);
	CHECK(v92_qc1_bit_count(message) == count, "sequence length");
	for (size_t i = 0; i < count; ++i) {
		uint8_t actual = 9;

		CHECK(v92_qc1_bit(message, i, &actual) && actual == bits[i],
		      "wire bit %zu", i);
	}
	for (size_t omitted = 0; omitted <= 8; ++omitted) {
		struct v92_qc1_receiver receiver;

		CHECK(v92_qc1_receiver_init(&receiver) &&
		      feed(&receiver, bits + omitted, count - omitted, message),
		      "carrier acquisition omitted %zu initial marks", omitted);
	}
	for (size_t error = 8; error < count; ++error) {
		struct v92_qc1_receiver receiver;

		bits[error] ^= 1;
		CHECK(v92_qc1_receiver_init(&receiver) &&
		      feed(&receiver, bits, count, NULL), "corruption bit %zu", error);
		bits[error] ^= 1;
	}
	for (size_t cut = 0; cut < count; ++cut) {
		struct v92_qc1_receiver receiver, resumed;

		CHECK(v92_qc1_receiver_init(&receiver) &&
		      feed(&receiver, bits, cut, NULL), "prefix before copy");
		resumed = receiver;
		memset(&receiver, 0, sizeof(receiver));
		CHECK(feed(&resumed, bits + cut, count - cut, message),
		      "receiver copy lost ownership at bit %zu", cut);
	}
	return true;
}

static bool test_bits(void)
{
	for (unsigned i = 0; i < 80; ++i) {
		struct v92_qc1_message message = message_at(i);

		CHECK(check_wire(&message), "wire combination %u", i);
	}
	return true;
}

static bool test_bad_wire(void)
{
	const struct v92_qc1_message message = {
		.sender = V90_DIGITAL_MODEM, .acknowledgment = true,
	};
	const size_t positions[] = { 20, 24, 25, 26, 29 };
	struct v92_qc1_receiver receiver;
	uint8_t bits[70], bad[71];
	uint32_t noise = 0xfabc83a5;

	wire_bits(&message, bits);
	/* Identical copies still must have valid start, stop and reserved bits. */
	for (size_t e = 0; e < ARRAY_LENGTH(positions); ++e) {
		memcpy(bad, bits, sizeof(bits));
		bad[positions[e]] ^= 1;
		bad[positions[e] + 30] ^= 1;
		CHECK(v92_qc1_receiver_init(&receiver) &&
		      feed(&receiver, bad, sizeof(bits), NULL) &&
		      feed(&receiver, bits, sizeof(bits), &message),
		      "invalid field %zu and reacquisition", positions[e]);
	}
	/* An inserted/deleted mark between identical records is not confirmation. */
	for (unsigned insert = 0; insert < 2; ++insert) {
		memcpy(bad, bits, 30);
		bad[30] = 1;
		memcpy(bad + 30 + insert, bits + 30 + !insert, 40 - !insert);
		CHECK(v92_qc1_receiver_init(&receiver) &&
		      feed(&receiver, bad, 69 + 2 * insert, NULL), "record spacing");
	}
	CHECK(v92_qc1_receiver_init(&receiver), "noise init");
	for (size_t i = 0; i < 200000; ++i) {
		uint8_t bit;

		noise = noise * 1664525u + 1013904223u;
		bit = noise >> 31;
		CHECK(feed(&receiver, &bit, 1, NULL), "noise at %zu", i);
	}
	CHECK(feed(&receiver, bits, sizeof(bits), &message), "post-noise decode");
	return true;
}

static bool test_validation(void)
{
	struct v92_qc1_message message = { .sender = V90_ANALOG_MODEM };
	struct v92_qc1_receiver receiver;
	uint8_t bit = 9;

	CHECK(!v92_qc1_bit(NULL, 0, &bit) &&
	      !v92_qc1_bit(&message, 60, &bit) &&
	      !v92_qc1_bit(&message, 0, NULL) && bit == 9,
	      "invalid transmit modified output");
	message.signal.qts_index = 16;
	CHECK(!v92_qc1_bit_count(&message), "invalid UQTS");
	message.sender = V90_DIGITAL_MODEM;
	message.signal.anspcm_level = 4;
	CHECK(!v92_qc1_bit_count(&message), "invalid ANSpcm level");
	message.sender = (enum v90_modem_role)-1;
	CHECK(!v92_qc1_bit_count(&message), "invalid role");
	CHECK(!v92_qc1_receiver_init(NULL) && v92_qc1_receiver_init(&receiver),
	      "validation init");
	for (unsigned mutation = 0; mutation < 11; ++mutation) {
		struct v92_qc1_receiver bad = receiver;
		struct v92_qc1_receiver saved;
		const struct v92_qc1_message old = message;
		uint8_t input = 0;
		bool complete = true;

		switch (mutation) {
		case 0: input = 2; break;
		case 1: bad.window_bits = 23; break;
		case 2: bad.window = 1; break;
		case 3: bad.elapsed_bits = 1; break;
		case 4: bad.candidate = 1; break;
		case 5: bad.stage = (enum v92_qc1_receive_stage)99; break;
		case 6: bad.stage = V92_QC1_ACK_MARKS; break;
		case 7: bad.stage = V92_QC1_REPEAT; break;
		default: break; /* NULL receiver, message, or completion output. */
		}
		saved = bad;
		CHECK(!v92_qc1_receive_bit(mutation == 8 ? NULL : &bad, input,
			      mutation == 9 ? NULL : &message,
			      mutation == 10 ? NULL : &complete) && complete &&
		      memcmp(&bad, &saved, sizeof(bad)) == 0 &&
		      memcmp(&message, &old, sizeof(message)) == 0,
		      "transactionality mutation %u", mutation);
	}
	return true;
}

static bool audio_case(const struct v92_qc1_message *message,
	enum g711_law law, bool impaired, bool following_menu)
{
	const bool ack = message->acknowledgment;
	const uint8_t information[] = { 0x41, 0x85, 0x40 };
	const size_t delay = impaired ? 257 : 13;
	struct v8_fsk source, destination;
	struct v92_qc1_receiver receiver;
	struct v8_control_generator cm;
	struct v8_control_receiver menus;
	int16_t delayed[257] = { 0 };
	size_t transmitted = 0, menu_octets = 0, delay_cursor = 0;
	unsigned events = 0;
	uint8_t transmit = 1;
	uint32_t noise = 0xa456eef9;
	bool streaming = true;

	CHECK(v8_fsk_init(&source, ack ? V8_ANSWERING_STATION :
		      V8_CALLING_STATION) == V8_FSK_OK &&
	      v8_fsk_init(&destination, ack ? V8_CALLING_STATION :
		      V8_ANSWERING_STATION) == V8_FSK_OK &&
	      v92_qc1_receiver_init(&receiver) &&
	      v8_control_generator_init(&cm, information, sizeof(information)) ==
		      V8_FRAMING_OK &&
	      v8_control_receiver_init(&menus) == V8_FRAMING_OK, "audio init");
	for (size_t clock = 0; clock < 16000; ++clock) {
		int16_t sample, arrival;
		uint8_t recovered;
		bool consumed, ready;

		if (streaming && v8_fsk_can_accept_bit(&source)) {
			if (transmitted < v92_qc1_bit_count(message)) {
				CHECK(v92_qc1_bit(message, transmitted, &transmit),
				      "audio transmit");
			} else if (following_menu) {
				bool end;

				CHECK(v8_control_next_bit(&cm, &transmit, &end) ==
				      V8_FRAMING_OK, "following menu");
			} else {
				streaming = false;
			}
			transmitted++;
		}
		if (streaming) {
			CHECK(v8_fsk_transmit_sample(&source, transmit, &sample,
				      &consumed) == V8_FSK_OK, "audio tx");
		} else {
			CHECK(v8_fsk_filter_transmit_sample(&source, 0, &sample) ==
			      V8_FSK_OK, "tail silence");
		}
		if (impaired) {
			noise = noise * 1664525u + 1013904223u;
			sample = sample / 2 + (int)(noise >> 27) - 16;
		}
		sample = law == G711_LAW_PCMU ?
			g711_decode_pcmu(g711_encode_pcmu(sample)) :
			g711_decode_pcma(g711_encode_pcma(sample));
		arrival = delayed[delay_cursor];
		delayed[delay_cursor] = sample;
		delay_cursor = (delay_cursor + 1) % delay;
		CHECK(v8_fsk_receive_sample(&destination, arrival, &recovered,
			      &ready) == V8_FSK_OK, "audio rx");
		if (ready) {
			struct v92_qc1_message actual;
			struct v8_control_event event;
			bool complete;

			CHECK(v92_qc1_receive_bit(&receiver, recovered, &actual,
				      &complete) &&
			      v8_control_receive_bit(&menus, recovered, &event) ==
				      V8_FRAMING_OK, "audio bits");
			if (complete) {
				CHECK(same_message(message, &actual), "audio fields");
				events++;
			}
			if (event.type == V8_CONTROL_INFORMATION_OCTET) {
				CHECK(events == 1 && event.octet ==
				      information[menu_octets % sizeof(information)],
				      "QC confused with CM/JM");
				menu_octets++;
			}
		}
	}
	CHECK(events == 1 && (following_menu ? menu_octets >= 6 : menu_octets == 0),
	      "audio events %u, menu octets %zu", events, menu_octets);
	return true;
}

static bool test_audio(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned i = 0; i < 80; ++i)
			for (unsigned impaired = 0; impaired < 2; ++impaired) {
				struct v92_qc1_message message = message_at(i);

				CHECK(audio_case(&message, law, impaired, true),
				      "audio law %u combination %u impaired %u",
				      law, i, impaired);
				if (message.acknowledgment)
					CHECK(audio_case(&message, law, impaired, false),
					      "ACK tail law %u combination %u impaired %u",
					      law, i, impaired);
			}
	return true;
}

int main(void)
{
	const struct { const char *name; bool (*run)(void); } tests[] = {
		{ "QC1/QCA1 wire fields and receiver ownership", test_bits },
		{ "QC1/QCA1 corruption and reacquisition", test_bad_wire },
		{ "QC1/QCA1 validation", test_validation },
		{ "QC1/QCA1 V.21 and G.711 audio", test_audio },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		printf("[ RUN      ] %s\n", tests[i].name);
		if (!tests[i].run())
			return EXIT_FAILURE;
		printf("[       OK ] %s\n", tests[i].name);
	}
	return EXIT_SUCCESS;
}
