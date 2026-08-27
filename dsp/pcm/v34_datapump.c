// SPDX-License-Identifier: BSD-3-Clause

#include "v34_datapump.h"

#include "datapump_timing.h"
#include "modem.h"
#include "modem_param.h"
#include "training_session.h"
#include "v34_call.h"
#include "v34_data_quality.h"
#include "v34_frame_schedule.h"
#include "v34_phase2_selection.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	V34_MAX_RATE_UNITS = V34_MAX_BIT_RATE / V34_RATE_STEP,
	/* Phase 2 L2 is 6 dB below its 8192-peak L1.  The
	 * normalized Phase 3 line has a separate scale so its S/S-bar power
	 * matches the fixed V.34 training constellation. */
	V34_PHASE2_L2_AMPLITUDE = 4096,
	V34_PHASE3_NOMINAL_AMPLITUDE = 3792,
	/* A local full retrain does not receive the peer's response until both
	 * stations have observed their 70 ms silence and 50 ms tone guards. */
	V34_CARRIER_LOSS_SAMPLES = MODEM_RATE / 2,
	V34_MIN_LINE_POWER = 1000000,
};

struct v34_datapump {
	struct dp dp;
	struct v34_call call;
	struct v34_data_quality data_quality;
	struct training_session training;
	uint32_t missing_carrier_samples;
	uint16_t rate_mask;
	bool caller;
	bool retrain_carrier_acquired;
};

static struct dp *create_v34(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations);
static int delete_v34(struct dp *dp);
static int process_v34(
	struct dp *dp, void *receive, void *transmit, int count);
static int retrain_v34(struct dp *dp);

static struct dp_operations v34_operations = {
	.name = "V.34",
	.create = create_v34,
	.delete = delete_v34,
	.process = process_v34,
	.retrain = retrain_v34,
};

static enum v34_station_role station_role(bool caller)
{
	return caller ? V34_CALLING_STATION : V34_ANSWERING_STATION;
}

static bool symbol_rate_from_number(
	unsigned int number, enum v34_symbol_rate *symbol_rate)
{
	if (symbol_rate == NULL)
		return false;
	switch (number) {
	case 2400:
		*symbol_rate = V34_SYMBOL_RATE_2400;
		return true;
	case 2743:
		*symbol_rate = V34_SYMBOL_RATE_2743;
		return true;
	case 2800:
		*symbol_rate = V34_SYMBOL_RATE_2800;
		return true;
	case 3000:
		*symbol_rate = V34_SYMBOL_RATE_3000;
		return true;
	case 3200:
		*symbol_rate = V34_SYMBOL_RATE_3200;
		return true;
	case 3429:
		*symbol_rate = V34_SYMBOL_RATE_3429;
		return true;
	default:
		return false;
	}
}

static uint16_t requested_rate_mask(const struct modem *modem)
{
	uint16_t mask = 0;

	if (modem == NULL)
		return 0;
	for (uint8_t units = 1; units <= V34_MAX_RATE_UNITS;
	     ++units) {
		unsigned int bit_rate = (unsigned int)units * V34_RATE_STEP;

		if (modem->min_rate <= bit_rate && modem->max_rate >= bit_rate)
			mask |= (uint16_t)(1u << (units - 1u));
	}
	return mask;
}

static uint8_t highest_rate(uint16_t mask)
{
	for (uint8_t units = V34_MAX_RATE_UNITS; units != 0; --units)
		if ((mask & (uint16_t)(1u << (units - 1u))) != 0)
			return units;
	return 0;
}

static uint16_t enabled_rate_mask(
	const struct modem *modem, enum v34_symbol_rate *symbol_rate,
	uint8_t *highest_rate_units)
{
	uint16_t requested = requested_rate_mask(modem);
	uint16_t enabled = 0;
	uint8_t highest = highest_rate(requested);
	uint8_t maximum;
	enum v34_symbol_rate selected;

	if (symbol_rate == NULL || highest_rate_units == NULL || highest == 0)
		return 0;
	if (modem->v34_symbol_rate == 0) {
		selected = highest == V34_MIN_BIT_RATE / V34_RATE_STEP ?
				   V34_SYMBOL_RATE_2400 : V34_SYMBOL_RATE_3429;
	} else if (!symbol_rate_from_number(
			   modem->v34_symbol_rate, &selected)) {
		return 0;
	}
	maximum = v34_phase2_projected_rate_limit(selected);
	if (maximum == 0)
		return 0;
	/* Advertise only rate/cadence pairs that pass generated full-duplex
	 * calls through both G.711 laws and in both station roles. */
	for (uint8_t units = 1; units <= maximum; ++units) {
		struct v34_frame_geometry geometry;

		if ((requested & (uint16_t)(1u << (units - 1u))) != 0 &&
		    v34_frame_geometry_init(
			    &geometry, selected, units, false) ==
			    V34_FRAME_SCHEDULE_OK)
			enabled |= (uint16_t)(1u << (units - 1u));
	}
	*symbol_rate = selected;
	*highest_rate_units = highest_rate(enabled);
	return enabled;
}

static struct v34_info0 baseline_info0(void)
{
	return (struct v34_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_2800_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.constellation_1664_supported = true,
		.maximum_symbol_rate_difference_steps = 0,
		.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL,
	};
}

static union v34_local_info1 baseline_info1(
	bool caller, enum v34_symbol_rate symbol_rate,
	uint8_t projected_rate_units)
{
	union v34_local_info1 info1 = { 0 };

	if (caller) {
		info1.calling.common.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		info1.calling.answer_transmit[symbol_rate]
			.high_carrier = symbol_rate == V34_SYMBOL_RATE_2743;
		info1.calling.answer_transmit[symbol_rate]
			.projected_data_rate_2400_bps_units = projected_rate_units;
	} else {
		info1.answering.common.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE;
		info1.answering.call_transmit
			.high_carrier = symbol_rate == V34_SYMBOL_RATE_2743;
		info1.answering.call_transmit
			.projected_data_rate_2400_bps_units = projected_rate_units;
		info1.answering.answer_to_call_symbol_rate =
			symbol_rate;
		info1.answering.call_to_answer_symbol_rate =
			symbol_rate;
	}
	return info1;
}

static struct v34_mp_record baseline_mp(
	uint16_t rate_mask, uint8_t highest_rate_units)
{
	return (struct v34_mp_record){
		.type = V34_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_call_to_answer_rate_2400_bps_units =
				highest_rate_units,
			.maximum_answer_to_call_rate_2400_bps_units =
				highest_rate_units,
			/* Use the mandatory 16-state mode until higher-complexity
			 * receivers have comparable interop coverage. */
			.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
			.data_rate_capability_mask = rate_mask,
		},
	};
}

static bool call_configuration(
	const struct modem *modem, bool caller,
	struct v34_call_configuration *configuration,
	uint16_t *selected_rate_mask)
{
	uint8_t highest_rate_units = 0;
	enum v34_symbol_rate symbol_rate;
	uint16_t rate_mask = enabled_rate_mask(
		modem, &symbol_rate, &highest_rate_units);

	if (configuration == NULL || selected_rate_mask == NULL ||
	    rate_mask == 0)
		return false;
	*configuration = (struct v34_call_configuration){
		.local_role = station_role(caller),
		.phase2_nominal_amplitude = V34_PHASE2_L2_AMPLITUDE,
		.local_info0 = baseline_info0(),
		.local_info1 = baseline_info1(
			caller, symbol_rate, highest_rate_units),
		.phase3 = {
			.requested_remote_constellation =
				V34_CONSTELLATION_4_POINT,
			.sample_rate = MODEM_RATE,
			.nominal_peak_amplitude =
				V34_PHASE3_NOMINAL_AMPLITUDE,
		},
		.phase4 = {
			.transmit_mp = baseline_mp(
				rate_mask, highest_rate_units),
			.data_equalizer_adaptation =
				V34_DATA_EQUALIZER_TRACKING_STEP,
		},
	};
	*selected_rate_mask = rate_mask;
	return true;
}

static struct v34_datapump *v34_from_dp(struct dp *dp)
{
	struct v34_datapump *datapump;

	if (dp == NULL || dp->id != DP_V34 || dp->op != &v34_operations ||
	    dp->modem == NULL || dp->dp_data == NULL)
		return NULL;
	datapump = dp->dp_data;
	if (&datapump->dp != dp || datapump->rate_mask == 0 ||
	    !v34_data_quality_is_valid(&datapump->data_quality) ||
	    !training_session_is_valid(&datapump->training) ||
	    datapump->missing_carrier_samples > V34_CARRIER_LOSS_SAMPLES ||
	    !v34_call_is_valid(&datapump->call) ||
	    datapump->call.local_role != station_role(datapump->caller) ||
	    (v34_call_has_failed(&datapump->call) &&
	     !training_session_has_failed(&datapump->training)) ||
	    (!training_session_has_failed(&datapump->training) &&
	     training_session_is_online(&datapump->training) !=
		     v34_call_is_online(&datapump->call) &&
	     !(training_session_is_retraining(&datapump->training) &&
	       !v34_call_is_online(&datapump->call))) ||
	    (!training_session_has_failed(&datapump->training) &&
	     v34_call_is_online(&datapump->call) &&
	     dp->status != DPSTAT_CONNECT) ||
	    (!training_session_has_failed(&datapump->training) &&
	     !v34_call_is_online(&datapump->call) &&
	     dp->status == DPSTAT_CONNECT &&
	     !training_session_is_retraining(&datapump->training)))
		return NULL;
	return datapump;
}

static struct dp *create_v34(
	struct modem *modem, enum DP_ID id, int caller, int sample_rate,
	int max_fragment, struct dp_operations *operations)
{
	struct v34_call_configuration configuration;
	struct v34_datapump *datapump;
	uint16_t rate_mask;

	if (modem == NULL || id != DP_V34 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v34_operations || operations->use_count == INT_MAX ||
	    !call_configuration(
		    modem, caller != 0, &configuration, &rate_mask))
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	if (v34_call_init(&datapump->call, &configuration) != V34_CALL_OK) {
		free(datapump);
		return NULL;
	}
	if (v34_data_quality_init(&datapump->data_quality) !=
	    V34_DATA_QUALITY_OK) {
		free(datapump);
		return NULL;
	}
	datapump->caller = caller != 0;
	datapump->rate_mask = rate_mask;
	if (training_session_init(
		    &datapump->training,
		    datapump_carrier_wait_ticks(modem)) !=
	    TRAINING_SESSION_OK) {
		free(datapump);
		return NULL;
	}
	datapump->dp = (struct dp){
		.id = id,
		.modem = modem,
		.status = DPSTAT_OK,
		.op = operations,
		.dp_data = datapump,
	};
	operations->use_count++;
	return &datapump->dp;
}

static int delete_v34(struct dp *dp)
{
	struct v34_datapump *datapump = v34_from_dp(dp);

	if (datapump == NULL || v34_operations.use_count <= 0)
		return -1;
	v34_operations.use_count--;
	datapump->dp.dp_data = NULL;
	free(datapump);
	return 0;
}

static int start_local_retrain(struct v34_datapump *datapump)
{
	struct training_session training;
	struct v34_call call;

	if (!training_session_is_online(&datapump->training))
		return -1;
	training = datapump->training;
	call = datapump->call;
	if (v34_call_start_retrain(&call) != V34_CALL_OK ||
	    training_session_start_retrain(
		    &training, TRAINING_LOCAL_RETRAIN) !=
		    TRAINING_SESSION_OK)
		return -1;
	datapump->call = call;
	datapump->training = training;
	datapump->missing_carrier_samples = 0;
	datapump->retrain_carrier_acquired = false;
	return 0;
}

static int retrain_v34(struct dp *dp)
{
	struct v34_datapump *datapump = v34_from_dp(dp);

	if (datapump == NULL)
		return -1;
	/* A remote or quality-triggered full retrain may begin during the guard
	 * interval before ATO1 reaches the datapump. Join that same transaction;
	 * the modem wrapper will report CONNECT when this generation completes. */
	if (training_session_is_retraining(&datapump->training))
		return 0;
	return start_local_retrain(datapump);
}

static bool source_mapping_frame(
	void *context, const struct v34_mapping_frame *request,
	uint8_t *primary_bits, uint8_t *auxiliary_bit)
{
	struct v34_datapump *datapump = context;
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS + 1];
	size_t bit_count;
	int supplied = 0;

	if (datapump == NULL || request == NULL || primary_bits == NULL ||
	    request->primary_bits > V34_MAX_MAPPING_FRAME_BITS)
		return false;
	bit_count = request->primary_bits + (auxiliary_bit != NULL ? 1u : 0u);
	memset(bits, 1, bit_count);
	if (training_session_is_online(&datapump->training) &&
	    datapump->dp.modem->get_bits != NULL)
		supplied = modem_get_bits(
			datapump->dp.modem, 1, bits, (int)bit_count);
	if (supplied < 0)
		supplied = 0;
	if ((size_t)supplied > bit_count)
		supplied = (int)bit_count;
	for (size_t index = 0; index < (size_t)supplied; ++index)
		bits[index] &= 1u;
	for (size_t index = (size_t)supplied; index < bit_count; ++index)
		bits[index] = 1;
	memcpy(primary_bits, bits, request->primary_bits);
	if (auxiliary_bit != NULL)
		*auxiliary_bit = bits[request->primary_bits];
	return true;
}

static bool deliver_mapping_frame(
	struct v34_datapump *datapump,
	const struct v34_received_mapping_frame *received)
{
	uint8_t bits[V34_MAX_MAPPING_FRAME_BITS];

	if (received == NULL ||
	    received->primary_bit_count > V34_MAX_MAPPING_FRAME_BITS)
		return false;
	if (!training_session_is_online(&datapump->training) ||
	    datapump->dp.modem->put_bits == NULL)
		return true;
	memcpy(bits, received->primary_bits, received->primary_bit_count);
	return modem_put_bits(
		       datapump->dp.modem, 1, bits,
		       received->primary_bit_count) ==
	       received->primary_bit_count;
}

static bool enter_link(struct v34_datapump *datapump)
{
	struct v34_data_startup startup;
	struct v34_data_quality data_quality;
	const struct v34_adaptive_equalizer_quality *measurement;
	bool retrained;
	unsigned int transmit_rate;
	unsigned int receive_rate;

	if (!v34_call_get_data_startup(&datapump->call, &startup))
		return false;
	measurement = &datapump->call.state.phase4.receive.equalizer.quality;
	data_quality = datapump->data_quality;
	if (v34_data_quality_start(
		    &data_quality, startup.receive.geometry.symbol_rate,
		    startup.receive.geometry.primary_data_rate_2400_bps_units,
		    measurement) != V34_DATA_QUALITY_OK)
		return false;
	transmit_rate =
		(unsigned int)startup.mode.transmit
			.primary_data_rate_2400_bps_units * V34_RATE_STEP;
	receive_rate =
		(unsigned int)startup.mode.receive
			.primary_data_rate_2400_bps_units * V34_RATE_STEP;
	if (modem_set_param(
		    datapump->dp.modem, MDMPRM_TX_RATE,
		    (int)transmit_rate) != 0 ||
	    modem_set_param(
		    datapump->dp.modem, MDMPRM_RX_RATE,
		    (int)receive_rate) != 0)
		return false;
	retrained = training_session_is_retraining(&datapump->training);
	if (training_session_connect(&datapump->training) !=
	    TRAINING_SESSION_OK)
		return false;
	datapump->data_quality = data_quality;
	datapump->missing_carrier_samples = 0;
	datapump->dp.status = DPSTAT_CONNECT;
	if (retrained)
		modem_dp_reconnected(datapump->dp.modem);
	else
		modem_dp_connected(datapump->dp.modem);
	return true;
}

static bool observation_has_progress(
	const struct v34_call_observation *observation)
{
	if (observation->processed != observation->next)
		return true;
	switch (observation->processed) {
	case V34_CALL_PHASE2:
		return observation->events.phase2.kind !=
		       V34_PHASE2_EVENT_NONE;
	case V34_CALL_PHASE3:
		return observation->events.phase3 != 0;
	case V34_CALL_PHASE4:
		return observation->events.phase4 != 0;
	case V34_CALL_RETRAIN_SILENCE:
		return false;
	}
	return false;
}

static void fail_training(struct v34_datapump *datapump)
{
	(void)training_session_fail(&datapump->training);
}

static bool observe_online_quality(struct v34_datapump *datapump)
{
	struct v34_data_quality next = datapump->data_quality;
	const struct v34_adaptive_equalizer_quality *measurement =
		&datapump->call.state.phase4.receive.equalizer.quality;
	enum v34_data_quality_result result =
		v34_data_quality_observe(&next, measurement);

	if (result == V34_DATA_QUALITY_FREEZE_TIMING ||
	    result == V34_DATA_QUALITY_RESUME_TIMING) {
		if (v34_phase34_receiver_set_data_timing_frozen(
			    &datapump->call.state.phase4.receive,
			    result == V34_DATA_QUALITY_FREEZE_TIMING) !=
		    V34_PHASE34_RECEIVER_OK)
			return false;
	} else if (result == V34_DATA_QUALITY_RETRAIN) {
		if (start_local_retrain(datapump) != 0)
			return false;
	} else if (result != V34_DATA_QUALITY_OK) {
		return false;
	}
	datapump->data_quality = next;
	return true;
}

static bool data_link_is_online(const struct v34_datapump *datapump)
{
	const struct modem *modem = datapump->dp.modem;

	return modem != NULL && modem->get_chars != NULL &&
	       modem->put_chars != NULL;
}

static bool update_carrier(
	struct v34_datapump *datapump, const int16_t *samples, int count)
{
	uint64_t power = 0;

	for (int index = 0; index < count; ++index) {
		int32_t sample = samples[index];

		power += (uint64_t)(sample * sample);
	}
	if (power / (unsigned int)count >= V34_MIN_LINE_POWER) {
		datapump->missing_carrier_samples = 0;
		return true;
	} else if ((uint32_t)(unsigned int)count >=
		   V34_CARRIER_LOSS_SAMPLES -
			   datapump->missing_carrier_samples) {
		datapump->missing_carrier_samples = V34_CARRIER_LOSS_SAMPLES;
	} else {
		datapump->missing_carrier_samples += (uint32_t)(unsigned int)count;
	}
	return false;
}

static int process_v34(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v34_datapump *datapump = v34_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	bool connected = false;

	if (datapump == NULL || receive == NULL || transmit == NULL ||
	    count <= 0 || count > MODEM_FRAG) {
		if (transmit != NULL && count > 0 && count <= MODEM_FRAG)
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (training_session_has_failed(&datapump->training)) {
		memset(transmit, 0, (size_t)count * sizeof(*transmit));
		return DPSTAT_ERROR;
	}
	if (!training_session_is_online(&datapump->training)) {
		if (training_session_advance(
			    &datapump->training,
			    (uint64_t)(unsigned int)count) !=
		    TRAINING_SESSION_OK) {
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	for (int index = 0; index < count; ++index) {
		struct v34_call_observation observation;
		struct v34_received_mapping_frame received;
		enum v34_call_result result = v34_call_sample(
			&datapump->call, receive[index], source_mapping_frame,
			datapump, &transmit[index], &observation, &received);

		if (result != V34_CALL_OK) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (training_session_is_online(&datapump->training) &&
		    observation.processed != V34_CALL_RETRAIN_SILENCE &&
		    observation.next == V34_CALL_RETRAIN_SILENCE) {
			if (training_session_start_retrain(
				    &datapump->training,
				    TRAINING_REMOTE_RETRAIN) !=
			    TRAINING_SESSION_OK) {
				fail_training(datapump);
				memset(transmit, 0,
				       (size_t)count * sizeof(*transmit));
				return DPSTAT_ERROR;
			}
			datapump->retrain_carrier_acquired = false;
		}
		if (!training_session_is_online(&datapump->training) &&
		    observation_has_progress(&observation) &&
		    training_session_note_progress(&datapump->training) !=
			    TRAINING_SESSION_OK) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (!training_session_is_online(&datapump->training) &&
		    v34_call_is_online(&datapump->call)) {
			if (!enter_link(datapump)) {
				fail_training(datapump);
				memset(transmit, 0,
				       (size_t)count * sizeof(*transmit));
				return DPSTAT_ERROR;
			}
			connected = true;
		}
		if (observation.processed == V34_CALL_PHASE4 &&
		    (observation.events.phase4 &
		     V34_PHASE4_EXCHANGE_EVENT_MAPPING_FRAME_RECEIVED) != 0 &&
		    !deliver_mapping_frame(datapump, &received)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (training_session_is_online(&datapump->training) &&
		    data_link_is_online(datapump) &&
		    !observe_online_quality(datapump)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	if (training_session_is_online(&datapump->training)) {
		(void)update_carrier(datapump, receive, count);
	} else if (training_session_is_retraining(&datapump->training) &&
		   !datapump->retrain_carrier_acquired &&
		   update_carrier(datapump, receive, count)) {
		/* Once the peer has answered a full retrain, later half-duplex
		 * training gaps are not carrier loss.  The training-session deadline
		 * still bounds a peer that responds but never completes. */
		datapump->retrain_carrier_acquired = true;
	}
	if (connected)
		return DPSTAT_CONNECT;
	if (datapump->missing_carrier_samples >= V34_CARRIER_LOSS_SAMPLES) {
		fail_training(datapump);
		return DPSTAT_ERROR;
	}
	return DPSTAT_OK;
}

int v34_datapump_register(void)
{
	return modem_dp_register(DP_V34, &v34_operations);
}

int v34_datapump_unregister(void)
{
	return modem_dp_deregister(DP_V34, &v34_operations);
}

bool v34_datapump_is_busy(void)
{
	return v34_operations.use_count != 0;
}
