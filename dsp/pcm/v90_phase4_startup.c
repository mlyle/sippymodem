// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_startup.h"

const char *v90_phase4_startup_result_name(
	enum v90_phase4_startup_result result)
{
	switch (result) {
	case V90_PHASE4_STARTUP_OK:
		return "ok";
	case V90_PHASE4_STARTUP_DONE:
		return "complete";
	case V90_PHASE4_STARTUP_FAILED:
		return "failed";
	case V90_PHASE4_STARTUP_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_STARTUP_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_STARTUP_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_PHASE4_STARTUP_OUT_OF_RANGE:
		return "out of range";
	case V90_PHASE4_STARTUP_CLIPPING_RISK:
		return "clipping risk";
	case V90_PHASE4_STARTUP_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_PHASE4_STARTUP_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
