#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import modem_terminal as terminal


class ModemTerminalTests(unittest.TestCase):
    def test_profile_is_private_data_not_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            values = {"SIP_LOGIN": "account:$(false); secret@private.invalid",
                      "SIP_PCM_CODEC_GRID": "1", "SIP_TRACE": "1",
                      "SIP_LOG_LEVEL": "3"}
            path.write_text(json.dumps({"2600": values, "pstn": {"SIP_PORT": "0"}}))
            path.chmod(0o600)
            self.assertEqual(terminal.load_profile(path, "2600"), values)
            self.assertEqual(terminal.load_profile(path, "pstn"), {"SIP_PORT": "0"})
            path.chmod(0o644)
            with self.assertRaisesRegex(ValueError, "owner-only"):
                terminal.load_profile(path, "2600")

    def test_profile_errors_do_not_quote_private_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            for content in ("SECRET", '{"2600":{"SECRET":"value"}}',
                            '{"2600":{"SIP_LOGIN":"SECRET\\n"}}',
                            '{"2600":{"SIP_LOGIN":123}}',
                            '{"2600":{}}', '[]', '{}', " " * 65537):
                path.write_text(content)
                path.chmod(0o600)
                with self.subTest(content=content[:40]):
                    with self.assertRaises(ValueError) as error:
                        terminal.load_profile(path, "2600")
                    self.assertNotIn("SECRET", str(error.exception))

    def test_environment_does_not_inherit_another_provider(self):
        with mock.patch.dict(os.environ, {"SIP_PASSWORD": "secret", "TERM": "xterm"}, clear=True):
            self.assertEqual(terminal.clean_environment(), {"TERM": "xterm"})

    def test_panes_and_child_ownership(self):
        calls = []
        modem = mock.Mock()
        modem.poll.return_value = None
        screen = mock.Mock()
        screen.poll.return_value = None

        def spawn(arguments, **kwargs):
            calls.append((arguments, kwargs))
            return modem if len(calls) == 1 else screen

        # Normal splitvt exit, an unexpected daemon exit, and startup failure
        # must all stop the daemon; no child sees the provider secret except it.
        for failed in (False, True):
            calls.clear()
            modem.reset_mock()
            screen.reset_mock()
            modem.poll.return_value = None
            screen.wait.return_value = 0
            publication = (terminal.LiveTerminalError("no terminal") if failed else "/dev/pts/123")
            with (mock.patch.object(terminal.subprocess, "Popen", side_effect=spawn),
                  mock.patch.object(terminal, "read_published_device", side_effect=[publication]),
                  mock.patch.dict(os.environ, {"SIP_LOGIN": "old-secret", "TERM": "xterm"}, clear=True)):
                if failed:
                    with self.assertRaises(terminal.LiveTerminalError):
                        terminal.run(Path("/modem"), {"SIP_LOGIN": "new-secret"}, "/splitvt", "/kermit")
                else:
                    self.assertEqual(terminal.run(Path("/modem"), {"SIP_LOGIN": "new-secret"}, "/splitvt", "/kermit"), 0)
            self.assertEqual(calls[0][0], ["/modem"])
            self.assertEqual(calls[0][1]["env"]["SIP_LOGIN"], "new-secret")
            self.assertEqual(calls[0][1]["env"]["SIP_REGISTER"], "1")
            self.assertEqual(calls[0][1]["env"]["SIP_TRACE"], "1")
            modem.terminate.assert_called_once()
            modem.stdout.close.assert_called_once()
            if not failed:
                args, settings = calls[1]
                self.assertEqual(args[:4], ["/splitvt", "-norc", "-nologin", "-bottom"])
                self.assertIn("-upper", args)
                self.assertIn("-lower", args)
                self.assertNotIn("secret", repr(args) + repr(settings))
                screen.terminate.assert_called_once()

    def test_daemon_failure_reports_trace_and_stops_screen(self):
        modem = mock.Mock()
        modem.poll.return_value = 1
        screen = mock.Mock()
        screen.poll.return_value = None
        screen.wait.side_effect = [subprocess.TimeoutExpired("splitvt", .2), 0]
        diagnostic = "cannot open SIP transport\n"
        calls = []

        def spawn(arguments, **kwargs):
            calls.append((arguments, kwargs))
            if len(calls) == 1:
                kwargs["stderr"].write(diagnostic.encode())
                return modem
            return screen

        with (mock.patch.object(terminal.subprocess, "Popen", side_effect=spawn),
              mock.patch.object(terminal, "read_published_device", return_value="/dev/pts/123"),
              mock.patch.object(terminal.sys, "stderr", new_callable=io.StringIO) as errors):
            with self.assertRaisesRegex(RuntimeError, "modem exited"):
                terminal.run(Path("/modem"), {}, "/splitvt", "/kermit")
        screen.terminate.assert_called_once()
        self.assertEqual(errors.getvalue(), diagnostic)

    def test_pane_commands_need_no_shell_quoting(self):
        modem = mock.Mock()
        modem.poll.return_value = None
        screen = mock.Mock()
        screen.poll.return_value = 0
        screen.wait.return_value = 0
        with (mock.patch.object(terminal.subprocess, "Popen", side_effect=[modem, screen]) as spawn,
              mock.patch.object(terminal, "read_published_device", return_value="/dev/pts/123")):
            terminal.run(Path("/modem path"), {}, "/splitvt", "/kermit ' path")
        args = spawn.call_args.args[0]
        self.assertEqual(len(args[args.index("-upper") + 1].split()), 5)
        self.assertEqual(len(args[args.index("-lower") + 1].split()), 3)
        self.assertEqual(spawn.call_args.kwargs["env"]["MODEM_TERMINAL_KERMIT"], "/kermit ' path")
        self.assertEqual(spawn.call_args_list[0].args[0], ["/modem path"])

    def test_kermit_exit_notifies_owner_and_does_not_dial(self):
        with tempfile.TemporaryDirectory() as directory:
            done = Path(directory) / "done"
            child = mock.Mock()
            child.wait.return_value = 0
            child.poll.return_value = 0
            with mock.patch.object(terminal.subprocess, "Popen", return_value=child) as spawn:
                self.assertEqual(terminal.terminal_pane("/kermit", "/dev/pts/123", done), 0)
            self.assertTrue(done.exists())
            self.assertEqual(done.read_text(), "0")
            args = spawn.call_args.args[0]
            self.assertEqual(args[:6], ["/kermit", "-Y", "-l", "/dev/pts/123", "-b", "115200"])
            self.assertIn("carrier-watch off", args[-1])
            self.assertIn("flow-control none", args[-1])
            self.assertNotIn("dial", args[-1])

    def test_kermit_completion_stops_modem_and_splitvt(self):
        modem = mock.Mock()
        modem.poll.return_value = None
        screen = mock.Mock()
        screen.poll.return_value = None
        screen.wait.side_effect = [subprocess.TimeoutExpired("splitvt", .2), 0]
        calls = 0

        def spawn(arguments, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                return modem
            # This is the same completion notification emitted when Ctrl-\ Q
            # exits Kermit; splitvt may keep its trace pane alive afterward.
            Path(kwargs["env"]["MODEM_TERMINAL_DONE"]).write_text("0")
            return screen

        with (mock.patch.object(terminal.subprocess, "Popen", side_effect=spawn),
              mock.patch.object(terminal, "read_published_device", return_value="/dev/pts/123")):
            self.assertEqual(terminal.run(Path("/modem"), {}, "/splitvt", "/kermit"), 0)
        screen.terminate.assert_called_once()
        modem.terminate.assert_called_once()
        modem.stdout.close.assert_called_once()

    def test_stop_bounds_unresponsive_child(self):
        child = mock.Mock()
        child.poll.return_value = None
        child.wait.side_effect = [subprocess.TimeoutExpired("child", 3), 0]
        terminal.stop(child)
        child.terminate.assert_called_once()
        child.kill.assert_called_once()
        self.assertEqual(child.wait.call_count, 2)


if __name__ == "__main__":
    unittest.main()
