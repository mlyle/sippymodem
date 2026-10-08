// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "g711.h"
#include "v90_info1.h"
#include "v90_phase3_sd.h"
#include "pcm_s_pattern_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint8_t expected_octet(
	enum g711_law law, uint8_t ucode, bool positive)
{
	uint8_t octet = law == G711_LAW_PCMA ?
				(uint8_t)(ucode ^ 0xd5u) :
				(uint8_t)~ucode;

	return positive ? octet : (uint8_t)(octet ^ 0x80u);
}

static uint8_t encode_sample(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ? g711_encode_pcma(sample) :
					   g711_encode_pcmu(sample);
}

static bool check_sequence(
	enum g711_law law, enum v90_phase3_sd_kind kind,
	uint8_t training_ucode)
{
	struct v90_phase3_sd sequence;
	struct v90_pcm_symbol sentinel = {
		.ucode = 0xa5,
		.positive = true,
		.octet = 0x5a,
		.sample = 1234,
	};
	struct v90_pcm_symbol unchanged = sentinel;
	size_t count = kind == V90_PHASE3_SD ?
			       V90_PHASE3_SD_SYMBOLS :
			       V90_PHASE3_SD_BAR_SYMBOLS;
	uint8_t w_ucode = (uint8_t)((training_ucode + 16u) & 0x7fu);

	CHECK(v90_phase3_sd_init(&sequence, law, training_ucode, kind) ==
		      V90_PHASE3_SD_OK &&
		      v90_phase3_sd_remaining(&sequence) == count,
	      "Sd initialization failed for law %u kind %u",
	      (unsigned int)law, (unsigned int)kind);
	for (size_t index = 0; index < count; ++index) {
		struct v90_pcm_symbol symbol = sentinel;
		unsigned int phase = index % 6u;
		uint8_t ucode = phase == 1 || phase == 4 ? 0 : w_ucode;
		bool positive = (phase < 3) != (kind == V90_PHASE3_SD_BAR);

		CHECK(v90_phase3_sd_next(&sequence, &symbol) ==
			      V90_PHASE3_SD_OK,
		      "Sd generation failed at symbol %zu", index);
		CHECK(symbol.ucode == ucode && symbol.positive == positive &&
			      symbol.octet == expected_octet(law, ucode, positive),
		      "Sd symbol %zu has the wrong code or polarity", index);
		CHECK(encode_sample(law, symbol.sample) == symbol.octet,
		      "Sd symbol %zu lost its exact network octet", index);
		CHECK(v90_phase3_sd_remaining(&sequence) == count - index - 1,
		      "Sd remaining count is wrong at symbol %zu", index);
	}
	CHECK(v90_phase3_sd_next(&sequence, &sentinel) ==
		      V90_PHASE3_SD_COMPLETE &&
		      sentinel.ucode == unchanged.ucode &&
		      sentinel.positive == unchanged.positive &&
		      sentinel.octet == unchanged.octet &&
		      sentinel.sample == unchanged.sample &&
		      sequence.emitted_symbols == count,
	      "completed Sd sequence changed its state or output");
	return true;
}

static bool test_exact_sequences(void)
{
	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		CHECK(check_sequence(law, V90_PHASE3_SD, 91),
		      "Sd failed for law %u", (unsigned int)law);
		CHECK(check_sequence(law, V90_PHASE3_SD_BAR, 91),
		      "Sd-bar failed for law %u", (unsigned int)law);
	}
	return true;
}

static bool test_training_code_boundaries(void)
{
	CHECK(check_sequence(
		      G711_LAW_PCMU, V90_PHASE3_SD,
		      V90_INFO1A_MIN_TRAINING_UCODE) &&
		      check_sequence(
			      G711_LAW_PCMA, V90_PHASE3_SD_BAR,
			      V90_INFO1A_MAX_TRAINING_UCODE),
	      "Sd rejected a legal training-code boundary");
	return true;
}

static bool next_octet(
	struct v90_phase3_sd *sequence, uint8_t *octet)
{
	struct v90_pcm_symbol symbol;

	if (v90_phase3_sd_next(sequence, &symbol) != V90_PHASE3_SD_OK)
		return false;
	*octet = symbol.octet;
	return true;
}

static bool check_receiver(
	enum g711_law law, uint8_t training_ucode, uint8_t starting_phase)
{
	struct pcm_s_pattern_rx receiver;
	struct v90_phase3_sd sd;
	struct v90_phase3_sd sd_bar;
	enum pcm_s_pattern_rx_result result;
	uint8_t octet;

	CHECK(pcm_s_pattern_rx_init(
		      &receiver, law, (training_ucode + 16u) & 0x7fu) ==
		      PCM_S_PATTERN_RX_OK &&
		      v90_phase3_sd_init(
			      &sd, law, training_ucode,
			      V90_PHASE3_SD) == V90_PHASE3_SD_OK &&
		      v90_phase3_sd_init(
			      &sd_bar, law, training_ucode,
			      V90_PHASE3_SD_BAR) == V90_PHASE3_SD_OK,
	      "receiver initialization failed for law %u code %u",
	      (unsigned int)law, training_ucode);
	for (uint8_t phase = 0; phase < starting_phase; ++phase)
		CHECK(next_octet(&sd, &octet),
		      "could not skip to Sd phase %u", starting_phase);

	for (size_t index = starting_phase;
	     index < V90_PHASE3_SD_SYMBOLS; ++index) {
		CHECK(next_octet(&sd, &octet),
		      "could not generate Sd symbol %zu", index);
		result = pcm_s_pattern_rx_receive(&receiver, octet);
		CHECK(result == (index == starting_phase +
				       PCM_S_PATTERN_ACQUISITION_SYMBOLS - 1u ?
				       PCM_S_PATTERN_RX_ACQUIRED :
				       PCM_S_PATTERN_RX_OK),
		      "receiver returned %s at Sd symbol %zu",
		      pcm_s_pattern_rx_result_name(result), index);
	}
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		CHECK(next_octet(&sd_bar, &octet),
		      "could not generate S-bar symbol %zu", index);
		result = pcm_s_pattern_rx_receive(&receiver, octet);
		if (index == PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS - 1u)
			CHECK(result ==
				      PCM_S_PATTERN_RX_REVERSAL_DETECTED,
			      "receiver did not qualify S-bar at symbol %zu",
			      index);
		else if (index == V90_PHASE3_SD_BAR_SYMBOLS - 1u)
			CHECK(result ==
				      PCM_S_PATTERN_RX_SEQUENCE_COMPLETE,
			      "receiver did not complete S-bar at symbol %zu",
			      index);
		else
			CHECK(result == PCM_S_PATTERN_RX_OK,
			      "receiver returned %s at S-bar symbol %zu",
			      pcm_s_pattern_rx_result_name(result), index);
	}
	CHECK(pcm_s_pattern_rx_is_complete(&receiver),
	      "receiver did not retain completion");
	{
		struct pcm_s_pattern_rx saved = receiver;

		CHECK(pcm_s_pattern_rx_receive(&receiver, 0xa5) ==
			      PCM_S_PATTERN_RX_SEQUENCE_COMPLETE &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "completed receiver consumed another character");
	}
	return true;
}

static bool test_receiver_exact_sequences(void)
{
	static const uint8_t training_ucodes[] = {
		V90_INFO1A_MIN_TRAINING_UCODE,
		112,
		V90_INFO1A_MAX_TRAINING_UCODE,
	};

	for (enum g711_law law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (size_t index = 0;
		     index < ARRAY_LENGTH(training_ucodes); ++index) {
			for (uint8_t phase = 0;
			     phase < V90_PHASE3_SD_PATTERN_SYMBOLS; ++phase) {
				CHECK(check_receiver(
					      law, training_ucodes[index], phase),
				      "receiver failed for law %u code %u phase %u",
				      (unsigned int)law,
				      training_ucodes[index], phase);
			}
		}
	}
	return true;
}

static bool test_receiver_acquisition(void)
{
	struct pcm_s_pattern_rx receiver;
	struct v90_phase3_sd sd;
	uint32_t noise = UINT32_C(0x36a9f17b);
	uint8_t octet;

	CHECK(pcm_s_pattern_rx_init(
		      &receiver, G711_LAW_PCMU, 107) ==
		      PCM_S_PATTERN_RX_OK,
	      "could not initialize receiver for noise test");
	for (size_t index = 0; index < 256; ++index) {
		noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
		CHECK(pcm_s_pattern_rx_receive(
			      &receiver, (uint8_t)(noise >> 24)) ==
			      PCM_S_PATTERN_RX_OK,
		      "random input falsely acquired Sd at symbol %zu", index);
	}
	CHECK(receiver.stage == PCM_S_PATTERN_RX_SEARCH &&
		      v90_phase3_sd_init(
			      &sd, G711_LAW_PCMU, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_OK,
	      "noise search damaged receiver state");
	for (size_t index = 0;
	     index < PCM_S_PATTERN_ACQUISITION_SYMBOLS; ++index) {
		enum pcm_s_pattern_rx_result result;

		CHECK(next_octet(&sd, &octet),
		      "could not generate acquisition symbol %zu", index);
		if (index == 3 || index == 14)
			octet ^= UINT8_C(0x80);
		result = pcm_s_pattern_rx_receive(&receiver, octet);
		CHECK(result == (index ==
				       PCM_S_PATTERN_ACQUISITION_SYMBOLS - 1u ?
				       PCM_S_PATTERN_RX_ACQUIRED :
				       PCM_S_PATTERN_RX_OK),
		      "two-error acquisition returned %s at symbol %zu",
		      pcm_s_pattern_rx_result_name(result), index);
	}
	return true;
}

static bool test_receiver_sbar_error_budget(void)
{
	struct pcm_s_pattern_rx receiver;
	struct v90_phase3_sd sd;
	struct v90_phase3_sd sd_bar;
	uint8_t octet;

	CHECK(pcm_s_pattern_rx_init(
		      &receiver, G711_LAW_PCMA, 107) ==
		      PCM_S_PATTERN_RX_OK &&
		      v90_phase3_sd_init(
			      &sd, G711_LAW_PCMA, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_OK &&
		      v90_phase3_sd_init(
			      &sd_bar, G711_LAW_PCMA, 91,
			      V90_PHASE3_SD_BAR) == V90_PHASE3_SD_OK,
	      "could not initialize S-bar error test");
	for (size_t index = 0; index < V90_PHASE3_SD_SYMBOLS; ++index) {
		CHECK(next_octet(&sd, &octet), "could not generate Sd");
		(void)pcm_s_pattern_rx_receive(&receiver, octet);
	}
	for (size_t index = 0; index < V90_PHASE3_SD_BAR_SYMBOLS; ++index) {
		enum pcm_s_pattern_rx_result result;

		CHECK(next_octet(&sd_bar, &octet), "could not generate S-bar");
		if (index == 20 || index == 30)
			octet ^= UINT8_C(0x80);
		if (index == 40) {
			struct pcm_s_pattern_rx saved = receiver;
			uint8_t corrupt = expected_octet(G711_LAW_PCMA, 107, true);

			CHECK(pcm_s_pattern_rx_receive(
				      &receiver, corrupt) ==
				      PCM_S_PATTERN_RX_INVALID_SEQUENCE &&
				      memcmp(&receiver, &saved,
					     sizeof(receiver)) == 0,
			      "third S-bar error was not transactional");
		}
		result = pcm_s_pattern_rx_receive(&receiver, octet);
		CHECK(result == (index == V90_PHASE3_SD_BAR_SYMBOLS - 1u ?
				       PCM_S_PATTERN_RX_SEQUENCE_COMPLETE :
				       index ==
					       PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS - 1u ?
				       PCM_S_PATTERN_RX_REVERSAL_DETECTED :
				       PCM_S_PATTERN_RX_OK),
		      "S-bar error test returned %s at symbol %zu",
		      pcm_s_pattern_rx_result_name(result), index);
	}
	CHECK(receiver.bar_errors == PCM_S_PATTERN_BAR_MAX_ERRORS &&
		      pcm_s_pattern_rx_is_complete(&receiver),
	      "S-bar receiver did not retain its error budget");
	return true;
}

static bool check_sampled_receiver(
	enum g711_law law, uint8_t training_ucode, unsigned int starting_phase,
	unsigned int gain_percent, bool inverted)
{
	struct pcm_s_pattern_rx receiver;
	int32_t previous = 0, second = 0;
	unsigned int changed = 0, detected = 0, bar_detected = 0;
	size_t received = 0;

	CHECK(pcm_s_pattern_rx_init(&receiver, law,
		      (training_ucode + 16u) & 0x7fu) ==
		      PCM_S_PATTERN_RX_OK, "sampled receiver init failed");
	for (enum v90_phase3_sd_kind kind = V90_PHASE3_SD;
	     kind <= V90_PHASE3_SD_BAR; ++kind) {
		struct v90_phase3_sd tx;

		CHECK(v90_phase3_sd_init(&tx, law, training_ucode, kind) ==
			      V90_PHASE3_SD_OK, "sampled source init failed");
		for (size_t i = 0; v90_phase3_sd_remaining(&tx); ++i) {
			struct v90_pcm_symbol symbol;
			enum pcm_s_pattern_rx_result result;
			int32_t filtered;
			uint8_t octet;

			CHECK(v90_phase3_sd_next(&tx, &symbol) == V90_PHASE3_SD_OK,
			      "sampled source failed");
			/* Gain, two postcursors, and a second G.711 boundary. The
			 * filter is continuous through both acquisition and reversal. */
			filtered = (90 * symbol.sample + 8 * previous + 2 * second) *
				   (int32_t)gain_percent / 10000;
			second = previous;
			previous = symbol.sample;
			if (inverted)
				filtered = -filtered;
			octet = encode_sample(law, (int16_t)filtered);
			changed += octet != symbol.octet;
			if (kind == V90_PHASE3_SD && i < starting_phase)
				continue;
			result = pcm_s_pattern_rx_receive(&receiver, octet);
			received++;
			if (result == PCM_S_PATTERN_RX_ACQUIRED) {
				CHECK(kind == V90_PHASE3_SD && received ==
					      PCM_S_PATTERN_ACQUISITION_SYMBOLS,
				      "sampled Sd acquired at the wrong boundary");
				detected++;
			} else if (result == PCM_S_PATTERN_RX_REVERSAL_DETECTED) {
				CHECK(kind == V90_PHASE3_SD_BAR && i + 1 ==
					      PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS,
				      "sampled S-bar acquired at the wrong boundary");
				bar_detected++;
			} else {
				CHECK(result == (kind == V90_PHASE3_SD_BAR &&
						 i + 1 == V90_PHASE3_SD_BAR_SYMBOLS ?
					PCM_S_PATTERN_RX_SEQUENCE_COMPLETE :
					PCM_S_PATTERN_RX_OK),
				      "sampled Sd returned %s at kind %u index %zu",
				      pcm_s_pattern_rx_result_name(result),
				      (unsigned int)kind, i);
			}
		}
	}
	CHECK(changed > V90_PHASE3_SD_SYMBOLS / 2 && detected == 1 &&
		      bar_detected == 1 &&
		      pcm_s_pattern_rx_is_complete(&receiver),
	      "sampled Sd was not acquired through a lossy channel");
	return true;
}

static bool test_sampled_receiver(void)
{
	static const uint8_t codes[] = { 67, 79, 91 };
	static const unsigned int gains[] = { 50, 100, 125 };

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (size_t c = 0; c < ARRAY_LENGTH(codes); ++c)
			for (size_t g = 0; g < ARRAY_LENGTH(gains); ++g)
				for (unsigned int phase = 0; phase < 6; ++phase)
					for (unsigned int invert = 0; invert < 2; ++invert)
						CHECK(check_sampled_receiver(law, codes[c],
							      phase, gains[g], invert != 0),
						      "law=%u code=%u phase=%u gain=%u invert=%u",
						      (unsigned int)law, codes[c], phase,
						      gains[g], invert);
	return true;
}

static bool test_reject_unrelated_samples(void)
{
	static const int16_t tone[] = { 1000, 500, -500, -1000, -500, 500 };

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (unsigned int kind = 0; kind < 5; ++kind) {
			struct pcm_s_pattern_rx receiver;
			uint32_t noise = UINT32_C(0x8a1937cb);

			CHECK(pcm_s_pattern_rx_init(&receiver, law, 95) ==
				      PCM_S_PATTERN_RX_OK, "noise init failed");
			for (unsigned int i = 0; i < 16000; ++i) {
				int16_t sample = kind == 0 ? 0 : kind == 1 ? 2000 :
					kind == 2 ? tone[i % ARRAY_LENGTH(tone)] :
					(i % 2 == 0 ? 2000 : -2000);
				uint8_t octet = encode_sample(law, sample);

				noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
				if (kind == 4)
					octet = (uint8_t)(noise >> 24);
				CHECK(pcm_s_pattern_rx_receive(&receiver, octet) ==
					      PCM_S_PATTERN_RX_OK &&
					      receiver.stage == PCM_S_PATTERN_RX_SEARCH,
				      "false Sd: law=%u kind=%u sample=%u",
				      (unsigned int)law, kind, i);
			}
		}
	}
	return true;
}

static bool test_short_reversal(void)
{
	struct pcm_s_pattern_rx receiver;
	struct v90_phase3_sd sequence;
	uint8_t octet;

	CHECK(pcm_s_pattern_rx_init(&receiver, G711_LAW_PCMU, 95) ==
		      PCM_S_PATTERN_RX_OK &&
	      v90_phase3_sd_init(&sequence, G711_LAW_PCMU, 79, V90_PHASE3_SD) ==
		      V90_PHASE3_SD_OK, "short-reversal init failed");
	for (unsigned int i = 0; i < PCM_S_PATTERN_ACQUISITION_SYMBOLS; ++i) {
		CHECK(next_octet(&sequence, &octet), "short-reversal source failed");
		(void)pcm_s_pattern_rx_receive(&receiver, octet);
	}
	CHECK(receiver.stage == PCM_S_PATTERN_RX_LOCKED && receiver.level > 0,
	      "short-reversal test did not acquire sampled Sd");
	const int32_t level = receiver.level;

	for (unsigned int i = 0; i < 36; ++i) {
		CHECK(next_octet(&sequence, &octet), "short-reversal source failed");
		if (i < 6)
			octet ^= UINT8_C(0x80);
		else if (i < 18)
			octet = encode_sample(G711_LAW_PCMU, 0);
		CHECK(pcm_s_pattern_rx_receive(&receiver, octet) ==
			      PCM_S_PATTERN_RX_OK &&
		      receiver.stage == PCM_S_PATTERN_RX_LOCKED &&
		      receiver.level == level,
		      "short reversal or silence qualified S-bar or retuned its level");
	}
	CHECK(receiver.bar_candidate_symbols == 0,
	      "short-reversal qualification did not reset");
	return true;
}

static bool test_receiver_validation(void)
{
	struct pcm_s_pattern_rx receiver;
	struct pcm_s_pattern_rx saved;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(pcm_s_pattern_rx_init(
		      NULL, G711_LAW_PCMU, 91) ==
		      PCM_S_PATTERN_RX_INVALID_ARGUMENT &&
		      pcm_s_pattern_rx_init(
			      &receiver, (enum g711_law)2, 91) ==
			      PCM_S_PATTERN_RX_INVALID_ARGUMENT &&
		      pcm_s_pattern_rx_init(
			      &receiver, G711_LAW_PCMU, 128) ==
			      PCM_S_PATTERN_RX_INVALID_ARGUMENT &&
		      pcm_s_pattern_rx_init(
			      &receiver, G711_LAW_PCMU, 255) ==
			      PCM_S_PATTERN_RX_INVALID_ARGUMENT &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid receiver initialization changed state");
	CHECK(pcm_s_pattern_rx_receive(NULL, 0) ==
		      PCM_S_PATTERN_RX_INVALID_ARGUMENT &&
		      pcm_s_pattern_rx_receive(&receiver, 0) ==
			      PCM_S_PATTERN_RX_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      !pcm_s_pattern_rx_is_valid(&receiver) &&
		      !pcm_s_pattern_rx_is_complete(&receiver) &&
		      !pcm_s_pattern_rx_is_valid(NULL),
	      "invalid receiver state was not rejected transactionally");
	CHECK(strcmp(pcm_s_pattern_rx_result_name(
			     PCM_S_PATTERN_RX_ACQUIRED),
		     "pattern acquired") == 0 &&
		      strcmp(pcm_s_pattern_rx_result_name(
			     (enum pcm_s_pattern_rx_result)7),
			     "unknown") == 0,
	      "receiver result names are wrong");
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		CHECK(pcm_s_pattern_rx_init(&receiver, law, 0) == PCM_S_PATTERN_RX_OK &&
		      receiver.signal_ucode == 0 && pcm_s_pattern_rx_is_valid(&receiver) &&
		      pcm_s_pattern_rx_init(&receiver, law, 127) == PCM_S_PATTERN_RX_OK &&
		      receiver.signal_ucode == 127 && pcm_s_pattern_rx_is_valid(&receiver),
		      "shared receiver rejected a signal-code boundary");
	}
	receiver.signal_ucode = 128;
	saved = receiver;
	CHECK(pcm_s_pattern_rx_receive(&receiver, 0xff) ==
		      PCM_S_PATTERN_RX_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid retained signal code was not transactional");
	CHECK(pcm_s_pattern_rx_init(&receiver, G711_LAW_PCMU, 95) ==
		      PCM_S_PATTERN_RX_OK, "level validation init failed");
	const int32_t invalid_levels[] = { -1, 1, INT32_MAX };

	for (size_t i = 0; i < ARRAY_LENGTH(invalid_levels); ++i) {
		receiver.level = invalid_levels[i];
		saved = receiver;
		CHECK(pcm_s_pattern_rx_receive(&receiver, 0xff) ==
			      PCM_S_PATTERN_RX_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
		      "invalid sampled level was not transactional");
	}
	return true;
}

static bool test_validation(void)
{
	struct v90_phase3_sd sequence;
	struct v90_phase3_sd saved;
	struct v90_pcm_symbol symbol = {
		.ucode = 0xa5,
		.octet = 0x5a,
		.sample = 1234,
	};
	struct v90_pcm_symbol saved_symbol = symbol;

	memset(&sequence, 0xa5, sizeof(sequence));
	saved = sequence;
	CHECK(v90_phase3_sd_init(
		      &sequence, G711_LAW_PCMU,
		      V90_INFO1A_MIN_TRAINING_UCODE - 1u,
		      V90_PHASE3_SD) == V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      &sequence, G711_LAW_PCMU,
			      V90_INFO1A_MAX_TRAINING_UCODE + 1u,
			      V90_PHASE3_SD) ==
			      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      &sequence, (enum g711_law)2, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      &sequence, G711_LAW_PCMU, 91,
			      (enum v90_phase3_sd_kind)2) ==
			      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_init(
			      NULL, G711_LAW_PCMU, 91,
			      V90_PHASE3_SD) == V90_PHASE3_SD_INVALID_ARGUMENT &&
		      memcmp(&sequence, &saved, sizeof(sequence)) == 0,
	      "invalid Sd initialization changed output state");
	CHECK(v90_phase3_sd_next(NULL, &symbol) ==
		      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_next(&sequence, NULL) ==
			      V90_PHASE3_SD_INVALID_ARGUMENT &&
		      v90_phase3_sd_next(&sequence, &symbol) ==
			      V90_PHASE3_SD_INVALID_STATE &&
		      memcmp(&sequence, &saved, sizeof(sequence)) == 0 &&
		      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
		      v90_phase3_sd_remaining(&sequence) == 0 &&
		      v90_phase3_sd_remaining(NULL) == 0,
	      "invalid Sd generation was not transactional");
	CHECK(strcmp(v90_phase3_sd_result_name(V90_PHASE3_SD_COMPLETE),
		     "complete") == 0 &&
		      strcmp(v90_phase3_sd_result_name(
			     (enum v90_phase3_sd_result)4), "unknown") == 0,
	      "Sd result names are wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact PCMU and PCMA sequences", test_exact_sequences },
		{ "training-code boundaries", test_training_code_boundaries },
		{ "receiver exact sequences", test_receiver_exact_sequences },
		{ "receiver acquisition", test_receiver_acquisition },
		{ "receiver S-bar error budget",
		  test_receiver_sbar_error_budget },
		{ "sampled receiver gain and two-postcursor channel",
		  test_sampled_receiver },
		{ "reject unrelated sampled signals", test_reject_unrelated_samples },
		{ "reject short reversal and silence", test_short_reversal },
		{ "receiver validation", test_receiver_validation },
		{ "validation and transactionality", test_validation },
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
