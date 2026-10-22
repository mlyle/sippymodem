#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import os
from pathlib import Path
import re
import sys
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.live_terminal import (  # noqa: E402
    LiveTerminalError,
    ModemTerminal,
    ONLINE_CAPTURE_MAX,
    ONLINE_INITIAL_RESPONSE,
    ONLINE_PING_REQUEST,
    REMOTE_RETRAIN_REQUEST,
    ResponseDecoder,
    read_published_device,
)


class LiveTerminalTests(unittest.TestCase):
    def run_stream(self, chunks, operation):
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 48000\r\n")
        chunks = iter(chunks)

        def read(_descriptor, limit):
            data = next(chunks)
            self.assertLessEqual(len(data), limit)
            return data

        with (
            mock.patch.object(terminal, "_write"),
            mock.patch("tools.live_terminal._wait_for_descriptor"),
            mock.patch("tools.live_terminal.os.read", side_effect=read),
        ):
            return operation(terminal)

    def test_online_payload_survives_long_raw_transition(self) -> None:
        expected = bytes(range(256)) * 2
        # Both sides of the half-window boundary, with every possible split
        # of the longest supported exact payload. No bytes may be skipped.
        chunk = ONLINE_CAPTURE_MAX // 2
        for split in range(1, len(expected)):
            with self.subTest(split=split):
                verified, received = self.run_stream(
                    [b"x" * chunk] * 7
                    + [b"x" * (chunk - split) + expected[:split],
                       expected[split:]],
                    lambda terminal: terminal.exchange_online(b"", expected, 1.0),
                )
                self.assertTrue(verified)
                self.assertTrue(received.endswith(expected))
                self.assertLessEqual(len(received), ONLINE_CAPTURE_MAX)

    def test_online_noise_keeps_original_deadline_and_requires_exact_data(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 48000\r\n")
        noise = b"x" * (ONLINE_CAPTURE_MAX // 2)
        with (
            mock.patch("tools.live_terminal.time.monotonic", return_value=12.0),
            mock.patch("tools.live_terminal._wait_for_descriptor",
                       side_effect=[None] * 20 + [TimeoutError]) as wait,
            mock.patch("tools.live_terminal.os.read", return_value=noise),
        ):
            verified, received = terminal.exchange_online(b"", b"xxxYxxx", 2.0)
        self.assertFalse(verified)
        self.assertTrue(received)
        self.assertLessEqual(len(received), ONLINE_CAPTURE_MAX)
        self.assertEqual(wait.call_count, 21)
        self.assertTrue(all(call.args[-1] == 14.0 for call in wait.call_args_list))

    def test_rolling_window_does_not_invent_line_start(self) -> None:
        nonce = b"a" * 32
        start = b"GETTY:RETRAIN:START:" + nonce + b"\r\n"
        chunk = ONLINE_CAPTURE_MAX // 2
        # This mid-line marker moves to offset zero only after clipping.
        # A later real line boundary must still be required and recognized.
        false_start = start + b"x" * (chunk - len(start))
        observed, received = self.run_stream(
            [b"x" * chunk, false_start, b"x" * chunk, b"\r\n" + start],
            lambda terminal: terminal.start_remote_retrain(1.0),
        )
        self.assertEqual(observed, nonce)
        self.assertTrue(received.endswith(b"\r\n" + start))

    def test_retrain_continuation_tracks_new_bytes_after_clipping(self) -> None:
        nonce = b"a" * 32
        start = b"GETTY:RETRAIN:START:" + nonce + b"\r\n"
        end = b"GETTY:RETRAIN:END:" + nonce + b":1234\r\n"
        noise = b"x" * (ONLINE_CAPTURE_MAX // 2)
        report = self.run_stream(
            [noise] * 8 + [end[:20], end[20:]],
            lambda terminal: terminal.complete_remote_retrain(nonce, start, 1.0),
        )
        self.assertTrue(report.complete)
        self.assertTrue(report.continuation.endswith(end))
        self.assertNotIn(start, report.continuation)
        self.assertLessEqual(len(report.continuation), ONLINE_CAPTURE_MAX)

        # An END split between the seed and a fresh read must retain context
        # for matching, but must not duplicate that context in the capture.
        for prefix in (start, b"x" * (ONLINE_CAPTURE_MAX - 20)):
            report = self.run_stream(
                [end[20:]],
                lambda terminal: terminal.complete_remote_retrain(
                    nonce, prefix + end[:20], 1.0),
            )
            self.assertTrue(report.complete)
            self.assertEqual(report.continuation, end[20:])

    def test_retrain_nonce_comes_from_the_qualified_match(self) -> None:
        chunk = ONLINE_CAPTURE_MAX // 2
        false_start = b"GETTY:RETRAIN:START:" + b"a" * 32 + b"\r\n"
        true_start = b"GETTY:RETRAIN:START:" + b"b" * 32 + b"\r\n"
        nonce, received = self.run_stream(
            [b"x" * chunk,
             false_start + b"x" * (chunk - len(false_start)),
             b"\r\n" + true_start],
            lambda terminal: terminal.start_remote_retrain(1.0),
        )
        # Clipping retains the mid-line marker at offset zero. Extraction
        # must use the original qualified match, not search this tail again.
        self.assertTrue(received.startswith(false_start))
        self.assertEqual(nonce, b"b" * 32)

    def test_buffered_online_bytes_are_not_lost_after_a_match(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 48000\r\n")
        chunk = ONLINE_CAPTURE_MAX // 2
        first = b"x" * (chunk - 5) + b"FIRST"
        second = b"y" * (chunk - 6) + b"SECOND"
        terminal._decoder.feed(first + second)
        with mock.patch("tools.live_terminal._wait_for_descriptor") as wait:
            self.assertEqual(terminal.exchange_online(b"", b"FIRST", 1.0), (True, first))
            self.assertEqual(terminal.exchange_online(b"", b"SECOND", 1.0), (True, second))
        wait.assert_not_called()

    def test_completed_retrain_seed_leaves_buffered_data_for_next_exchange(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 48000\r\nnext-payload")
        nonce = b"a" * 32
        end = b"GETTY:RETRAIN:END:" + nonce + b":1234\r\n"
        report = terminal.complete_remote_retrain(nonce, end, 1.0)
        self.assertTrue(report.complete)
        self.assertEqual(report.continuation, b"")
        self.assertEqual(terminal.exchange_online(b"", b"next-payload", 1.0),
                         (True, b"next-payload"))

    def test_upstream_report_is_explicit_allowlisted_and_reset(self) -> None:
        decoder = ResponseDecoder()
        for value, expected in ((b"PCM", "PCM"), (b"V34", "V.34"),
                                (b"sip:private@example.invalid", None), (b"", None)):
            with self.subTest(value=value):
                decoder.begin_command_response()
                decoder.feed(b"Modulation: V92\r\nUpstream: " + value
                             + b"\r\nCONNECT 24000\r\n")
                self.assertEqual(decoder.connect_report.upstream_mode, expected)
        decoder.begin_command_response()
        decoder.feed(b"Modulation: V92\r\nCONNECT 48000\r\n")
        self.assertIsNone(decoder.connect_report.upstream_mode)

    def test_startup_report_is_observed_allowlisted_and_reset(self) -> None:
        decoder = ResponseDecoder()
        for value, expected in ((b"QC", "QC"), (b"V8", "V.8"),
                                (b"V8-FALLBACK", "V.8 fallback"),
                                (b"private-peer", None)):
            decoder.begin_command_response()
            decoder.feed(b"Startup: " + value + b"\r\nCONNECT 52000\r\n")
            self.assertEqual(decoder.connect_report.startup, expected)
        decoder.begin_command_response()
        decoder.feed(b"CONNECT 52000\r\n")
        self.assertIsNone(decoder.connect_report.startup)

    def test_response_decoder_allowlists_high_speed_profile_labels(self) -> None:
        for wire_name, expected in (
            (b"V34", "V.34"),
            (b"V90", "V.90"),
            (b"V91", "V.91"),
            (b"V92", "V.92"),
        ):
            with self.subTest(wire_name=wire_name):
                decoder = ResponseDecoder()
                decoder.feed(b"Modulation: " + wire_name + b"\r\nCONNECT 9600\r\n")
                self.assertEqual(decoder.pop(), "connect")
                self.assertEqual(decoder.connect_report.modulation, expected)

    def test_hangup_only_sends_local_escape_and_ath0(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        events = []
        with (
            mock.patch("tools.live_terminal.time.sleep",
                       side_effect=lambda seconds: events.append(seconds)),
            mock.patch("tools.live_terminal.time.monotonic", return_value=10.0),
            mock.patch.object(terminal, "_write",
                              side_effect=lambda data, deadline: events.append(data)) as write,
            mock.patch.object(
                terminal, "_read_result", side_effect=("ok", "ok")
            ),
        ):
            self.assertTrue(terminal.escape_and_hang_up())
        self.assertEqual(events, [1.1, b"+++", b"ATH0\r"])
        self.assertEqual(write.call_args_list[0], mock.call(b"+++", 13.0))

    def test_hangup_in_command_mode_or_training_does_not_escape(self) -> None:
        for response in ("ok", "no-carrier", "error"):
            with self.subTest(response=response):
                terminal = ModemTerminal(-1)
                with (
                    mock.patch("tools.live_terminal.time.sleep") as sleep,
                    mock.patch.object(terminal, "_write") as write,
                    mock.patch.object(
                        terminal, "_read_result", return_value=response
                    ),
                ):
                    self.assertEqual(
                        terminal.escape_and_hang_up(), response != "error"
                    )
                sleep.assert_not_called()
                self.assertEqual(write.call_args.args[0], b"ATH0\r")
                self.assertEqual(write.call_count, 1)

    def test_online_hangup_reports_escape_and_command_failures(self) -> None:
        for responses, expected, writes in (
            (("no-carrier",), True, [b"+++"]),
            (("error",), False, [b"+++"]),
            ((TimeoutError,), False, [b"+++"]),
            (("ok", "error"), False, [b"+++", b"ATH0\r"]),
            (("ok", TimeoutError), False, [b"+++", b"ATH0\r"]),
        ):
            with self.subTest(responses=responses):
                terminal = ModemTerminal(-1)
                terminal._decoder.feed(b"CONNECT 300\r\n")
                with (
                    mock.patch("tools.live_terminal.time.sleep"),
                    mock.patch.object(terminal, "_write") as write,
                    mock.patch.object(
                        terminal, "_read_result", side_effect=responses
                    ),
                ):
                    self.assertEqual(terminal.escape_and_hang_up(), expected)
                self.assertEqual(
                    [call.args[0] for call in write.call_args_list], writes
                )

    def test_hangup_requires_a_fresh_response(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"OK\r\n")
        with (
            mock.patch.object(terminal, "_write"),
            mock.patch(
                "tools.live_terminal._wait_for_descriptor",
                side_effect=TimeoutError,
            ),
        ):
            self.assertFalse(terminal.escape_and_hang_up())

    def test_hangup_allows_both_link_release_deadlines(self) -> None:
        terminal = ModemTerminal(-1)
        with mock.patch.object(terminal, "command", return_value="ok") as command:
            self.assertTrue(terminal.escape_and_hang_up())
        command.assert_called_once_with("ATH0", 5.0)

    def test_hangup_reports_a_closed_terminal(self) -> None:
        terminal = ModemTerminal(-1)
        with mock.patch.object(terminal, "_write", side_effect=OSError):
            self.assertFalse(terminal.escape_and_hang_up())

    def test_ping_probe_requires_a_complete_structured_response(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        response = (
            b"GETTY:PONG:0123456789abcdef0123456789abcdef:"
            b"1783737600123\r\n"
        )
        with (
            mock.patch.object(terminal, "_write") as write,
            mock.patch(
                "tools.live_terminal._wait_for_descriptor"
            ),
            mock.patch(
                "tools.live_terminal.os.read",
                side_effect=(response[:-2], response[-2:]),
            ),
        ):
            verified, received = terminal.probe_online_ping(1.0)

        self.assertTrue(verified)
        self.assertEqual(received, response)
        self.assertEqual(write.call_args.args[0], ONLINE_PING_REQUEST)

        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        malformed = (
            b"GETTY:PONG:0123456789abcdef0123456789abcdef:"
            b"not-a-time\r\n"
        )
        with (
            mock.patch.object(terminal, "_write"),
            mock.patch(
                "tools.live_terminal._wait_for_descriptor",
                side_effect=(None, TimeoutError),
            ),
            mock.patch(
                "tools.live_terminal.os.read",
                return_value=malformed,
            ),
        ):
            verified, received = terminal.probe_online_ping(1.0)

        self.assertFalse(verified)
        self.assertEqual(received, malformed)

    def test_initial_probe_uses_the_established_fixed_payload(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        with (
            mock.patch.object(terminal, "_write") as write,
            mock.patch("tools.live_terminal._wait_for_descriptor"),
            mock.patch(
                "tools.live_terminal.os.read",
                side_effect=(
                    ONLINE_INITIAL_RESPONSE[:17],
                    ONLINE_INITIAL_RESPONSE[17:],
                ),
            ),
        ):
            verified, received = terminal.probe_initial_data(1.0)
        self.assertTrue(verified)
        self.assertEqual(received, ONLINE_INITIAL_RESPONSE)
        write.assert_not_called()

    def test_remote_retrain_requires_matching_complete_markers(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 14400\r\n")
        nonce = b"0123456789abcdef0123456789abcdef"
        start = b"GETTY:RETRAIN:START:" + nonce + b"\r\n"
        end = b"+++GETTY:RETRAIN:END:" + nonce + b":1834\r\n"
        with (
            mock.patch.object(terminal, "_write") as write,
            mock.patch("tools.live_terminal._wait_for_descriptor"),
            mock.patch(
                "tools.live_terminal.os.read",
                side_effect=(start, end),
            ),
        ):
            observed, received = terminal.start_remote_retrain(1.0)
            report = terminal.complete_remote_retrain(
                observed, received, 1.0
            )
        self.assertEqual(observed, nonce)
        self.assertEqual(received, start)
        self.assertTrue(report.complete)
        self.assertEqual(report.continuation, end)
        self.assertEqual(write.call_args.args[0], REMOTE_RETRAIN_REQUEST)

        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 14400\r\n")
        mismatch = start.replace(nonce, b"f" * 32) + end[3:]
        with (
            mock.patch.object(terminal, "_write"),
            mock.patch(
                "tools.live_terminal._wait_for_descriptor",
                side_effect=(None, TimeoutError),
            ),
            mock.patch("tools.live_terminal.os.read", return_value=mismatch),
        ):
            observed, received = terminal.start_remote_retrain(1.0)
            report = terminal.complete_remote_retrain(
                observed, received, 1.0
            )
        self.assertEqual(observed, b"f" * 32)
        self.assertEqual(received, mismatch)
        self.assertFalse(report.complete)
        self.assertEqual(report.continuation, b"")

        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 14400\r\n")
        with (
            mock.patch.object(terminal, "_write"),
            mock.patch(
                "tools.live_terminal._wait_for_descriptor",
                side_effect=(None, None, None),
            ) as wait,
            mock.patch(
                "tools.live_terminal.os.read",
                side_effect=(start, b"+++", end[3:]),
            ),
        ):
            observed, received = terminal.start_remote_retrain(1.0)
            report = terminal.complete_remote_retrain(
                observed, received, 1.0
            )
        self.assertEqual(observed, nonce)
        self.assertTrue(report.complete)
        self.assertEqual(report.continuation, end)
        self.assertEqual(wait.call_count, 3)

    def test_online_send_requires_connected_bounded_data(self) -> None:
        terminal = ModemTerminal(-1)
        with self.assertRaises(LiveTerminalError):
            terminal.send_online(b"data", 1.0)

        terminal._decoder.feed(b"CONNECT 300\r\n")
        with mock.patch.object(terminal, "_write") as write:
            terminal.send_online(b"data", 1.0)
        self.assertEqual(write.call_args.args[0], b"data")

        for data, timeout in ((b"", 1.0), (b"x" * 513, 1.0), (b"x", 0.0)):
            with self.subTest(size=len(data), timeout=timeout):
                with self.assertRaises(LiveTerminalError):
                    terminal.send_online(data, timeout)

    def test_online_pattern_waits_for_a_complete_fragmented_marker(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 2400\r\n")
        marker = re.compile(
            rb"GETTY:CALLBACK:START:(?P<nonce>[0-9a-f]{32}):"
        )
        response = b"GETTY:CALLBACK:START:" + b"a" * 32 + b":ready\r\n"
        with (
            mock.patch.object(terminal, "_write") as write,
            mock.patch("tools.live_terminal._wait_for_descriptor"),
            mock.patch(
                "tools.live_terminal.os.read",
                side_effect=(response[:30], response[30:]),
            ),
        ):
            verified, received = terminal.exchange_online_pattern(
                b"\r\nDIAL\r\n", marker, 1.0
            )
        self.assertTrue(verified)
        self.assertEqual(received, response)
        self.assertEqual(write.call_args.args[0], b"\r\nDIAL\r\n")

        with self.assertRaises(LiveTerminalError):
            terminal.exchange_online_pattern(b"", re.compile("text"), 1.0)

    def test_ato1_escapes_and_waits_for_a_new_connect(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        with (
            mock.patch("tools.live_terminal._wait_for_descriptor",
                       side_effect=TimeoutError) as wait,
            mock.patch.object(terminal, "_write") as write,
            mock.patch.object(
                terminal, "_read_result", side_effect=("ok", "ok", "connect")
            ),
        ):
            report = terminal.return_online(2.0, 0.1)
        self.assertTrue(report.connected)
        self.assertEqual(report.escape_result, "ok")
        self.assertEqual(report.command_result, "ok")
        self.assertEqual(report.retrain_result, "connect")
        wait.assert_called_once()
        self.assertEqual(
            [call.args[0] for call in write.call_args_list],
            [b"+++", b"ATO1\r"],
        )

        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        with (
            mock.patch("tools.live_terminal._wait_for_descriptor",
                       side_effect=TimeoutError),
            mock.patch.object(terminal, "_write") as write,
            mock.patch.object(
                terminal, "_read_result", return_value="no-carrier"
            ),
        ):
            report = terminal.return_online(2.0, 0.1)
        self.assertFalse(report.connected)
        self.assertEqual(report.escape_result, "no-carrier")
        self.assertIsNone(report.command_result)
        self.assertIsNone(report.retrain_result)
        self.assertEqual(
            [call.args[0] for call in write.call_args_list], [b"+++"]
        )

        terminal = ModemTerminal(-1)
        with self.assertRaises(LiveTerminalError):
            terminal.return_online(2.0, 0.1)

    def test_escape_guard_preserves_payload_before_command_mode(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 14400\r\ns")
        with (
            mock.patch("tools.live_terminal.time.monotonic", return_value=12.0),
            mock.patch("tools.live_terminal._wait_for_descriptor",
                       side_effect=[None, TimeoutError]) as wait,
            mock.patch("tools.live_terminal.os.read", return_value=b"\r\nOK\r\n"),
            mock.patch.object(terminal, "_write"),
            mock.patch.object(terminal, "_read_result",
                              side_effect=("ok", "ok", "connect")),
        ):
            report = terminal.return_online(2.0, 0.1)
        self.assertTrue(report.connected)
        self.assertEqual(report.received, b"s\r\nOK\r\n")
        self.assertEqual(terminal._decoder.take_online_data(), b"")
        self.assertTrue(all(call.args[-1] == 12.1 for call in wait.call_args_list))

    def test_hangup_escapes_after_ato1_response_timeout(self) -> None:
        for acknowledged in (False, True):
            with self.subTest(acknowledged=acknowledged):
                terminal = ModemTerminal(-1)
                terminal._decoder.feed(b"CONNECT 300\r\n")
                initial_replies = 2 if acknowledged else 1
                with (
                    mock.patch("tools.live_terminal.time.sleep"),
                    mock.patch.object(terminal, "_write") as write,
                    mock.patch("tools.live_terminal._wait_for_descriptor",
                               side_effect=[None] * initial_replies
                               + [TimeoutError, None, None]),
                    mock.patch("tools.live_terminal.os.read",
                               return_value=b"\r\nOK\r\n"),
                ):
                    with self.assertRaises(TimeoutError):
                        terminal.return_online(2.0, 0.0)
                    self.assertFalse(terminal._decoder.online)
                    self.assertTrue(terminal.escape_and_hang_up())
                self.assertEqual(
                    [call.args[0] for call in write.call_args_list],
                    [b"+++", b"ATO1\r", b"+++", b"ATH0\r"],
                )

    def test_rejected_ato1_keeps_command_mode_for_hangup(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 300\r\n")
        with (
            mock.patch.object(terminal, "_write") as write,
            mock.patch("tools.live_terminal._wait_for_descriptor"),
            mock.patch("tools.live_terminal.os.read",
                       side_effect=[b"OK\r\n", b"ERROR\r\n", b"OK\r\n"]),
        ):
            self.assertFalse(terminal.return_online(2.0, 0.0).connected)
            self.assertTrue(terminal.escape_and_hang_up())
        self.assertEqual(
            [call.args[0] for call in write.call_args_list],
            [b"+++", b"ATO1\r", b"ATH0\r"],
        )

    def test_escape_guard_capture_is_bounded_even_if_escape_fails(self) -> None:
        terminal = ModemTerminal(-1)
        terminal._decoder.feed(b"CONNECT 14400\r\n")
        with (
            mock.patch("tools.live_terminal._wait_for_descriptor",
                       side_effect=[None] * 20 + [TimeoutError]),
            mock.patch("tools.live_terminal.os.read",
                       return_value=b"x" * (ONLINE_CAPTURE_MAX // 2)),
            mock.patch.object(terminal, "_write"),
            mock.patch.object(terminal, "_read_result", return_value="no-carrier"),
        ):
            report = terminal.return_online(2.0, 0.1)
        self.assertFalse(report.connected)
        self.assertTrue(report.received)
        self.assertLessEqual(len(report.received), ONLINE_CAPTURE_MAX)

    def test_response_decoder_handles_fragments_and_bounds(self) -> None:
        decoder = ResponseDecoder()
        decoder.feed(
            b"ATDpeer\r\nModulation: V21\r\nProtocol: LAPM\r\n"
            b"Compression: V44\r\nTxRate: 14400\r\nCON"
        )
        self.assertIsNone(decoder.pop())
        decoder.feed(b"NECT 12000\r")
        self.assertEqual(decoder.pop(), "connect")
        self.assertEqual(decoder.connect_report.modulation, "V.21")
        self.assertEqual(decoder.connect_report.protocol, "LAPM")
        self.assertEqual(decoder.connect_report.compression, "V.44")
        self.assertEqual(decoder.connect_report.receive_bit_rate, 12000)
        self.assertEqual(decoder.connect_report.transmit_bit_rate, 14400)
        decoder.feed(b"\nMODEM")
        self.assertEqual(decoder.take_online_data(), b"MODEM")
        decoder.feed(b"-LIVE\r\n")
        self.assertEqual(decoder.take_online_data(), b"-LIVE\r\n")
        decoder.begin_command_response()
        self.assertIsNone(decoder.connect_report.modulation)
        decoder.feed(b"7\r\nNO CARRIER\r\n")
        self.assertEqual(decoder.pop(), "busy")
        self.assertEqual(decoder.pop(), "no-carrier")
        decoder.feed(b"x" * 600 + b"OK\r\nERROR\r\n")
        self.assertEqual(decoder.pop(), "error")
        self.assertIsNone(decoder.pop())

        decoder.feed(
            b"Modulation: sip:private@example.invalid\r\n"
            b"TxRate: 99999999\r\nCONNECT 300\r\nSAME-READ"
        )
        self.assertEqual(decoder.pop(), "connect")
        self.assertIsNone(decoder.connect_report.modulation)
        self.assertIsNone(decoder.connect_report.transmit_bit_rate)
        self.assertEqual(decoder.connect_report.receive_bit_rate, 300)
        self.assertEqual(decoder.take_online_data(), b"SAME-READ")

    def test_published_device_requires_a_bounded_pts_path(self) -> None:
        for published, expected in (
            (b"/dev/pts/17\n", "/dev/pts/17"),
            (
                b"sippymodem: source-built\n/dev/pts/23\n",
                "/dev/pts/23",
            ),
            (b"relative-device\n", None),
            (b"/dev/pts/private\n", None),
            (b"/dev/pts/1\xff\n", None),
            (b"x" * 513 + b"\n", None),
        ):
            with self.subTest(published=published[:20]):
                read_descriptor, write_descriptor = os.pipe()
                try:
                    os.write(write_descriptor, published)
                    os.close(write_descriptor)
                    write_descriptor = -1
                    with os.fdopen(read_descriptor, "rb") as stream:
                        read_descriptor = -1
                        if expected is None:
                            with self.assertRaises(LiveTerminalError):
                                read_published_device(stream, 0.1)
                        else:
                            self.assertEqual(
                                read_published_device(stream, 0.1), expected
                            )
                finally:
                    if read_descriptor >= 0:
                        os.close(read_descriptor)
                    if write_descriptor >= 0:
                        os.close(write_descriptor)


if __name__ == "__main__":
    unittest.main()
