// SPDX-License-Identifier: BSD-3-Clause

/* Reuse the real modem and generated answer-mark/S1/data peer. */
#define main integration_test_main
#include "test_fsk_integration.c"
#undef main

/* Only this serial test retimes a notification from the independent tone
 * detector. The received waveform and the S1 qualifier remain native. */
static struct {
	unsigned prefix;
	unsigned notifications;
	unsigned interruptions;
	unsigned reacquisitions;
	bool active;
	bool interrupt;
} mark_loss;

extern __typeof__(single_tone_detector_sample) __real_single_tone_detector_sample;
enum single_tone_result __wrap_single_tone_detector_sample(
	struct single_tone_detector *detector, int16_t sample,
	struct single_tone_event *event)
{
	enum single_tone_result result = __real_single_tone_detector_sample(detector, sample, event);
	if (result != SINGLE_TONE_OK || !mark_loss.active || detector->profile.frequency_hz != 2250)
		return result;
	/* A real captured call reports tone loss with ten S1 symbols already
	 * observed. Repeat/retime that notification at this boundary. An
	 * earlier acquisition remains available to half-symbol phases. */
	if (mark_loss.notifications && event->kind == SINGLE_TONE_ENDED)
		event->kind = SINGLE_TONE_NO_EVENT;
	if (!mark_loss.notifications && mark_loss.prefix == 10u) {
		event->kind = SINGLE_TONE_ENDED;
		mark_loss.notifications++;
	}
	return result;
}

extern __typeof__(v22bis_rate_signal_observe) __real_v22bis_rate_signal_observe;
enum v22_framing_result __wrap_v22bis_rate_signal_observe(
	struct v22bis_rate_signal_detector *detector, uint8_t quarter_turns,
	enum v22bis_rate_signal_event *event)
{
	/* One wrong differential decision breaks the unqualified prefix.
	 * A later complete on-wire S1 must recover data. */
	if (mark_loss.active && mark_loss.interrupt && mark_loss.notifications &&
	    !mark_loss.interruptions && detector->alternating_symbols >= 10u) {
		quarter_turns = detector->previous_turn;
		mark_loss.interruptions++;
	}
	enum v22_framing_result result = __real_v22bis_rate_signal_observe(detector, quarter_turns, event);
	if (mark_loss.active && result == V22_FRAMING_OK)
		mark_loss.prefix = detector->alternating_symbols;
	return result;
}

extern __typeof__(qam_demodulator_reacquire) __real_qam_demodulator_reacquire;
enum qam_result __wrap_qam_demodulator_reacquire(
	struct qam_demodulator *demodulator, uint32_t symbols)
{
	enum qam_result result = __real_qam_demodulator_reacquire(demodulator, symbols);
	if (mark_loss.active && mark_loss.notifications && result == QAM_OK)
		mark_loss.reacquisitions++;
	return result;
}

static bool run_mark_loss(bool interrupt)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned phase = 0; phase < 14u; ++phase) {
			memset(&mark_loss, 0, sizeof(mark_loss));
			mark_loss.active = true;
			mark_loss.interrupt = interrupt;
			bool payload = run_v22bis_mark_transition(phase, law, interrupt);
			mark_loss.active = false;
			CHECK(payload && mark_loss.notifications == 1u &&
			      mark_loss.interruptions == (unsigned)interrupt &&
			      mark_loss.reacquisitions == (unsigned)interrupt,
			      "mark loss law %u phase %u interrupted %u: payload %u notifications %u breaks %u reacquisitions %u",
			      law, phase, interrupt, payload, mark_loss.notifications,
			      mark_loss.interruptions, mark_loss.reacquisitions);
		}
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.22bis progressing S1 survives tone loss\n");
	if (!run_mark_loss(false)) return 1;
	fprintf(stderr, "[       OK ] V.22bis progressing S1 survives tone loss\n");
	fprintf(stderr, "[ RUN      ] V.22bis broken S1 resumes timing acquisition\n");
	if (!run_mark_loss(true)) return 1;
	fprintf(stderr, "[       OK ] V.22bis broken S1 resumes timing acquisition\n");
	return 0;
}
