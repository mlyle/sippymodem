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
    MATURITY_REQUEST,
    MATURITY_RETRAIN,
    OUTCOME_BUSY,
    OUTCOME_COMPLETE,
    OUTCOME_INFRASTRUCTURE_ERROR,
    OUTCOME_SETUP_ERROR,
    base_schedule,
    extra_schedule,
    peer_input_errors,
    render_dashboard,
)


class LiveCampaignReportTests(unittest.TestCase):
    def test_peer_input_reports_do_not_double_count_cumulative_advisories(self) -> None:
        error = b"ERROR: unknown command; send HELP for a list\r\n"
        advisory = b"ADVISORY: %d invalid commands since the last accepted commands\r\n"
        payload = b"\r\n" + error * 2 + advisory % 4 + advisory % 4 + advisory % 2
        self.assertEqual(peer_input_errors(payload), (2, 4))
        self.assertEqual(peer_input_errors(
            b"ADVISORY: 1 invalid command since the last accepted command\n"
        ), (0, 1))
        self.assertEqual(peer_input_errors(
            b"HELP: " + error + b"ADVISORY: 99999999999 invalid commands "
            b"since the last accepted commands\r\nADVISORY: 4 invalid commands"
        ), (0, 0))

    def test_peer_input_warning_prevents_success_without_erasing_markers(self) -> None:
        case = cases_for_suite("smoke")[0]
        for field in ("peer_unknown_commands", "peer_invalid_command_burst"):
            with self.subTest(field=field):
                result = self.result(
                    case_name=case.name, family=case.family, role=case.role,
                    connected=True, initial_data=True, remote_retrain=True,
                    retrain_connected=True, retrain_data=True, **{field: 4},
                )
                self.assertEqual(result.maturity, MATURITY_FULL)
                self.assertFalse(result.successful())
                dashboard = render_dashboard(
                    (case,), 1, (result,), revision="c" * 40,
                    campaign_id="test", elapsed_seconds=1, recent_limit=3,
                )
                self.assertIn("Peer input warnings: 1 calls", dashboard)
                self.assertIn("peer-input=", dashboard)

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

    def test_schedule_biases_equal_progress_toward_originators(self) -> None:
        representatives = {}
        for case in cases_for_suite("smoke"):
            representatives.setdefault((case.family, case.role), case)
        originators = [
            case
            for (_, role), case in representatives.items()
            if role == "originator"
        ][:2]
        callbacks = [
            case
            for (_, role), case in representatives.items()
            if role == "callback"
        ][:6]
        cases = tuple(originators + callbacks)
        self.assertEqual((len(originators), len(callbacks)), (2, 6))

        originator_first = sum(
            base_schedule(cases, 1, seed)[0].case.role == "originator"
            for seed in range(4096)
        )
        self.assertGreater(originator_first, 4096 * 72 // 100)
        self.assertLess(originator_first, 4096 * 78 // 100)

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
            campaign_id="test-campaign",
            elapsed_seconds=12.4,
            current="v21-300-originator / waiting for CONNECT",
            current_seconds=12.6,
        )
        self.assertIn("retries=1", dashboard)
        self.assertEqual(
            dashboard.splitlines()[0],
            f"revision={'a' * 12} campaign=test-campaign elapsed=12s retries=1",
        )
        self.assertNotIn("seed=", dashboard)
        self.assertIn("  1/3", dashboard)
        self.assertIn(
            "current=v21-300-originator / waiting for CONNECT for=13s",
            dashboard,
        )
        lines = dashboard.splitlines()
        role_header = next(line for line in lines if "Originator" in line)
        column_header = next(line for line in lines if line.startswith("Modulation"))
        first_row = next(line for line in lines if line.startswith("V.21"))
        self.assertEqual(role_header[13:45].strip(), "Originator")
        self.assertEqual(role_header[48:80].strip(), "Callback")
        self.assertEqual(
            column_header[13:45], "done/exp+x   F  R  D  C  Q  N  E"
        )
        self.assertEqual(
            column_header[48:80], "done/exp+x   F  R  D  C  Q  N  E"
        )
        table_lines = (role_header, column_header, first_row)
        self.assertEqual({line.index("|") for line in table_lines}, {46})
        self.assertTrue(all(len(line) == 80 for line in table_lines))
        self.assertTrue(any(line.startswith("V.22bis/r") for line in lines))
        self.assertTrue(any(line.startswith("V.22bis/l") for line in lines))
        self.assertEqual(
            len(lines), len({(case.family, case.link_mode) for case in cases}) + 8
        )
        self.assertLessEqual(max(map(len, dashboard.splitlines())), 100)

    def test_callback_request_has_a_separate_v22bis_lapm_bucket(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v34" and case.role == "callback"
        )
        result = self.result(
            case_name=case.name,
            family=case.family,
            role=case.role,
            callback_request_attempted=True,
            callback_request_maturity=MATURITY_REQUEST,
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (result,),
            revision="b" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=1,
        )
        v22bis = next(
            line
            for line in dashboard.splitlines()
            if line.startswith("V.22bis/l")
        )
        originator = v22bis[13:46].split()
        self.assertEqual(
            originator,
            ["0/0", "+1", "0", "0", "0", "0", "1", "0", "0"],
        )
        callback_label = "V.34/l" if case.link_mode == "lapm" else "V.34/r"
        callback = next(
            line
            for line in dashboard.splitlines()
            if line.startswith(callback_label)
        )[48:80].split()
        self.assertEqual(
            callback,
            ["0/1", "+0", "0", "0", "0", "0", "0", "0"],
        )
        self.assertEqual(extra_schedule((case,), (result,), 1, 1, 9), ())
        self.assertIn(
            "r=raw l=LAPM; + includes failure extras and request legs",
            dashboard,
        )

    def test_callback_counts_after_returning_ring_reaches_answerer(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v34" and case.role == "callback"
        )
        result = self.result(
            case_name=case.name,
            family=case.family,
            role=case.role,
            callback_request_attempted=True,
            callback_request_maturity=MATURITY_REQUEST,
            callback_return_attempted=True,
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (result,),
            revision="b" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=1,
        )
        callback_label = "V.34/l" if case.link_mode == "lapm" else "V.34/r"
        callback = next(
            line
            for line in dashboard.splitlines()
            if line.startswith(callback_label)
        )[48:80].split()
        self.assertEqual(
            callback,
            ["1/1", "+0", "0", "0", "0", "0", "1", "0"],
        )

    def test_return_error_does_not_also_become_request_error(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v34" and case.role == "callback"
        )
        result = self.result(
            case_name=case.name,
            family=case.family,
            role=case.role,
            outcome=OUTCOME_INFRASTRUCTURE_ERROR,
            callback_request_attempted=True,
            callback_request_maturity=MATURITY_REQUEST,
            callback_return_attempted=True,
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (result,),
            revision="b" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=1,
        )
        request = next(
            line
            for line in dashboard.splitlines()
            if line.startswith("V.22bis/l")
        )[13:46].split()
        self.assertEqual(
            request,
            ["0/0", "+1", "0", "0", "0", "0", "1", "0", "0"],
        )
        callback_label = "V.34/l" if case.link_mode == "lapm" else "V.34/r"
        callback = next(
            line
            for line in dashboard.splitlines()
            if line.startswith(callback_label)
        )[48:80].split()
        self.assertEqual(
            callback,
            ["1/1", "+0", "0", "0", "0", "0", "0", "1"],
        )

    def test_raw_and_lapm_results_have_independent_rows(self) -> None:
        raw = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v22bis"
            and case.role == "originator"
            and case.link_mode == "raw"
        )
        lapm = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v22bis"
            and case.role == "originator"
            and case.link_mode == "lapm"
        )
        results = (
            self.result(
                case_name=raw.name,
                family=raw.family,
                link_mode=raw.link_mode,
                connected=True,
                initial_data=True,
            ),
            self.result(
                case_name=lapm.name,
                family=lapm.family,
                link_mode=lapm.link_mode,
                connected=True,
            ),
        )
        dashboard = render_dashboard(
            (raw, lapm),
            1,
            results,
            revision="d" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=2,
        )
        rows = dashboard.splitlines()
        raw_fields = next(
            line for line in rows if line.startswith("V.22bis/r")
        )[13:46].split()
        lapm_fields = next(
            line for line in rows if line.startswith("V.22bis/l")
        )[13:46].split()
        self.assertEqual(
            raw_fields,
            ["1/1", "+0", "0", "0", "1", "0", "0", "0"],
        )
        self.assertEqual(
            lapm_fields,
            ["1/1", "+0", "0", "0", "0", "1", "0", "0", "0"],
        )

    def test_dashboard_recent_calls_are_bounded_and_nonce_free(self) -> None:
        cases = cases_for_suite("smoke")[:3]
        results = tuple(
            self.result(
                case_name=case.name,
                family=case.family,
                role=case.role,
                physical_try=index,
                connected=True,
                initial_data=index > 1,
                remote_retrain_nonce="0123456789abcdef0123456789abcdef",
                duration_seconds=10.0 + index,
            )
            for index, case in enumerate(cases, 1)
        )
        dashboard = render_dashboard(
            cases,
            1,
            results,
            revision="c" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=33,
            recent_limit=2,
        )
        lines = dashboard.splitlines()
        recent = lines[lines.index(
            "Recent calls (newest first): o=originator c=callback q=request l=LAPM r=raw"
        ) + 2 :]
        self.assertEqual(len(recent), 2)
        self.assertTrue(recent[0].startswith("003"))
        self.assertTrue(recent[1].startswith("002"))
        self.assertNotIn("0123456789abcdef", dashboard)
        self.assertLessEqual(max(map(len, recent)), 74)

    def test_recent_call_shows_actual_connected_profile(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v22bis"
            and case.role == "originator"
            and case.link_mode == "lapm"
        )
        result = self.result(
            case_name=case.name,
            family=case.family,
            role=case.role,
            link_mode=case.link_mode,
            connected=True,
            modulation="V.22",
            protocol="none",
            receive_rate=1200,
            transmit_rate=1200,
            detail="CONNECT selected V.22 instead of V.22bis",
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (result,),
            revision="e" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=3,
            recent_limit=2,
        )
        recent, tried = dashboard.splitlines()[-2:]
        self.assertEqual(recent.split()[2], "V.22")
        self.assertIn("1200/1200@-", recent)
        self.assertIn(" r ", recent)
        self.assertTrue(recent.endswith("connect/mismatch"))
        self.assertEqual(tried, "     tried: V.22bis 2400/2400@600 l")

    def test_history_shows_only_explicit_upstream_evidence(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "v92" and case.feature == "pcm-upstream")
        for upstream in (None, "V.34", "PCM", "private-value"):
            with self.subTest(upstream=upstream):
                result = self.result(case_name=case.name, family=case.family,
                                     role=case.role, connected=True, modulation="V.92",
                                     protocol="none", receive_rate=24000,
                                     transmit_rate=56000, upstream_mode=upstream)
                dashboard = render_dashboard((case,), 1, (result,), revision="e" * 40,
                                             campaign_id="test-campaign",
                                             elapsed_seconds=3, recent_limit=1)
                if upstream in {"V.34", "PCM"}:
                    self.assertTrue(dashboard.endswith(f"connect up={upstream}"))
                else:
                    self.assertNotIn("up=", dashboard)
                    self.assertNotIn("private-value", dashboard)

    def test_changed_retrain_profile_does_not_reclassify_initial_results(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "v92" and case.feature == "pcm-upstream"
                    and case.role == "originator" and case.link_mode == "raw")
        initial = dict(modulation="V.92", protocol="none", compression="none",
                       receive_rate=24000, transmit_rate=56000, upstream_mode="PCM")
        common = dict(case_name=case.name, family=case.family, role=case.role,
                      connected=True, initial_data=True, remote_retrain=True,
                      retrain_connected=True, retrain_data=True)

        def dashboard(result: AttemptResult, limit: int = 3) -> str:
            return render_dashboard((case,), 1, (result,), revision="e" * 40,
                                    campaign_id="test-campaign", elapsed_seconds=3,
                                    recent_limit=limit)

        legacy = self.result(**common, **initial)
        unchanged = self.result(**common, **initial,
                                **{"retrain_" + name: value for name, value in initial.items()})
        self.assertEqual(dashboard(unchanged), dashboard(legacy))
        for upstream in ("V.34", None, "private-value"):
            with self.subTest(upstream=upstream):
                post = dict(initial, transmit_rate=52000, upstream_mode=upstream)
                changed = self.result(**common, **initial,
                                      **{"retrain_" + name: value for name, value in post.items()})
                rendered = dashboard(changed)
                self.assertEqual(rendered.split("Recent calls")[0],
                                 dashboard(legacy).split("Recent calls")[0])
                self.assertEqual(changed.maturity, MATURITY_FULL)
                self.assertTrue(changed.successful())
                self.assertIn("full up=PCM", rendered)
                self.assertTrue(rendered.endswith(
                    "     after ATO1: V.92 24000/52000 r up="
                    + ("V.34" if upstream == "V.34" else "?")))
                self.assertNotIn("private-value", rendered)
                bounded = dashboard(changed, 1)
                self.assertNotIn("after ATO1", bounded)
                self.assertFalse(any(line[:3].isdigit() for line in bounded.splitlines()))

        mismatch = self.result(**common, **initial, profile_error="requested rate not selected",
                               retrain_modulation="V.34", retrain_protocol="LAPM",
                               retrain_compression="V.42bis", retrain_receive_rate=28800,
                               retrain_transmit_rate=28800)
        self.assertNotIn("after ATO1", dashboard(mismatch, 2))
        complete = dashboard(mismatch, 3).splitlines()
        self.assertTrue(complete[-3].endswith("full/mismatch up=PCM"))
        self.assertTrue(complete[-2].startswith("     tried:"))
        self.assertEqual(complete[-1],
                         "     after ATO1: V.34 28800/28800 l up=? compression=V.42bis")

    def test_pcm_fallback_credits_v34_without_regrouping_scheduled_progress(self) -> None:
        cases = cases_for_suite("standard")
        for feature in ("analogue-fallback", "digital-headend"):
            with self.subTest(feature=feature):
                case = next(
                    case for case in cases
                    if case.family == "v90"
                    and case.role == "originator"
                    and case.link_mode == "raw"
                    and case.feature == feature
                )
                expected_fallback = feature == "analogue-fallback"
                result = self.result(
                    case_name=case.name,
                    family=case.family,
                    role=case.role,
                    link_mode=case.link_mode,
                    connected=True,
                    modulation="V.34",
                    protocol="none",
                    receive_rate=28800,
                    transmit_rate=28800,
                    initial_data=True,
                    remote_retrain=True,
                    retrain_connected=True,
                    retrain_data=True,
                    profile_error="" if expected_fallback else
                        "CONNECT selected V.34 instead of V.90",
                )
                dashboard = render_dashboard(
                    (case,), 1, (result,), revision="e" * 40,
                    campaign_id="test-campaign", elapsed_seconds=3, recent_limit=2,
                )
                recent = next(
                    line for line in dashboard.splitlines() if line.startswith("001")
                )
                self.assertEqual(recent.split()[2], "V.34")
                self.assertTrue(recent.endswith(
                    "full" if expected_fallback else "full/mismatch"
                ))
                row = next(
                    line for line in dashboard.splitlines()
                    if line.startswith("V.90/r")
                )
                self.assertEqual(row[13:46].split()[:2], ["1/1", "+0"])
                self.assertEqual(row[25:36].split(), ["0"] * 4)
                v34 = next(line for line in dashboard.splitlines()
                           if line.startswith("V.34/r"))
                self.assertEqual(v34[13:45].split(),
                                 ["0/0", "+0", "1", "0", "0", "0", "0", "0"])
                self.assertEqual(result.successful(), expected_fallback)
                self.assertEqual(len(extra_schedule((case,), (result,), 1, 1, 7)),
                                 int(not expected_fallback))

    def test_recent_unconnected_call_shows_requested_profile(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.family == "v22bis"
            and case.role == "originator"
            and case.link_mode == "lapm"
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (
                self.result(
                    case_name=case.name,
                    family=case.family,
                    role=case.role,
                    link_mode=case.link_mode,
                ),
            ),
            revision="f" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=3,
            recent_limit=1,
        )
        recent = dashboard.splitlines()[-1]
        self.assertEqual(recent.split()[2], "V.22bis")
        self.assertIn("2400/2400@600", recent)
        self.assertIn(" l ", recent)

    def test_recent_callback_request_profile_mismatch_is_explicit(self) -> None:
        case = next(
            case
            for case in cases_for_suite("smoke")
            if case.role == "callback"
        )
        dashboard = render_dashboard(
            (case,),
            1,
            (
                self.result(
                    case_name=case.name,
                    family=case.family,
                    role=case.role,
                    callback_request_attempted=True,
                    callback_request_maturity=MATURITY_CONNECT,
                    detail=(
                        "callback request CONNECT selected V.21 instead of "
                        "V.22bis"
                    ),
                ),
            ),
            revision="1" * 40,
            campaign_id="test-campaign",
            elapsed_seconds=3,
            recent_limit=2,
        )
        recent, tried = dashboard.splitlines()[-2:]
        self.assertTrue(recent.endswith("connect/mismatch"))
        self.assertEqual(recent.split()[1:4], ["q", "?", "-/-@-"])
        self.assertEqual(
            tried, "     tried: V.22bis 2400/2400@600 l (callback request)"
        )
        lines = dashboard.splitlines()
        index = next(index for index, line in enumerate(lines)
                     if line.startswith("V.22bis/l"))
        self.assertEqual(lines[index + 1][25:36].split(), ["0", "0", "0", "1"])
        self.assertEqual(lines[index][25:36].split(), ["0"] * 4)
        unknown = next(line for line in lines if line.startswith("?/?"))
        self.assertEqual(unknown[25:36].split(), ["0", "0", "0", "1"])

    def test_zero_mismatch_rows_are_hidden(self) -> None:
        cases = cases_for_suite("standard")
        for results in ((), (self.result(connected=True, initial_data=True),),
                        (self.result(connected=True, profile_error="profile mismatch",
                                     outcome=OUTCOME_INFRASTRUCTURE_ERROR),)):
            with self.subTest(results=results):
                dashboard = render_dashboard(
                    cases, 1, results, revision="a",
                    campaign_id="test-campaign", elapsed_seconds=1,
                )
                self.assertFalse(any(line.startswith("  mismatch")
                                     for line in dashboard.splitlines()))

    def test_mismatch_row_visibility_is_specific_to_modulation_and_link_mode(self) -> None:
        cases = cases_for_suite("standard")
        for role in ("originator", "callback"):
            for link_mode, suffix in (("raw", "r"), ("lapm", "l")):
                with self.subTest(role=role, link_mode=link_mode):
                    case = next(case for case in cases if case.family == "v90"
                                and case.role == role and case.link_mode == link_mode)
                    result = self.result(
                        case_name=case.name, family=case.family, role=role,
                        link_mode=link_mode, connected=True, initial_data=True,
                        modulation="V.34", protocol="none",
                        profile_error="CONNECT selected V.34 instead of V.90",
                    )
                    lines = render_dashboard(
                        cases, 1, (result,), revision="a",
                        campaign_id="test-campaign", elapsed_seconds=1,
                    ).splitlines()
                    mismatch_rows = [i for i, line in enumerate(lines)
                                     if line.startswith("  mismatch")]
                    self.assertEqual(len(mismatch_rows), 1)
                    index = mismatch_rows[0]
                    self.assertTrue(lines[index - 1].startswith(f"V.90/{suffix}"))
                    expected = ["0", "0", "1", "0"]
                    self.assertEqual(lines[index][25:36].split(),
                                     expected if role == "originator" else ["0"] * 4)
                    self.assertEqual(lines[index][60:71].split(),
                                     expected if role == "callback" else ["0"] * 4)

    def test_mismatch_rows_preserve_requested_buckets_and_credit_actual_once(self) -> None:
        cases = tuple(case for case in cases_for_suite("standard")
                      if case.family == "v90" and case.link_mode == "raw"
                      and case.feature == "digital-headend")[:2]
        self.assertEqual({case.role for case in cases}, {"originator", "callback"})
        results = []
        for case in cases:
            for maturity in (MATURITY_FULL, MATURITY_RETRAIN, MATURITY_DATA, MATURITY_CONNECT):
                results.append(self.result(
                    case_name=case.name, family=case.family, role=case.role,
                    link_mode=case.link_mode, connected=True,
                    initial_data=maturity != MATURITY_CONNECT,
                    remote_retrain=maturity in {MATURITY_FULL, MATURITY_RETRAIN},
                    retrain_connected=maturity in {MATURITY_FULL, MATURITY_RETRAIN},
                    retrain_data=maturity == MATURITY_FULL,
                    extra=maturity == MATURITY_DATA,
                    modulation="V.34", protocol="none",
                    profile_error="CONNECT selected V.34 instead of V.90",
                ))
            for outcome in (OUTCOME_BUSY, OUTCOME_INFRASTRUCTURE_ERROR):
                results.append(self.result(
                    case_name=case.name, family=case.family, role=case.role,
                    outcome=outcome, connected=True,
                    detail="CONNECT selected V.34 instead of V.90",
                ))
        dashboard = render_dashboard(
            cases, 3, results, revision="a",
            campaign_id="test-campaign", elapsed_seconds=1,
        )
        lines = dashboard.splitlines()
        index = next(i for i, line in enumerate(lines) if line.startswith("V.90/r"))
        mismatch = lines[index + 1]
        self.assertTrue(mismatch.startswith("  mismatch"))
        self.assertEqual(len(mismatch), 80)
        self.assertEqual(mismatch[13:45].split(), ["1", "1", "1", "1"])
        self.assertEqual(mismatch[48:80].split(), ["1", "1", "1", "1"])
        self.assertEqual(lines[index][13:45].split(),
                         ["4/3", "+1", "0", "0", "0", "0", "0", "1"])
        actual = next(line for line in lines if line.startswith("V.34/r"))
        for side in (slice(13, 45), slice(48, 80)):
            self.assertEqual(actual[side].split(),
                             ["0/0", "+0", "1", "1", "1", "1", "0", "0"])
            total = next(line for line in lines if line.startswith("TOTAL"))
            self.assertEqual(total[side].split(),
                             ["4/3", "+1", "1", "1", "1", "1", "0", "1"])

    def test_request_cells_are_blank_only_where_inapplicable(self) -> None:
        cases = cases_for_suite("smoke")
        dashboard = render_dashboard(
            cases, 1, (), revision="a", campaign_id="test-campaign", elapsed_seconds=0,
        )
        for line in dashboard.splitlines():
            if "/" not in line[:12] and not line.startswith("TOTAL"):
                continue
            self.assertEqual(line[72:74], "  ")
            self.assertEqual(
                line[37:39],
                " 0" if line.startswith(("V.22bis/l", "TOTAL")) else "  ",
            )

    def test_rate_and_link_mismatches_credit_the_actual_group_once(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "v90" and case.role == "originator"
                    and case.link_mode == "lapm" and case.feature == "digital-headend")
        for protocol, label in (("LAPM", "V.90/l"), ("none", "V.90/r"),
                                (None, "V.90/?")):
            with self.subTest(protocol=protocol):
                result = self.result(
                    case_name=case.name, family=case.family, role=case.role,
                    link_mode=case.link_mode, connected=True, initial_data=True,
                    modulation="V.90", protocol=protocol, transmit_rate=24000,
                    profile_error="CONNECT selected TX 24000 instead of 28000",
                    detail="remote retrain END marker was not verified",
                )
                lines = render_dashboard(
                    (case,), 1, (result,), revision="a",
                    campaign_id="test-campaign", elapsed_seconds=1,
                ).splitlines()
                actual = next(line for line in lines if line.startswith(label))
                self.assertEqual(actual[25:36].split(), ["0", "0", "1", "0"])
                index = next(i for i, line in enumerate(lines) if line.startswith("V.90/l"))
                self.assertEqual(lines[index + 1][25:36].split(), ["0", "0", "1", "0"])
                total = next(line for line in lines if line.startswith("TOTAL"))
                self.assertEqual(total[13:45].split(),
                                 ["1/1", "+0", "0", "0", "1", "0", "0", "0"])

    def test_request_and_return_mismatches_are_independent_observations(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "v90" and case.role == "callback"
                    and case.link_mode == "lapm" and case.feature == "digital-headend")
        result = self.result(
            case_name=case.name, family=case.family, role=case.role,
            link_mode=case.link_mode, connected=True, initial_data=True,
            modulation="V.34", protocol="none",
            profile_error="CONNECT selected V.34 instead of V.90",
            detail="remote retrain END marker was not verified",
            callback_request_attempted=True, callback_return_attempted=True,
            callback_request_maturity=MATURITY_REQUEST,
            callback_request_modulation="V.22", callback_request_protocol="none",
            callback_request_profile_error="CONNECT selected V.22 instead of V.22bis",
        )
        lines = render_dashboard(
            (case,), 1, (result,), revision="a",
            campaign_id="test-campaign", elapsed_seconds=1,
            recent_limit=4,
        ).splitlines()
        index = next(i for i, line in enumerate(lines) if line.startswith("V.22bis/l"))
        self.assertEqual(lines[index][13:45].split(),
                         ["0/0", "+1", "0", "0", "0", "0", "0", "0", "0"])
        self.assertEqual(lines[index + 1][25:39].split(), ["0", "0", "0", "0", "1"])
        request = next(line for line in lines if line.startswith("V.22/r"))
        self.assertEqual(request[37:39], " 1")
        actual = next(line for line in lines if line.startswith("V.34/r"))
        self.assertEqual(actual[60:71].split(), ["0", "0", "1", "0"])
        index = next(i for i, line in enumerate(lines) if line.startswith("V.90/l"))
        self.assertEqual(lines[index][60:71].split(), ["0"] * 4)
        self.assertEqual(lines[index + 1][60:71].split(), ["0", "0", "1", "0"])
        total = next(line for line in lines if line.startswith("TOTAL"))
        self.assertEqual(total[13:45].split(),
                         ["0/0", "+1", "0", "0", "0", "0", "1", "0", "0"])
        self.assertEqual(total[48:80].split(),
                         ["1/1", "+0", "0", "0", "1", "0", "0", "0"])
        self.assertTrue(lines[-4].endswith("data/mismatch"))
        self.assertTrue(lines[-2].endswith("request/mismatch"))
        self.assertEqual(sum(line.startswith("     tried:") for line in lines), 2)

    def test_bell_212_connect_label_maps_to_existing_family(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "bell212" and case.link_mode == "raw"
                    and case.role == "originator")
        result = self.result(
            case_name=case.name, family=case.family, role=case.role,
            link_mode=case.link_mode, connected=True,
            modulation="Bell 212A", protocol="none",
        )
        lines = render_dashboard(
            (case,), 1, (result,), revision="a",
            campaign_id="test-campaign", elapsed_seconds=1,
        ).splitlines()
        row = next(line for line in lines if line.startswith("Bell 212/r"))
        self.assertEqual(row[25:36].split(), ["0", "0", "0", "1"])
        self.assertFalse(any(line.startswith("?/") for line in lines))

    def test_history_includes_request_legs_with_a_row_limit(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "v90" and case.role == "callback"
                    and case.feature == "digital-headend")
        results = (
            self.result(case_name=case.name, family=case.family, role=case.role,
                        callback_request_attempted=True, outcome=OUTCOME_BUSY),
            self.result(case_name=case.name, family=case.family, role=case.role,
                        callback_request_attempted=True,
                        callback_request_maturity=MATURITY_REQUEST,
                        callback_return_attempted=True, connected=True,
                        modulation="V.34", protocol="none",
                        receive_rate=28800, transmit_rate=28800,
                        detail="CONNECT selected V.34 instead of V.90"),
        )
        dashboard = render_dashboard(
            (case,), 1, results, revision="a",
            campaign_id="test-campaign", elapsed_seconds=1,
            recent_limit=4,
        )
        recent = [line for line in dashboard.splitlines() if line[:1].isdigit()]
        self.assertEqual([line.split()[:3] for line in recent],
                         [["002", "c", "V.34"], ["002", "q", "V.22bis"],
                          ["001", "q", "V.22bis"]])
        self.assertIn("Q1 C1 D1 R0 L0 P0", recent[1])
        self.assertTrue(recent[1].endswith(" request"))
        self.assertTrue(recent[2].endswith("request-busy"))
        self.assertIn("     tried: V.90 4800-14400/28000@3000 r", dashboard)
        bounded = render_dashboard(
            (case,), 1, results, revision="a",
            campaign_id="test-campaign", elapsed_seconds=1,
            recent_limit=2,
        )
        self.assertEqual(sum(line[:1].isdigit() for line in bounded.splitlines()), 1)
        self.assertTrue(bounded.splitlines()[-1].startswith("     tried:"))

    def test_history_uses_the_saved_request_profile(self) -> None:
        case = next(case for case in cases_for_suite("standard")
                    if case.family == "v90" and case.role == "callback")
        result = self.result(
            case_name=case.name, family=case.family, role=case.role,
            callback_request_attempted=True,
            callback_request_maturity=MATURITY_CONNECT,
            callback_request_modulation="V.22",
            callback_request_protocol="none",
            callback_request_receive_rate=1200,
            callback_request_transmit_rate=1200,
            detail="callback request CONNECT selected V.22 instead of V.22bis",
        )
        dashboard = render_dashboard(
            (case,), 1, (result,), revision="a",
            campaign_id="test-campaign", elapsed_seconds=1,
            recent_limit=2,
        )
        recent, tried = dashboard.splitlines()[-2:]
        self.assertEqual(recent.split()[1:5], ["q", "V.22", "1200/1200@-", "r"])
        self.assertTrue(recent.endswith("connect/mismatch"))
        self.assertEqual(tried, "     tried: V.22bis 2400/2400@600 l (callback request)")


if __name__ == "__main__":
    unittest.main()
