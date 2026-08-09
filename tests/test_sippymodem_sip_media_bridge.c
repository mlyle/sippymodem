// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_sip_media_bridge.h>

#include "test.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOTS = 16,
	TEST_WORKSPACE = 8,
};

struct bridge_fixture {
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOTS];
	struct sippymodem_rtp_stream stream;
	struct sippymodem_rtp_endpoint endpoint;
	struct sippymodem_sip_media_bridge bridge;
	uint8_t workspace[TEST_WORKSPACE];
};

struct callback_capture {
	struct sippymodem_sip_media_bridge *bridge;
	unsigned int ready;
	unsigned int error;
	unsigned int trace;
	unsigned int registration;
	unsigned int call;
	unsigned int media;
	bool observed_callback_depth;
	enum sippymodem_sip_rtp_result observed_binding;
};

static void capture_ready(void *context)
{
	struct callback_capture *capture = context;

	capture->ready++;
	capture->observed_callback_depth |=
		sippymodem_sip_media_bridge_is_in_callback(capture->bridge);
}

#define DEFINE_CAPTURE(function_name, member, record_type)                   \
	static void function_name(void *context, const record_type *record)    \
	{                                                                      \
		struct callback_capture *capture = context;                       \
	                                                                       \
		(void)record;                                                   \
		capture->member++;                                              \
		capture->observed_callback_depth |=                              \
			sippymodem_sip_media_bridge_is_in_callback(               \
				capture->bridge);                                  \
	}

DEFINE_CAPTURE(capture_error, error,
	       struct sippymodem_sip_signaling_error)
DEFINE_CAPTURE(capture_trace, trace,
	       struct sippymodem_sip_trace_record)
DEFINE_CAPTURE(capture_registration, registration,
	       struct sippymodem_sip_registration_event)
DEFINE_CAPTURE(capture_call, call, struct sippymodem_sip_call_event)

#undef DEFINE_CAPTURE

static void capture_media(
	void *context, const struct sippymodem_sip_media_event *event)
{
	struct callback_capture *capture = context;

	(void)event;
	capture->media++;
	capture->observed_binding =
		sippymodem_sip_media_bridge_last_binding(capture->bridge);
	capture->observed_callback_depth |=
		sippymodem_sip_media_bridge_is_in_callback(capture->bridge);
}

static bool open_fixture(struct bridge_fixture *fixture,
			 struct callback_capture *capture)
{
	const struct sippymodem_rtp_stream_config stream_config = {
		.codec = SIPPYMODEM_CODEC_PCMU,
		.payload_type = 0,
	};
	const struct sippymodem_rtp_endpoint_config endpoint_config = {
		.learn_remote = true,
		.symmetric_rtp = true,
	};
	const struct sockaddr_in local = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	const struct sippymodem_sip_signaling_callbacks destination = {
		.context = capture,
		.ready = capture_ready,
		.error = capture_error,
		.trace = capture_trace,
		.registration_changed = capture_registration,
		.call_changed = capture_call,
		.media_changed = capture_media,
	};

	memset(fixture, 0, sizeof(*fixture));
	return sippymodem_rtp_stream_init(
		       &fixture->stream, &stream_config, fixture->slots,
		       ARRAY_LENGTH(fixture->slots), fixture->workspace,
		       sizeof(fixture->workspace)) &&
	       sippymodem_rtp_endpoint_init(
		       &fixture->endpoint, &fixture->stream, &endpoint_config) &&
	       sippymodem_rtp_endpoint_open(&fixture->endpoint, &local) ==
		       SIPPYMODEM_RTP_ENDPOINT_OK &&
	       sippymodem_sip_media_bridge_init(
		       &fixture->bridge, &fixture->endpoint, &destination);
}

static bool remote_is(const struct bridge_fixture *fixture,
		      unsigned int port)
{
	struct sockaddr_in remote;
	bool learned = true;

	return sippymodem_rtp_endpoint_transmit_remote(
		       &fixture->endpoint, &remote, &learned) &&
	       !learned && remote.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
	       remote.sin_port == htons((uint16_t)port);
}

static bool test_forwards_and_binds_before_media_callback(void)
{
	struct bridge_fixture fixture;
	struct callback_capture capture = { 0 };
	const struct sippymodem_sip_signaling_callbacks *source;
	const struct sippymodem_sip_signaling_error error = { 0 };
	const struct sippymodem_sip_trace_record trace = { 0 };
	const struct sippymodem_sip_registration_event registration = { 0 };
	const struct sippymodem_sip_call_event call = { 0 };
	struct sippymodem_sip_media_event media = {
		.state = SIPPYMODEM_SIP_MEDIA_CONFIGURED,
		.remote_address_family = SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4,
		.codec = SIPPYMODEM_CODEC_PCMU,
		.payload_type = 0,
		.sample_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE,
		.channels = 1,
		.remote_address = "127.0.0.1",
		.remote_port = 42000,
	};

	CHECK(open_fixture(&fixture, &capture), "could not open bridge fixture");
	capture.bridge = &fixture.bridge;
	media.local_port =
		sippymodem_rtp_endpoint_local_port(&fixture.endpoint);
	source = sippymodem_sip_media_bridge_callbacks(&fixture.bridge);
	source->ready(source->context);
	source->error(source->context, &error);
	source->trace(source->context, &trace);
	source->registration_changed(source->context, &registration);
	source->call_changed(source->context, &call);
	source->media_changed(source->context, &media);
	CHECK(capture.ready == 1 && capture.error == 1 && capture.trace == 1 &&
		      capture.registration == 1 && capture.call == 1 &&
		      capture.media == 1 && capture.observed_callback_depth,
	      "bridge did not forward every signaling callback");
	CHECK(capture.observed_binding == SIPPYMODEM_SIP_RTP_OK &&
		      remote_is(&fixture, 42000) &&
		      sippymodem_sip_media_bridge_is_configured(
			      &fixture.bridge) &&
		      sippymodem_sip_media_bridge_binding_failures(
			      &fixture.bridge) == 0,
	      "bridge did not bind RTP before forwarding media");

	media.payload_type = 8;
	media.remote_port = 43000;
	source->media_changed(source->context, &media);
	CHECK(capture.observed_binding ==
		      SIPPYMODEM_SIP_RTP_INCOMPATIBLE_MEDIA &&
		      remote_is(&fixture, 42000) &&
		      sippymodem_sip_media_bridge_is_configured(
			      &fixture.bridge) &&
		      sippymodem_sip_media_bridge_binding_failures(
			      &fixture.bridge) == 1,
	      "failed media update changed the established peer");
	sippymodem_sip_media_bridge_clear_media(&fixture.bridge);
	CHECK(!remote_is(&fixture, 42000) &&
		      !sippymodem_sip_media_bridge_is_configured(
			      &fixture.bridge),
	      "explicit media reset retained negotiated state");
	media.payload_type = 0;
	media.remote_port = 42000;
	source->media_changed(source->context, &media);
	CHECK(remote_is(&fixture, 42000) &&
		      sippymodem_sip_media_bridge_is_configured(&fixture.bridge),
	      "bridge did not rebind after an explicit reset");
	media.state = SIPPYMODEM_SIP_MEDIA_INACTIVE;
	source->media_changed(source->context, &media);
	CHECK(sippymodem_sip_media_bridge_last_binding(&fixture.bridge) ==
		      SIPPYMODEM_SIP_RTP_OK &&
		      !remote_is(&fixture, 42000) &&
		      !sippymodem_sip_media_bridge_is_configured(
			      &fixture.bridge),
	      "inactive media did not clear the established peer");
	sippymodem_rtp_endpoint_close(&fixture.endpoint);
	return true;
}

static bool test_validates_initialization(void)
{
	struct sippymodem_sip_media_bridge bridge;
	struct sippymodem_rtp_endpoint endpoint = { 0 };

	CHECK(!sippymodem_sip_media_bridge_init(NULL, &endpoint, NULL) &&
		      !sippymodem_sip_media_bridge_init(&bridge, NULL, NULL) &&
		      !sippymodem_sip_media_bridge_init(&bridge, &endpoint, NULL),
	      "bridge accepted incomplete ownership");
	CHECK(sippymodem_sip_media_bridge_callbacks(NULL) == NULL &&
		      sippymodem_sip_media_bridge_last_binding(NULL) ==
			      SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT &&
		      !sippymodem_sip_media_bridge_is_in_callback(NULL),
	      "bridge null accessors are inconsistent");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "forward and bind", test_forwards_and_binds_before_media_callback },
		{ "initialization validation", test_validates_initialization },
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
