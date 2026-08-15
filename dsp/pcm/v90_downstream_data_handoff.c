// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_data_handoff.h"

#include <stddef.h>

enum v90_downstream_data_tx_result v90_downstream_data_tx_start_after_ed(
	struct v90_downstream_data_tx *transmitter,
	const struct pcm_downstream_tx *completed_training,
	const struct v90_pcm_mapping *data_mapping)
{
	if (transmitter == NULL || completed_training == NULL ||
	    data_mapping == NULL)
		return V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT;
	if (!pcm_downstream_tx_is_complete(completed_training))
		return V90_DOWNSTREAM_DATA_TX_INVALID_STATE;
	if (!v90_pcm_mapping_is_valid(data_mapping) ||
	    data_mapping->kind != V90_CP_DATA ||
	    data_mapping->law != completed_training->encoder.mapping.law)
		return V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS;
	return v90_downstream_data_tx_init(transmitter, data_mapping);
}

enum v90_downstream_data_rx_result v90_downstream_data_rx_start_after_ed(
	struct v90_downstream_data_rx *receiver,
	const struct pcm_downstream_rx *completed_training,
	const struct v90_pcm_mapping *data_mapping)
{
	if (receiver == NULL || completed_training == NULL ||
	    data_mapping == NULL)
		return V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT;
	if (!pcm_downstream_rx_is_complete(completed_training))
		return V90_DOWNSTREAM_DATA_RX_INVALID_STATE;
	if (!v90_pcm_mapping_is_valid(data_mapping) ||
	    data_mapping->kind != V90_CP_DATA ||
	    data_mapping->law !=
		    completed_training->stream.decoder.mapping.law)
		return V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS;
	return v90_downstream_data_rx_init(
		receiver, data_mapping,
		completed_training->stream.decoder.decision_plane);
}
