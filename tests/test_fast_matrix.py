#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import string
import unittest
from pathlib import Path
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.fast_matrix import (  # noqa: E402
    BASELINE_ORIGINATES,
    CANDIDATE_ORIGINATES,
    FAST_PROFILE_NAMES,
    FULL_DUPLEX,
    MEDIA_CODECS,
    ORIGINATOR_ORDERS,
    PAYLOAD_EPOCHS,
    TURN_TAKING,
    build_fast_matrix,
    payload_pair,
    supported_media_codecs,
)
from tools.live_profiles import (  # noqa: E402
    PROFILES,
    V34_RUNNABLE_RATES_BY_SYMBOL_RATE,
)


class FastMatrixTests(unittest.TestCase):
    def test_matrix_covers_every_supported_rate_codec_and_role(self) -> None:
        matrix = build_fast_matrix()

        self.assertEqual(len(FAST_PROFILE_NAMES), 70)
        self.assertEqual(len(matrix), 280)
        self.assertIn("v22_600", FAST_PROFILE_NAMES)
        self.assertEqual(
            {case.profile.name for case in matrix}, set(FAST_PROFILE_NAMES)
        )
        expected = {
            (name, codec, order)
            for name in FAST_PROFILE_NAMES
            for codec in supported_media_codecs(PROFILES[name])
            for order in ORIGINATOR_ORDERS
        }
        self.assertEqual(
            {
                (case.profile.name, case.media_codec, case.originator_order)
                for case in matrix
            },
            expected,
        )
        self.assertEqual(len({case.name for case in matrix}), len(matrix))

    def test_v34_uses_its_proven_media_paths(self) -> None:
        matrix = build_fast_matrix()
        v34 = [case for case in matrix if case.profile.family == "v34"]

        self.assertEqual(len(v34), 220)
        self.assertEqual({case.media_codec for case in v34}, set(MEDIA_CODECS))
        self.assertEqual(
            {
                (case.profile.symbol_rate, case.profile.receive_rate)
                for case in v34
            },
            {
                (symbol_rate, rate)
                for symbol_rate, rates in (
                    V34_RUNNABLE_RATES_BY_SYMBOL_RATE.items()
                )
                for rate in rates
            },
        )
        self.assertTrue(
            all(
                supported_media_codecs(PROFILES[name]) == MEDIA_CODECS
                for name in FAST_PROFILE_NAMES
            )
        )

    def test_v23_rates_follow_the_originating_implementation(self) -> None:
        cases = {
            case.originator_order: case
            for case in build_fast_matrix(("v23_1200_75",))
            if case.media_codec == "PCMU"
        }

        candidate = cases[CANDIDATE_ORIGINATES]
        self.assertEqual(
            (candidate.candidate_rates.receive, candidate.candidate_rates.transmit),
            (1200, 75),
        )
        self.assertEqual(
            (candidate.baseline_rates.receive, candidate.baseline_rates.transmit),
            (75, 1200),
        )
        baseline = cases[BASELINE_ORIGINATES]
        self.assertEqual(baseline.candidate_rates, candidate.baseline_rates)
        self.assertEqual(baseline.baseline_rates, candidate.candidate_rates)

    def test_v90_rates_follow_analog_and_digital_line_roles(self) -> None:
        cases = {
            case.originator_order: case
            for case in build_fast_matrix(("v90_28000_19200_b3000",))
            if case.media_codec == "PCMU"
        }

        candidate = cases[CANDIDATE_ORIGINATES]
        self.assertEqual(
            (candidate.candidate_rates.receive, candidate.candidate_rates.transmit),
            (28000, 19200),
        )
        self.assertEqual(
            (candidate.baseline_rates.receive, candidate.baseline_rates.transmit),
            (19200, 28000),
        )
        baseline = cases[BASELINE_ORIGINATES]
        self.assertEqual(baseline.candidate_rates, candidate.baseline_rates)
        self.assertEqual(baseline.baseline_rates, candidate.candidate_rates)

    def test_payloads_distinguish_both_directions_and_ato1_epochs(self) -> None:
        seen: set[bytes] = set()
        for case in build_fast_matrix():
            for epoch in PAYLOAD_EPOCHS:
                pair = payload_pair(case, epoch)
                self.assertNotIn(pair.candidate, pair.baseline)
                self.assertNotIn(pair.baseline, pair.candidate)
                self.assertLessEqual(len(pair.candidate), 64)
                self.assertLessEqual(len(pair.baseline), 64)
                self.assertNotIn(pair.candidate, seen)
                self.assertNotIn(pair.baseline, seen)
                seen.update((pair.candidate, pair.baseline))

    def test_baudot_payloads_are_turn_based_and_representable(self) -> None:
        allowed = set((string.ascii_uppercase + string.digits + " ?").encode())
        for case in build_fast_matrix(("baudot_45", "baudot_50")):
            self.assertEqual(case.exchange, TURN_TAKING)
            pair = payload_pair(case, "initial")
            self.assertTrue(set(pair.candidate) <= allowed)
            self.assertTrue(set(pair.baseline) <= allowed)
        self.assertTrue(
            all(
                case.exchange == FULL_DUPLEX
                for case in build_fast_matrix(("v21_300", "v32_9600"))
            )
        )

    def test_invalid_profiles_and_epochs_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "runnable"):
            build_fast_matrix(("v90_down_28000",))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            build_fast_matrix(("v21_300", "v21_300"))
        case = build_fast_matrix(("v21_300",))[0]
        with self.assertRaisesRegex(ValueError, "epoch"):
            payload_pair(case, "later")


if __name__ == "__main__":
    unittest.main()
