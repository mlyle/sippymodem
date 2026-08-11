#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import os
from pathlib import Path
import sys
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.sippymodem_live_terminal import (  # noqa: E402
    LiveTerminalError,
    ModemTerminal,
    ResponseDecoder,
    read_published_device,
)


class LiveTerminalTests(unittest.TestCase):
    def test_escape_cleanup_tolerates_a_missing_response(self) -> None:
        terminal = ModemTerminal(-1)
        with (
            mock.patch("tools.sippymodem_live_terminal.time.sleep"),
            mock.patch.object(terminal, "_write"),
            mock.patch.object(
                terminal, "_read_result", side_effect=TimeoutError
            ),
        ):
            terminal.escape_and_hang_up()

    def test_response_decoder_handles_fragments_and_bounds(self) -> None:
        decoder = ResponseDecoder()
        decoder.feed(b"ATDpeer\r\nModulation: V.21\r\nCON")
        self.assertIsNone(decoder.pop())
        decoder.feed(b"NECT 300\r\n")
        self.assertEqual(decoder.pop(), "connect")
        decoder.feed(b"7\r\nNO CARRIER\r\n")
        self.assertEqual(decoder.pop(), "busy")
        self.assertEqual(decoder.pop(), "no-carrier")
        decoder.feed(b"x" * 600 + b"OK\r\nERROR\r\n")
        self.assertEqual(decoder.pop(), "error")
        self.assertIsNone(decoder.pop())

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
