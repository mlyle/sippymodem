#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import unittest
from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.live_profiles import (
    LINK_MODE_AT_COMMANDS,
    PROFILE_GROUPS,
    PROFILES,
    READINESS_PREREQUISITE,
    READINESS_RUNNABLE,
    V34_RATES_BY_SYMBOL_RATE,
    V34_RUNNABLE_RATES_BY_SYMBOL_RATE,
    V90_DOWNSTREAM_RATES,
    V90_UPSTREAM_RATES_BY_SYMBOL_RATE,
    V92_PCM_UPSTREAM_RATES,
)


class LiveProfileTests(unittest.TestCase):
    def test_current_profiles_use_registered_controls(self) -> None:
        self.assertEqual(len(PROFILES), 324)
        self.assertEqual(len(PROFILE_GROUPS["implemented"]), 71)
        self.assertEqual(len(PROFILE_GROUPS["analog-fallback"]), 5)
        self.assertEqual(
            LINK_MODE_AT_COMMANDS["raw"],
            ("ATZ", "ATE0", "AT&A7", "ATX3", "AT\\N0", "AT+DS=0,0"),
        )
        self.assertEqual(
            PROFILE_GROUPS["lower"][0:2], ("bell103_300", "v21_300")
        )
        self.assertNotIn("v22_600", PROFILE_GROUPS["implemented"])
        self.assertEqual(
            PROFILES["baudot_50"].at_commands,
            ("AT+MS=45,0,50,50",),
        )
        self.assertEqual(
            PROFILES["v32bis_14400"].at_commands,
            ("AT+MS=132,0,14400,14400",),
        )
        self.assertEqual(
            PROFILES["v34_9600_b2400"].at_commands,
            ("AT+MS=34,0,9600,9600", "AT+V34B=2400"),
        )
        self.assertEqual(
            PROFILES["v34_9600_b2400"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v90_28000_19200_b3000"].at_commands,
            (
                "AT+MS=90,0,28000,28000",
                "AT+V90U=19200,19200,3000",
            ),
        )
        self.assertEqual(
            PROFILES["v90_28000_19200_b3000"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v90_42667_26400_b3200"].at_commands,
            (
                "AT+MS=90,0,42667,42667",
                "AT+V90U=26400,26400,3200",
            ),
        )
        self.assertEqual(
            PROFILES["v90_56000_31200_b3429"].at_commands,
            (
                "AT+MS=90,0,56000,56000",
                "AT+V90U=31200,31200,3429",
            ),
        )

    def test_future_profiles_are_inert(self) -> None:
        unsupported = ("AT+V92Q", "AT+V92U")
        runnable = tuple(
            profile
            for profile in PROFILES.values()
            if profile.readiness == READINESS_RUNNABLE
        )

        self.assertEqual(len(runnable), 130)
        for profile in runnable:
            self.assertTrue(profile.at_commands)
            self.assertTrue(
                all(
                    command.startswith(
                        ("AT+MS=", "AT+V34B=", "AT+V90U=")
                    )
                    for command in profile.at_commands
                )
            )
            self.assertFalse(
                any(
                    command.startswith(prefix)
                    for command in profile.at_commands
                    for prefix in unsupported
                )
            )
        self.assertTrue(
            all(
                PROFILES[name].readiness == READINESS_PREREQUISITE
                for name in PROFILE_GROUPS["future-digital"]
            )
        )

    def test_generated_v34_matrix_matches_rate_table(self) -> None:
        expected = {
            (symbol_rate, rate)
            for symbol_rate, rates in V34_RATES_BY_SYMBOL_RATE.items()
            for rate in rates
        }
        receive = {
            (PROFILES[name].symbol_rate, PROFILES[name].receive_rate)
            for name in PROFILE_GROUPS["v34-rx"]
        }
        transmit = {
            (PROFILES[name].symbol_rate, PROFILES[name].transmit_rate)
            for name in PROFILE_GROUPS["v34-tx"]
        }
        matched = {
            (PROFILES[name].symbol_rate, PROFILES[name].receive_rate)
            for name in PROFILE_GROUPS["v34-matched"]
        }
        current = {
            (PROFILES[name].symbol_rate, PROFILES[name].receive_rate)
            for name in PROFILE_GROUPS["v34-current"]
        }
        runnable_expected = {
            (symbol_rate, rate)
            for symbol_rate, rates in V34_RUNNABLE_RATES_BY_SYMBOL_RATE.items()
            for rate in rates
        }

        self.assertEqual(matched, expected)
        self.assertEqual(current, runnable_expected)
        self.assertEqual(receive, expected)
        self.assertEqual(transmit, expected - {(2400, 2400)})
        self.assertNotIn(2743, V34_RATES_BY_SYMBOL_RATE)
        self.assertFalse(any("_b2743" in name for name in PROFILES))
        self.assertEqual(len(PROFILE_GROUPS["v34-rx"]), 55)
        self.assertEqual(len(PROFILE_GROUPS["v34-tx"]), 54)
        self.assertEqual(len(PROFILE_GROUPS["v34-matched"]), 55)
        self.assertEqual(len(PROFILE_GROUPS["v34-current"]), 55)
        self.assertEqual(
            PROFILES["v34_14400_b2400"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_16800_b3429"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_16800_b2400"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_19200_b3429"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_19200_b2400"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_21600_b3429"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_24000_b3429"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_24000_b2800"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_26400_b2800"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_28800_b3000"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_28800_b3429"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_31200_b3200"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_31200_b3429"].readiness,
            READINESS_RUNNABLE,
        )
        self.assertEqual(
            PROFILES["v34_33600_b3429"].readiness,
            READINESS_RUNNABLE,
        )

    def test_generated_digital_matrix_is_directional(self) -> None:
        self.assertEqual(len(PROFILE_GROUPS["v90-down"]), 22)
        self.assertEqual(len(PROFILE_GROUPS["v90-up"]), 36)
        self.assertEqual(len(PROFILE_GROUPS["v92-down"]), 22)
        self.assertEqual(len(PROFILE_GROUPS["v92-up"]), 36)
        self.assertEqual(len(PROFILE_GROUPS["v92-pcmup"]), 19)
        self.assertEqual(len(PROFILE_GROUPS["v92-headend"]), 2)
        self.assertTrue(
            all(
                PROFILES[name].readiness == READINESS_RUNNABLE
                for name in PROFILE_GROUPS["v90"]
            )
        )
        self.assertTrue(
            all(
                PROFILES[name].readiness == READINESS_PREREQUISITE
                for name in PROFILE_GROUPS["v92"]
            )
        )
        self.assertEqual(
            tuple(
                PROFILES[name].receive_rate
                for name in PROFILE_GROUPS["v90-down"]
            ),
            V90_DOWNSTREAM_RATES,
        )
        expected_upstream = {
            (symbol_rate, rate)
            for symbol_rate, rates in V90_UPSTREAM_RATES_BY_SYMBOL_RATE.items()
            for rate in rates
        }
        self.assertEqual(
            {
                (PROFILES[name].symbol_rate, PROFILES[name].transmit_rate)
                for name in PROFILE_GROUPS["v92-up"]
            },
            expected_upstream,
        )
        self.assertEqual(
            tuple(
                PROFILES[name].transmit_rate
                for name in PROFILE_GROUPS["v92-pcmup"]
            ),
            V92_PCM_UPSTREAM_RATES,
        )
        self.assertEqual(
            PROFILES["v92_pcmup_originator_28000"].at_commands,
            (
                "AT+V92Q=0",
                "AT+V92U=1,28000",
                "AT+MS=92,0,25000,56000",
            ),
        )


if __name__ == "__main__":
    unittest.main()
