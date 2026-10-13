// SPDX-License-Identifier: BSD-3-Clause
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <recording_files.h>

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
	TEST_STREAM_COUNT = RECORD_RTP_PACKETS + 1,
};

static pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_ready = PTHREAD_COND_INITIALIZER;
static bool block_write, entered_write, producer_done, fail_write, fail_thread;

ssize_t __real_write(int descriptor, const void *data, size_t size);
ssize_t __wrap_write(int descriptor, const void *data, size_t size)
{
	pthread_mutex_lock(&write_mutex);
	entered_write = true;
	pthread_cond_broadcast(&write_ready);
	while (block_write)
		pthread_cond_wait(&write_ready, &write_mutex);
	bool fail = fail_write;
	pthread_mutex_unlock(&write_mutex);
	if (fail) {
		errno = ENOSPC;
		return -1;
	}
	return __real_write(descriptor, data, size);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
	void *(*run)(void *), void *context)
{
	return fail_thread ? EAGAIN : __real_pthread_create(thread, attributes, run, context);
}

static bool wait_flag(bool *flag)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	while (!*flag)
		if (pthread_cond_timedwait(&write_ready, &write_mutex, &deadline) != 0)
			return false;
	return true;
}

struct recording_fixture {
	char directory[PATH_MAX];
	char prefix[PATH_MAX];
	char paths[TEST_STREAM_COUNT][PATH_MAX];
	struct recording_files *files;
};

static void fixture_close(struct recording_fixture *fixture)
{
	recording_files_destroy(fixture->files);
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
	char template[] = "/tmp/modem-recording-XXXXXX";
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
	fixture->files = recording_files_create(fixture->prefix);
	if (fixture->files == NULL)
		goto fail;
	for (size_t index = 0; index < TEST_STREAM_COUNT; ++index) {
		if (!recording_files_path(
			    fixture->files,
			    (enum recording_stream)index,
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
	struct recording_files *duplicate;
	bool passed = true;

	CHECK(fixture_open(&fixture), "could not create recording fixture");
	CHECK(strstr(fixture.paths[RECORD_RX_LINE_PCM],
		     ".rx.line_pcm.8000hz.s16le") != NULL &&
		      strstr(fixture.paths[
			     RECORD_TX_RTP_PAYLOAD_SENT],
			     ".tx.rtp_payload.sent.8000hz.octets") != NULL &&
		      strstr(fixture.paths[RECORD_RTP_PACKETS], ".rtp.packets.tsv") != NULL,
	      "recording paths do not identify their evidence format");
	errno = 0;
	duplicate = recording_files_create(fixture.prefix);
	CHECK(duplicate == NULL && errno == EEXIST,
	      "recording owner would overwrite an existing capture");
	recording_files_destroy(fixture.files);
	fixture.files = NULL;
	for (size_t index = 0; index < TEST_STREAM_COUNT; ++index)
		passed &= file_is_private_and_empty(fixture.paths[index]);
	fixture_close(&fixture);
	CHECK(passed, "recording file set is incomplete or not private");
	return true;
}

static bool test_validates_prefix_and_paths(void)
{
	char oversized[RECORDING_PREFIX_MAX + 1];
	char path[8];
	struct recording_fixture fixture;
	struct recording_files_stats stats = { 1, 1 };
	bool passed;

	memset(oversized, 'x', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	errno = 0;
	CHECK(recording_files_create(NULL) == NULL &&
		      errno == EINVAL,
	      "recording owner accepted a null prefix");
	errno = 0;
	CHECK(recording_files_create("") == NULL && errno == EINVAL,
	      "recording owner accepted an empty prefix");
	errno = 0;
	CHECK(recording_files_create(oversized) == NULL &&
		      errno == ENAMETOOLONG,
	      "recording owner accepted an oversized prefix");
	CHECK(fixture_open(&fixture), "could not create path validation fixture");
	passed = !recording_files_path(
			 fixture.files, (enum recording_stream)99,
			 path, sizeof(path)) &&
		 !recording_files_path(
			 fixture.files, RECORD_RX_LINE_PCM,
			 path, sizeof(path)) &&
		 !recording_files_path(
			 NULL, RECORD_RX_LINE_PCM,
			 oversized, sizeof(oversized));
	fixture_close(&fixture);
	recording_files_get_stats(NULL, &stats);
	passed = passed && stats.blocks_written == 0 && stats.bytes_written == 0 &&
		 recording_files_recorder(NULL) == NULL &&
		 recording_files_last_result(NULL) ==
			 RECORDING_FILES_INVALID_BLOCK &&
		 recording_files_system_error(NULL) == EINVAL &&
		 strcmp(recording_files_result_name(
				RECORDING_FILES_WRITE_ERROR),
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
	const struct media_recorder *recorder;
	struct recording_files_stats stats;
	uint8_t saved_payload[sizeof(payload)];
	uint8_t saved_pcm[sizeof(expected_pcm)];
	bool passed;

	CHECK(fixture_open(&fixture), "could not create evidence fixture");
	recorder = recording_files_recorder(fixture.files);
	CHECK(recorder != NULL &&
		      recording_emit(
			      recorder,
			      RECORD_RX_RTP_PAYLOAD_ARRIVAL,
			      RECORD_OCTETS,
			      MEDIA_SAMPLE_RATE, payload,
			      ARRAY_LENGTH(payload)) &&
		      recording_emit(
			      recorder, RECORD_TX_MODEM_PCM,
			      RECORD_S16_NATIVE,
			      MEDIA_SAMPLE_RATE, samples,
			      ARRAY_LENGTH(samples)),
	      "valid evidence blocks were rejected");
	CHECK(recording_files_flush(fixture.files) == RECORDING_FILES_OK,
	      "could not finish canonical evidence");
	recording_files_get_stats(fixture.files, &stats);
	passed = recording_files_last_result(fixture.files) ==
			 RECORDING_FILES_OK &&
		 stats.blocks_written == 2 &&
		 stats.bytes_written == sizeof(payload) + sizeof(expected_pcm);
	recording_files_destroy(fixture.files);
	fixture.files = NULL;
	passed = passed &&
		 read_exact_file(
			 fixture.paths[RECORD_RX_RTP_PAYLOAD_ARRIVAL],
			 saved_payload, sizeof(saved_payload)) &&
		 read_exact_file(fixture.paths[RECORD_TX_MODEM_PCM],
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
	const struct media_recorder *recorder;
	const uint8_t octet = 0;
	struct recording_files_stats stats;
	bool passed;

	CHECK(fixture_open(&fixture), "could not create metadata fixture");
	recorder = recording_files_recorder(fixture.files);
	recorder->write(recorder->context,
			&(const struct recording_block){
				.stream = RECORD_RX_LINE_PCM,
				.format = RECORD_OCTETS,
				.sample_rate = MEDIA_SAMPLE_RATE,
				.data = &octet,
				.count = 1,
			});
	CHECK(recording_emit(
		      recorder, RECORD_RX_RTP_PAYLOAD_ARRIVAL,
		      RECORD_OCTETS, MEDIA_SAMPLE_RATE,
		      &octet, 1),
	      "valid block was rejected after a latched recording failure");
	recording_files_get_stats(fixture.files, &stats);
	passed = recording_files_last_result(fixture.files) ==
			 RECORDING_FILES_INVALID_BLOCK &&
		 recording_files_system_error(fixture.files) == 0 &&
		 stats.blocks_written == 0 && stats.bytes_written == 0;
	fixture_close(&fixture);
	CHECK(passed, "recording sink accepted ambiguous evidence");
	return true;
}

static bool test_packet_journal(void)
{
	static const char expected[] =
		"sent\t18446744073709551615\t65535\t4294967295\t4294967295\t127\t1\t160\t18446744073709551615\t42\n"
		"late\t0\t0\t0\t4294967295\t127\t1\t160\t18446744073709551615\t42\n";
	struct recording_fixture fixture;
	struct recording_files_stats stats;
	struct recording_packet packet = {
		.event = RECORD_PACKET_SENT, .monotonic_ns = UINT64_MAX,
		.sequence = UINT16_MAX, .timestamp = UINT32_MAX, .ssrc = UINT32_MAX,
		.payload_type = 127, .marker = true, .payload_octets = 160,
		.playout_samples = UINT64_MAX, .concealed_samples = 42,
	};
	const struct media_recorder *recorder;
	char actual[sizeof(expected) - 1];
	bool passed;

	CHECK(fixture_open(&fixture), "could not create packet journal fixture");
	recorder = recording_files_recorder(fixture.files);
	CHECK(recorder->packet != NULL, "packet journal callback is missing");
	recorder->packet(recorder->context, &packet);
	packet.event = RECORD_PACKET_LATE;
	packet.monotonic_ns = 0;
	packet.sequence = 0;
	packet.timestamp = 0;
	recorder->packet(recorder->context, &packet);
	CHECK(recording_files_flush(fixture.files) == RECORDING_FILES_OK,
	      "could not finish packet journal");
	recording_files_get_stats(fixture.files, &stats);
	passed = read_exact_file(fixture.paths[RECORD_RTP_PACKETS], actual, sizeof(actual)) &&
		memcmp(actual, expected, sizeof(actual)) == 0 &&
		stats.blocks_written == 2 && stats.bytes_written == sizeof(actual);
	packet.event = (enum recording_packet_event)99;
	recorder->packet(recorder->context, &packet);
	packet.event = RECORD_PACKET_SENT;
	recorder->packet(recorder->context, &packet);
	passed &= recording_files_last_result(fixture.files) == RECORDING_FILES_INVALID_BLOCK &&
		read_exact_file(fixture.paths[RECORD_RTP_PACKETS], actual, sizeof(actual));
	fixture_close(&fixture);
	CHECK(passed, "packet journal changed fields or ignored a latched error");
	return true;
}

static bool test_packet_journal_collision(void)
{
	struct recording_fixture fixture;
	bool passed = true;

	CHECK(fixture_open(&fixture), "could not create packet collision fixture");
	recording_files_destroy(fixture.files);
	fixture.files = NULL;
	/* Leave only the last file, so creation must unwind the earlier files. */
	for (size_t i = 0; i < RECORD_RTP_PACKETS; ++i)
		CHECK(unlink(fixture.paths[i]) == 0, "could not prepare journal collision");
	errno = 0;
	fixture.files = recording_files_create(fixture.prefix);
	passed = fixture.files == NULL && errno == EEXIST &&
		file_is_private_and_empty(fixture.paths[RECORD_RTP_PACKETS]);
	for (size_t i = 0; i < RECORD_RTP_PACKETS; ++i)
		passed &= access(fixture.paths[i], F_OK) < 0 && errno == ENOENT;
	fixture_close(&fixture);
	CHECK(passed, "journal collision left partial files or removed existing evidence");
	return true;
}

struct producer {
	const struct media_recorder *recorder;
	uint8_t bytes[8];
};

static void *produce(void *context)
{
	struct producer *producer = context;
	(void)recording_emit(producer->recorder, RECORD_RX_RTP_PAYLOAD_ARRIVAL,
		RECORD_OCTETS, MEDIA_SAMPLE_RATE, producer->bytes, sizeof(producer->bytes));
	pthread_mutex_lock(&write_mutex);
	producer_done = true;
	pthread_cond_broadcast(&write_ready);
	pthread_mutex_unlock(&write_mutex);
	return NULL;
}

static bool test_stalled_writer_keeps_producer_bounded(void)
{
	struct recording_fixture fixture;
	struct recording_files_stats stats;
	uint8_t block[32 * 1024];
	pthread_t thread;
	CHECK(fixture_open(&fixture), "could not create stalled writer fixture");
	struct producer producer = { .recorder = recording_files_recorder(fixture.files),
		.bytes = { 1, 2, 3, 4, 5, 6, 7, 8 } };
	pthread_mutex_lock(&write_mutex);
	block_write = true;
	entered_write = producer_done = false;
	pthread_mutex_unlock(&write_mutex);
	CHECK(pthread_create(&thread, NULL, produce, &producer) == 0,
	      "could not start recording producer");
	pthread_mutex_lock(&write_mutex);
	bool entered = wait_flag(&entered_write);
	bool returned = entered && wait_flag(&producer_done);
	pthread_mutex_unlock(&write_mutex);
	unsigned accepted = 0;
	memset(block, 0xa5, sizeof(block));
	if (returned) {
		memset(producer.bytes, 0, sizeof(producer.bytes));
		for (unsigned i = 0; i < 20; ++i) {
			(void)recording_emit(producer.recorder, RECORD_RX_RTP_PAYLOAD_ARRIVAL,
				RECORD_OCTETS, MEDIA_SAMPLE_RATE, block, sizeof(block));
			if (recording_files_last_result(fixture.files) != RECORDING_FILES_OK)
				break;
			accepted++;
		}
	}
	pthread_mutex_lock(&write_mutex);
	block_write = false;
	pthread_cond_broadcast(&write_ready);
	pthread_mutex_unlock(&write_mutex);
	pthread_join(thread, NULL);
	bool overflow = recording_files_flush(fixture.files) == RECORDING_FILES_QUEUE_FULL &&
		recording_files_system_error(fixture.files) == 0;
	recording_files_get_stats(fixture.files, &stats);
	recording_files_destroy(fixture.files);
	fixture.files = NULL;
	size_t size = 8 + accepted * sizeof(block);
	uint8_t *saved = malloc(size);
	bool exact = saved != NULL && read_exact_file(
		fixture.paths[RECORD_RX_RTP_PAYLOAD_ARRIVAL], saved, size);
	for (size_t i = 0; exact && i < size; ++i)
		exact = saved[i] == (i < 8 ? i + 1 : 0xa5);
	free(saved);
	fixture_close(&fixture);
	CHECK(entered && returned, "media producer waited behind a stalled file write");
	CHECK(accepted > 0 && accepted < 20 && overflow,
	      "recording queue failed to report bounded overflow");
	CHECK(exact && stats.blocks_written == accepted + 1 && stats.bytes_written == size,
	      "queued evidence retained borrowed memory or lost accepted blocks");
	return true;
}

static bool test_writer_failure_and_startup_cleanup(void)
{
	struct recording_fixture fixture;
	CHECK(fixture_open(&fixture), "could not create writer failure fixture");
	const struct media_recorder *recorder = recording_files_recorder(fixture.files);
	const uint8_t octet = 1;
	pthread_mutex_lock(&write_mutex);
	fail_write = true;
	pthread_mutex_unlock(&write_mutex);
	(void)recording_emit(recorder, RECORD_TX_RTP_PAYLOAD_SENT, RECORD_OCTETS,
		MEDIA_SAMPLE_RATE, &octet, 1);
	bool failed = recording_files_flush(fixture.files) == RECORDING_FILES_WRITE_ERROR &&
		recording_files_system_error(fixture.files) == ENOSPC;
	pthread_mutex_lock(&write_mutex);
	fail_write = false;
	pthread_mutex_unlock(&write_mutex);
	(void)recording_emit(recorder, RECORD_TX_RTP_PAYLOAD_SENT, RECORD_OCTETS,
		MEDIA_SAMPLE_RATE, &octet, 1);
	recording_files_destroy(fixture.files);
	fixture.files = NULL;
	bool empty = file_is_private_and_empty(fixture.paths[RECORD_TX_RTP_PAYLOAD_SENT]);
	for (size_t i = 0; i < TEST_STREAM_COUNT; ++i)
		unlink(fixture.paths[i]);
	fail_thread = true;
	errno = 0;
	fixture.files = recording_files_create(fixture.prefix);
	bool unwound = fixture.files == NULL && errno == EAGAIN;
	fail_thread = false;
	for (size_t i = 0; i < TEST_STREAM_COUNT; ++i)
		unwound &= access(fixture.paths[i], F_OK) < 0 && errno == ENOENT;
	fixture_close(&fixture);
	CHECK(failed && empty, "writer failure was hidden or recording resumed with a hole");
	CHECK(unwound, "failed writer startup left evidence files behind");
	return true;
}

static bool test_queue_wrap_and_destroy_drain(void)
{
	struct recording_fixture fixture;
	uint8_t block[8191], saved[64 * sizeof(block)];
	CHECK(fixture_open(&fixture), "could not create wrapped queue fixture");
	const struct media_recorder *recorder = recording_files_recorder(fixture.files);
	for (unsigned i = 0; i < 64; ++i) {
		memset(block, i, sizeof(block));
		(void)recording_emit(recorder, RECORD_TX_RTP_PAYLOAD_SENT,
			RECORD_OCTETS, MEDIA_SAMPLE_RATE, block, sizeof(block));
		/* The last block is drained only by destroy. Odd sizes wrap both
		 * metadata and payload independently across the byte ring. */
		if (i != 63)
			CHECK(recording_files_flush(fixture.files) == RECORDING_FILES_OK,
			      "wrapped queue failed to drain");
	}
	recording_files_destroy(fixture.files);
	fixture.files = NULL;
	bool exact = read_exact_file(fixture.paths[RECORD_TX_RTP_PAYLOAD_SENT], saved, sizeof(saved));
	for (size_t i = 0; exact && i < sizeof(saved); ++i)
		exact = saved[i] == i / sizeof(block);
	fixture_close(&fixture);
	CHECK(exact, "wrapped queue or destroy lost/reordered copied evidence");
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
		{ "packet journal", test_packet_journal },
		{ "packet journal collision", test_packet_journal_collision },
		{ "stalled writer and queue bounds", test_stalled_writer_keeps_producer_bounded },
		{ "writer failure and cleanup", test_writer_failure_and_startup_cleanup },
		{ "queue wrap and destroy drain", test_queue_wrap_and_destroy_drain },
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
