#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.live_campaign_profiles import cases_for_suite  # noqa: E402
from tools.live_campaign_report import (  # noqa: E402
    AttemptResult,
    MATURITY_CONNECT,
    MATURITY_DATA,
    MATURITY_FULL,
    MATURITY_NO_CONNECT,
    MATURITY_RETRAIN,
    OUTCOME_BUSY,
    OUTCOME_COMPLETE,
    base_schedule,
    extra_schedule,
    render_dashboard,
)


class LiveCampaignReportTests(unittest.TestCase):
    def result(self, **changes: object) -> AttemptResult:
        fields: dict[str, object] = {
            "case_name": "v21-300-originator",
            "family": "v21",
            "role": "originator",
            "ordinal": 1,
            "extra": False,
            "physical_try": 1,
            "outcome": OUTCOME_COMPLETE,
            "retrain_requested": True,
        }
        fields.update(changes)
        return AttemptResult(**fields)  # type: ignore[arg-type]

    def test_maturity_buckets_are_mutually_exclusive(self) -> None:
        self.assertEqual(
            self.result(
                connected=True,
                initial_data=True,
                retrain_connected=True,
                retrain_data=True,
            ).maturity,
            MATURITY_FULL,
        )
        self.assertEqual(
            self.result(
                connected=True, initial_data=True, retrain_connected=True
            ).maturity,
            MATURITY_RETRAIN,
        )
        self.assertEqual(
            self.result(connected=True, initial_data=True).maturity,
            MATURITY_DATA,
        )
        self.assertEqual(
            self.result(connected=True).maturity,
            MATURITY_CONNECT,
        )
        self.assertEqual(self.result().maturity, MATURITY_NO_CONNECT)
        self.assertIsNone(self.result(outcome=OUTCOME_BUSY).maturity)

    def test_schedule_is_seeded_and_breadth_first(self) -> None:
        cases = cases_for_suite("smoke")
        first = base_schedule(cases, 3, 1234)
        second = base_schedule(cases, 3, 1234)
        different = base_schedule(cases, 3, 4321)
        self.assertEqual(first, second)
        self.assertNotEqual(first, different)
        for offset in range(0, len(first), len(cases)):
            self.assertEqual(
                {item.case.name for item in first[offset : offset + len(cases)]},
                {case.name for case in cases},
            )

    def test_one_extra_round_only_contains_failed_cases(self) -> None:
        cases = cases_for_suite("smoke")[:3]
        results = [
            self.result(
                case_name=cases[0].name,
                family=cases[0].family,
                connected=True,
                initial_data=True,
                retrain_connected=True,
                retrain_data=True,
            ),
            self.result(case_name=cases[1].name, family=cases[1].family),
            self.result(
                case_name=cases[2].name,
                family=cases[2].family,
                outcome=OUTCOME_BUSY,
            ),
        ]
        extra = extra_schedule(cases, results, 3, 1, 99)
        self.assertEqual([item.case.name for item in extra], [cases[1].name])
        self.assertTrue(extra[0].extra)
        self.assertEqual(extra[0].ordinal, 4)

    def test_dashboard_is_compact_and_excludes_busy_from_done(self) -> None:
        cases = cases_for_suite("smoke")
        results = [
            self.result(
                case_name=cases[0].name,
                family=cases[0].family,
                connected=True,
            ),
            self.result(
                case_name=cases[0].name,
                family=cases[0].family,
                outcome=OUTCOME_BUSY,
                physical_try=2,
            ),
        ]
        dashboard = render_dashboard(
            cases,
            3,
            results,
            revision="a" * 40,
            seed=7,
            elapsed_seconds=12.4,
            current="v21-300-originator / waiting for CONNECT",
            current_seconds=12.6,
        )
        self.assertIn("retries=1", dashboard)
        self.assertIn("  1/3", dashboard)
        self.assertIn(
            "current=v21-300-originator / waiting for CONNECT for=13s",
            dashboard,
        )
        self.assertLessEqual(len(dashboard.splitlines()), 16)
        self.assertLessEqual(max(map(len, dashboard.splitlines())), 100)


if __name__ == "__main__":
    unittest.main()
