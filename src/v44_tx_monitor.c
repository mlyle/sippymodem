// SPDX-License-Identifier: BSD-3-Clause

#include "v44_tx_monitor.h"

#include <stddef.h>
#include <stdlib.h>

struct v44_tx_monitor {
	struct v44_parameters parameters;
	struct v44_tx_monitor_config config;
	struct v44_encoder *shadow;
	struct v44_encoder_progress baseline;
	struct v44_tx_monitor_sample last_sample;
	enum v44_encoder_mode mode;
	enum v44_tx_monitor_decision pending_decision;
	bool has_last_sample;
	bool disabled;
};

static bool mode_is_valid(enum v44_encoder_mode mode)
{
	return mode == V44_ENCODER_COMPRESSED ||
	       mode == V44_ENCODER_TRANSPARENT;
}

static bool config_is_valid(
	const struct v44_parameters *parameters,
	const struct v44_tx_monitor_config *config)
{
	return v44_parameters_are_valid(parameters) &&
	       config != NULL &&
	       config->window_octets >= parameters->max_string_length;
}

static bool monitor_is_valid(
	const struct v44_tx_monitor *monitor)
{
	return monitor != NULL &&
	       config_is_valid(&monitor->parameters, &monitor->config) &&
	       mode_is_valid(monitor->mode) &&
	       monitor->baseline.mode == V44_ENCODER_COMPRESSED &&
	       monitor->pending_decision >= V44_TX_MONITOR_STAY &&
	       monitor->pending_decision <=
		       V44_TX_MONITOR_ENTER_TRANSPARENT &&
	       (monitor->disabled == (monitor->shadow == NULL));
}

struct v44_tx_monitor_config
v44_tx_monitor_default_config(void)
{
	const struct v44_tx_monitor_config config = {
		.window_octets =
			V44_TX_MONITOR_DEFAULT_WINDOW_OCTETS,
		.leave_compressed_excess_bits =
			V44_TX_MONITOR_DEFAULT_LEAVE_EXCESS_BITS,
		.enter_compressed_savings_bits =
			V44_TX_MONITOR_DEFAULT_ENTER_SAVINGS_BITS,
	};

	return config;
}

struct v44_tx_monitor *v44_tx_monitor_create(
	const struct v44_parameters *parameters,
	const struct v44_tx_monitor_config *config)
{
	struct v44_tx_monitor *monitor;

	if (!config_is_valid(parameters, config))
		return NULL;
	monitor = calloc(1, sizeof(*monitor));
	if (monitor == NULL)
		return NULL;
	monitor->shadow = v44_encoder_create(parameters, 2);
	if (monitor->shadow == NULL) {
		free(monitor);
		return NULL;
	}
	monitor->parameters = *parameters;
	monitor->config = *config;
	monitor->mode = V44_ENCODER_COMPRESSED;
	monitor->baseline.mode = V44_ENCODER_COMPRESSED;
	return monitor;
}

void v44_tx_monitor_destroy(
	struct v44_tx_monitor *monitor)
{
	if (monitor == NULL)
		return;
	v44_encoder_destroy(monitor->shadow);
	free(monitor);
}

static enum v44_tx_monitor_result disable_monitor(
	struct v44_tx_monitor *monitor)
{
	v44_encoder_destroy(monitor->shadow);
	monitor->shadow = NULL;
	monitor->pending_decision = V44_TX_MONITOR_STAY;
	monitor->disabled = true;
	return V44_TX_MONITOR_DISABLED;
}

static bool drain_shadow(struct v44_tx_monitor *monitor)
{
	uint8_t discarded[2];

	while (v44_encoder_pending(monitor->shadow) != 0) {
		if (v44_encoder_read(
			    monitor->shadow, discarded, sizeof(discarded)) == 0)
			return false;
	}
	return true;
}

static bool drive_octet(
	struct v44_tx_monitor *monitor, uint8_t octet)
{
	const uint8_t *input = &octet;
	size_t input_length = 1;

	for (;;) {
		enum v44_encode_result result =
			v44_encoder_process(
				monitor->shadow, &input, &input_length);

		if (result == V44_ENCODE_NEED_INPUT)
			return input_length == 0 &&
			       v44_encoder_pending(
				       monitor->shadow) == 0;
		if (result != V44_ENCODE_OK &&
		    result != V44_ENCODE_NEED_OUTPUT)
			return false;
		if (result == V44_ENCODE_NEED_OUTPUT &&
		    v44_encoder_pending(monitor->shadow) == 0)
			return false;
		if (!drain_shadow(monitor))
			return false;
	}
}

static bool drive_flush(struct v44_tx_monitor *monitor)
{
	for (;;) {
		enum v44_encode_result result =
			v44_encoder_flush(monitor->shadow);

		if (result == V44_ENCODE_FLUSHED)
			return v44_encoder_pending(
				       monitor->shadow) == 0;
		if (result != V44_ENCODE_NEED_OUTPUT ||
		    !drain_shadow(monitor))
			return false;
	}
}

static bool drive_mode(
	struct v44_tx_monitor *monitor,
	enum v44_encoder_mode mode)
{
	for (;;) {
		enum v44_encode_result result =
			v44_encoder_request_mode(
				monitor->shadow, mode);

		if (result == V44_ENCODE_OK)
			return v44_encoder_pending(
				       monitor->shadow) == 0;
		if (result != V44_ENCODE_NEED_OUTPUT ||
		    !drain_shadow(monitor))
			return false;
	}
}

static enum v44_tx_monitor_result evaluate_sample(
	struct v44_tx_monitor *monitor,
	enum v44_tx_monitor_decision *decision)
{
	struct v44_encoder_progress progress;
	uint64_t represented_octets;
	uint64_t compressed_bits;
	uint64_t raw_bits;

	if (!v44_encoder_get_progress(
		    monitor->shadow, &progress) ||
	    progress.mode != V44_ENCODER_COMPRESSED ||
	    progress.represented_octets < monitor->baseline.represented_octets ||
	    progress.wire_bits < monitor->baseline.wire_bits)
		return disable_monitor(monitor);
	represented_octets =
		progress.represented_octets - monitor->baseline.represented_octets;
	if (represented_octets < monitor->config.window_octets)
		return V44_TX_MONITOR_OK;
	if (represented_octets > UINT64_MAX / 8U)
		return disable_monitor(monitor);
	compressed_bits = progress.wire_bits - monitor->baseline.wire_bits;
	raw_bits = represented_octets * 8U;
	if (monitor->mode == V44_ENCODER_COMPRESSED &&
	    compressed_bits > raw_bits &&
	    compressed_bits - raw_bits >
		    monitor->config.leave_compressed_excess_bits)
		*decision = V44_TX_MONITOR_ENTER_TRANSPARENT;
	else if (monitor->mode == V44_ENCODER_TRANSPARENT &&
		 raw_bits > compressed_bits &&
		 raw_bits - compressed_bits >
			 monitor->config.enter_compressed_savings_bits)
		*decision = V44_TX_MONITOR_ENTER_COMPRESSED;
	monitor->last_sample = (struct v44_tx_monitor_sample) {
		.mode = monitor->mode,
		.represented_octets = represented_octets,
		.compressed_bits = compressed_bits,
		.decision = *decision,
	};
	monitor->has_last_sample = true;
	monitor->baseline = progress;
	monitor->pending_decision = *decision;
	return V44_TX_MONITOR_OK;
}

enum v44_tx_monitor_result v44_tx_monitor_observe(
	struct v44_tx_monitor *monitor,
	uint8_t octet,
	enum v44_tx_monitor_decision *decision)
{
	if (!monitor_is_valid(monitor) || decision == NULL)
		return V44_TX_MONITOR_INVALID_ARGUMENT;
	*decision = V44_TX_MONITOR_STAY;
	if (monitor->disabled)
		return V44_TX_MONITOR_DISABLED;
	if (monitor->pending_decision != V44_TX_MONITOR_STAY)
		return V44_TX_MONITOR_INVALID_STATE;
	if (!drive_octet(monitor, octet))
		return disable_monitor(monitor);
	return evaluate_sample(monitor, decision);
}

enum v44_tx_monitor_result v44_tx_monitor_flush(
	struct v44_tx_monitor *monitor,
	enum v44_tx_monitor_decision *decision)
{
	if (!monitor_is_valid(monitor) || decision == NULL)
		return V44_TX_MONITOR_INVALID_ARGUMENT;
	*decision = V44_TX_MONITOR_STAY;
	if (monitor->disabled)
		return V44_TX_MONITOR_DISABLED;
	if (monitor->mode != V44_ENCODER_COMPRESSED ||
	    monitor->pending_decision != V44_TX_MONITOR_STAY)
		return V44_TX_MONITOR_INVALID_STATE;
	if (!drive_flush(monitor))
		return disable_monitor(monitor);
	return evaluate_sample(monitor, decision);
}

enum v44_tx_monitor_result
v44_tx_monitor_confirm_mode(
	struct v44_tx_monitor *monitor,
	enum v44_encoder_mode mode)
{
	struct v44_encoder_progress progress;
	enum v44_tx_monitor_decision expected;
	bool transitioned;

	if (!monitor_is_valid(monitor) || !mode_is_valid(mode))
		return V44_TX_MONITOR_INVALID_ARGUMENT;
	if (monitor->disabled)
		return V44_TX_MONITOR_DISABLED;
	expected = mode == V44_ENCODER_COMPRESSED ?
			   V44_TX_MONITOR_ENTER_COMPRESSED :
			   V44_TX_MONITOR_ENTER_TRANSPARENT;
	if (monitor->pending_decision != V44_TX_MONITOR_STAY &&
	    monitor->pending_decision != expected)
		return V44_TX_MONITOR_INVALID_STATE;
	if (mode == monitor->mode) {
		if (monitor->pending_decision != V44_TX_MONITOR_STAY)
			return V44_TX_MONITOR_INVALID_STATE;
		return V44_TX_MONITOR_OK;
	}
	if (mode == V44_ENCODER_TRANSPARENT) {
		transitioned = drive_flush(monitor);
	} else {
		transitioned = drive_mode(
			monitor, V44_ENCODER_TRANSPARENT);
		if (transitioned)
			transitioned = drive_mode(
				monitor, V44_ENCODER_COMPRESSED);
	}
	if (!transitioned || !v44_encoder_get_progress(
				      monitor->shadow, &progress) ||
	    progress.mode != V44_ENCODER_COMPRESSED)
		return disable_monitor(monitor);
	monitor->mode = mode;
	monitor->baseline = progress;
	monitor->pending_decision = V44_TX_MONITOR_STAY;
	return V44_TX_MONITOR_OK;
}

bool v44_tx_monitor_get_last_sample(
	const struct v44_tx_monitor *monitor,
	struct v44_tx_monitor_sample *sample)
{
	if (!monitor_is_valid(monitor) || sample == NULL ||
	    !monitor->has_last_sample)
		return false;
	*sample = monitor->last_sample;
	return true;
}

const char *v44_tx_monitor_result_name(
	enum v44_tx_monitor_result result)
{
	switch (result) {
	case V44_TX_MONITOR_OK:
		return "ok";
	case V44_TX_MONITOR_DISABLED:
		return "disabled";
	case V44_TX_MONITOR_INVALID_ARGUMENT:
		return "invalid argument";
	case V44_TX_MONITOR_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
