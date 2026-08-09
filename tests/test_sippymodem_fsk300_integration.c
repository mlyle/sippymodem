// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>
#include <sippymodem_datapumps.h>
#include <sippymodem_media.h>
#include <sippymodem_fsk300_datapump.h>

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
	SIP_FRAME_SAMPLES = 160,
	ENDPOINT_OUTPUT_CAPACITY = 1024,
	TRAINING_FRAME_LIMIT = 250,
	DATA_FRAME_LIMIT = 400,
};

struct integration_endpoint {
	struct modem *modem;
	int modem_fd;
	int client_fd;
	int16_t receive[SIP_FRAME_SAMPLES];
	int16_t transmit[SIP_FRAME_SAMPLES];
	uint8_t output[ENDPOINT_OUTPUT_CAPACITY];
	size_t output_count;
};

struct recording_capture {
	size_t blocks[SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT + 1];
	size_t units[SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT + 1];
	bool invalid;
};

static int start_modem(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int stop_modem(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int control_modem(
	struct modem *modem, unsigned int command, unsigned long argument)
{
	(void)modem;
	(void)command;
	(void)argument;
	return 0;
}

static struct modem_driver integration_driver = {
	.name = "V.21 SIP media test",
	.start = start_modem,
	.stop = stop_modem,
	.ioctl = control_modem,
};

static bool endpoint_init(
	struct integration_endpoint *endpoint, const char *name)
{
	int sockets[2];
	int flags;

	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->modem_fd = -1;
	endpoint->client_fd = -1;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
		return false;
	flags = fcntl(sockets[1], F_GETFL);
	if (flags < 0 || fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK) != 0) {
		close(sockets[1]);
		close(sockets[0]);
		return false;
	}
	endpoint->modem = modem_create(&integration_driver, name);
	if (endpoint->modem == NULL) {
		close(sockets[1]);
		close(sockets[0]);
		return false;
	}
	endpoint->modem_fd = sockets[0];
	endpoint->client_fd = sockets[1];
	endpoint->modem->pty = endpoint->modem_fd;
	return true;
}

static void endpoint_destroy(struct integration_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	modem_delete(endpoint->modem);
	if (endpoint->client_fd >= 0)
		close(endpoint->client_fd);
	if (endpoint->modem_fd >= 0)
		close(endpoint->modem_fd);
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->modem_fd = -1;
	endpoint->client_fd = -1;
}

static bool endpoint_drain(struct integration_endpoint *endpoint)
{
	for (;;) {
		ssize_t count;
		size_t available = sizeof(endpoint->output) - endpoint->output_count;

		if (available == 0)
			return false;
		count = read(endpoint->client_fd,
			     endpoint->output + endpoint->output_count, available);
		if (count > 0) {
			endpoint->output_count += (size_t)count;
			continue;
		}
		if (count == 0)
			return true;
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}
}

static bool output_contains(
	const struct integration_endpoint *endpoint, const char *text)
{
	size_t text_length = strlen(text);

	if (text_length > endpoint->output_count)
		return false;
	for (size_t offset = 0;
	     offset <= endpoint->output_count - text_length; ++offset)
		if (memcmp(endpoint->output + offset, text, text_length) == 0)
			return true;
	return false;
}

static void capture_recording(
	void *context, const struct sippymodem_recording_block *block)
{
	struct recording_capture *capture = context;

	if (capture == NULL || block == NULL || block->data == NULL ||
	    block->count == 0 ||
	    block->stream < SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL ||
	    block->stream > SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT) {
		if (capture != NULL)
			capture->invalid = true;
		return;
	}
	capture->blocks[block->stream]++;
	capture->units[block->stream] += block->count;
}

static bool transcode(
	enum sippymodem_media_codec codec,
	const int16_t transmit[SIP_FRAME_SAMPLES],
	int16_t receive[SIP_FRAME_SAMPLES],
	const struct sippymodem_recorder *recorder)
{
	uint8_t payload[SIP_FRAME_SAMPLES];

	return sippymodem_media_encode(
		       codec, transmit, SIP_FRAME_SAMPLES,
		       payload, ARRAY_LENGTH(payload), recorder) ==
		       SIPPYMODEM_MEDIA_OK &&
	       sippymodem_media_decode(
		       codec, payload, ARRAY_LENGTH(payload), receive,
		       SIP_FRAME_SAMPLES, recorder) == SIPPYMODEM_MEDIA_OK;
}

static bool process_sip_frame(
	struct integration_endpoint *caller,
	struct integration_endpoint *answerer,
	enum sippymodem_media_codec codec,
	const struct sippymodem_recorder *recorder)
{
	modem_process(caller->modem, caller->receive, caller->transmit,
		      SIP_FRAME_SAMPLES);
	modem_process(answerer->modem, answerer->receive, answerer->transmit,
		      SIP_FRAME_SAMPLES);
	return transcode(
		       codec, caller->transmit, answerer->receive, recorder) &&
	       transcode(
		       codec, answerer->transmit, caller->receive, recorder);
}

static bool modem_is_online(const struct modem *modem)
{
	return modem != NULL && modem->started && modem->dp != NULL &&
	       modem->dp->id == DP_V21 && modem->dp->status == DPSTAT_CONNECT &&
	       modem->get_bits != NULL && modem->put_bits != NULL;
}

static bool run_media_call(enum sippymodem_media_codec codec)
{
	static const uint8_t caller_payload[] = {
		0x00, 0x01, 0x55, 0xaa, 0x7f, 0x80, 0xfe, 0xff,
		'V', '2', '1', 'A',
	};
	static const uint8_t answer_payload[] = {
		0xff, 0x81, 0x42, 0x24, 0x18, 0x00, 0x7e, 0xe7,
		'S', 'I', 'P', 'B',
	};
	static const char dial_command[] = "ATDpeer\r";
	struct sippymodem_datapumps datapumps = { 0 };
	struct integration_endpoint caller;
	struct integration_endpoint answerer;
	struct recording_capture capture = { 0 };
	const struct sippymodem_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	bool online = false;
	bool transferred = false;

	CHECK(sippymodem_datapumps_start(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_OK,
	      "V.21 integration registrations did not start");
	CHECK(endpoint_init(&caller, "V.21 caller") &&
		      endpoint_init(&answerer, "V.21 answerer"),
	      "V.21 integration endpoints did not initialize");
	CHECK(MODEM_DP(caller.modem) == DP_V21 &&
		      MODEM_DP(answerer.modem) == DP_V21 &&
		      MODEM_AUTOMODE(caller.modem) == 0 &&
		      MODEM_AUTOMODE(answerer.modem) == 0,
	      "source-built modem did not default to its available capability");
	CHECK(modem_set_sreg(answerer.modem, SREG_ANS_DELAY, 0) == 0 &&
		      modem_write(caller.modem, dial_command,
			  (int)(sizeof(dial_command) - 1)) ==
			      (int)(sizeof(dial_command) - 1) &&
		      caller.modem->dp != NULL &&
		      caller.modem->dp->id == DP_CALLPROG,
	      "outgoing SIP call did not enter call progress");
	CHECK(modem_report_outgoing_call_connected(caller.modem) == 0 &&
		      caller.modem->dp != NULL &&
		      caller.modem->dp->id == DP_V21 &&
		      modem_answer(answerer.modem) == 0,
	      "connected SIP call did not select V.21 roles");

	for (unsigned int frame = 0; frame < TRAINING_FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder),
		      "V.21 training failed at the SIP media boundary");
		if (modem_is_online(caller.modem) &&
		    modem_is_online(answerer.modem)) {
			online = true;
			break;
		}
	}
	CHECK(online && caller.modem->tx_rate == SIPPYMODEM_FSK300_BIT_RATE &&
		      caller.modem->rx_rate == SIPPYMODEM_FSK300_BIT_RATE &&
		      answerer.modem->tx_rate == SIPPYMODEM_FSK300_BIT_RATE &&
		      answerer.modem->rx_rate == SIPPYMODEM_FSK300_BIT_RATE,
	      "V.21 peers did not acquire carrier and report 300 bit/s");
	CHECK(endpoint_drain(&caller) && endpoint_drain(&answerer) &&
		      output_contains(&caller, "CONNECT 300") &&
		      output_contains(&answerer, "CONNECT 300"),
	      "V.21 connection did not reach both terminal interfaces");
	caller.output_count = 0;
	answerer.output_count = 0;

	CHECK(modem_write(caller.modem, (const char *)caller_payload,
			  (int)sizeof(caller_payload)) ==
		      (int)sizeof(caller_payload) &&
		      modem_write(answerer.modem, (const char *)answer_payload,
			  (int)sizeof(answer_payload)) ==
		      (int)sizeof(answer_payload),
	      "V.21 terminals did not accept full-duplex payloads");
	for (unsigned int frame = 0; frame < DATA_FRAME_LIMIT; ++frame) {
		CHECK(process_sip_frame(
		      &caller, &answerer, codec, &recorder) &&
			      endpoint_drain(&caller) && endpoint_drain(&answerer),
		      "V.21 data failed at the SIP media boundary");
		if (caller.output_count >= sizeof(answer_payload) &&
		    answerer.output_count >= sizeof(caller_payload)) {
			transferred = true;
			break;
		}
	}
	CHECK(transferred && caller.output_count == sizeof(answer_payload) &&
		      answerer.output_count == sizeof(caller_payload) &&
		      memcmp(caller.output, answer_payload,
			     sizeof(answer_payload)) == 0 &&
		      memcmp(answerer.output, caller_payload,
			     sizeof(caller_payload)) == 0,
	      "V.21 full-duplex terminal payload changed in transit");
	CHECK(!capture.invalid &&
		      capture.blocks[SIPPYMODEM_RECORD_TX_LINE_PCM] != 0 &&
		      capture.blocks[
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED] != 0 &&
		      capture.blocks[
			      SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_PLAYOUT] != 0 &&
		      capture.blocks[SIPPYMODEM_RECORD_RX_LINE_PCM] != 0,
	      "V.21 SIP call did not retain recording boundaries");
	CHECK(sippymodem_datapumps_stop(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_BUSY,
	      "live V.21 calls did not protect their process-global registration");
	memset(caller.receive, 0, sizeof(caller.receive));
	for (unsigned int frame = 0;
	     frame * SIP_FRAME_SAMPLES < 1600; ++frame)
		modem_process(caller.modem, caller.receive, caller.transmit,
			      SIP_FRAME_SAMPLES);
	CHECK(caller.modem->process == NULL,
	      "V.21 carrier loss did not stop the modem data path");

	endpoint_destroy(&answerer);
	endpoint_destroy(&caller);
	CHECK(sippymodem_datapumps_stop(&datapumps) ==
		      SIPPYMODEM_DATAPUMPS_OK,
	      "V.21 integration registrations remained busy");
	return true;
}

static bool test_full_duplex_sip_media(void)
{
	return run_media_call(SIPPYMODEM_CODEC_PCMU) &&
	       run_media_call(SIPPYMODEM_CODEC_PCMA);
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] full-duplex V.21 SIP media\n");
	if (!test_full_duplex_sip_media()) {
		fprintf(stderr, "[  FAILED  ] full-duplex V.21 SIP media\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] full-duplex V.21 SIP media\n");
	return 0;
}
