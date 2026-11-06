// SPDX-License-Identifier: BSD-3-Clause
#include <v8_handoff.h>
#include <g711.h>
#include "test.h"

#include <string.h>

struct peer {
	struct v8_fsk fsk;
	struct v8_control_generator menu;
	struct v8_cj_detector cj;
	uint8_t bit;
	unsigned sequences;
};

static bool setup(struct v8_handoff_retry *retry, struct peer *peer, bool changed, bool malformed)
{
	struct v8_menu local, analog, joint;
	uint8_t information[V8_MAX_INFORMATION_OCTETS];
	size_t count;
	CHECK(v8_pcm_call_menu_init(&local, V8_MODULATION_V34_DUPLEX,
		      V8_PCM_DIGITAL, V8_PSTN_DIGITAL, true) == V8_MENU_OK &&
	      v8_pcm_call_menu_init(&analog, V8_MODULATION_V34_DUPLEX, V8_PCM_ANALOG, 0, true) == V8_MENU_OK &&
	      v8_joint_menu_select(&local, &analog, &joint) == V8_MENU_OK &&
	      v8_handoff_retry_init(retry, &local, &joint), "retry init");
	if (changed) joint.lapm = false;
	CHECK(v8_menu_encode(&joint, information, sizeof(information), &count) == V8_MENU_OK,
	      "peer menu");
	if (malformed) information[0] = 0xff;
	*peer = (struct peer){ 0 };
	CHECK(v8_fsk_init(&peer->fsk, V8_ANSWERING_STATION) == V8_FSK_OK &&
	      v8_cj_detector_init(&peer->cj) == V8_FRAMING_OK &&
	      v8_control_generator_init(&peer->menu, information, count) == V8_FRAMING_OK,
	      "peer init");
	return true;
}

static bool peer_sample(struct peer *peer, bool quiet, int16_t input, int16_t *output)
{
	bool consumed, ready;
	uint8_t bit;
	if (quiet || peer->cj.detected) {
		*output = 0;
	} else {
		if (v8_fsk_can_accept_bit(&peer->fsk)) {
			bool complete;
			CHECK(v8_control_next_bit(&peer->menu, &peer->bit, &complete) == V8_FRAMING_OK,
			      "peer bit");
			peer->sequences += complete;
		}
		CHECK(v8_fsk_transmit_sample(&peer->fsk, peer->bit, output, &consumed) == V8_FSK_OK,
		      "peer transmit");
	}
	CHECK(v8_fsk_receive_sample(&peer->fsk, input, &bit, &ready) == V8_FSK_OK,
	      "peer receive");
	if (ready) {
		bool detected;
		CHECK(v8_cj_detect_bit(&peer->cj, bit, &detected) == V8_FRAMING_OK, "peer CJ");
	}
	return true;
}

enum scenario { RECOVER, CHANGED, MALFORMED, SINGLE_MENU, LOST_MEDIA, RETRY_TIMEOUT };

static bool run_case(enum g711_law law, enum scenario scenario)
{
	struct v8_handoff_retry retry, isolated;
	struct peer peer;
	CHECK(setup(&retry, &peer, scenario == CHANGED, scenario == MALFORMED), "setup");
	isolated = retry;
	int16_t input = 0;
	unsigned sends = 0, restarts = 0;
	bool finished = false;
	for (unsigned n = 0; n < 2 * V8_HANDOFF_WINDOW_SAMPLES + 2u; ++n) {
		int16_t received, output = 1234, untouched = 5678;
		bool quiet = (scenario == SINGLE_MENU && peer.sequences != 0) ||
			(scenario == RETRY_TIMEOUT && retry.transmitting);
		CHECK(peer_sample(&peer, quiet, input, &received), "peer step");
		enum v8_handoff_result result = v8_handoff_retry_sample(&retry,
			g711_quantize(law, received), scenario != LOST_MEDIA || n % 1024 != 0, &output);
		CHECK(result != V8_HANDOFF_INVALID, "invalid retry scenario %u", scenario);
		enum v8_handoff_result other = v8_handoff_retry_sample(&isolated, 0, true, &untouched);
		CHECK((other == V8_HANDOFF_LISTEN || other == V8_HANDOFF_FINISHED) && untouched == 5678,
		      "cross-instance signal or output mutation");
		input = result == V8_HANDOFF_SEND || result == V8_HANDOFF_RESTART ?
			g711_quantize(law, output) : 0;
		sends += result == V8_HANDOFF_SEND;
		restarts += result == V8_HANDOFF_RESTART;
		if (result == V8_HANDOFF_FINISHED || result == V8_HANDOFF_RESTART) {
			finished = true;
			break;
		}
		if (result == V8_HANDOFF_LISTEN) CHECK(output == 1234, "listen changed transmit");
	}
	/* The peer has its own causal receive filter. It must finish CJ within
	 * the ordinary silence supplied by the following Phase 2 owner. */
	if (scenario == RECOVER)
		for (unsigned n = 0; n < V8_CALLING_POST_CJ_SILENCE_SAMPLES; ++n) {
			int16_t output;
			CHECK(peer_sample(&peer, false, input, &output), "CJ receive tail");
			input = 0;
		}
	CHECK(finished && restarts == (scenario == RECOVER) &&
	      (scenario != RECOVER || (sends && peer.cj.detected)),
	      "retry outcome scenario %u finished %u restarts %u sends %u CJ %u matched %u",
	      scenario, finished, restarts, sends, peer.cj.detected, peer.cj.matched_octets);
	CHECK(scenario == RECOVER || scenario == RETRY_TIMEOUT || !sends, "false activation %u", scenario);
	for (unsigned n = 0; n < 1000; ++n) {
		int16_t output = 1234;
		CHECK(v8_handoff_retry_sample(&retry, 1000, true, &output) == V8_HANDOFF_FINISHED &&
		      output == 1234, "completed retry rearmed");
	}
	return true;
}

static bool test_retry(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (enum scenario scenario = RECOVER; scenario <= RETRY_TIMEOUT; ++scenario)
			CHECK(run_case(law, scenario), "scenario %u law %u", scenario, law);
	return true;
}

static bool test_bounds(void)
{
	struct v8_handoff_retry retry, saved;
	struct peer peer;
	int16_t output = 1234;
	CHECK(setup(&retry, &peer, false, false), "setup");
	retry.remaining = V8_HANDOFF_WINDOW_SAMPLES + 1u;
	saved = retry;
	CHECK(v8_handoff_retry_sample(&retry, 0, true, &output) == V8_HANDOFF_INVALID &&
	      !memcmp(&retry, &saved, sizeof(retry)) && output == 1234, "invalid count changed state");
	CHECK(v8_handoff_retry_sample(NULL, 0, true, &output) == V8_HANDOFF_INVALID &&
	      v8_handoff_retry_sample(&retry, 0, true, NULL) == V8_HANDOFF_INVALID &&
	      v8_handoff_retry_sample(&retry, 0, true, (int16_t *)&retry.remaining) == V8_HANDOFF_INVALID &&
	      !v8_handoff_retry_init(NULL, &retry.local_menu, &retry.joint_menu) &&
	      !v8_handoff_retry_init(&retry, NULL, &retry.joint_menu), "argument bounds");
	return true;
}

int main(void)
{
	return test_retry() && test_bounds() ? 0 : 1;
}
