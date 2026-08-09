// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_sdp.h>

#include "test.h"

#include <string.h>

static bool expect_attribute(
	const struct sippymodem_sdp_audio_description *description,
	enum sippymodem_sdp_audio_attribute attribute, const char *expected)
{
	char output[64];
	enum sippymodem_sdp_result result;

	result = sippymodem_sdp_audio_format_attribute(
		description, attribute, output, sizeof(output));
	CHECK(result == SIPPYMODEM_SDP_OK, "%s failed: %s",
	      sippymodem_sdp_audio_attribute_name(attribute),
	      sippymodem_sdp_result_name(result));
	CHECK(strcmp(output, expected) == 0, "%s was '%s', expected '%s'",
	      sippymodem_sdp_audio_attribute_name(attribute), output, expected);
	return true;
}

static bool test_pcmu_description(void)
{
	struct sippymodem_sdp_audio_description description;

	CHECK(sippymodem_sdp_audio_description_init(
		      &description, SIPPYMODEM_CODEC_PCMU, 4000, 20, true) ==
		      SIPPYMODEM_SDP_OK,
	      "could not initialize PCMU description");
	CHECK(description.codec == SIPPYMODEM_CODEC_PCMU &&
		      description.payload_type == 0 &&
		      description.rtp_port == 4000 && description.rtcp_port == 4000 &&
		      description.sample_rate == 8000 && description.channels == 1,
	      "PCMU description metadata is wrong");
	CHECK(strcmp(sippymodem_sdp_audio_encoding_name(&description), "PCMU") ==
		      0,
	      "PCMU encoding name is wrong");
	return expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_RTPMAP,
				"0 PCMU/8000") &&
	       expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_PTIME, "20") &&
	       expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_MAXPTIME,
				"20") &&
	       expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_RTCP, "4000") &&
	       expect_attribute(&description,
				SIPPYMODEM_SDP_AUDIO_SILENCE_SUPPRESSION,
				"off - - - -") &&
	       expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_GPMD,
				"0 vbd=yes");
}

static bool test_other_codecs(void)
{
	struct sippymodem_sdp_audio_description description;

	CHECK(sippymodem_sdp_audio_description_init(
		      &description, SIPPYMODEM_CODEC_PCMA, 4002, 10, true) ==
		      SIPPYMODEM_SDP_OK,
	      "could not initialize PCMA description");
	CHECK(expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_RTPMAP,
			       "8 PCMA/8000"),
	      "PCMA rtpmap is wrong");
	CHECK(sippymodem_sdp_audio_description_init(
		      &description, SIPPYMODEM_CODEC_CLEARMODE, 4004, 10, true) ==
		      SIPPYMODEM_SDP_OK,
	      "could not initialize CLEARMODE description");
	return expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_RTPMAP,
				"96 CLEARMODE/8000") &&
	       expect_attribute(&description, SIPPYMODEM_SDP_AUDIO_GPMD,
				"96 vbd=yes");
}

static bool test_optional_attributes_and_bounds(void)
{
	struct sippymodem_sdp_audio_description description;
	struct sippymodem_sdp_audio_description original;
	char output[4] = "old";
	enum sippymodem_sdp_result result;

	CHECK(sippymodem_sdp_audio_description_init(
		      &description, SIPPYMODEM_CODEC_PCMU, 4000, 10, false) ==
		      SIPPYMODEM_SDP_OK,
	      "could not initialize description");
	original = description;
	result = sippymodem_sdp_audio_format_attribute(
		&description, SIPPYMODEM_SDP_AUDIO_GPMD, output, sizeof(output));
	CHECK(result == SIPPYMODEM_SDP_NOT_PRESENT && output[0] == '\0',
	      "disabled VBD attribute was formatted");
	memcpy(output, "old", sizeof(output));
	result = sippymodem_sdp_audio_format_attribute(
		&description, SIPPYMODEM_SDP_AUDIO_RTPMAP, output, sizeof(output));
	CHECK(result == SIPPYMODEM_SDP_OUTPUT_TOO_SMALL && output[0] == '\0',
	      "short attribute output did not fail safely");
	CHECK(sippymodem_sdp_audio_description_init(
		      &description, (enum sippymodem_media_codec)99, 4000, 10,
		      true) == SIPPYMODEM_SDP_INVALID_ARGUMENT,
	      "unknown codec was accepted");
	CHECK(memcmp(&description, &original, sizeof(description)) == 0,
	      "failed initialization changed the description");
	CHECK(sippymodem_sdp_audio_description_init(
		      &description, SIPPYMODEM_CODEC_PCMU, 0, 10, true) ==
		      SIPPYMODEM_SDP_INVALID_ARGUMENT,
	      "zero RTP port was accepted");
	CHECK(sippymodem_sdp_audio_description_init(
		      &description, SIPPYMODEM_CODEC_PCMU, 4000,
		      SIPPYMODEM_SDP_MAX_PACKET_DURATION_MS + 1, true) ==
		      SIPPYMODEM_SDP_INVALID_ARGUMENT,
	      "excessive packet duration was accepted");
	return true;
}

static bool test_remote_format_acceptance(void)
{
	struct sippymodem_sdp_audio_description pcmu;
	struct sippymodem_sdp_audio_description clear;
	struct sippymodem_sdp_audio_format format = {
		.payload_type = 0,
		.encoding_name = "pcmu",
		.sample_rate = 8000,
		.channels = 1,
	};

	CHECK(sippymodem_sdp_audio_description_init(
		      &pcmu, SIPPYMODEM_CODEC_PCMU, 4000, 10, true) ==
		      SIPPYMODEM_SDP_OK &&
		      sippymodem_sdp_audio_description_init(
			      &clear, SIPPYMODEM_CODEC_CLEARMODE, 4000, 10,
			      true) == SIPPYMODEM_SDP_OK,
	      "could not initialize format tests");
	CHECK(sippymodem_sdp_audio_accepts_format(&pcmu, &format),
	      "matching explicit PCMU format was rejected");
	format.encoding_name = NULL;
	format.sample_rate = 0;
	format.channels = 0;
	CHECK(sippymodem_sdp_audio_accepts_format(&pcmu, &format),
	      "static PCMU payload without rtpmap was rejected");
	format.payload_type = 8;
	CHECK(!sippymodem_sdp_audio_accepts_format(&pcmu, &format),
	      "wrong static payload was accepted");
	format.payload_type = 96;
	CHECK(!sippymodem_sdp_audio_accepts_format(&clear, &format),
	      "dynamic payload without rtpmap was accepted");
	format.encoding_name = "CLEARMODE";
	format.sample_rate = 8000;
	format.channels = 0;
	CHECK(sippymodem_sdp_audio_accepts_format(&clear, &format),
	      "matching CLEARMODE format was rejected");
	format.sample_rate = 16000;
	CHECK(!sippymodem_sdp_audio_accepts_format(&clear, &format),
	      "wrong media clock was accepted");
	format.sample_rate = 8000;
	format.channels = 2;
	CHECK(!sippymodem_sdp_audio_accepts_format(&clear, &format),
	      "stereo modem media was accepted");
	format.channels = 1;
	format.encoding_name = "telephone-event";
	CHECK(!sippymodem_sdp_audio_accepts_format(&clear, &format),
	      "wrong encoding was accepted");
	return true;
}

static bool test_owned_attributes(void)
{
	CHECK(sippymodem_sdp_attribute_is_media_owned("RTPMAP") &&
		      sippymodem_sdp_attribute_is_media_owned("silenceSupp") &&
		      sippymodem_sdp_attribute_is_media_owned("ssrc-group"),
	      "owned SDP attributes were missed");
	CHECK(!sippymodem_sdp_attribute_is_media_owned("sendrecv") &&
		      !sippymodem_sdp_attribute_is_media_owned(NULL),
	      "session-owned SDP attribute was claimed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "PCMU description", test_pcmu_description },
		{ "other codecs", test_other_codecs },
		{ "optional attributes and bounds",
		  test_optional_attributes_and_bounds },
		{ "remote format acceptance", test_remote_format_acceptance },
		{ "owned attributes", test_owned_attributes },
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
