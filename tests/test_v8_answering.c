// SPDX-License-Identifier: BSD-3-Clause

#include <v8_answering.h>
#include <v8_calling.h>
#include <g711.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static uint64_t hash_word(uint64_t hash, uint16_t word)
{
	hash ^= word & UINT16_C(0xff);
	hash *= UINT64_C(1099511628211);
	hash ^= word >> 8;
	return hash * UINT64_C(1099511628211);
}

static bool endpoints_init(
	struct v8_calling *calling,
	struct v8_answering *answering,
	uint16_t calling_modulations, uint16_t answering_modulations)
{
	struct v8_menu calling_menu;
	struct v8_menu answering_menu;

	return v8_call_menu_init(
		       &calling_menu, calling_modulations, true) ==
		       V8_MENU_OK &&
	       v8_call_menu_init(
		       &answering_menu, answering_modulations, true) ==
		       V8_MENU_OK &&
	       v8_calling_init(calling, &calling_menu) ==
		       V8_CALLING_OK &&
	       v8_answering_init(answering, &answering_menu) ==
		       V8_ANSWERING_OK;
}

static bool negotiate_menus(const struct v8_menu *call_menu,
	const struct v8_menu *answer_menu, enum v8_modulation expected)
{
	struct v8_answering answering;
	struct v8_calling calling;
	struct v8_menu calling_selection;
	struct v8_menu answering_selection;
	enum v8_modulation calling_modulation;
	enum v8_modulation answering_modulation;
	int16_t calling_transmit = 0;
	size_t completed = 0;

	CHECK(v8_calling_init(&calling, call_menu) == V8_CALLING_OK &&
	      v8_answering_init(&answering, answer_menu) == V8_ANSWERING_OK,
	      "V.8 endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		int16_t answering_transmit;
		enum v8_answering_state before = answering.state;

		CHECK(v8_answering_process(
			      &answering, &calling_transmit,
			      &answering_transmit, 1) ==
			      V8_ANSWERING_OK &&
		      v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      V8_CALLING_OK,
		      "V.8 endpoints failed at sample %zu", sample_index);
		if (before == V8_ANSWERING_COMPLETE)
			CHECK(answering_transmit == 0,
			      "V.8 answerer emitted audio after CJ");
		if (before != answering.state &&
			 answering.state == V8_ANSWERING_COMPLETE)
			completed = sample_index + 1u;
		if (v8_calling_is_complete(&calling) &&
		    v8_answering_is_complete(&answering))
			break;
	}
	CHECK(completed != 0 &&
	      v8_calling_is_complete(&calling) &&
	      v8_answering_is_complete(&answering),
	      "V.8 endpoints did not complete the timed handshake");
	CHECK(v8_calling_selection(
		      &calling, &calling_selection, &calling_modulation) ==
		      V8_CALLING_OK &&
	      v8_answering_selection(
		      &answering, &answering_selection,
		      &answering_modulation) == V8_ANSWERING_OK &&
	      calling_modulation == expected &&
	      answering_modulation == calling_modulation &&
	      answering_selection.modulations ==
		      calling_selection.modulations &&
	      !answering_selection.pcm_present && !calling_selection.pcm_present &&
	      answering_selection.lapm && calling_selection.lapm,
	      "V.8 endpoints disagreed on the selected data mode");
	return true;
}

static bool test_call_negotiation(void)
{
	struct v8_menu menu;

	CHECK(v8_call_menu_init(&menu, V8_MODULATION_V21, true) == V8_MENU_OK,
	      "V.21 menu setup failed");
	return negotiate_menus(&menu, &menu, V8_MODULATION_V21);
}

static bool test_pcm_with_non_pcm_peer(void)
{
	struct v8_menu pcm;
	struct v8_menu v34;

	CHECK(v8_call_menu_init(&v34, V8_MODULATION_V34_DUPLEX, true) == V8_MENU_OK,
	      "V.34 menu setup failed");
	for (unsigned int digital = 0; digital < 2; ++digital) {
		CHECK(v8_pcm_call_menu_init(&pcm,
			      digital ? V8_PCM_DIGITAL : V8_PCM_ANALOG,
			      digital ? V8_PSTN_DIGITAL : 0, true) == V8_MENU_OK &&
		      negotiate_menus(&v34, &pcm, V8_MODULATION_V34_DUPLEX) &&
		      negotiate_menus(&pcm, &v34, V8_MODULATION_V34_DUPLEX),
		      "V.8 did not complete V.34 fallback with one PCM peer");
	}
	return true;
}

static bool test_answer_signal_wire(void)
{
	struct v8_answering answering;
	struct v8_menu menu;
	int16_t receive[73] = { 0 };
	int16_t transmit[ARRAY_LENGTH(receive)];
	uint64_t hash = UINT64_C(14695981039346656037);
	size_t first_nonzero = SIZE_MAX;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V34_DUPLEX, false) == V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) == V8_ANSWERING_OK,
	      "V.8 answer-signal oracle did not initialize");
	for (size_t offset = 0; offset < 9000;) {
		size_t count = ARRAY_LENGTH(receive);

		if (count > 9000u - offset)
			count = 9000u - offset;
		CHECK(v8_answering_process(
			      &answering, receive, transmit, count) ==
			      V8_ANSWERING_OK,
		      "V.8 answer signal stopped at %zu", offset);
		for (size_t index = 0; index < count; ++index) {
			if (first_nonzero == SIZE_MAX && transmit[index] != 0)
				first_nonzero = offset + index;
			hash = hash_word(hash, (uint16_t)transmit[index]);
		}
		offset += count;
	}
	CHECK(first_nonzero == V8_ANSWERING_QUIET_SAMPLES &&
		      hash == UINT64_C(0xd927fafa7f14d42f) &&
		      answering.quiet_samples == 0 &&
		      answering.state == V8_ANSWERING_WAIT_CALL_MENU,
	      "V.8 answer wire changed at %zu: %#llx",
	      first_nonzero, (unsigned long long)hash);
	return true;
}

static bool test_no_common_modulation(void)
{
	struct v8_answering answering;
	struct v8_calling calling;
	enum v8_answering_result result =
		V8_ANSWERING_OK;
	int16_t calling_transmit = 0;
	int16_t answering_transmit = 0;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V21,
		      V8_MODULATION_V22),
	      "V.8 incompatible endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 6u * V8_ANSAM_SAMPLE_RATE;
	     ++sample_index) {
		result = v8_answering_process(
			&answering, &calling_transmit, &answering_transmit, 1);
		if (result != V8_ANSWERING_OK)
			break;
		CHECK(v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      V8_CALLING_OK,
		      "V.8 incompatible caller failed early");
	}
	CHECK(result == V8_ANSWERING_NO_COMMON_MODULATION &&
	      answering.state == V8_ANSWERING_FAILED &&
	      v8_answering_process(
		      &answering, &calling_transmit,
		      &answering_transmit, 1) ==
		      V8_ANSWERING_NO_COMMON_MODULATION,
	      "V.8 answerer did not latch the incompatible offer");
	return true;
}

static bool test_call_menu_timeout(void)
{
	struct v8_answering answering;
	struct v8_menu menu;
	int16_t silence[V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[V8_ANSWERING_MAX_FRAGMENT_SAMPLES];
	enum v8_answering_result result =
		V8_ANSWERING_OK;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) ==
		      V8_ANSWERING_OK,
	      "V.8 answerer timeout fixture did not initialize");
	for (size_t offset = 0;
	     offset < V8_ANSWERING_CALL_MENU_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence))
		result = v8_answering_process(
			&answering, silence, transmit, ARRAY_LENGTH(silence));
	CHECK(result == V8_ANSWERING_CALL_MENU_TIMEOUT &&
	      answering.state == V8_ANSWERING_FAILED &&
	      v8_answering_process(
		      &answering, silence, transmit, 1) ==
		      V8_ANSWERING_CALL_MENU_TIMEOUT,
	      "V.8 call-menu timeout did not latch");
	return true;
}

static bool test_cj_timeout(void)
{
	struct v8_answering answering;
	struct v8_calling calling;
	int16_t calling_transmit = 0;
	int16_t silence[V8_ANSWERING_MAX_FRAGMENT_SAMPLES] = { 0 };
	int16_t transmit[V8_ANSWERING_MAX_FRAGMENT_SAMPLES];
	enum v8_answering_result result =
		V8_ANSWERING_OK;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V21,
		      V8_MODULATION_V21),
	      "V.8 CJ-timeout endpoints did not initialize");
	for (size_t sample_index = 0;
	     sample_index < 4u * V8_ANSAM_SAMPLE_RATE &&
	     answering.state == V8_ANSWERING_WAIT_CALL_MENU;
	     ++sample_index) {
		int16_t answering_transmit;

		CHECK(v8_answering_process(
			      &answering, &calling_transmit,
			      &answering_transmit, 1) ==
			      V8_ANSWERING_OK &&
		      v8_calling_process(
			      &calling, &answering_transmit,
			      &calling_transmit, 1) ==
			      V8_CALLING_OK,
		      "V.8 CJ-timeout endpoints failed during CM");
	}
	CHECK(answering.state == V8_ANSWERING_SEND_JOINT_MENU,
	      "V.8 answerer did not reach the CJ wait");
	for (size_t offset = 0;
	     offset < V8_ANSWERING_CJ_TIMEOUT_SAMPLES;
	     offset += ARRAY_LENGTH(silence))
		result = v8_answering_process(
			&answering, silence, transmit, ARRAY_LENGTH(silence));
	CHECK(result == V8_ANSWERING_CJ_TIMEOUT &&
	      answering.state == V8_ANSWERING_FAILED &&
	      v8_answering_process(
		      &answering, silence, transmit, 1) ==
		      V8_ANSWERING_CJ_TIMEOUT,
	      "V.8 CJ timeout did not latch");
	return true;
}

static bool reach_cj_exchange(
	struct v8_calling *calling, struct v8_answering *answering)
{
	int16_t calling_transmit = 0;

	for (size_t sample = 0;
	     sample < 4u * V8_ANSAM_SAMPLE_RATE; ++sample) {
		int16_t answering_transmit;

		if (v8_answering_process(
			    answering, &calling_transmit,
			    &answering_transmit, 1) !=
			    V8_ANSWERING_OK ||
		    v8_calling_process(
			    calling, &answering_transmit,
			    &calling_transmit, 1) != V8_CALLING_OK)
			return false;
		if (calling->state == V8_CALLING_SEND_CJ &&
		    answering->state == V8_ANSWERING_SEND_JOINT_MENU)
			return true;
	}
	return false;
}

static bool drive_damaged_cj(
	struct v8_calling *calling, struct v8_answering *answering,
	size_t damaged_bit, bool expect_fallback)
{
	uint8_t transmit_bit = 1;
	size_t next_bit = 0;
	bool saw_guard_boundary = false;

	for (size_t sample = 0;
	     sample < 2u * V8_ANSAM_SAMPLE_RATE; ++sample) {
		bool consumed;
		int16_t receive;
		int16_t transmit;

		if (v8_fsk_can_accept_bit(&calling->fsk)) {
			if (next_bit < V8_CJ_BITS) {
				if (v8_cj_bit(next_bit, &transmit_bit) !=
				    V8_FRAMING_OK)
					return false;
				if (next_bit == damaged_bit)
					transmit_bit ^= 1u;
			} else {
				transmit_bit = 1;
			}
			next_bit++;
		}
		if (v8_fsk_transmit_sample(
			    &calling->fsk, transmit_bit, &receive,
			    &consumed) != V8_FSK_OK ||
		    v8_answering_process(
			    answering, &receive, &transmit, 1) !=
			    V8_ANSWERING_OK)
			return false;
		if (answering->cj_fallback_armed &&
		    answering->cj_fallback_bits ==
			    V8_ANSWERING_CJ_FALLBACK_BITS - 1u) {
			if (answering->state !=
			    V8_ANSWERING_SEND_JOINT_MENU)
				return false;
			saw_guard_boundary = true;
		}
		if (answering->state == V8_ANSWERING_COMPLETE)
			return expect_fallback && saw_guard_boundary;
		if (!expect_fallback && next_bit > V8_CJ_BITS +
							 2u * V8_FRAMED_OCTET_BITS)
			return !answering->cj_fallback_armed;
	}
	return false;
}

static bool test_cj_completion_fallback(void)
{
	struct v8_answering answering;
	struct v8_calling calling;

	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V34_DUPLEX,
		      V8_MODULATION_V34_DUPLEX) &&
	      reach_cj_exchange(&calling, &answering) &&
	      drive_damaged_cj(
		      &calling, &answering, V8_CJ_BITS - 1u, true),
	      "V.8 answerer did not bound a damaged final CJ stop bit");
	CHECK(endpoints_init(
		      &calling, &answering, V8_MODULATION_V34_DUPLEX,
		      V8_MODULATION_V34_DUPLEX) &&
	      reach_cj_exchange(&calling, &answering) &&
	      drive_damaged_cj(
		      &calling, &answering,
		      2u * V8_FRAMED_OCTET_BITS - 1u, false),
	      "V.8 answerer accepted fewer than two complete CJ octets");
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

static bool qc_message_equal(const struct v92_qc1_message *a,
	const struct v92_qc1_message *b)
{
	return a->sender == b->sender && a->acknowledgment == b->acknowledgment &&
		a->lapm == b->lapm && (a->sender == V90_DIGITAL_MODEM ?
		a->signal.anspcm_level == b->signal.anspcm_level :
		a->signal.qts_index == b->signal.qts_index);
}

static bool qc_answer_case(bool digital, unsigned lapm, enum g711_law law,
	bool impaired, unsigned variant)
{
	struct v92_qc1_message response = {
		.sender = digital ? V90_DIGITAL_MODEM : V90_ANALOG_MODEM,
		.acknowledgment = true, .lapm = (lapm & 1u) != 0,
	};
	struct v92_qc1_message request = {
		.sender = digital ? V90_ANALOG_MODEM : V90_DIGITAL_MODEM,
		.lapm = (lapm & 2u) != 0,
	};
	struct v8_answering answering;
	struct v8_menu answer_menu, call_menu;
	struct v8_ansam_detector ansam;
	struct v8_fsk caller;
	struct v8_control_generator cm;
	struct v8_menu_detector jm;
	struct v92_qc1_receiver qca;
	int16_t to_caller[257] = { 0 }, to_answerer[257] = { 0 };
	const size_t delay = impaired ? 257 : 13;
	size_t cursor = 0, sent = 0, qca_start = 0, qca_end = 0;
	uint8_t information[V8_MAX_INFORMATION_OCTETS], transmit_bit = 1;
	size_t information_count;
	uint32_t noise = 0xabc456ef;
	unsigned acknowledgements = 0;
	bool started = false, menu_received = false;

	response.signal.qts_index = digital ? 2 : 10;
	request.signal.qts_index = digital ? 7 : 1;
	if (variant == 2) { /* Same-role QC is optional; use full V.8 instead. */
		request.sender = response.sender;
		request.signal.qts_index = 1;
	} else if (variant == 3) {
		request.acknowledgment = true;
	} else if (variant == 4) {
		request.sender = V90_ANALOG_MODEM;
		request.signal.qts_index = 15; /* On-hold cleardown, not startup. */
	}
	CHECK(v8_pcm_call_menu_init(&answer_menu, digital ? V8_PCM_DIGITAL :
		      V8_PCM_ANALOG, digital ? V8_PSTN_DIGITAL : 0,
		      response.lapm) == V8_MENU_OK &&
	      v8_pcm_call_menu_init(&call_menu, digital ? V8_PCM_ANALOG :
		      V8_PCM_DIGITAL, digital ? 0 : V8_PSTN_DIGITAL,
		      request.lapm) == V8_MENU_OK &&
	      v8_answering_init(&answering, &answer_menu) == V8_ANSWERING_OK &&
	      (variant == 1 || v8_answering_enable_quick_connect(&answering,
		      &response) == V8_ANSWERING_OK) &&
	      v8_ansam_detector_init(&ansam) == V8_ANSAM_OK &&
	      v8_fsk_init(&caller, V8_CALLING_STATION) == V8_FSK_OK &&
	      v92_qc1_receiver_init(&qca) && v8_menu_detector_init(&jm) == V8_MENU_OK &&
	      v8_menu_encode(&call_menu, information, sizeof(information),
		      &information_count) == V8_MENU_OK &&
	      v8_control_generator_init(&cm, information, information_count) ==
		      V8_FRAMING_OK, "QC answer fixture");
	if (variant != 1) {
		response.signal.qts_index ^= 1;
		CHECK(!qc_message_equal(&answering.qc_response, &response),
		      "QC response borrowed caller storage");
		response.signal.qts_index ^= 1;
	}
	for (size_t clock = 0; clock < 64000; ++clock) {
		int16_t answer_tx, caller_tx = 0;
		enum v8_answering_state before = answering.state;
		bool ready, consumed;
		uint8_t bit;

		CHECK(v8_answering_process(&answering, &to_answerer[cursor],
			      &answer_tx, 1) == V8_ANSWERING_OK,
		      "QC answer processing at %zu", clock);
		if (before != answering.state) {
			if (answering.state == V8_ANSWERING_SEND_QCA1)
				qca_start = clock;
			if (answering.state == V8_ANSWERING_QC_COMPLETE)
				qca_end = clock;
		}
		if (before == V8_ANSWERING_QC_COMPLETE)
			CHECK(answer_tx == 0, "QCA1 repeated after completion");
		if (!started) {
			CHECK(v8_ansam_detect(&ansam, &caller.line, &to_caller[cursor],
				      1, &started) == V8_ANSAM_OK, "initial ANSam");
		} else {
			if (acknowledgements == 0) {
				if (v8_fsk_can_accept_bit(&caller)) {
					bool end;

					if (variant != 6 && sent < v92_qc1_bit_count(&request)) {
						CHECK(v92_qc1_bit(&request, sent, &transmit_bit),
						      "QC1 source");
						if (variant == 5 && sent == 52)
							transmit_bit ^= 1;
						sent++;
					} else {
						CHECK(v8_control_next_bit(&cm, &transmit_bit,
							      &end) == V8_FRAMING_OK, "CM source");
					}
				}
				CHECK(v8_fsk_transmit_sample(&caller, transmit_bit,
					      &caller_tx, &consumed) == V8_FSK_OK, "caller FSK");
			}
			CHECK(v8_fsk_receive_sample(&caller, to_caller[cursor], &bit,
				      &ready) == V8_FSK_OK, "caller receiver");
			if (ready) {
				struct v92_qc1_message actual;
				struct v8_menu menu;
				bool complete;

				CHECK(v92_qc1_receive_bit(&qca, bit, &actual, &complete) &&
				      v8_menu_detect_bit(&jm, bit, &menu, &menu_received) ==
					      V8_MENU_OK, "QC and menu decoding");
				if (complete) {
					CHECK(qc_message_equal(&actual, &response), "QCA1 fields");
					acknowledgements++;
				}
			}
		}
		to_caller[cursor] = qc_channel(answer_tx, law, impaired, &noise);
		to_answerer[cursor] = qc_channel(caller_tx, law, impaired, &noise);
		cursor = (cursor + 1) % delay;
		if ((qca_end != 0 && clock > qca_end + delay + 800) || menu_received)
			break;
	}
	if (variant == 0) {
		struct v8_menu menu;
		enum v8_modulation modulation = V8_MODULATION_NONE;

		CHECK(started && acknowledgements == 1 && !menu_received &&
		      answering.state == V8_ANSWERING_QC_COMPLETE &&
		      v8_answering_is_complete(&answering) &&
		      qc_message_equal(&answering.qc_request, &request) &&
		      qc_message_equal(&answering.qc_response, &response) &&
		      qca_end - qca_start == (70 * 8000 + 299) / 300 +
			      V8_FSK_TRANSMIT_FILTER_TAPS &&
		      v8_answering_selection(&answering, &menu, &modulation) ==
			      V8_ANSWERING_NOT_READY && modulation == V8_MODULATION_NONE,
		      "QC completion or handoff timing: %zu..%zu ACKs=%u",
		      qca_start, qca_end, acknowledgements);
	} else {
		CHECK(started && menu_received && acknowledgements == 0 &&
		      answering.state == V8_ANSWERING_SEND_JOINT_MENU && qca_start == 0,
		      "QC variant %u did not retain ordinary V.8", variant);
	}
	return true;
}

static bool test_quick_connect(void)
{
	for (unsigned digital = 0; digital < 2; ++digital)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (unsigned impaired = 0; impaired < 2; ++impaired) {
				for (unsigned lapm = 0; lapm < 4; ++lapm)
					CHECK(qc_answer_case(digital, lapm, law, impaired, 0),
					      "QC role=%u law=%u impaired=%u LAPM=%u",
					      digital, law, impaired, lapm);
				for (unsigned variant = 1; variant <= 6; ++variant)
					CHECK(qc_answer_case(digital, 3, law, impaired, variant),
					      "QC fallback role=%u law=%u impaired=%u variant=%u",
					      digital, law, impaired, variant);
			}
	return true;
}

static bool test_quick_connect_validation(void)
{
	for (unsigned mutation = 0; mutation < 10; ++mutation) {
		struct v8_answering answering, saved;
		struct v8_menu menu;
		struct v92_qc1_message response = {
			.sender = V90_DIGITAL_MODEM, .acknowledgment = true,
			.lapm = true, .signal.anspcm_level = 2,
		};
		int16_t silence = 0, transmit;

		CHECK(v8_pcm_call_menu_init(&menu, V8_PCM_DIGITAL,
			      V8_PSTN_DIGITAL, true) == V8_MENU_OK, "QC validation menu");
		switch (mutation) {
		case 2: response.acknowledgment = false; break;
		case 3: response.sender = (enum v90_modem_role)99; break;
		case 4: response.signal.anspcm_level = 4; break;
		case 5: response.lapm = false; break;
		case 6: menu.pcm_availability = V8_PCM_ANALOG; break;
		case 7:
			CHECK(v8_call_menu_init(&menu, V8_MODULATION_V21, true) ==
				      V8_MENU_OK, "non-PCM menu");
			break;
		case 9:
			menu.pcm_availability = V8_PCM_ANALOG;
			response.sender = V90_ANALOG_MODEM;
			response.signal.qts_index = 15;
			break;
		default: break;
		}
		CHECK(v8_answering_init(&answering, &menu) == V8_ANSWERING_OK,
		      "QC validation init");
		if (mutation == 8)
			CHECK(v8_answering_process(&answering, &silence, &transmit, 1) ==
				      V8_ANSWERING_OK, "late configuration fixture");
		saved = answering;
		CHECK(v8_answering_enable_quick_connect(mutation == 0 ? NULL : &answering,
			      mutation == 1 ? NULL : &response) == V8_ANSWERING_INVALID_ARGUMENT &&
		      memcmp(&answering, &saved, sizeof(saved)) == 0,
		      "invalid QC admission %u changed state", mutation);
	}
	return true;
}

static bool test_validation(void)
{
	struct v8_answering answering;
	struct v8_answering saved;
	struct v8_menu menu;
	struct v8_menu selected_menu;
	enum v8_modulation selected =
		V8_MODULATION_V34_DUPLEX;
	int16_t receive = 0;
	int16_t transmit = 123;

	CHECK(v8_call_menu_init(
		      &menu, V8_MODULATION_V21, false) ==
		      V8_MENU_OK &&
	      v8_answering_init(&answering, &menu) ==
		      V8_ANSWERING_OK,
	      "V.8 answering validation fixture did not initialize");
	saved = answering;
	CHECK(v8_answering_init(NULL, &menu) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      v8_answering_process(
		      &answering, &receive, &transmit,
		      V8_ANSWERING_MAX_FRAGMENT_SAMPLES + 1u) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      transmit == 123 &&
	      memcmp(&answering, &saved, sizeof(answering)) == 0 &&
	      v8_answering_process(
		      &answering, NULL, &transmit, 1) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      v8_answering_process(
		      &answering, NULL, NULL, 0) ==
		      V8_ANSWERING_OK &&
	      v8_answering_selection(
		      &answering, &selected_menu, &selected) ==
		      V8_ANSWERING_NOT_READY &&
	      selected == V8_MODULATION_V34_DUPLEX &&
	      strcmp(v8_answering_result_name(
			     V8_ANSWERING_CJ_TIMEOUT),
		     "CJ timeout") == 0,
	      "V.8 answering API accepted invalid arguments");
	answering.cj_fallback_armed = true;
	saved = answering;
	CHECK(v8_answering_process(
		      &answering, &receive, &transmit, 1) ==
		      V8_ANSWERING_INVALID_ARGUMENT &&
	      memcmp(&answering, &saved, sizeof(answering)) == 0,
	      "V.8 answering accepted a fallback outside the CJ wait");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "answer signal wire", test_answer_signal_wire },
		{ "call negotiation", test_call_negotiation },
		{ "PCM with non-PCM peer", test_pcm_with_non_pcm_peer },
		{ "Quick Connect admission and V.8 fallback", test_quick_connect },
		{ "Quick Connect configuration validation", test_quick_connect_validation },
		{ "no common modulation", test_no_common_modulation },
		{ "call menu timeout", test_call_menu_timeout },
		{ "CJ timeout", test_cj_timeout },
		{ "CJ completion fallback", test_cj_completion_fallback },
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
