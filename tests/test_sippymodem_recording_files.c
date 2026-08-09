// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_recording_files.h>

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	TEST_STREAM_COUNT = SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT + 1,
};

struct recording_fixture {
	char directory[PATH_MAX];
	char prefix[PATH_MAX];
	char paths[TEST_STREAM_COUNT][PATH_MAX];
	struct sippymodem_recording_files *files;
};

static void fixture_close(struct recording_fixture *fixture)
{
	sippymodem_recording_files_destroy(fixture->files);
	fixture->files = NULL;
	for (size_t index = 0; index < TEST_STREAM_COUNT; ++index) {
		if (fixture->paths[index][0] != '\0')
			unlink(fixture->paths[index]);
	}
	if (fixture->directory[0] != '\0')
		rmdir(fixture->directory);
}

static bool fixture_open(struct recording_fixture *fixture)
{
	char template[] = "/tmp/sippymodem-recording-XXXXXX";
	char *directory;

	memset(fixture, 0, sizeof(*fixture));
	directory = mkdtemp(template);
	if (directory == NULL)
		return false;
	if (snprintf(fixture->directory, sizeof(fixture->directory), "%s",
		     directory) >= (int)sizeof(fixture->directory)) {
		rmdir(directory);
		return false;
	}
	if (snprintf(fixture->prefix, sizeof(fixture->prefix), "%s/call",
		     directory) >= (int)sizeof(fixture->prefix))
		goto fail;
	fixture->files = sippymodem_recording_files_create(fixture->prefix);
	if (fixture->files == NULL)
		goto fail;
	for (size_t index = 0; index < TEST_STREAM_COUNT; ++index) {
		if (!sippymodem_recording_files_path(
			    fixture->files,
			    (enum sippymodem_recording_stream)index,
			    fixture->paths[index],
			    sizeof(fixture->paths[index])))
			goto fail;
	}
	return true;

fail:
	fixture_close(fixture);
	return false;
}

static bool file_is_private_and_empty(const char *path)
{
	struct stat status;

	return stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
	       (status.st_mode & 0777) == 0600 && status.st_size == 0;
}

static bool read_exact_file(const char *path, void *data, size_t size)
{
	uint8_t *cursor = data;
	size_t received = 0;
	int descriptor = open(path, O_RDONLY | O_CLOEXEC);

	if (descriptor < 0)
		return false;
	while (received < size) {
		ssize_t result = read(descriptor, cursor + received,
				      size - received);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			goto fail;
		received += (size_t)result;
	}
	if (read(descriptor, cursor, 1) != 0)
		goto fail;
	close(descriptor);
	return true;

fail:
	close(descriptor);
	return false;
}

static bool test_creates_one_private_file_per_evidence_stream(void)
{
	struct recording_fixture fixture;
	struct sippymodem_recording_files *duplicate;
	bool passed = true;

	CHECK(fixture_open(&fixture), "could not create recording fixture");
	CHECK(strstr(fixture.paths[SIPPYMODEM_RECORD_RX_LINE_PCM],
		     ".rx.line_pcm.8000hz.s16le") != NULL &&
		      strstr(fixture.paths[
			     SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT],
			     ".tx.rtp_payload.sent.8000hz.octets") != NULL,
	      "recording paths do not identify their evidence format");
	errno = 0;
	duplicate = sippymodem_recording_files_create(fixture.prefix);
	CHECK(duplicate == NULL && errno == EEXIST,
	      "recording owner would overwrite an existing capture");
	sippymodem_recording_files_destroy(fixture.files);
	fixture.files = NULL;
	for (size_t index = 0; index < TEST_STREAM_COUNT; ++index)
		passed &= file_is_private_and_empty(fixture.paths[index]);
	fixture_close(&fixture);
	CHECK(passed, "recording file set is incomplete or not private");
	return true;
}

static bool test_validates_prefix_and_paths(void)
{
	char oversized[SIPPYMODEM_RECORDING_PREFIX_MAX + 1];
	char path[8];
	struct recording_fixture fixture;
	struct sippymodem_recording_files_stats stats = { 1, 1 };
	bool passed;

	memset(oversized, 'x', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	errno = 0;
	CHECK(sippymodem_recording_files_create(NULL) == NULL &&
		      errno == EINVAL,
	      "recording owner accepted a null prefix");
	errno = 0;
	CHECK(sippymodem_recording_files_create("") == NULL && errno == EINVAL,
	      "recording owner accepted an empty prefix");
	errno = 0;
	CHECK(sippymodem_recording_files_create(oversized) == NULL &&
		      errno == ENAMETOOLONG,
	      "recording owner accepted an oversized prefix");
	CHECK(fixture_open(&fixture), "could not create path validation fixture");
	passed = !sippymodem_recording_files_path(
			 fixture.files, (enum sippymodem_recording_stream)99,
			 path, sizeof(path)) &&
		 !sippymodem_recording_files_path(
			 fixture.files, SIPPYMODEM_RECORD_RX_LINE_PCM,
			 path, sizeof(path)) &&
		 !sippymodem_recording_files_path(
			 NULL, SIPPYMODEM_RECORD_RX_LINE_PCM,
			 oversized, sizeof(oversized));
	fixture_close(&fixture);
	sippymodem_recording_files_get_stats(NULL, &stats);
	passed = passed && stats.blocks_written == 0 && stats.bytes_written == 0 &&
		 sippymodem_recording_files_recorder(NULL) == NULL &&
		 sippymodem_recording_files_last_result(NULL) ==
			 SIPPYMODEM_RECORDING_FILES_INVALID_BLOCK &&
		 sippymodem_recording_files_system_error(NULL) == EINVAL &&
		 strcmp(sippymodem_recording_files_result_name(
				SIPPYMODEM_RECORDING_FILES_WRITE_ERROR),
			"recording write failed") == 0;
	CHECK(passed, "recording owner accepted an invalid output path");
	return true;
}

static bool test_writes_bounded_canonical_evidence(void)
{
	static const uint8_t payload[] = { 0xff, 0x7f, 0x12 };
	static const int16_t samples[] = { 0x1234, -2, INT16_MIN, INT16_MAX };
	static const uint8_t expected_pcm[] = {
		0x34, 0x12, 0xfe, 0xff, 0x00, 0x80, 0xff, 0x7f,
	};
	struct recording_fixture fixture;
	const struct sippymodem_recorder *recorder;
	struct sippymodem_recording_files_stats stats;
	uint8_t saved_payload[sizeof(payload)];
	uint8_t saved_pcm[sizeof(expected_pcm)];
	bool passed;

	CHECK(fixture_open(&fixture), "could not create evidence fixture");
	recorder = sippymodem_recording_files_recorder(fixture.files);
	CHECK(recorder != NULL &&
		      sippymodem_recording_emit(
			      recorder,
			      SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL,
			      SIPPYMODEM_RECORD_OCTETS,
			      SIPPYMODEM_MEDIA_SAMPLE_RATE, payload,
			      ARRAY_LENGTH(payload)) &&
		      sippymodem_recording_emit(
			      recorder, SIPPYMODEM_RECORD_TX_MODEM_PCM,
			      SIPPYMODEM_RECORD_S16_NATIVE,
			      SIPPYMODEM_MEDIA_SAMPLE_RATE, samples,
			      ARRAY_LENGTH(samples)),
	      "valid evidence blocks were rejected");
	sippymodem_recording_files_get_stats(fixture.files, &stats);
	passed = sippymodem_recording_files_last_result(fixture.files) ==
			 SIPPYMODEM_RECORDING_FILES_OK &&
		 stats.blocks_written == 2 &&
		 stats.bytes_written == sizeof(payload) + sizeof(expected_pcm);
	sippymodem_recording_files_destroy(fixture.files);
	fixture.files = NULL;
	passed = passed &&
		 read_exact_file(
			 fixture.paths[SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL],
			 saved_payload, sizeof(saved_payload)) &&
		 read_exact_file(fixture.paths[SIPPYMODEM_RECORD_TX_MODEM_PCM],
				 saved_pcm, sizeof(saved_pcm)) &&
		 memcmp(saved_payload, payload, sizeof(payload)) == 0 &&
		 memcmp(saved_pcm, expected_pcm, sizeof(expected_pcm)) == 0;
	fixture_close(&fixture);
	CHECK(passed, "recorded evidence is incomplete or noncanonical");
	return true;
}

static bool test_latches_ambiguous_evidence(void)
{
	struct recording_fixture fixture;
	const struct sippymodem_recorder *recorder;
	const uint8_t octet = 0;
	struct sippymodem_recording_files_stats stats;
	bool passed;

	CHECK(fixture_open(&fixture), "could not create metadata fixture");
	recorder = sippymodem_recording_files_recorder(fixture.files);
	recorder->write(recorder->context,
			&(const struct sippymodem_recording_block){
				.stream = SIPPYMODEM_RECORD_RX_LINE_PCM,
				.format = SIPPYMODEM_RECORD_OCTETS,
				.sample_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE,
				.data = &octet,
				.count = 1,
			});
	CHECK(sippymodem_recording_emit(
		      recorder, SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL,
		      SIPPYMODEM_RECORD_OCTETS, SIPPYMODEM_MEDIA_SAMPLE_RATE,
		      &octet, 1),
	      "valid block was rejected after a latched recording failure");
	sippymodem_recording_files_get_stats(fixture.files, &stats);
	passed = sippymodem_recording_files_last_result(fixture.files) ==
			 SIPPYMODEM_RECORDING_FILES_INVALID_BLOCK &&
		 sippymodem_recording_files_system_error(fixture.files) == 0 &&
		 stats.blocks_written == 0 && stats.bytes_written == 0;
	fixture_close(&fixture);
	CHECK(passed, "recording sink accepted ambiguous evidence");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "private evidence files",
		  test_creates_one_private_file_per_evidence_stream },
		{ "prefix and path validation", test_validates_prefix_and_paths },
		{ "canonical evidence", test_writes_bounded_canonical_evidence },
		{ "ambiguous evidence", test_latches_ambiguous_evidence },
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
