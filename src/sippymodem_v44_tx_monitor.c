// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_tx_monitor.h"

#include <stddef.h>
#include <stdlib.h>

struct sippymodem_v44_tx_monitor {
	struct sippymodem_v44_parameters parameters;
	struct sippymodem_v44_tx_monitor_config config;
	struct sippymodem_v44_encoder *shadow;
	struct sippymodem_v44_encoder_progress baseline;
	struct sippymodem_v44_tx_monitor_sample last_sample;
	enum sippymodem_v44_encoder_mode mode;
	enum sippymodem_v44_tx_monitor_decision pending_decision;
	bool has_last_sample;
	bool disabled;
};

static bool mode_is_valid(enum sippymodem_v44_encoder_mode mode)
{
	return mode == SIPPYMODEM_V44_ENCODER_COMPRESSED ||
	       mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT;
}

static bool config_is_valid(
	const struct sippymodem_v44_parameters *parameters,
	const struct sippymodem_v44_tx_monitor_config *config)
{
	return sippymodem_v44_parameters_are_valid(parameters) &&
	       config != NULL &&
	       config->window_octets >= parameters->max_string_length;
}

static bool monitor_is_valid(
	const struct sippymodem_v44_tx_monitor *monitor)
{
	return monitor != NULL &&
	       config_is_valid(&monitor->parameters, &monitor->config) &&
	       mode_is_valid(monitor->mode) &&
	       monitor->baseline.mode == SIPPYMODEM_V44_ENCODER_COMPRESSED &&
	       monitor->pending_decision >= SIPPYMODEM_V44_TX_MONITOR_STAY &&
	       monitor->pending_decision <=
		       SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT &&
	       (monitor->disabled == (monitor->shadow == NULL));
}

struct sippymodem_v44_tx_monitor_config
sippymodem_v44_tx_monitor_default_config(void)
{
	const struct sippymodem_v44_tx_monitor_config config = {
		.window_octets =
			SIPPYMODEM_V44_TX_MONITOR_DEFAULT_WINDOW_OCTETS,
		.leave_compressed_excess_bits =
			SIPPYMODEM_V44_TX_MONITOR_DEFAULT_LEAVE_EXCESS_BITS,
		.enter_compressed_savings_bits =
			SIPPYMODEM_V44_TX_MONITOR_DEFAULT_ENTER_SAVINGS_BITS,
	};

	return config;
}

struct sippymodem_v44_tx_monitor *sippymodem_v44_tx_monitor_create(
	const struct sippymodem_v44_parameters *parameters,
	const struct sippymodem_v44_tx_monitor_config *config)
{
	struct sippymodem_v44_tx_monitor *monitor;

	if (!config_is_valid(parameters, config))
		return NULL;
	monitor = calloc(1, sizeof(*monitor));
	if (monitor == NULL)
		return NULL;
	monitor->shadow = sippymodem_v44_encoder_create(parameters, 2);
	if (monitor->shadow == NULL) {
		free(monitor);
		return NULL;
	}
	monitor->parameters = *parameters;
	monitor->config = *config;
	monitor->mode = SIPPYMODEM_V44_ENCODER_COMPRESSED;
	monitor->baseline.mode = SIPPYMODEM_V44_ENCODER_COMPRESSED;
	return monitor;
}

void sippymodem_v44_tx_monitor_destroy(
	struct sippymodem_v44_tx_monitor *monitor)
{
	if (monitor == NULL)
		return;
	sippymodem_v44_encoder_destroy(monitor->shadow);
	free(monitor);
}

static enum sippymodem_v44_tx_monitor_result disable_monitor(
	struct sippymodem_v44_tx_monitor *monitor)
{
	sippymodem_v44_encoder_destroy(monitor->shadow);
	monitor->shadow = NULL;
	monitor->pending_decision = SIPPYMODEM_V44_TX_MONITOR_STAY;
	monitor->disabled = true;
	return SIPPYMODEM_V44_TX_MONITOR_DISABLED;
}

static bool drain_shadow(struct sippymodem_v44_tx_monitor *monitor)
{
	uint8_t discarded[2];

	while (sippymodem_v44_encoder_pending(monitor->shadow) != 0) {
		if (sippymodem_v44_encoder_read(
			    monitor->shadow, discarded, sizeof(discarded)) == 0)
			return false;
	}
	return true;
}

static bool drive_octet(
	struct sippymodem_v44_tx_monitor *monitor, uint8_t octet)
{
	const uint8_t *input = &octet;
	size_t input_length = 1;

	for (;;) {
		enum sippymodem_v44_encode_result result =
			sippymodem_v44_encoder_process(
				monitor->shadow, &input, &input_length);

		if (result == SIPPYMODEM_V44_ENCODE_NEED_INPUT)
			return input_length == 0 &&
			       sippymodem_v44_encoder_pending(
				       monitor->shadow) == 0;
		if (result != SIPPYMODEM_V44_ENCODE_OK &&
		    result != SIPPYMODEM_V44_ENCODE_NEED_OUTPUT)
			return false;
		if (result == SIPPYMODEM_V44_ENCODE_NEED_OUTPUT &&
		    sippymodem_v44_encoder_pending(monitor->shadow) == 0)
			return false;
		if (!drain_shadow(monitor))
			return false;
	}
}

static bool drive_flush(struct sippymodem_v44_tx_monitor *monitor)
{
	for (;;) {
		enum sippymodem_v44_encode_result result =
			sippymodem_v44_encoder_flush(monitor->shadow);

		if (result == SIPPYMODEM_V44_ENCODE_FLUSHED)
			return sippymodem_v44_encoder_pending(
				       monitor->shadow) == 0;
		if (result != SIPPYMODEM_V44_ENCODE_NEED_OUTPUT ||
		    !drain_shadow(monitor))
			return false;
	}
}

static bool drive_mode(
	struct sippymodem_v44_tx_monitor *monitor,
	enum sippymodem_v44_encoder_mode mode)
{
	for (;;) {
		enum sippymodem_v44_encode_result result =
			sippymodem_v44_encoder_request_mode(
				monitor->shadow, mode);

		if (result == SIPPYMODEM_V44_ENCODE_OK)
			return sippymodem_v44_encoder_pending(
				       monitor->shadow) == 0;
		if (result != SIPPYMODEM_V44_ENCODE_NEED_OUTPUT ||
		    !drain_shadow(monitor))
			return false;
	}
}

static enum sippymodem_v44_tx_monitor_result evaluate_sample(
	struct sippymodem_v44_tx_monitor *monitor,
	enum sippymodem_v44_tx_monitor_decision *decision)
{
	struct sippymodem_v44_encoder_progress progress;
	uint64_t represented_octets;
	uint64_t compressed_bits;
	uint64_t raw_bits;

	if (!sippymodem_v44_encoder_get_progress(
		    monitor->shadow, &progress) ||
	    progress.mode != SIPPYMODEM_V44_ENCODER_COMPRESSED ||
	    progress.represented_octets < monitor->baseline.represented_octets ||
	    progress.wire_bits < monitor->baseline.wire_bits)
		return disable_monitor(monitor);
	represented_octets =
		progress.represented_octets - monitor->baseline.represented_octets;
	if (represented_octets < monitor->config.window_octets)
		return SIPPYMODEM_V44_TX_MONITOR_OK;
	if (represented_octets > UINT64_MAX / 8U)
		return disable_monitor(monitor);
	compressed_bits = progress.wire_bits - monitor->baseline.wire_bits;
	raw_bits = represented_octets * 8U;
	if (monitor->mode == SIPPYMODEM_V44_ENCODER_COMPRESSED &&
	    compressed_bits > raw_bits &&
	    compressed_bits - raw_bits >
		    monitor->config.leave_compressed_excess_bits)
		*decision = SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT;
	else if (monitor->mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT &&
		 raw_bits > compressed_bits &&
		 raw_bits - compressed_bits >
			 monitor->config.enter_compressed_savings_bits)
		*decision = SIPPYMODEM_V44_TX_MONITOR_ENTER_COMPRESSED;
	monitor->last_sample = (struct sippymodem_v44_tx_monitor_sample) {
		.mode = monitor->mode,
		.represented_octets = represented_octets,
		.compressed_bits = compressed_bits,
		.decision = *decision,
	};
	monitor->has_last_sample = true;
	monitor->baseline = progress;
	monitor->pending_decision = *decision;
	return SIPPYMODEM_V44_TX_MONITOR_OK;
}

enum sippymodem_v44_tx_monitor_result sippymodem_v44_tx_monitor_observe(
	struct sippymodem_v44_tx_monitor *monitor,
	uint8_t octet,
	enum sippymodem_v44_tx_monitor_decision *decision)
{
	if (!monitor_is_valid(monitor) || decision == NULL)
		return SIPPYMODEM_V44_TX_MONITOR_INVALID_ARGUMENT;
	*decision = SIPPYMODEM_V44_TX_MONITOR_STAY;
	if (monitor->disabled)
		return SIPPYMODEM_V44_TX_MONITOR_DISABLED;
	if (monitor->pending_decision != SIPPYMODEM_V44_TX_MONITOR_STAY)
		return SIPPYMODEM_V44_TX_MONITOR_INVALID_STATE;
	if (!drive_octet(monitor, octet))
		return disable_monitor(monitor);
	return evaluate_sample(monitor, decision);
}

enum sippymodem_v44_tx_monitor_result sippymodem_v44_tx_monitor_flush(
	struct sippymodem_v44_tx_monitor *monitor,
	enum sippymodem_v44_tx_monitor_decision *decision)
{
	if (!monitor_is_valid(monitor) || decision == NULL)
		return SIPPYMODEM_V44_TX_MONITOR_INVALID_ARGUMENT;
	*decision = SIPPYMODEM_V44_TX_MONITOR_STAY;
	if (monitor->disabled)
		return SIPPYMODEM_V44_TX_MONITOR_DISABLED;
	if (monitor->mode != SIPPYMODEM_V44_ENCODER_COMPRESSED ||
	    monitor->pending_decision != SIPPYMODEM_V44_TX_MONITOR_STAY)
		return SIPPYMODEM_V44_TX_MONITOR_INVALID_STATE;
	if (!drive_flush(monitor))
		return disable_monitor(monitor);
	return evaluate_sample(monitor, decision);
}

enum sippymodem_v44_tx_monitor_result
sippymodem_v44_tx_monitor_confirm_mode(
	struct sippymodem_v44_tx_monitor *monitor,
	enum sippymodem_v44_encoder_mode mode)
{
	struct sippymodem_v44_encoder_progress progress;
	enum sippymodem_v44_tx_monitor_decision expected;
	bool transitioned;

	if (!monitor_is_valid(monitor) || !mode_is_valid(mode))
		return SIPPYMODEM_V44_TX_MONITOR_INVALID_ARGUMENT;
	if (monitor->disabled)
		return SIPPYMODEM_V44_TX_MONITOR_DISABLED;
	expected = mode == SIPPYMODEM_V44_ENCODER_COMPRESSED ?
			   SIPPYMODEM_V44_TX_MONITOR_ENTER_COMPRESSED :
			   SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT;
	if (monitor->pending_decision != SIPPYMODEM_V44_TX_MONITOR_STAY &&
	    monitor->pending_decision != expected)
		return SIPPYMODEM_V44_TX_MONITOR_INVALID_STATE;
	if (mode == monitor->mode) {
		if (monitor->pending_decision != SIPPYMODEM_V44_TX_MONITOR_STAY)
			return SIPPYMODEM_V44_TX_MONITOR_INVALID_STATE;
		return SIPPYMODEM_V44_TX_MONITOR_OK;
	}
	if (mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT) {
		transitioned = drive_flush(monitor);
	} else {
		transitioned = drive_mode(
			monitor, SIPPYMODEM_V44_ENCODER_TRANSPARENT);
		if (transitioned)
			transitioned = drive_mode(
				monitor, SIPPYMODEM_V44_ENCODER_COMPRESSED);
	}
	if (!transitioned || !sippymodem_v44_encoder_get_progress(
				      monitor->shadow, &progress) ||
	    progress.mode != SIPPYMODEM_V44_ENCODER_COMPRESSED)
		return disable_monitor(monitor);
	monitor->mode = mode;
	monitor->baseline = progress;
	monitor->pending_decision = SIPPYMODEM_V44_TX_MONITOR_STAY;
	return SIPPYMODEM_V44_TX_MONITOR_OK;
}

bool sippymodem_v44_tx_monitor_get_last_sample(
	const struct sippymodem_v44_tx_monitor *monitor,
	struct sippymodem_v44_tx_monitor_sample *sample)
{
	if (!monitor_is_valid(monitor) || sample == NULL ||
	    !monitor->has_last_sample)
		return false;
	*sample = monitor->last_sample;
	return true;
}

const char *sippymodem_v44_tx_monitor_result_name(
	enum sippymodem_v44_tx_monitor_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_TX_MONITOR_OK:
		return "ok";
	case SIPPYMODEM_V44_TX_MONITOR_DISABLED:
		return "disabled";
	case SIPPYMODEM_V44_TX_MONITOR_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V44_TX_MONITOR_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
