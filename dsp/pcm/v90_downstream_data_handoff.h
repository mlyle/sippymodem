// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_DATA_HANDOFF_H
#define V90_DOWNSTREAM_DATA_HANDOFF_H

#include "v90_downstream_data_rx.h"
#include "v90_downstream_data_tx.h"
#include "pcm_downstream_rx.h"
#include "pcm_downstream_tx.h"

/* Start a fresh B1d coding epoch only after the complete Ed training epoch.
 * Training coding histories are deliberately discarded. The transmitter
 * retains only the network companding law; the receiver also retains the
 * decision plane learned during training. */
enum v90_downstream_data_tx_result v90_downstream_data_tx_start_after_ed(
	struct v90_downstream_data_tx *transmitter,
	const struct pcm_downstream_tx *completed_training,
	const struct v90_pcm_mapping *data_mapping);

enum v90_downstream_data_rx_result v90_downstream_data_rx_start_after_ed(
	struct v90_downstream_data_rx *receiver,
	const struct pcm_downstream_rx *completed_training,
	const struct v90_pcm_mapping *data_mapping);

#endif /* V90_DOWNSTREAM_DATA_HANDOFF_H */
