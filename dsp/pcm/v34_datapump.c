// SPDX-License-Identifier: BSD-3-Clause

#include "v34_datapump.h"

#include "datapump_timing.h"
#include "modem.h"
#include "modem_param.h"
#include "training_session.h"
#include "v8_datapump.h"
#include "v34_call.h"
#include "v34_data_quality.h"
#include "v34_profile.h"
#include "v90_datapump.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	/* A local full retrain does not receive the peer's response until both
	 * stations have observed their 70 ms silence and 50 ms tone guards. */
	V34_CARRIER_LOSS_SAMPLES = MODEM_RATE / 2,
	/* A 2.5-ms window spans at least six V.34 symbols.  Requiring every
	 * window avoids treating a half-carrier, half-silence RTP frame as data. */
	V34_CARRIER_WINDOW_SAMPLES = MODEM_RATE / 400,
	/* One 20-ms media interval exceeds the trained equalizer's 20-symbol
	 * centre delay at every V.34 symbol rate. */
	V34_CARRIER_RECOVERY_SAMPLES = MODEM_RATE / 50,
	V34_MIN_LINE_POWER = 1000000,
};

struct v34_datapump {
	struct dp dp;
	struct v34_call call;
	struct v34_data_quality data_quality;
	struct training_session training;
	uint32_t missing_carrier_samples;
	uint16_t carrier_recovery_samples;
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

static enum v34_profile_result fallback_profile(
	struct v34_call_configuration *configuration, uint16_t *rate_mask,
	const struct v34_phase2_handoff *handoff)
{
	struct v34_phase3_profile caller;
	struct v34_phase3_profile answerer;
	uint8_t symmetric_rate_units;

	if (v34_phase2_handoff_profiles(handoff, &caller, &answerer) !=
	    V34_PHASE2_SELECTION_OK)
		return V34_PROFILE_INVALID_ARGUMENT;
	/* The current MP policy has one cap for both directions. Do not let it
	 * exceed either projection retained from the completed exchange. */
	symmetric_rate_units =
		caller.projected_data_rate_2400_bps_units <
				answerer.projected_data_rate_2400_bps_units ?
			caller.projected_data_rate_2400_bps_units :
			answerer.projected_data_rate_2400_bps_units;
	return v34_profile_init(
		configuration, rate_mask, handoff->local_role,
		V34_MIN_BIT_RATE,
		(unsigned int)symmetric_rate_units * V34_RATE_STEP, 0);
}

static enum v34_profile_result v8_fallback_profile(
	struct v34_call_configuration *configuration, uint16_t *rate_mask,
	const struct modem *modem, enum v34_station_role role)
{
	unsigned int minimum = modem->min_rate;
	unsigned int maximum = modem->max_rate;

	/* +MS V.90 downstream rates do not share V.34's 2400-bit/s grid.
	 * Translate both bounds downward when V.8 selects conventional V.34. */
	if (maximum > V34_MAX_BIT_RATE)
		maximum = V34_MAX_BIT_RATE;
	maximum -= maximum % V34_RATE_STEP;
	if (maximum < V34_MIN_BIT_RATE)
		return V34_PROFILE_UNSUPPORTED_RATE;
	if (minimum > V34_MAX_BIT_RATE)
		minimum = V34_MAX_BIT_RATE;
	minimum -= minimum % V34_RATE_STEP;
	if (minimum < V34_MIN_BIT_RATE)
		minimum = V34_MIN_BIT_RATE;
	if (minimum > maximum)
		minimum = maximum;
	return v34_profile_init(
		configuration, rate_mask, role, minimum, maximum,
		modem->v34_symbol_rate);
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
	    datapump->carrier_recovery_samples >
		    V34_CARRIER_RECOVERY_SAMPLES ||
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
	struct v90_v34_fallback_handoff fallback;
	enum v34_profile_result profile_result;
	enum v34_station_role role;
	bool from_v90;
	bool from_v8_fallback;
	uint16_t rate_mask;

	from_v90 = modem != NULL && modem->dp != NULL &&
		v90_datapump_v34_fallback_handoff(modem->dp, &fallback);
	from_v8_fallback = modem != NULL && modem->dp != NULL &&
		v8_datapump_selected_v34_fallback(modem->dp);
	role = from_v90 ? fallback.phase2.local_role :
		station_role(caller != 0);
	if (modem == NULL || id != DP_V34 || (caller != 0 && caller != 1) ||
	    sample_rate != MODEM_RATE || max_fragment != MODEM_FRAG ||
	    operations != &v34_operations || operations->use_count == INT_MAX)
		return NULL;
	if (from_v90)
		profile_result = fallback_profile(
			&configuration, &rate_mask, &fallback.phase2);
	else if (from_v8_fallback)
		profile_result = v8_fallback_profile(
			&configuration, &rate_mask, modem, role);
	else
		profile_result = v34_profile_init(
			&configuration, &rate_mask, role, modem->min_rate,
			modem->max_rate, modem->v34_symbol_rate);
	if (profile_result != V34_PROFILE_OK)
		return NULL;
	datapump = calloc(1, sizeof(*datapump));
	if (datapump == NULL)
		return NULL;
	if ((from_v90 ?
	     v34_call_init_from_phase2(
		     &datapump->call, &configuration, &fallback.phase2) :
	     v34_call_init(&datapump->call, &configuration)) != V34_CALL_OK) {
		free(datapump);
		return NULL;
	}
	if (v34_data_quality_init(&datapump->data_quality) !=
	    V34_DATA_QUALITY_OK) {
		free(datapump);
		return NULL;
	}
	datapump->caller = role == V34_CALLING_STATION;
	datapump->rate_mask = rate_mask;
	if (from_v90)
		datapump->training = fallback.training;
	else if (training_session_init(
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
	datapump->carrier_recovery_samples = 0;
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
	datapump->carrier_recovery_samples = 0;
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

static bool window_has_carrier(const int16_t *samples, int count)
{
	uint64_t power = 0;

	for (int index = 0; index < count; ++index) {
		int32_t sample = samples[index];

		power += (uint64_t)(sample * sample);
	}
	return power / (unsigned int)count >= V34_MIN_LINE_POWER;
}

static bool fragment_has_carrier(const int16_t *samples, int count)
{
	int offset;

	if (count <= V34_CARRIER_WINDOW_SAMPLES)
		return window_has_carrier(samples, count);
	for (offset = 0;
	     offset + V34_CARRIER_WINDOW_SAMPLES <= count;
	     offset += V34_CARRIER_WINDOW_SAMPLES)
		if (!window_has_carrier(
			    samples + offset, V34_CARRIER_WINDOW_SAMPLES))
			return false;
	/* Also cover a short tail without making a sub-window zero crossing its
	 * own carrier decision. */
	if (offset != count &&
	    !window_has_carrier(
		    samples + count - V34_CARRIER_WINDOW_SAMPLES,
		    V34_CARRIER_WINDOW_SAMPLES))
		return false;
	return true;
}

static void note_missing_carrier(
	struct v34_datapump *datapump, int count)
{
	if ((uint32_t)(unsigned int)count >=
		   V34_CARRIER_LOSS_SAMPLES -
			   datapump->missing_carrier_samples) {
		datapump->missing_carrier_samples = V34_CARRIER_LOSS_SAMPLES;
	} else {
		datapump->missing_carrier_samples += (uint32_t)(unsigned int)count;
	}
}

static bool set_data_erasure(
	struct v34_datapump *datapump, bool erasure)
{
	return v34_phase34_receiver_set_data_erasure(
		       &datapump->call.state.phase4.receive, erasure) ==
	       V34_PHASE34_RECEIVER_OK;
}

static int process_v34(
	struct dp *dp, void *receive_buffer, void *transmit_buffer, int count)
{
	struct v34_datapump *datapump = v34_from_dp(dp);
	const int16_t *receive = receive_buffer;
	int16_t *transmit = transmit_buffer;
	bool connected = false;
	bool carrier_present;
	bool carrier_erased = false;

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
	carrier_present = fragment_has_carrier(receive, count);
	if (training_session_is_online(&datapump->training)) {
		carrier_erased =
			datapump->call.state.phase4.receive.data_erasure;
		if (!carrier_present) {
			datapump->carrier_recovery_samples = 0;
			note_missing_carrier(datapump, count);
			carrier_erased = true;
		} else if (datapump->missing_carrier_samples != 0u) {
			datapump->missing_carrier_samples = 0;
			datapump->carrier_recovery_samples =
				V34_CARRIER_RECOVERY_SAMPLES;
			carrier_erased = true;
		} else if (datapump->carrier_recovery_samples != 0u) {
			carrier_erased = true;
		}
		if (carrier_erased &&
		    !datapump->call.state.phase4.receive.data_erasure &&
		    !set_data_erasure(datapump, true)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (carrier_present &&
		    datapump->carrier_recovery_samples != 0u) {
			if ((unsigned int)count >=
			    datapump->carrier_recovery_samples) {
				datapump->carrier_recovery_samples = 0;
			} else {
				datapump->carrier_recovery_samples = (uint16_t)(
					datapump->carrier_recovery_samples -
					(unsigned int)count);
			}
		}
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
		    !carrier_erased &&
		    !deliver_mapping_frame(datapump, &received)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
		if (training_session_is_online(&datapump->training) &&
		    data_link_is_online(datapump) &&
		    !carrier_erased &&
		    !observe_online_quality(datapump)) {
			fail_training(datapump);
			memset(transmit, 0, (size_t)count * sizeof(*transmit));
			return DPSTAT_ERROR;
		}
	}
	/* A qualified silence prefix remains an active remote-retrain candidate
	 * throughout the following tone guard.  Do not leak that tone as data or
	 * resume semantic state merely because line power has returned. */
	if (training_session_is_online(&datapump->training) &&
	    datapump->call.state.phase4.receive.data_erasure &&
	    carrier_present && datapump->carrier_recovery_samples == 0u &&
	    datapump->call.retrain_detector.silence_samples == 0u &&
	    !set_data_erasure(datapump, false)) {
		fail_training(datapump);
		return DPSTAT_ERROR;
	}
	if (training_session_is_retraining(&datapump->training) &&
	    !datapump->retrain_carrier_acquired && carrier_present) {
		/* Once the peer has answered a full retrain, later half-duplex
		 * training gaps are not carrier loss.  The training-session deadline
		 * still bounds a peer that responds but never completes. */
		datapump->retrain_carrier_acquired = true;
		datapump->missing_carrier_samples = 0;
	} else if (training_session_is_retraining(&datapump->training) &&
		   !datapump->retrain_carrier_acquired) {
		note_missing_carrier(datapump, count);
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
