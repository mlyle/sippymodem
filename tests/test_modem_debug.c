// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define CONFIG_DEBUG_MODEM 1
#include <modem_debug.h>

struct capture {
	FILE *file;
	int saved_stderr;
};

int __wrap_gettimeofday(struct timeval *now, void *zone)
{
	(void)zone;
	*now = (struct timeval){ 12, 345678 };
	return 0;
}

static bool capture_start(struct capture *capture)
{
	capture->file = tmpfile();
	if (capture->file == NULL)
		return false;
	capture->saved_stderr = dup(STDERR_FILENO);
	if (capture->saved_stderr >= 0 && fflush(stderr) == 0 &&
	    dup2(fileno(capture->file), STDERR_FILENO) >= 0)
		return true;
	if (capture->saved_stderr >= 0)
		close(capture->saved_stderr);
	fclose(capture->file);
	return false;
}

/* Restore stderr before returning, including on capture errors. */
static long capture_finish(struct capture *capture, char *output, size_t size)
{
	bool failed = fflush(stderr) != 0;
	long length;

	failed |= dup2(capture->saved_stderr, STDERR_FILENO) < 0;
	close(capture->saved_stderr);
	clearerr(stderr);
	length = ftell(capture->file);
	if (length < 0 || (size_t)length >= size ||
	    fseek(capture->file, 0, SEEK_SET) != 0 ||
	    fread(output, 1, (size_t)length, capture->file) != (size_t)length)
		failed = true;
	fclose(capture->file);
	if (failed)
		return -1;
	output[length] = '\0';
	return length;
}

static bool test_disabled_output(void)
{
	struct capture capture;
	char output[64];
	int result;

	CHECK(capture_start(&capture), "could not capture stderr");
	modem_debug_level = 0;
	result = modem_debug_printf("disabled message\n");
	long length = capture_finish(&capture, output, sizeof(output));
	CHECK(result == 0 && length == 0, "disabled diagnostics produced output");
	return true;
}

static bool test_complete_output(void)
{
	struct capture capture;
	char message[2049], output[4096];
	const char prefix[] = "<012.345678> ";
	int result;

	memset(message, 'x', sizeof(message) - 1);
	message[sizeof(message) - 1] = '\0';
	CHECK(capture_start(&capture), "could not capture stderr");
	modem_debug_level = 1;
	result = modem_debug_printf("%s\n", message);
	long length = capture_finish(&capture, output, sizeof(output));
	CHECK(result == 2049 && length == 2049 + (long)strlen(prefix),
	      "long diagnostic was truncated or miscounted");
	CHECK(memcmp(output, prefix, strlen(prefix)) == 0 &&
	      memcmp(output + strlen(prefix), message, 2048) == 0 &&
	      output[length - 1] == '\n', "diagnostic contents changed");
	return true;
}

static bool test_output_error(void)
{
	int descriptors[2], result;
	int saved_stderr = dup(STDERR_FILENO);

	CHECK(saved_stderr >= 0, "could not save stderr");
	if (pipe(descriptors) != 0) {
		close(saved_stderr);
		CHECK(false, "could not create read-only output fixture");
	}
	bool redirected = dup2(descriptors[0], STDERR_FILENO) >= 0;
	close(descriptors[0]);
	close(descriptors[1]);
	modem_debug_level = 1;
	result = redirected ? modem_debug_printf("cannot write\n") : 0;
	bool restored = dup2(saved_stderr, STDERR_FILENO) >= 0;
	close(saved_stderr);
	clearerr(stderr);
	CHECK(redirected && restored && result < 0,
	      "unwritable diagnostics reported successful output");
	return true;
}

enum { WRITERS = 4, MESSAGES = 32, PAYLOAD = 1024 };

static void *write_messages(void *context)
{
	const unsigned id = *(unsigned *)context;
	char payload[PAYLOAD + 1];

	memset(payload, 'a' + id, PAYLOAD);
	payload[PAYLOAD] = '\0';
	for (unsigned sequence = 0; sequence < MESSAGES; sequence++)
		(void)modem_debug_printf("%u:%u:%s\n", id, sequence, payload);
	return NULL;
}

static bool test_concurrent_output(void)
{
	struct capture capture;
	pthread_t writers[WRITERS];
	unsigned ids[WRITERS], started = 0;
	bool seen[WRITERS][MESSAGES] = { { false } };
	char output[WRITERS * MESSAGES * (PAYLOAD + 64)];
	bool joined = true;

	CHECK(capture_start(&capture), "could not capture stderr");
	modem_debug_level = 1;
	for (; started < WRITERS; started++) {
		ids[started] = started;
		if (pthread_create(&writers[started], NULL, write_messages,
				   &ids[started]) != 0)
			break;
	}
	for (unsigned i = 0; i < started; i++)
		joined &= pthread_join(writers[i], NULL) == 0;
	long length = capture_finish(&capture, output, sizeof(output));
	CHECK(started == WRITERS && joined && length >= 0,
	      "concurrent diagnostic fixture failed");
	const char *next = output;
	for (unsigned i = 0; i < WRITERS * MESSAGES; i++) {
		unsigned id, sequence;
		int prefix = 0;
		CHECK(sscanf(next, "<012.345678> %u:%u:%n", &id, &sequence,
			     &prefix) == 2 && prefix > 0 &&
		      id < WRITERS && sequence < MESSAGES && !seen[id][sequence],
		      "concurrent diagnostics lost, duplicated, or mixed a message");
		CHECK(output + length - next >= prefix + PAYLOAD + 1,
		      "concurrent diagnostic was truncated");
		next += prefix;
		for (unsigned j = 0; j < PAYLOAD; j++)
			CHECK(next[j] == (char)('a' + id), "message payload was overwritten");
		CHECK(next[PAYLOAD] == '\n', "message boundary was overwritten");
		next += PAYLOAD + 1;
		seen[id][sequence] = true;
	}
	CHECK(next == output + length, "unexpected concurrent output");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "disabled modem diagnostics", test_disabled_output },
		{ "complete modem diagnostics", test_complete_output },
		{ "modem diagnostic output error", test_output_error },
		{ "concurrent modem diagnostics", test_concurrent_output },
	};
	unsigned failures = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(tests); i++) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		if (tests[i].run())
			fprintf(stderr, "[       OK ] %s\n", tests[i].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[i].name);
			failures++;
		}
	}
	return failures != 0;
}
