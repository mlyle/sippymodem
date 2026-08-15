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
    FAMILY_ORDER,
    ROLES,
    cases_for_suite,
    select_cases,
)


class LiveCampaignProfileTests(unittest.TestCase):
    def test_curated_suite_sizes_are_bounded(self) -> None:
        self.assertEqual(len(cases_for_suite("smoke")), 12)
        self.assertEqual(len(cases_for_suite("standard")), 53)
        self.assertEqual(len(cases_for_suite("extended")), 76)

    def test_cases_have_safe_unique_generated_commands(self) -> None:
        self.assertEqual(len(CASES), len({case.name for case in CASES}))
        self.assertTrue({case.family for case in CASES} <= set(FAMILY_ORDER))
        for case in CASES:
            self.assertIn(case.role, ROLES)
            self.assertTrue(case.at_commands)
            for command in case.at_commands:
                self.assertTrue(command.startswith("AT"), case.name)
                command.encode("ascii")

    def test_standard_crosses_v34_cadence_boundaries_in_both_roles(self) -> None:
        v34 = [case for case in cases_for_suite("standard") if case.family == "v34"]
        self.assertEqual({case.symbol_rate for case in v34}, {2400, 2800, 3000, 3200, 3429})
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
        self.assertEqual({case.transmit_rate for case in pcm}, {24000, 36000, 48000})
        for rate in {24000, 36000, 48000}:
            self.assertEqual(
                {case.role for case in pcm if case.transmit_rate == rate},
                set(ROLES),
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
            ("v34-9600-b2400-callback",),
        )

        explicit = select_cases(
            "smoke", names=("v22-600-callback",)
        )
        self.assertEqual(
            tuple(case.name for case in explicit),
            ("v22-600-callback",),
        )


if __name__ == "__main__":
    unittest.main()
