#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.live_campaign_profiles import (  # noqa: E402
    CASES,
    CASES_BY_NAME,
    FAMILY_ORDER,
    LINK_LAPM,
    LINK_MODES,
    LINK_RAW,
    RAW_ONLY_FAMILIES,
    ROLES,
    cases_for_suite,
    select_cases,
)


class LiveCampaignProfileTests(unittest.TestCase):
    def test_curated_suite_sizes_are_bounded(self) -> None:
        self.assertEqual(len(cases_for_suite("smoke")), 22)
        self.assertEqual(len(cases_for_suite("standard")), 100)
        self.assertEqual(len(cases_for_suite("extended")), 146)

    def test_cases_have_safe_unique_generated_commands(self) -> None:
        self.assertEqual(len(CASES), len({case.name for case in CASES}))
        self.assertTrue({case.family for case in CASES} <= set(FAMILY_ORDER))
        for case in CASES:
            self.assertIn(case.role, ROLES)
            self.assertIn(case.link_mode, LINK_MODES)
            self.assertTrue(case.at_commands)
            for command in case.at_commands:
                self.assertTrue(command.startswith("AT"), case.name)
                command.encode("ascii")

            if case.link_mode == LINK_RAW:
                self.assertIn("AT\\N0", case.at_commands)
                self.assertIn("AT+DS=0,0", case.at_commands)
                self.assertFalse(
                    any(command.startswith("AT+DSC=") for command in case.at_commands)
                )
                self.assertTrue(
                    {"V42", "V44"}.isdisjoint(case.callback_features)
                )
            else:
                self.assertIn("AT\\N4", case.at_commands)
                self.assertIn("AT+DS=3,1", case.at_commands)

    def test_only_slowest_modes_omit_lapm_variants(self) -> None:
        modes_by_profile: dict[tuple[object, ...], set[str]] = {}
        for case in CASES:
            identity = (
                case.family,
                case.role,
                case.feature,
                case.receive_rate,
                case.transmit_rate,
                case.symbol_rate,
                case.level,
            )
            modes_by_profile.setdefault(identity, set()).add(case.link_mode)
        for identity, modes in modes_by_profile.items():
            expected = {LINK_RAW} if identity[0] in RAW_ONLY_FAMILIES else set(LINK_MODES)
            self.assertEqual(modes, expected, identity)

    def test_standard_crosses_v34_cadence_boundaries_in_both_roles(self) -> None:
        v34 = [case for case in cases_for_suite("standard") if case.family == "v34"]
        self.assertEqual({case.symbol_rate for case in v34}, {2400, 2800, 3000, 3200, 3429})
        self.assertTrue(
            all(
                case.receive_rate is not None
                and case.receive_rate >= 4800
                and case.transmit_rate is not None
                and case.transmit_rate >= 4800
                for case in v34
            )
        )
        for symbol_rate in {2400, 2800, 3000, 3200, 3429}:
            self.assertEqual(
                {case.role for case in v34 if case.symbol_rate == symbol_rate},
                set(ROLES),
            )

    def test_standard_has_headend_fallback_and_pcm_upstream(self) -> None:
        standard = cases_for_suite("standard")
        features = {case.feature for case in standard}
        self.assertTrue(
            {"digital-headend", "analogue-fallback", "pcm-upstream", "quick-connect"}
            <= features
        )
        pcm = [case for case in standard if case.feature == "pcm-upstream"]
        self.assertEqual({case.receive_rate for case in pcm}, {24000, 36000, 48000})
        for rate in {24000, 36000, 48000}:
            self.assertEqual(
                {case.role for case in pcm if case.receive_rate == rate},
                set(ROLES),
            )

    def test_asymmetric_rates_use_the_local_modem_perspective(self) -> None:
        self.assertEqual(
            (
                CASES_BY_NAME["v23-1200-75-originator"].receive_rate,
                CASES_BY_NAME["v23-1200-75-originator"].transmit_rate,
            ),
            (1200, 75),
        )
        self.assertEqual(
            (
                CASES_BY_NAME["v23-1200-75-callback"].receive_rate,
                CASES_BY_NAME["v23-1200-75-callback"].transmit_rate,
            ),
            (75, 1200),
        )
        digital = CASES_BY_NAME[
            "v90-d28000-u14400-b3000-lapm-originator"
        ]
        pcm_upstream = CASES_BY_NAME["v92-pcmup-36000-lapm-callback"]
        self.assertEqual(
            (digital.receive_rate, digital.transmit_rate),
            (14400, 28000),
        )
        self.assertEqual(
            (pcm_upstream.receive_rate, pcm_upstream.transmit_rate),
            (36000, 56000),
        )

    def test_filters_intersect_without_expanding_the_suite(self) -> None:
        selected = select_cases(
            "standard",
            families=("v34",),
            roles=("callback",),
            rates=(9600,),
            symbol_rates=(2400,),
        )
        self.assertEqual(
            tuple(case.name for case in selected),
            (
                "v34-9600-b2400-raw-callback",
                "v34-9600-b2400-lapm-callback",
            ),
        )

        explicit = select_cases(
            "smoke", names=("v22-600-callback",)
        )
        self.assertEqual(
            tuple(case.name for case in explicit),
            ("v22-600-lapm-callback",),
        )

    def test_v34_lapm_uses_each_cell_as_a_rate_ceiling(self) -> None:
        raw = CASES_BY_NAME["v34-26400-b3200-raw-originator"]
        lapm = CASES_BY_NAME["v34-26400-b3200-lapm-originator"]

        self.assertIn("AT+MS=34,0,26400,26400", raw.at_commands)
        self.assertIsNone(raw.minimum_rate)
        self.assertIn("AT+MS=34,0,4800,26400", lapm.at_commands)
        self.assertEqual(lapm.minimum_rate, 4800)


if __name__ == "__main__":
    unittest.main()
