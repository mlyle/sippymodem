// SPDX-License-Identifier: BSD-3-Clause

#include <v8_calling.h>
#include <v8_answering.h>
#include <g711.h>

#include "test.h"

#include <string.h>

enum remote_answer_state {
	REMOTE_ANSWER_SIGNAL,
	REMOTE_JOINT_MENU,
	REMOTE_SILENCE,
};

struct remote_answerer {
	struct v8_ansam_generator answer_signal;
	struct v8_fsk fsk;
	struct v8_menu_detector call_menu_detector;
	struct v8_control_generator joint_menu_bits;
	struct v8_cj_detector cj_detector;
	struct v8_menu local_menu;
	struct v8_menu joint_menu;
	enum remote_answer_state state;
	uint8_t transmit_bit;
	bool call_menu_confirmed;
	bool cj_detected;
};

static bool remote_answerer_init(
	struct remote_answerer *answerer, uint16_t modulations)
{
	struct remote_answerer initialized = {
		.transmit_bit = 1,
	};

	if (v8_ansam_generator_init(
		    &initialized.answer_signal, 16000, true) !=
		    V8_ANSAM_OK ||
	    v8_fsk_init(
		    &initialized.fsk, V8_ANSWERING_STATION) !=
		    V8_FSK_OK ||
	    v8_menu_detector_init(
		    &initialized.call_menu_detector) != V8_MENU_OK ||
	    v8_cj_detector_init(&initialized.cj_detector) !=
		    V8_FRAMING_OK ||
	    v8_call_menu_init(
		    &initialized.local_menu, modulations, true) !=
		    V8_MENU_OK)
		return false;
	*answerer = initialized;
	return true;
}

static bool remote_answerer_transmit(
	struct remote_answerer *answerer, int16_t *sample)
{
	bool consumed;

	if (answerer->state == REMOTE_ANSWER_SIGNAL) {
		int16_t unfiltered;

		return v8_ansam_generate(
			       &answerer->answer_signal, &unfiltered, 1) ==
			       V8_ANSAM_OK &&
		       v8_fsk_filter_transmit_sample(
			       &answerer->fsk, unfiltered, sample) ==
			       V8_FSK_OK;
	}
	if (answerer->state == REMOTE_JOINT_MENU) {
		if (v8_fsk_can_accept_bit(&answerer->fsk)) {
			bool sequence_complete;

			if (v8_control_next_bit(
				    &answerer->joint_menu_bits,
				    &answerer->transmit_bit,
				    &sequence_complete) !=
			    V8_FRAMING_OK)
				return false;
		}
		return v8_fsk_transmit_sample(
			       &answerer->fsk, answerer->transmit_bit, sample,
			       &consumed) == V8_FSK_OK;
	}
	*sample = 0;
	return true;
}

static bool remote_answerer_receive(
	struct remote_answerer *answerer, int16_t sample)
{
	uint8_t bit;
	bool ready;

	if (answerer->state == REMOTE_SILENCE)
		return true;
	if (v8_fsk_receive_sample(
		    &answerer->fsk, sample, &bit, &ready) !=
	    V8_FSK_OK)
		return false;
	if (!ready)
		return true;
	if (answerer->state == REMOTE_ANSWER_SIGNAL) {
		struct v8_menu call_menu;
		uint8_t information[V8_MAX_INFORMATION_OCTETS];
		size_t information_count;
		bool confirmed;

		if (v8_menu_detect_bit(
			    &answerer->call_menu_detector, bit, &call_menu,
			    &confirmed) != V8_MENU_OK)
			return false;
		if (!confirmed)
			return true;
		if (v8_joint_menu_select(
			    &call_menu, &answerer->local_menu,
			    &answerer->joint_menu) != V8_MENU_OK ||
		    v8_menu_encode(
			    &answerer->joint_menu, information,
			    sizeof(information), &information_count) !=
			    V8_MENU_OK ||
		    v8_control_generator_init(
			    &answerer->joint_menu_bits, information,
			    information_count) != V8_FRAMING_OK)
			return false;
		answerer->call_menu_confirmed = true;
		answerer->state = REMOTE_JOINT_MENU;
		return true;
	}
	if (v8_cj_detect_bit(
		    &answerer->cj_detector, bit, &answerer->cj_detected) !=
	    V8_FRAMING_OK)
		return false;
	if (answerer->cj_detected)
		answerer->state = REMOTE_SILENCE;
	return true;
}

static bool test_call_negotiation(void)
{
	struct v8_calling calling;
	struct v8_menu local_menu;
	struct v8_menu negotiated_menu;
	struct remote_answerer answerer;
	enum v8_modulation modulation;
	size_t exchange_started = 0;
	size_t completed = 0;

	CHECK(v8_call_menu_init(
		      &local_menu, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &local_menu) ==
		      V8_CALLING_OK &&
	      remote_answerer_init(
		      &answerer, V8_MODULATION_V21),
	      "V.8 negotiation endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t receive;
		int16_t transmit;
		enum v8_calling_state before = calling.state;

		CHECK(remote_answerer_transmit(&answerer, &receive) &&
		      v8_calling_process(
			      &calling, &receive, &transmit, 1) ==
		      V8_CALLING_OK,
		      "V.8 calling station failed at sample %zu", sample_index);
		CHECK(remote_answerer_receive(&answerer, transmit),
		      "V.8 answering station failed at sample %zu", sample_index);
		if (before == V8_CALLING_WAIT_ANSWER_SIGNAL ||
		    before == V8_CALLING_COMPLETE)
			CHECK(transmit == 0,
			      "V.8 silent state emitted audio at sample %zu",
			      sample_index);
		if (before != calling.state) {
			if (calling.state ==
				 V8_CALLING_EXCHANGE_MENUS)
				exchange_started = sample_index + 1u;
			else if (calling.state ==
				 V8_CALLING_COMPLETE)
				completed = sample_index + 1u;
		}
		if (v8_calling_is_complete(&calling) &&
		    answerer.cj_detected)
			break;
	}
	CHECK(exchange_started >
		      V8_ANSAM_POST_DETECT_DWELL_BLOCKS *
			      V8_ANSAM_BLOCK_SAMPLES &&
	      completed != 0 &&
	      v8_calling_is_complete(&calling) &&
	      answerer.call_menu_confirmed && answerer.cj_detected,
	      "V.8 calling station did not complete the timed exchange");
	CHECK(v8_calling_selection(
		      &calling, &negotiated_menu, &modulation) ==
		      V8_CALLING_OK &&
	      modulation == V8_MODULATION_V21 &&
	      negotiated_menu.modulations == V8_MODULATION_V21 &&
	      negotiated_menu.lapm,
	      "V.8 calling station did not expose the selected V.21 menu");
	return true;
}

static bool test_no_common_modulation(void)
{
	struct v8_calling calling;
	struct v8_menu local_menu;
	struct remote_answerer answerer;
	enum v8_calling_result result = V8_CALLING_OK;
	int16_t receive = 0;
	int16_t transmit = 0;

	CHECK(v8_call_menu_init(
		      &local_menu, V8_MODULATION_V21, true) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &local_menu) ==
		      V8_CALLING_OK &&
	      remote_answerer_init(
		      &answerer, V8_MODULATION_V22),
	      "V.8 incompatible endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		CHECK(remote_answerer_transmit(&answerer, &receive),
		      "V.8 incompatible answerer failed to transmit");
		result = v8_calling_process(
			&calling, &receive, &transmit, 1);
		if (result != V8_CALLING_OK)
			break;
		CHECK(remote_answerer_receive(&answerer, transmit),
		      "V.8 incompatible answerer failed to receive");
	}
	CHECK(result == V8_CALLING_NO_COMMON_MODULATION &&
	      calling.state == V8_CALLING_FAILED &&
	      answerer.call_menu_confirmed && !answerer.cj_detected &&
	      v8_calling_process(
		      &calling, &receive, &transmit, 1) ==
		      V8_CALLING_NO_COMMON_MODULATION,
	      "V.8 incompatible modulation result did not latch");
	return true;
}

static bool test_answer_signal_timeout(void)
{
	struct v8_calling calling;
	struct v8_menu menu;
	int16_t silence[V8_CALLING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[V8_CALLING_MAX_FRAGMENT_SAMPLES];
	enum v8_calling_result result = V8_CALLING_OK;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &menu) ==
		      V8_CALLING_OK,
	      "V.8 timeout fixture did not initialize");
	for (size_t offset = 0;
	     offset < V8_CALLING_ANSWER_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence)) {
		memset(transmit, 1, sizeof(transmit));
		result = v8_calling_process(
			&calling, silence, transmit, ARRAY_LENGTH(silence));
		CHECK((result == V8_CALLING_OK ||
		       result == V8_CALLING_ANSWER_TIMEOUT) &&
			      memcmp(transmit, silence, sizeof(transmit)) == 0,
		      "V.8 answer timeout emitted audio or failed early");
	}
	CHECK(result == V8_CALLING_ANSWER_TIMEOUT &&
		      calling.state == V8_CALLING_FAILED &&
		      !v8_calling_is_complete(&calling) &&
		      v8_calling_process(
			      &calling, silence, transmit, 1) ==
			      V8_CALLING_ANSWER_TIMEOUT,
	      "V.8 calling timeout did not latch");
	return true;
}

static bool test_menu_timeout(void)
{
	struct v8_ansam_generator answer_signal;
	struct v8_calling calling;
	struct v8_menu menu;
	int16_t receive[V8_CALLING_MAX_FRAGMENT_SAMPLES];
	int16_t transmit[V8_CALLING_MAX_FRAGMENT_SAMPLES];
	enum v8_calling_result result = V8_CALLING_OK;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_ansam_generator_init(
		      &answer_signal, 10000, true) == V8_ANSAM_OK &&
	      v8_calling_init(&calling, &menu) ==
		      V8_CALLING_OK,
	      "V.8 menu-timeout fixture did not initialize");
	while (calling.state != V8_CALLING_EXCHANGE_MENUS) {
		CHECK(v8_ansam_generate(
			      &answer_signal, receive, ARRAY_LENGTH(receive)) ==
			      V8_ANSAM_OK &&
		      v8_calling_process(
			      &calling, receive, transmit,
			      ARRAY_LENGTH(receive)) == V8_CALLING_OK,
		      "V.8 menu-timeout fixture did not reach menu exchange");
	}
	memset(receive, 0, sizeof(receive));
	for (size_t offset = 0;
	     offset < V8_CALLING_MENU_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(receive))
		result = v8_calling_process(
			&calling, receive, transmit, ARRAY_LENGTH(receive));
	CHECK(result == V8_CALLING_MENU_TIMEOUT &&
	      calling.state == V8_CALLING_FAILED &&
	      v8_calling_process(
		      &calling, receive, transmit, 1) ==
		      V8_CALLING_MENU_TIMEOUT,
	      "V.8 menu timeout did not latch");
	return true;
}

static bool test_validation(void)
{
	struct v8_calling calling;
	struct v8_calling saved;
	struct v8_menu menu;
	struct v8_menu selected_menu;
	enum v8_modulation selected =
		V8_MODULATION_V34_DUPLEX;
	int16_t receive = 0;
	int16_t transmit = 123;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_calling_init(&calling, &menu) ==
		      V8_CALLING_OK,
	      "V.8 calling validation fixture did not initialize");
	saved = calling;
	CHECK(v8_calling_init(NULL, &menu) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      v8_calling_process(
		      &calling, &receive, &transmit,
		      V8_CALLING_MAX_FRAGMENT_SAMPLES + 1u) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      transmit == 123 && memcmp(&calling, &saved, sizeof(calling)) == 0 &&
	      v8_calling_process(
		      &calling, NULL, &transmit, 1) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      v8_calling_process(
		      &calling, NULL, NULL, 0) == V8_CALLING_OK &&
	      v8_calling_selection(
		      &calling, &selected_menu, &selected) ==
		      V8_CALLING_NOT_READY &&
	      selected == V8_MODULATION_V34_DUPLEX &&
	      v8_calling_selection(
		      &calling, NULL, &selected) ==
		      V8_CALLING_INVALID_ARGUMENT &&
	      strcmp(v8_calling_result_name(
			     V8_CALLING_ANSWER_TIMEOUT),
		     "answer signal timeout") == 0,
	      "V.8 calling API accepted invalid arguments");
	return true;
}

static int16_t qc_channel(int16_t sample, enum g711_law law,
	bool impaired, uint32_t *noise)
{
	if (impaired) {
		*noise = *noise * 1664525u + 1013904223u;
		sample = sample / 2 + (int)(*noise >> 27) - 16;
	}
	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool qc_caller_init(struct v8_calling *calling, bool digital, bool lapm)
{
	struct v8_menu menu;
	struct v92_qc1_message request = {
		.sender = digital ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM,
		.lapm = lapm,
	};

	request.signal.qts_index = digital ? 2 : 10;
	CHECK(v8_pcm_call_menu_init(&menu, digital ? V8_PCM_DIGITAL :
		      V8_PCM_ANALOG, 0, lapm) == V8_MENU_OK &&
	      v8_calling_init(calling, &menu) == V8_CALLING_OK &&
	      v8_calling_enable_quick_connect(calling, &request) == V8_CALLING_OK,
	      "QC caller fixture");
	request.signal.qts_index ^= 1;
	CHECK(calling->qc_request.signal.qts_index != request.signal.qts_index,
	      "QC request borrowed caller storage");
	return true;
}

/* Inspect the existing owner, not a second implementation of its protocol. */
static bool qc_caller_sample(struct v8_calling *calling, int16_t receive,
	int16_t *transmit, size_t clock, size_t *ack_at)
{
	struct v8_fsk before = calling->fsk;
	enum v8_calling_state state = calling->state;
	enum v8_calling_result result =
		v8_calling_process(calling, &receive, transmit, 1);

	CHECK(result == V8_CALLING_OK || result == V8_CALLING_MENU_TIMEOUT,
	      "QC processing at %zu: %s", clock, v8_calling_result_name(result));
	if (state == V8_CALLING_EXCHANGE_MENUS &&
	    calling->state == V8_CALLING_QC_TX_TAIL) {
		int16_t expected;

		CHECK(v8_fsk_filter_transmit_sample(&before, 0, &expected) == V8_FSK_OK &&
		      *transmit == expected && memcmp(&before.transmitter,
			      &calling->fsk.transmitter, sizeof(before.transmitter)) == 0,
		      "QCA1 did not stop CM on the acknowledgment sample");
		CHECK(*ack_at == 0, "repeated acknowledgment");
		*ack_at = clock;
	}
	if (calling->state == V8_CALLING_QC_COMPLETE) {
		struct v8_menu selected = calling->local_menu, saved = selected;
		enum v8_modulation modulation = V8_MODULATION_V34_DUPLEX;

		CHECK(v8_calling_is_complete(calling) &&
		      v8_calling_selection(calling, &selected, &modulation) ==
			      V8_CALLING_NOT_READY &&
		      memcmp(&selected, &saved, sizeof(saved)) == 0 &&
		      modulation == V8_MODULATION_V34_DUPLEX,
		      "QC fabricated full menu negotiation");
		if (state != V8_CALLING_QC_COMPLETE)
			CHECK(*ack_at != 0 && clock - *ack_at ==
				      V8_FSK_TRANSMIT_FILTER_TAPS - 1,
			      "QC filter tail length");
	}
	if (state == V8_CALLING_QC_COMPLETE)
		CHECK(*transmit == 0, "QC sent more CM after handoff");
	return true;
}

static bool qc_pair(bool digital, unsigned lapm, enum g711_law law,
	bool impaired, bool admitted)
{
	struct v8_calling calling;
	struct v8_answering answering;
	struct v8_menu menu;
	struct v92_qc1_message response = {
		.sender = digital ? V90_ANALOG_MODEM : V90_DIGITAL_MODEM,
		.acknowledgment = true, .lapm = (lapm & 2u) != 0,
	};
	int16_t to_caller[157] = { 0 }, to_answerer[157] = { 0 };
	const size_t delay = impaired ? ARRAY_LENGTH(to_caller) : 1;
	size_t cursor = 0, ack_at = 0, done_at = 0;
	uint32_t noise = 0x918ae5;

	response.signal.qts_index = digital ? 8 : 1;
	CHECK(qc_caller_init(&calling, digital, (lapm & 1u) != 0) &&
	      v8_pcm_call_menu_init(&menu, digital ? V8_PCM_ANALOG :
		      V8_PCM_DIGITAL, 0, response.lapm) == V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) == V8_ANSWERING_OK &&
	      (!admitted || v8_answering_enable_quick_connect(&answering,
		      &response) == V8_ANSWERING_OK), "QC pair fixture");
	for (size_t clock = 0; clock < 12u * V8_ANSAM_SAMPLE_RATE; ++clock) {
		int16_t caller_tx, answerer_tx;

		CHECK(qc_caller_sample(&calling, to_caller[cursor], &caller_tx,
			      clock, &ack_at) &&
		      v8_answering_process(&answering, &to_answerer[cursor],
			      &answerer_tx, 1) == V8_ANSWERING_OK, "QC pair processing");
		to_caller[cursor] = qc_channel(answerer_tx, law, impaired, &noise);
		to_answerer[cursor] = qc_channel(caller_tx, law, impaired, &noise);
		cursor = (cursor + 1) % delay;
		if (v8_calling_is_complete(&calling)) {
			if (done_at == 0)
				done_at = clock;
			if (clock > done_at + delay + 800)
				break;
		}
	}
	CHECK(calling.qc_bit_index == 60, "caller did not send exactly one QC1");
	if (admitted) {
		CHECK(calling.state == V8_CALLING_QC_COMPLETE &&
		      answering.state == V8_ANSWERING_QC_COMPLETE &&
		      calling.qc_response.sender == response.sender &&
		      calling.qc_response.lapm == response.lapm &&
		      calling.qc_response.signal.qts_index == response.signal.qts_index &&
		      answering.qc_request.sender == calling.qc_request.sender &&
		      answering.qc_request.lapm == calling.qc_request.lapm &&
		      answering.qc_request.signal.qts_index ==
			      calling.qc_request.signal.qts_index,
		      "QC request/response fields or completion");
	} else {
		CHECK(calling.state == V8_CALLING_COMPLETE &&
		      answering.state == V8_ANSWERING_COMPLETE && ack_at == 0 &&
		      calling.negotiated_menu.lapm == (lapm == 3),
		      "unadmitted QC did not finish ordinary V.8");
	}
	return true;
}

static bool qc_fallback(enum g711_law law, bool impaired, unsigned variant)
{
	struct v8_calling calling;
	struct v8_fsk answerer;
	struct v8_ansam_generator ansam;
	struct v8_menu menu;
	struct v8_control_generator jm;
	struct v92_qc1_message response = {
		.sender = V90_ANALOG_MODEM, .acknowledgment = true,
		.lapm = true, .signal.qts_index = 8,
	};
	uint8_t information[V8_MAX_INFORMATION_OCTETS], bit = 1;
	size_t count, after_qc = 0, sent = 0, ack_at = 0;
	uint32_t noise = 0xd982457;
	bool refused = false;

	CHECK(qc_caller_init(&calling, true, true) &&
	      v8_fsk_init(&answerer, V8_ANSWERING_STATION) == V8_FSK_OK &&
	      v8_ansam_generator_init(&ansam, 16000, true) == V8_ANSAM_OK &&
	      v8_pcm_call_menu_init(&menu, V8_PCM_ANALOG, 0, true) == V8_MENU_OK &&
	      v8_menu_encode(&menu, information, sizeof(information), &count) ==
		      V8_MENU_OK &&
	      v8_control_generator_init(&jm, information, count) == V8_FRAMING_OK,
	      "QC fallback fixture");
	if (variant == 6) {
		response.sender = V90_DIGITAL_MODEM;
		response.signal.anspcm_level = 1;
	} else if (variant == 7) {
		response.acknowledgment = false;
	} else if (variant == 9) {
		response.signal.qts_index = 15; /* On-hold cleardown, not startup. */
	}
	/* 0: ACK; 1: silence; 2: continued ANSam, late ACK, JM;
	 * 3: 0.9 s ANSam, ACK; 4: 0.5 s ANSam, JM; 5: 0.5 s ANSam,
	 * 0.5 s silence, ACK; 6..9: wrong role, request, damaged, on-hold ACK. */
	for (size_t clock = 0; clock < 12u * V8_ANSAM_SAMPLE_RATE; ++clock) {
		int16_t raw, filtered, transmit;
		bool initial = calling.state == V8_CALLING_WAIT_ANSWER_SIGNAL ||
			calling.state == V8_CALLING_SEND_QC1;
		bool pending = calling.qc_enabled;

		if (!initial && after_qc == 0)
			after_qc = clock;
		size_t age = initial ? 0 : clock - after_qc;
		bool tone = initial || (variant == 2 && age < 10000) ||
			(variant == 3 && age < 7200) ||
			((variant == 4 || variant == 5) && age < 4000);
		bool silence = !initial && (variant == 1 ||
			(variant == 5 && age >= 4000 && age < 8000));

		if (tone) {
			CHECK(v8_ansam_generate(&ansam, &raw, 1) == V8_ANSAM_OK &&
			      v8_fsk_filter_transmit_sample(&answerer, raw, &filtered) ==
				      V8_FSK_OK, "fallback ANSam");
		} else if (silence) {
			CHECK(v8_fsk_filter_transmit_sample(&answerer, 0, &filtered) ==
				      V8_FSK_OK, "fallback silence");
		} else {
			bool consumed;

			if (v8_fsk_can_accept_bit(&answerer)) {
				bool end;

				if (variant != 4 && sent < v92_qc1_bit_count(&response)) {
					CHECK(v92_qc1_bit(&response, sent, &bit), "QC response");
					if (variant == 8 && sent == 19)
						bit ^= 1;
					sent++;
				} else {
					CHECK(v8_control_next_bit(&jm, &bit, &end) ==
						      V8_FRAMING_OK, "fallback JM");
				}
			}
			CHECK(v8_fsk_transmit_sample(&answerer, bit, &filtered, &consumed) ==
				      V8_FSK_OK, "fallback FSK");
		}
		CHECK(qc_caller_sample(&calling, qc_channel(filtered, law, impaired,
			      &noise), &transmit, clock, &ack_at), "QC fallback caller");
		if (pending && !calling.qc_enabled &&
		    calling.state == V8_CALLING_EXCHANGE_MENUS)
			refused = true;
		if (v8_calling_is_complete(&calling) || calling.state == V8_CALLING_FAILED)
			break;
	}
	if (variant == 0 || variant == 3 || variant == 5) {
		CHECK(calling.state == V8_CALLING_QC_COMPLETE && ack_at != 0 && !refused,
		      "valid delayed QC acknowledgment was lost");
	} else if (variant == 1) {
		CHECK(calling.state == V8_CALLING_FAILED &&
		      calling.terminal_result == V8_CALLING_MENU_TIMEOUT &&
		      ack_at == 0 && !refused, "silence became ANSam refusal");
	} else {
		CHECK(calling.state == V8_CALLING_COMPLETE && ack_at == 0 &&
		      refused == (variant == 2), "QC fallback variant %u", variant);
	}
	return true;
}

static bool test_quick_connect(void)
{
	for (unsigned digital = 0; digital < 2; ++digital)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (unsigned impaired = 0; impaired < 2; ++impaired)
				for (unsigned lapm = 0; lapm < 4; ++lapm)
					for (unsigned admitted = 0; admitted < 2; ++admitted)
						CHECK(qc_pair(digital, lapm, law, impaired, admitted),
						      "QC role=%u law=%u impaired=%u LAPM=%u admit=%u",
						      digital, law, impaired, lapm, admitted);
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned impaired = 0; impaired < 2; ++impaired)
			for (unsigned variant = 0; variant < 10; ++variant)
				CHECK(qc_fallback(law, impaired, variant),
				      "QC law=%u impaired=%u variant=%u", law, impaired, variant);
	return true;
}

static bool test_quick_connect_validation(void)
{
	for (unsigned mutation = 0; mutation < 11; ++mutation) {
		struct v8_calling calling, saved;
		struct v8_menu menu;
		struct v92_qc1_message request = {
			.sender = V90_DIGITAL_MODEM, .lapm = true, .signal.anspcm_level = 1,
		};
		int16_t receive = 0, transmit = 123;

		CHECK(v8_pcm_call_menu_init(&menu, V8_PCM_DIGITAL, 0, true) == V8_MENU_OK,
		      "QC validation menu");
		if (mutation == 6)
			CHECK(v8_call_menu_init(&menu, V8_MODULATION_V34_DUPLEX, true) ==
				      V8_MENU_OK, "non-PCM menu");
		CHECK(v8_calling_init(&calling, &menu) == V8_CALLING_OK, "QC validation");
		switch (mutation) {
		case 0: request.acknowledgment = true; break;
		case 1: request.sender = V90_ANALOG_MODEM; break;
		case 2: request.lapm = false; break;
		case 3: request.signal.anspcm_level = 4; break;
		case 4: request.sender = (enum v90_modem_role)99; break;
		case 5:
			CHECK(v8_calling_process(&calling, &receive, &transmit, 1) ==
				      V8_CALLING_OK, "late configuration fixture");
			break;
		case 7: calling.state = V8_CALLING_COMPLETE; break;
		case 8: calling.qc_bit_index = 61; break;
		case 9:
			request.sender = V90_ANALOG_MODEM;
			request.signal.qts_index = 15;
			CHECK(v8_pcm_call_menu_init(&menu, V8_PCM_ANALOG, 0, true) ==
				      V8_MENU_OK && v8_calling_init(&calling, &menu) ==
				      V8_CALLING_OK, "on-hold fixture");
			break;
		default: break;
		}
		saved = calling;
		CHECK(v8_calling_enable_quick_connect(&calling, mutation == 10 ? NULL :
			      &request) == V8_CALLING_INVALID_ARGUMENT &&
		      memcmp(&calling, &saved, sizeof(saved)) == 0 &&
		      v8_calling_enable_quick_connect(NULL, &request) ==
			      V8_CALLING_INVALID_ARGUMENT,
		      "invalid QC admission %u changed state", mutation);
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "call negotiation", test_call_negotiation },
		{ "Quick Connect and V.8 fallback", test_quick_connect },
		{ "Quick Connect validation", test_quick_connect_validation },
		{ "no common modulation", test_no_common_modulation },
		{ "answer signal timeout", test_answer_signal_timeout },
		{ "menu timeout", test_menu_timeout },
		{ "validation", test_validation },
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
