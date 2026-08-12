// SPDX-License-Identifier: BSD-3-Clause

#include <audio_path.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_FRAME_SAMPLES = 4,
	TEST_RECORDINGS = 4,
};

struct sip_session {
	size_t frame_samples;
	uint8_t receive_codewords[TEST_FRAME_SAMPLES];
	int16_t receive_samples[TEST_FRAME_SAMPLES];
	int16_t sent_samples[TEST_FRAME_SAMPLES];
	enum media_result play_result;
	enum rtp_endpoint_result send_result;
	unsigned int play_calls;
	unsigned int send_calls;
};

size_t sip_session_frame_samples(
	const struct sip_session *session)
{
	return session != NULL ? session->frame_samples : 0;
}

enum media_result sip_session_play(
	struct sip_session *session, int16_t *samples,
	uint8_t *source_octets, size_t count,
	struct rtp_playout_report *report)
{
	if (session == NULL || samples == NULL || count != session->frame_samples)
		return MEDIA_INVALID_ARGUMENT;
	session->play_calls++;
	if (session->play_result != MEDIA_OK)
		return session->play_result;
	memcpy(samples, session->receive_samples,
	       count * sizeof(samples[0]));
	if (source_octets != NULL)
		memcpy(source_octets, session->receive_codewords, count);
	if (report != NULL)
		memset(report, 0, sizeof(*report));
	return MEDIA_OK;
}

enum rtp_endpoint_result sip_session_send(
	struct sip_session *session, const int16_t *samples, size_t count)
{
	if (session == NULL || samples == NULL || count != session->frame_samples)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	session->send_calls++;
	if (session->send_result == RTP_ENDPOINT_OK)
		memcpy(session->sent_samples, samples,
		       count * sizeof(samples[0]));
	return session->send_result;
}

struct core_capture {
	int16_t expected_receive[TEST_FRAME_SAMPLES];
	int16_t transmit[TEST_FRAME_SAMPLES];
	enum modem_core_result result;
	unsigned int calls;
	unsigned int g711_calls;
	bool receive_matched;
	bool codewords_matched;
	enum g711_law law;
};

static enum modem_core_result process_core(
	void *context, int16_t *receive_samples,
	int16_t *transmit_samples, size_t count)
{
	struct core_capture *capture = context;

	capture->calls++;
	capture->receive_matched =
		count == TEST_FRAME_SAMPLES &&
		memcmp(receive_samples, capture->expected_receive,
		       sizeof(capture->expected_receive)) == 0;
	memcpy(transmit_samples, capture->transmit,
	       count * sizeof(transmit_samples[0]));
	return capture->result;
}

static enum modem_core_result process_core_g711(
	void *context, enum g711_law law,
	const uint8_t *receive_codewords, int16_t *receive_samples,
	int16_t *transmit_samples, size_t count)
{
	struct core_capture *capture = context;

	capture->g711_calls++;
	capture->law = law;
	capture->codewords_matched =
		count == TEST_FRAME_SAMPLES &&
		memcmp(receive_codewords,
		       (uint8_t[]){ 0x7f, 0xff, 0x55, 0xd5 }, count) == 0;
	return process_core(
		context, receive_samples, transmit_samples, count);
}

struct recording_capture {
	enum recording_stream streams[TEST_RECORDINGS];
	int16_t samples[TEST_RECORDINGS][TEST_FRAME_SAMPLES];
	size_t calls;
};

static void capture_recording(
	void *context, const struct recording_block *block)
{
	struct recording_capture *capture = context;

	if (capture->calls < TEST_RECORDINGS &&
	    block->format == RECORD_S16_NATIVE &&
	    block->count == TEST_FRAME_SAMPLES) {
		capture->streams[capture->calls] = block->stream;
		memcpy(capture->samples[capture->calls], block->data,
		       sizeof(capture->samples[0]));
	}
	capture->calls++;
}

static bool test_processes_gained_recorded_frame(void)
{
	struct sip_session session = {
		.frame_samples = TEST_FRAME_SAMPLES,
		.receive_codewords = { 0x7f, 0xff, 0x55, 0xd5 },
		.receive_samples = { 1001, -1001, 20000, -20000 },
		.play_result = MEDIA_OK,
		.send_result = RTP_ENDPOINT_OK,
	};
	struct core_capture core = {
		.expected_receive = { 501, -501, 10000, -10000 },
		.transmit = { 1001, -1001, 20000, -20000 },
		.result = MODEM_CORE_OK,
	};
	struct recording_capture recordings = { 0 };
	const struct media_config config = {
		.receive_gain = 0.5,
		.transmit_gain = 2.0,
	};
	const struct media_recorder recorder = {
		.context = &recordings,
		.write = capture_recording,
	};
	const struct modem_core modem_core = {
		.context = &core,
		.process = process_core,
		.process_g711 = process_core_g711,
	};
	const int16_t expected_transmit[] = { 2002, -2002, 32767, -32768 };
	struct audio_path *path = audio_path_create(
		&session, &config, &recorder, &modem_core);

	CHECK(path != NULL && audio_path_frame_samples(path) ==
			      TEST_FRAME_SAMPLES,
	      "audio path creation failed");
	CHECK(audio_path_process_frame(path) ==
		      AUDIO_PATH_OK &&
		      session.play_calls == 1 && session.send_calls == 1 &&
		      core.calls == 1 && core.g711_calls == 1 &&
		      core.law == G711_LAW_PCMU && core.receive_matched &&
		      core.codewords_matched &&
		      memcmp(session.sent_samples, expected_transmit,
			     sizeof(expected_transmit)) == 0,
	      "audio path did not process or saturate the frame correctly");
	CHECK(recordings.calls == 2 &&
		      recordings.streams[0] == RECORD_RX_MODEM_PCM &&
		      recordings.streams[1] == RECORD_TX_MODEM_PCM &&
		      memcmp(recordings.samples[0], core.expected_receive,
			     sizeof(core.expected_receive)) == 0 &&
		      memcmp(recordings.samples[1], core.transmit,
			     sizeof(core.transmit)) == 0,
	      "audio path recordings do not bracket the modem core");
	CHECK(audio_path_last_media_result(path) ==
		      MEDIA_OK &&
		      audio_path_last_rtp_result(path) ==
			      RTP_ENDPOINT_OK,
	      "audio path did not retain typed results");
	audio_path_destroy(path);
	return true;
}

static bool test_stops_at_each_failed_boundary(void)
{
	struct sip_session session = {
		.frame_samples = TEST_FRAME_SAMPLES,
		.play_result = MEDIA_OUTPUT_TOO_SMALL,
		.send_result = RTP_ENDPOINT_NO_REMOTE,
	};
	struct core_capture core = {
		.result = MODEM_CORE_ERROR,
	};
	struct recording_capture recordings = { 0 };
	const struct media_config config = {
		.receive_gain = 1.0,
		.transmit_gain = 1.0,
	};
	const struct media_recorder recorder = {
		.context = &recordings,
		.write = capture_recording,
	};
	const struct modem_core modem_core = {
		.context = &core,
		.process = process_core,
	};
	struct audio_path *path = audio_path_create(
		&session, &config, &recorder, &modem_core);

	CHECK(path != NULL, "error-path audio creation failed");
	CHECK(audio_path_process_frame(path) ==
		      AUDIO_PATH_PLAYOUT_ERROR &&
		      core.calls == 0 && session.send_calls == 0 &&
		      recordings.calls == 0,
	      "playout failure crossed the modem boundary");
	session.play_result = MEDIA_OK;
	CHECK(audio_path_process_frame(path) ==
		      AUDIO_PATH_MODEM_ERROR &&
		      core.calls == 1 && session.send_calls == 0 &&
		      recordings.calls == 1,
	      "modem failure crossed the transmit boundary");
	core.result = MODEM_CORE_STOPPED;
	CHECK(audio_path_process_frame(path) ==
		      AUDIO_PATH_MODEM_STOPPED &&
		      session.send_calls == 0,
	      "stopped modem emitted a transmit frame");
	core.result = MODEM_CORE_OK;
	recordings.calls = 0;
	CHECK(audio_path_process_frame(path) ==
		      AUDIO_PATH_TRANSMIT_ERROR &&
		      session.send_calls == 1 && recordings.calls == 2 &&
		      audio_path_last_rtp_result(path) ==
			      RTP_ENDPOINT_NO_REMOTE,
	      "transmit failure was not retained at the RTP boundary");
	audio_path_destroy(path);
	return true;
}

static bool test_rejects_invalid_ownership_and_gain(void)
{
	struct sip_session session = {
		.frame_samples = TEST_FRAME_SAMPLES,
	};
	struct media_config config = {
		.receive_gain = NAN,
		.transmit_gain = 1.0,
	};
	struct modem_core modem_core = {
		.process = process_core,
	};

	CHECK(audio_path_create(&session, &config, NULL,
					   &modem_core) == NULL,
	      "audio path accepted a non-finite gain");
	config.receive_gain = 1.0;
	config.transmit_gain = 4.1;
	CHECK(audio_path_create(&session, &config, NULL,
					   &modem_core) == NULL,
	      "audio path accepted gain outside the configured range");
	config.transmit_gain = 1.0;
	config.codec = MEDIA_CODEC_CLEARMODE;
	CHECK(audio_path_create(&session, &config, NULL,
				   &modem_core) == NULL,
	      "audio path accepted a codec without G.711 codewords");
	config.codec = MEDIA_CODEC_PCMU;
	modem_core.process = NULL;
	CHECK(audio_path_create(&session, &config, NULL,
					   &modem_core) == NULL,
	      "audio path accepted a missing modem processor");
	CHECK(strcmp(audio_path_result_name(
			     AUDIO_PATH_MODEM_ERROR),
		     "modem core failed") == 0,
	      "audio path result name is wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "gained recorded frame", test_processes_gained_recorded_frame },
		{ "failed boundaries", test_stops_at_each_failed_boundary },
		{ "ownership and gain validation",
		  test_rejects_invalid_ownership_and_gain },
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
