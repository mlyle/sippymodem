#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

from collections import Counter
from fractions import Fraction
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
    OUTCOME_SETUP_ERROR,
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
                remote_retrain=True,
                retrain_connected=True,
                retrain_data=True,
            ).maturity,
            MATURITY_FULL,
        )
        self.assertEqual(
            self.result(
                connected=True,
                initial_data=True,
                remote_retrain=True,
                retrain_connected=True,
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

    def test_schedule_randomizes_only_among_least_complete_groups(self) -> None:
        cases = cases_for_suite("smoke")
        attempts = 3
        schedule = base_schedule(cases, attempts, 1234)
        expected = Counter((case.family, case.role) for case in cases)
        expected = Counter(
            {group: count * attempts for group, count in expected.items()}
        )
        completed: Counter[tuple[str, str]] = Counter()
        case_completed: Counter[str] = Counter()

        for item in schedule:
            group = (item.case.family, item.case.role)
            active = [
                candidate
                for candidate in expected
                if completed[candidate] < expected[candidate]
            ]
            least_complete = min(
                Fraction(completed[candidate], expected[candidate])
                for candidate in active
            )
            self.assertEqual(
                Fraction(completed[group], expected[group]), least_complete
            )
            group_case_counts = [
                case_completed[case.name]
                for case in cases
                if (case.family, case.role) == group
                and case_completed[case.name] < attempts
            ]
            self.assertEqual(
                case_completed[item.case.name], min(group_case_counts)
            )
            completed[group] += 1
            case_completed[item.case.name] += 1

    def test_one_extra_round_only_contains_failed_cases(self) -> None:
        cases = cases_for_suite("smoke")[:3]
        results = [
            self.result(
                case_name=cases[0].name,
                family=cases[0].family,
                connected=True,
                initial_data=True,
                remote_retrain=True,
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

    def test_local_setup_failures_do_not_get_live_failure_extras(self) -> None:
        case = cases_for_suite("smoke")[0]
        results = [
            self.result(
                case_name=case.name,
                family=case.family,
                outcome=OUTCOME_SETUP_ERROR,
            )
        ]
        self.assertEqual(extra_schedule((case,), results, 3, 1, 99), ())

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
        lines = dashboard.splitlines()
        role_header = next(line for line in lines if "Originator" in line)
        column_header = next(line for line in lines if line.startswith("Modulation"))
        first_row = next(line for line in lines if line.startswith("V.21"))
        self.assertEqual(role_header[13:42].strip(), "Originator")
        self.assertEqual(role_header[45:74].strip(), "Callback")
        self.assertEqual(column_header[13:42], "done/exp+x   F  R  D  C  N  E")
        self.assertEqual(column_header[45:74], "done/exp+x   F  R  D  C  N  E")
        table_lines = (role_header, column_header, first_row)
        self.assertEqual({line.index("|") for line in table_lines}, {43})
        self.assertTrue(all(len(line) == 74 for line in table_lines))
        self.assertLessEqual(len(lines), 17)
        self.assertLessEqual(max(map(len, dashboard.splitlines())), 100)

    def test_callback_request_connect_is_a_v22bis_observation(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v34" and case.role == "callback"
        )
        result = self.result(
            case_name=case.name,
            family=case.family,
            role=case.role,
            callback_request_maturity=MATURITY_CONNECT,
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (result,),
            revision="b" * 40,
            seed=9,
            elapsed_seconds=1,
        )
        v22bis = next(
            line for line in dashboard.splitlines() if line.startswith("V.22bis")
        )
        originator = v22bis[13:43].split()
        self.assertEqual(originator, ["0/0", "+1", "0", "0", "0", "1", "0", "0"])
        self.assertIn(
            "+ includes failure extras and V.22bis callback-request observations",
            dashboard,
        )


if __name__ == "__main__":
    unittest.main()
