#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.live_config import (  # noqa: E402
    LiveCallConfigError,
    expand_plan,
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
                "SIP_TARGET": "private-target",
                "SIP_USER": "private-user",
                "SIP_PASSWORD": "private-password",
                "SIP_DOMAIN": "private.example",
            },
            "at_commands": ["ATF0"],
            "startup_timeout_seconds": 2.5,
            "call_timeout_seconds": 30,
            "online_timeout_seconds": 4,
            "hold_seconds": 1,
            "inter_scenario_seconds": 2,
            "plans": [
                {
                    "name": "matrix",
                    "profiles": ["v21_300", "v32_9600"],
                    "profile_groups": ["v32"],
                    "link_modes": ["lapm", "raw"],
                    "attempts": 2,
                    "at_commands": ["ATL1"],
                    "online_expect_hex": "6d61726b6572",
                    "round_trip_probe": "ping",
                    "enabled": True,
                },
                {
                    "name": "fallback",
                    "profile_groups": ["analog-fallback"],
                    "enabled": False,
                },
            ],
        }

    def test_loads_and_expands_owned_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_config(directory, self.valid_document())
            config = load_live_call_config(path)
            scenarios = expand_plan(config, config.plans[0])

            self.assertEqual(config.slmodemd, Path(directory) / "bin/slmodemd")
            self.assertEqual(
                config.artifact_directory, Path(directory) / "private-artifacts"
            )
            self.assertEqual(config.environment_dict()["SIP_TARGET"], "private-target")
            self.assertEqual(config.inter_scenario_seconds, 2.0)
            self.assertEqual(len(scenarios), 12)
            self.assertEqual(
                scenarios[0].name,
                "matrix.v32_4800.lapm.1",
            )
            self.assertEqual(scenarios[0].profile_name, "v32_4800")
            self.assertEqual(scenarios[0].online_expect, b"marker")
            self.assertEqual(scenarios[0].round_trip_probe, "ping")
            self.assertEqual(
                scenarios[0].at_commands,
                (
                    "ATZ",
                    "ATE0",
                    "AT&A7",
                    "ATX3",
                    "AT+DS=3,1",
                    "ATF0",
                    "AT+MS=32,0,4800,4800",
                    "ATL1",
                    "ATE0V1Q0",
                    "ATS95=7",
                ),
            )

    def test_public_manifest_redacts_every_private_value(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_config(directory, self.valid_document())
            config = load_live_call_config(path)
            rendered = json.dumps(public_manifest(config), sort_keys=True)

            self.assertIn('"name": "matrix"', rendered)
            self.assertIn('"profile_groups": ["v32"]', rendered)
            self.assertIn('"scenario_count": 12', rendered)
            self.assertIn('"round_trip_probe": "ping"', rendered)
            self.assertIn('"readiness": "runnable"', rendered)
            self.assertIn('"runnable_scenario_count": 12', rendered)
            for private in (
                "private-user",
                "private-password",
                "private.example",
                "private-target",
                "bin/slmodemd",
                "private-artifacts",
            ):
                self.assertNotIn(private, rendered)

    def test_private_target_and_enabled_plans_require_private_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_config(directory, self.valid_document(), mode=0o644)
            with self.assertRaisesRegex(LiveCallConfigError, "readable only"):
                load_live_call_config(path)

            document = self.valid_document()
            document["environment"] = {"SIP_CODEC": "PCMU"}
            plans = document["plans"]
            assert isinstance(plans, list)
            for plan in plans:
                assert isinstance(plan, dict)
                plan["enabled"] = False
            path = self.write_config(directory, document, mode=0o644)
            load_live_call_config(path)

            plans[0]["enabled"] = True
            path = self.write_config(directory, document, mode=0o600)
            with self.assertRaisesRegex(LiveCallConfigError, "SIP_TARGET"):
                load_live_call_config(path)

    def test_rejects_unsafe_or_ambiguous_documents(self) -> None:
        mutations = (
            lambda value: value.update({"unexpected": True}),
            lambda value: value.update({"environment": {"LD_PRELOAD": "bad"}}),
            lambda value: value.update({"at_commands": ["+MS=21"]}),
            lambda value: value.update({"at_commands": ["AT"] * 65}),
            lambda value: value.update({"call_timeout_seconds": 0}),
            lambda value: value.update({"plans": []}),
            lambda value: value.update(
                {"plans": [{"name": "bad name", "profiles": ["v21_300"]}]}
            ),
            lambda value: value.update(
                {"plans": [{"name": "empty", "profiles": []}]}
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {"name": "same", "profiles": ["v21_300"]},
                        {"name": "same", "profiles": ["v32_4800"]},
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {"name": "unknown", "profiles": ["private-profile"]}
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "duplicate",
                            "profiles": ["v21_300", "v21_300"],
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "unknown-probe",
                            "profiles": ["v21_300"],
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
                            "profiles": ["v21_300"],
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
                            "profiles": ["v21_300"],
                            "online_send_hex": "00",
                            "online_expect_hex": "01",
                            "round_trip_probe": "ping",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "bad-hex",
                            "profiles": ["v21_300"],
                            "online_expect_hex": "0",
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "large-pattern",
                            "profiles": ["v21_300"],
                            "online_expect_hex": "00" * 513,
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": "too-many",
                            "profile_groups": ["all"],
                            "link_modes": ["lapm", "raw"],
                            "attempts": 20,
                        }
                    ]
                }
            ),
            lambda value: value.update(
                {
                    "plans": [
                        {
                            "name": f"disabled-{index}",
                            "profile_groups": ["all"],
                            "enabled": False,
                        }
                        for index in range(2)
                    ]
                }
            ),
        )

        with tempfile.TemporaryDirectory() as directory:
            for index, mutate in enumerate(mutations):
                with self.subTest(index=index):
                    document = self.valid_document()
                    mutate(document)
                    path = self.write_config(directory, document)
                    with self.assertRaises(LiveCallConfigError):
                        load_live_call_config(path)

    def test_rejects_duplicate_json_members(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".live-call.json"
            path.write_text(
                '{"plans":[{"name":"one","profiles":["v21_300"]}],'
                '"plans":[{"name":"two","profiles":["v21_300"]}]}',
                encoding="utf-8",
            )
            path.chmod(0o600)
            with self.assertRaisesRegex(LiveCallConfigError, "duplicate"):
                load_live_call_config(path)

    def test_example_is_safe_inert_and_broad(self) -> None:
        example = Path(__file__).resolve().parents[1] / "tools/live-call.example.json"
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / ".live-call.json"
            copied.write_bytes(example.read_bytes())
            config = load_live_call_config(copied)

        self.assertFalse(any(plan.enabled for plan in config.plans))
        self.assertNotIn("SIP_TARGET", config.environment_dict())
        self.assertEqual(config.slmodemd, Path(directory) / "src/slmodemd")
        self.assertEqual(
            config.artifact_directory,
            Path(directory) / "live-call-artifacts",
        )
        self.assertEqual(
            [plan.name for plan in config.plans],
            [
                "implemented-matrix",
                "v34-current",
                "v90-directional-sweeps",
                "v92-directional-sweeps",
                "v92-headend-originator",
                "v92-quick-connect",
                "v34-sweep",
                "analog-fallback-features",
                "matched-v22-600",
            ],
        )
        scenario_count = sum(
            len(expand_plan(config, plan)) for plan in config.plans
        )
        self.assertGreater(scenario_count, 100)
        self.assertTrue(all(plan.online_expect for plan in config.plans))


if __name__ == "__main__":
    unittest.main()
