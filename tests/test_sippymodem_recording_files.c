// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_recording_files.h>

#include "test.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
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
	CHECK(passed, "recording owner accepted an invalid output path");
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
