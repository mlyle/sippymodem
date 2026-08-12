#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.interop_config import (  # noqa: E402
    InteropConfigError,
    InteropPorts,
    build_child_environment,
    load_interop_config,
)


RAW_COMMANDS = ["ATZ", "AT\\N0", "AT+DS=0,1", "AT+MS=21,0,300,300"]


def write_config(directory: Path, *, enabled: bool = True) -> Path:
    payload = {
        "enabled": enabled,
        "artifact_directory": "artifacts",
        "originator": {
            "executable": sys.executable,
            "arguments": [],
            "environment": {"SIP_CODEC": "PCMU"},
            "at_commands": RAW_COMMANDS,
        },
        "answerer": {
            "executable": sys.executable,
            "arguments": [],
            "environment": {"SIP_LOG_LEVEL": "0"},
            "at_commands": RAW_COMMANDS,
        },
        "originator_payload_hex": b"ORIGIN-DATA".hex(),
        "answerer_payload_hex": b"ANSWER-DATA".hex(),
        "ports": {
            "originator_sip": 41001,
            "answerer_sip": 41002,
            "originator_rtp": 41003,
            "answerer_rtp": 41004,
        },
    }
    path = directory / "interop.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    path.chmod(0o600)
    return path


class InteropConfigTests(unittest.TestCase):
    def test_tracked_example_is_inert_and_uses_a_raw_baseline(self) -> None:
        example = (
            Path(__file__).resolve().parents[1]
            / "tools"
            / "local-interop.example.json"
        )
        config = load_interop_config(example)

        self.assertFalse(config.enabled)
        for endpoint in (config.originator, config.answerer):
            self.assertIn("AT\\N0", endpoint.at_commands)
            self.assertIn("AT+DS=0,1", endpoint.at_commands)
            self.assertNotIn("AT+DS=0,0", endpoint.at_commands)

    def test_enabled_configuration_must_be_private_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = write_config(directory)
            value = json.loads(path.read_text(encoding="utf-8"))

            path.chmod(0o644)
            with self.assertRaisesRegex(InteropConfigError, "must be private"):
                load_interop_config(path)

            path.chmod(0o600)
            value["answerer_payload_hex"] = b"ORIGIN-DATA-TAIL".hex()
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(InteropConfigError, "non-containing"):
                load_interop_config(path)

            value["answerer_payload_hex"] = b"ANSWER-DATA".hex()
            value["ports"]["answerer_sip"] = value["ports"]["originator_sip"]
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(InteropConfigError, "distinct"):
                load_interop_config(path)

    def test_at_setup_is_generic_but_cannot_start_a_call(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = write_config(Path(temporary))
            value = json.loads(path.read_text(encoding="utf-8"))
            value["answerer"]["at_commands"] = ["ATZ", "AT\\N3", "AT+DS=3"]
            path.write_text(json.dumps(value), encoding="utf-8")
            self.assertEqual(
                load_interop_config(path).answerer.at_commands,
                ("ATZ", "AT\\N3", "AT+DS=3"),
            )

            for command in ("ATA", "ATD", "ATD123", "ATO"):
                value["answerer"]["at_commands"] = ["ATZ", command]
                path.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaisesRegex(InteropConfigError, "setup command"):
                    load_interop_config(path)

    def test_environment_forces_loopback_and_scrubs_inherited_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            config = load_interop_config(write_config(directory))
            ports = InteropPorts(42001, 42002, 42003, 42004)
            environment = build_child_environment(
                config.originator,
                "originator",
                ports,
                directory / "private" / "media",
                {
                    "PATH": "/bin",
                    "SIP_TARGET": "inherited-private-target",
                    "SIP_PASSWORD": "must-be-removed",
                    "SIP_UNRECOGNIZED_PRIVATE": "must-be-removed",
                    "SLMODEMD_CONFIG": "must-be-removed",
                },
            )
            self.assertEqual(environment["PATH"], "/bin")
            self.assertEqual(environment["SIP_BOUND_ADDR"], "127.0.0.1")
            self.assertEqual(environment["SIP_PORT"], "42001")
            self.assertEqual(environment["SIP_RTP_PORT"], "42003")
            self.assertEqual(environment["SIP_AUTO_ANSWER"], "0")
            self.assertEqual(
                environment["SIP_TARGET"], "sip:interop@127.0.0.1:42002"
            )
            self.assertNotIn("SIP_PASSWORD", environment)
            self.assertNotIn("SIP_UNRECOGNIZED_PRIVATE", environment)
            self.assertNotIn("SLMODEMD_CONFIG", environment)

            answer_environment = build_child_environment(
                config.answerer,
                "answerer",
                ports,
                directory / "private" / "answer-media",
                {"SIP_TARGET": "must-be-removed"},
            )
            self.assertEqual(answer_environment["SIP_PORT"], "42002")
            self.assertEqual(answer_environment["SIP_RTP_PORT"], "42004")
            self.assertNotIn("SIP_TARGET", answer_environment)


if __name__ == "__main__":
    unittest.main()
