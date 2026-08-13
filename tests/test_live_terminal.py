#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import os
from pathlib import Path
import sys
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.live_terminal import (  # noqa: E402
    LiveTerminalError,
    ModemTerminal,
    ONLINE_PING_REQUEST,
    ResponseDecoder,
    read_published_device,
)


class LiveTerminalTests(unittest.TestCase):
    def test_response_decoder_allowlists_high_speed_profile_labels(self) -> None:
        for wire_name, expected in (
            (b"V34", "V.34"),
            (b"V90", "V.90"),
            (b"V92", "V.92"),
        ):
            with self.subTest(wire_name=wire_name):
                decoder = ResponseDecoder()
                decoder.feed(b"Modulation: " + wire_name + b"\r\nCONNECT 9600\r\n")
                self.assertEqual(decoder.pop(), "connect")
                self.assertEqual(decoder.connect_report.modulation, expected)

    def test_escape_cleanup_tolerates_a_missing_response(self) -> None:
        terminal = ModemTerminal(-1)
        with (
            mock.patch("tools.live_terminal.time.sleep"),
            mock.patch.object(terminal, "_write"),
            mock.patch.object(
                terminal, "_read_result", side_effect=TimeoutError
            ),
        ):
            terminal.escape_and_hang_up()

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
            b"178373760012\r\n"
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
                b"SmartLink Soft Modem: source-built\n/dev/pts/23\n",
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
