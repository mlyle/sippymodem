#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.sippymodem_live_config import (  # noqa: E402
    LiveCallConfigError,
    dial_form,
    load_live_call_config,
    public_manifest,
)


class LiveCallConfigTests(unittest.TestCase):
    def write_config(self, directory: str, document: object, mode: int = 0o600) -> Path:
        path = Path(directory) / ".live-call.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        path.chmod(mode)
        return path

    def valid_document(self) -> dict[str, object]:
        return {
            "slmodemd": "bin/slmodemd",
            "artifact_directory": "private-artifacts",
            "environment": {
                "SIP_USER": "private-user",
                "SIP_PASSWORD": "private-password",
                "SIP_DOMAIN": "private.example",
            },
            "at_commands": ["ATZ", "AT+MS=21"],
            "startup_timeout_seconds": 2.5,
            "call_timeout_seconds": 30,
            "online_timeout_seconds": 4,
            "hold_seconds": 1,
            "plans": [
                {
                    "name": "configured",
                    "dial": "",
                    "configured_target": "private-target",
                    "online_send_hex": "70696e67",
                    "online_expect_hex": "706f6e67",
                },
                {
                    "name": "bare",
                    "dial": "extension",
                    "online_expect_hex": "6d61726b6572",
                    "round_trip_probe": "ping",
                },
                {"name": "address", "dial": "peer@private.example"},
                {"name": "uri", "dial": "sip:peer@private.example"},
            ],
        }

    def test_loads_owned_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_config(directory, self.valid_document())
            config = load_live_call_config(path)

            self.assertEqual(config.slmodemd, Path(directory) / "bin/slmodemd")
            self.assertEqual(
                config.artifact_directory, Path(directory) / "private-artifacts"
            )
            self.assertEqual(config.environment_dict()["SIP_USER"], "private-user")
            self.assertEqual(config.at_commands, ("ATZ", "AT+MS=21"))
            self.assertEqual(config.startup_timeout_seconds, 2.5)
            self.assertEqual(config.call_timeout_seconds, 30.0)
            self.assertEqual(config.online_timeout_seconds, 4.0)
            self.assertEqual(config.hold_seconds, 1.0)
            self.assertEqual(config.plans[0].online_send, b"ping")
            self.assertEqual(config.plans[0].online_expect, b"pong")
            self.assertEqual(config.plans[1].round_trip_probe, "ping")
            self.assertEqual(
                [dial_form(plan) for plan in config.plans],
                [
                    "configured-target",
                    "domain-qualified",
                    "address",
                    "sip-uri",
                ],
            )

    def test_public_manifest_redacts_every_private_value(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_config(directory, self.valid_document())
            config = load_live_call_config(path)
            rendered = json.dumps(public_manifest(config), sort_keys=True)

            self.assertIn('"name": "configured"', rendered)
            self.assertIn('"form": "sip-uri"', rendered)
            self.assertIn('"terminal_data": true', rendered)
            self.assertIn('"round_trip_probe": "ping"', rendered)
            for private in (
                "private-user",
                "private-password",
                "private.example",
                "private-target",
                "bin/slmodemd",
                "private-artifacts",
                "extension",
            ):
                self.assertNotIn(private, rendered)

    def test_password_requires_owner_only_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_config(directory, self.valid_document(), mode=0o644)
            with self.assertRaisesRegex(LiveCallConfigError, "readable only"):
                load_live_call_config(path)

            document = self.valid_document()
            environment = document["environment"]
            assert isinstance(environment, dict)
            del environment["SIP_PASSWORD"]
            plans = document["plans"]
            assert isinstance(plans, list)
            for plan in plans:
                assert isinstance(plan, dict)
                plan["enabled"] = False
            path = self.write_config(directory, document, mode=0o644)
            load_live_call_config(path)

            plans[0]["enabled"] = True
            path = self.write_config(directory, document, mode=0o644)
            with self.assertRaisesRegex(LiveCallConfigError, "enabled plans"):
                load_live_call_config(path)

    def test_rejects_unsafe_or_ambiguous_documents(self) -> None:
        documents = []
        for mutate in (
            lambda value: value.update({"unexpected": True}),
            lambda value: value.update({"environment": {"LD_PRELOAD": "bad"}}),
            lambda value: value.update({"at_commands": ["+MS=21"]}),
            lambda value: value.update({"call_timeout_seconds": 0}),
            lambda value: value.update({"plans": []}),
            lambda value: value.update(
                {"plans": [{"name": "bad name", "dial": "peer"}]}
            ),
            lambda value: value.update(
                {"plans": [{"name": "empty", "dial": ""}]}
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {"name": "same", "dial": "one"},
                        {"name": "same", "dial": "two"},
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "unknown-probe",
                            "dial": "peer",
                            "online_expect_hex": "00",
                            "round_trip_probe": "arbitrary",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "probe-without-marker",
                            "dial": "peer",
                            "round_trip_probe": "ping",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "ambiguous-probe",
                            "dial": "peer",
                            "online_send_hex": "00",
                            "online_expect_hex": "01",
                            "round_trip_probe": "ping",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {"plans": [{"name": "control", "dial": "peer\nnext"}]}
            ),
            lambda value: value.update(
                {"plans": [{"name": "delete", "dial": "peer\x7fnext"}]}
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "bad-hex",
                            "dial": "peer",
                            "online_expect_hex": "0",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "unverified-send",
                            "dial": "peer",
                            "online_send_hex": "00",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "large-pattern",
                            "dial": "peer",
                            "online_expect_hex": "00" * 513,
                        }
                    ]
                }
            ),
        ):
            document = self.valid_document()
            mutate(document)
            documents.append(document)

        with tempfile.TemporaryDirectory() as directory:
            for index, document in enumerate(documents):
                with self.subTest(index=index):
                    path = self.write_config(directory, document)
                    with self.assertRaises(LiveCallConfigError):
                        load_live_call_config(path)

    def test_rejects_duplicate_json_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".live-call.json"
            path.write_text(
                '{"plans":[{"name":"one","dial":"peer"}],'
                '"plans":[{"name":"two","dial":"peer"}]}',
                encoding="utf-8",
            )
            path.chmod(0o600)
            with self.assertRaisesRegex(LiveCallConfigError, "duplicate"):
                load_live_call_config(path)

    def test_example_is_safe_and_inert(self) -> None:
        example = Path(__file__).resolve().parents[1] / "tools/live-call.example.json"
        config = load_live_call_config(example)

        self.assertTrue(config.plans)
        self.assertFalse(any(plan.enabled for plan in config.plans))
        self.assertNotIn("SIP_PASSWORD", config.environment_dict())
        self.assertTrue(config.plans[0].online_expect)
        self.assertEqual(config.plans[0].round_trip_probe, "ping")
        self.assertEqual(
            [dial_form(plan) for plan in config.plans],
            ["configured-target"],
        )


if __name__ == "__main__":
    unittest.main()
