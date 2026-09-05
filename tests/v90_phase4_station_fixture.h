// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE4_STATION_FIXTURE_H
#define V90_PHASE4_STATION_FIXTURE_H

#include <v90_phase3_return_line_rx.h>
#include <v90_phase4_analog_station.h>

#include <stdbool.h>

/* Generate the Phase 3 return boundary with the analogue Phase 4 owner
 * already sending CPt. The receive handoff was trained by that waveform. */
bool v90_phase4_station_fixture_init(
	enum g711_law law, uint8_t projected_data_rate_2400_bps_units,
	const struct v90_jd_parameters *jd,
	uint8_t training_ucode,
	const struct v90_cp_parameters *training,
	struct v90_phase4_analog_station *analog,
	struct v90_phase3_return_line_handoff *receive);

#endif /* V90_PHASE4_STATION_FIXTURE_H */
