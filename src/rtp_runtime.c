// SPDX-License-Identifier: BSD-3-Clause

#include "rtp_runtime.h"

#include "media.h"
#include "rtp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
#include <bcrypt.h>
#endif

enum {
	SAMPLES_PER_MILLISECOND = MEDIA_SAMPLE_RATE / 1000,
	REMOTE_CLOCK_BACKLOG_MILLISECONDS = 2000,
};

struct rtp_identity {
	uint16_t sequence;
	uint32_t timestamp;
	uint32_t ssrc;
};

struct rtp_runtime {
	struct rtp_jitter_slot *slots;
	uint8_t *receive_workspace;
	uint8_t *packet_workspace;
	size_t frame_samples;
	size_t remote_clock_frames;
	size_t packet_capacity;
	struct rtp_stream stream;
	struct rtp_endpoint endpoint;
	struct rtp_transmit_batch transmit_batch;
};

static bool fill_random(void *data, size_t size)
{
#ifdef _WIN32
	return size <= ULONG_MAX && BCryptGenRandom(NULL, data, (ULONG)size,
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	uint8_t *cursor = data;
	int descriptor;

	descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return false;
	while (size != 0) {
		ssize_t received = read(descriptor, cursor, size);

		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0) {
			close(descriptor);
			return false;
		}
		cursor += (size_t)received;
		size -= (size_t)received;
	}
	close(descriptor);
	return true;
#endif
}

static bool calculate_storage(const struct media_config *config,
			      size_t *frame_samples, size_t *slot_count)
{
	size_t frame;
	size_t prefill;
	size_t playout_window;

	if (config == NULL || config->frame_duration_ms == 0 ||
	    config->frame_duration_ms >
		    UINT_MAX / SAMPLES_PER_MILLISECOND ||
	    config->jitter_buffer_ms >
		    UINT_MAX / SAMPLES_PER_MILLISECOND)
		return false;
	frame = config->frame_duration_ms * SAMPLES_PER_MILLISECOND;
	prefill = config->jitter_buffer_ms * SAMPLES_PER_MILLISECOND;
	if (frame > RTP_ENDPOINT_MAX_DATAGRAM -
			    RTP_HEADER_SIZE ||
	    frame > (size_t)INT32_MAX /
			    (MEDIA_MAX_CATCHUP_FRAMES + 1))
		return false;
	/* The host drains the socket before processing overdue audio frames.
	 * Reserve those frames beyond the normal prefill window so an in-order
	 * burst is not rejected before the scheduler catches up. */
	playout_window = frame * (MEDIA_MAX_CATCHUP_FRAMES + 1);
	if (config->remote_clock) {
		size_t remote_backlog =
			REMOTE_CLOCK_BACKLOG_MILLISECONDS *
			SAMPLES_PER_MILLISECOND;

		/* A remote-clocked host consumes every accepted packet immediately,
		 * without adding this reserve to playout latency.  Keep enough future
		 * timestamps to survive an ordinary process-scheduling pause; otherwise
		 * the first packet beyond the small wall-clock catch-up window is rejected
		 * and can no longer provide the clock event needed to catch up. */
		if (playout_window < remote_backlog)
			playout_window = remote_backlog;
	}
	if (prefill > (size_t)INT32_MAX - playout_window)
		return false;
	*frame_samples = frame;
	*slot_count = prefill + playout_window;
	return true;
}

struct rtp_runtime *rtp_runtime_create(
	const struct media_config *config,
	const struct media_recorder *recorder)
{
	struct rtp_runtime *runtime;
	struct rtp_stream_config stream_config = { 0 };
	struct rtp_endpoint_config endpoint_config;
	struct rtp_identity identity;
	size_t frame_samples;
	size_t slot_count;
	int payload_type;

	if (!calculate_storage(config, &frame_samples, &slot_count))
		return NULL;
	payload_type = media_codec_default_payload_type(config->codec);
	if (payload_type < 0 || !fill_random(&identity, sizeof(identity)))
		return NULL;
	if (identity.ssrc == 0)
		identity.ssrc = 1;

	runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL)
		return NULL;
	runtime->endpoint.socket_fd = NET_INVALID_SOCKET;
	runtime->frame_samples = frame_samples;
	stream_config.prefill_samples =
		config->remote_clock ? 0 :
		config->jitter_buffer_ms * SAMPLES_PER_MILLISECOND;
	runtime->packet_capacity =
		RTP_HEADER_SIZE + runtime->frame_samples;
	runtime->slots = calloc(slot_count, sizeof(runtime->slots[0]));
	runtime->receive_workspace = malloc(runtime->frame_samples);
	runtime->packet_workspace = malloc(runtime->packet_capacity);
	if (runtime->slots == NULL || runtime->receive_workspace == NULL ||
	    runtime->packet_workspace == NULL)
		goto fail;

	stream_config.codec = config->codec;
	stream_config.payload_type = (uint8_t)payload_type;
	stream_config.transmit_sequence = identity.sequence;
	stream_config.transmit_timestamp = identity.timestamp;
	stream_config.transmit_ssrc = identity.ssrc;
	if (recorder != NULL)
		stream_config.recorder = *recorder;
	if (!rtp_stream_init(
		    &runtime->stream, &stream_config, runtime->slots, slot_count,
		    runtime->receive_workspace, runtime->frame_samples))
		goto fail;
	rtp_endpoint_config_init(&endpoint_config);
	if (!rtp_endpoint_init(
		    &runtime->endpoint, &runtime->stream, &endpoint_config))
		goto fail;
	return runtime;

fail:
	rtp_runtime_destroy(runtime);
	return NULL;
}

void rtp_runtime_destroy(
	struct rtp_runtime *runtime)
{
	if (runtime == NULL)
		return;
	rtp_endpoint_close(&runtime->endpoint);
	free(runtime->packet_workspace);
	free(runtime->receive_workspace);
	free(runtime->slots);
	free(runtime);
}

enum rtp_endpoint_result rtp_runtime_open(
	struct rtp_runtime *runtime,
	const struct sockaddr_in *local_address)
{
	if (runtime == NULL)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	return rtp_endpoint_open(&runtime->endpoint, local_address);
}

void rtp_runtime_close(struct rtp_runtime *runtime)
{
	if (runtime != NULL) {
		runtime->remote_clock_frames = 0;
		rtp_endpoint_close(&runtime->endpoint);
	}
}

size_t rtp_runtime_frame_samples(
	const struct rtp_runtime *runtime)
{
	return runtime != NULL ? runtime->frame_samples : 0;
}

net_socket rtp_runtime_fd(
	const struct rtp_runtime *runtime)
{
	return runtime != NULL ? rtp_endpoint_fd(&runtime->endpoint) : NET_INVALID_SOCKET;
}

uint16_t rtp_runtime_local_port(
	const struct rtp_runtime *runtime)
{
	return runtime != NULL ?
		       rtp_endpoint_local_port(&runtime->endpoint) :
		       0;
}

enum rtp_endpoint_result rtp_runtime_drain(
	struct rtp_runtime *runtime, size_t *datagrams_received)
{
	enum rtp_endpoint_result result;

	if (runtime == NULL)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	runtime->remote_clock_frames = 0;
	result = rtp_endpoint_drain(
		&runtime->endpoint, datagrams_received);
	if (result == RTP_ENDPOINT_OK)
		runtime->remote_clock_frames =
			rtp_receiver_take_clock_frames(
				&runtime->stream.receiver, runtime->frame_samples);
	return result;
}

size_t rtp_runtime_receive_tail_frames(const struct rtp_runtime *runtime)
{
	if (runtime == NULL || !runtime->stream.receiver.stream_active)
		return 0;
	const struct rtp_receiver *receiver = &runtime->stream.receiver;
	int32_t remaining = (int32_t)(receiver->buffered_timestamp -
				      receiver->play_timestamp);
	size_t samples = remaining > 0 ? (size_t)remaining : 0;
	samples += receiver->prefill_remaining;
	return samples / runtime->frame_samples +
	       (samples % runtime->frame_samples != 0);
}

size_t rtp_runtime_remote_clock_frames(
	const struct rtp_runtime *runtime)
{
	return runtime != NULL ? runtime->remote_clock_frames : 0;
}

struct rtp_playout_status rtp_runtime_playout_status(
	const struct rtp_runtime *runtime)
{
	return rtp_receiver_playout_status(
		runtime != NULL ? &runtime->stream.receiver : NULL,
		runtime != NULL ? runtime->frame_samples : 0);
}

bool rtp_runtime_pad_clock_frame(struct rtp_runtime *runtime)
{
	return runtime != NULL && rtp_receiver_pad_clock_frame(
		&runtime->stream.receiver, runtime->frame_samples);
}

enum media_result rtp_runtime_play(
	struct rtp_runtime *runtime, int16_t *samples,
	uint8_t *source_octets, bool *received_mask, size_t count,
	struct rtp_playout_report *report)
{
	if (runtime == NULL || count != runtime->frame_samples)
		return MEDIA_INVALID_ARGUMENT;
	return rtp_stream_play(&runtime->stream, samples, source_octets, received_mask, count,
					  report);
}

enum rtp_endpoint_result rtp_runtime_send(
	struct rtp_runtime *runtime, const int16_t *samples,
	size_t count)
{
	if (runtime == NULL || count != runtime->frame_samples)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	return rtp_endpoint_send(
		&runtime->endpoint, samples, count, runtime->packet_workspace,
		runtime->packet_capacity);
}

enum rtp_endpoint_result rtp_runtime_send_g711(
	struct rtp_runtime *runtime, const uint8_t *codewords,
	const int16_t *decoded_samples, size_t count)
{
	if (runtime == NULL || count != runtime->frame_samples)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	return rtp_endpoint_send_g711(
		&runtime->endpoint, codewords, decoded_samples, count,
		runtime->packet_workspace, runtime->packet_capacity);
}

struct rtp_endpoint *rtp_runtime_endpoint(
	struct rtp_runtime *runtime)
{
	return runtime != NULL ? &runtime->endpoint : NULL;
}

enum rtp_endpoint_result rtp_runtime_begin_transmit_batch(struct rtp_runtime *runtime)
{
	return runtime != NULL ? rtp_endpoint_begin_transmit_batch(
		&runtime->endpoint, &runtime->transmit_batch) : RTP_ENDPOINT_INVALID_ARGUMENT;
}

enum rtp_endpoint_result rtp_runtime_finish_transmit_batch(struct rtp_runtime *runtime)
{
	return runtime != NULL ? rtp_endpoint_finish_transmit_batch(&runtime->endpoint) :
		RTP_ENDPOINT_INVALID_ARGUMENT;
}
