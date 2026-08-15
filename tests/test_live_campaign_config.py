#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.live_campaign_config import (  # noqa: E402
    CampaignConfigError,
    child_environment,
    load_campaign_config,
)


class LiveCampaignConfigTests(unittest.TestCase):
    def value(self, root: Path) -> dict[str, object]:
        return {
            "enabled": True,
            "artifact_directory": "artifacts",
            "build_cache": "builds",
            "candidate": {
                "repository": ".",
                "revision": "HEAD",
                "command": ["make", "slmodemd"],
                "executable": "src/slmodemd",
                "arguments": [],
            },
            "outbound_environment": {
                "SIP_TARGET": "sip:private@example.invalid",
                "SIP_CODEC": "pcmu",
            },
            "callback_request_environment": {
                "SIP_TARGET": "sip:private@example.invalid",
                "SIP_PORT": "0",
            },
            "callback_answer_environment": {
                "SIP_BOUND_ADDR": "192.0.2.1",
                "SIP_AUTO_ANSWER": "0",
            },
        }

    def write(self, path: Path, value: object, mode: int = 0o600) -> None:
        path.write_text(json.dumps(value), encoding="utf-8")
        path.chmod(mode)

    def test_loads_private_three_leg_config(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "campaign.json"
            self.write(path, self.value(root))
            config = load_campaign_config(path)
            self.assertTrue(config.enabled)
            self.assertEqual(config.candidate.repository, root)
            self.assertEqual(config.candidate.executable, Path("src/slmodemd"))
            self.assertEqual(
                config.environment("outbound")["SIP_TARGET"],
                "sip:private@example.invalid",
            )
            self.assertNotIn("SIP_TARGET", config.environment("callback-answer"))
            self.assertEqual(
                config.environment("callback-request")["SIP_PORT"], "0"
            )
            self.assertEqual(
                config.environment("callback-answer")["SIP_PORT"], "5060"
            )
            self.assertEqual(config.callback_request_timeout_seconds, 90)
            self.assertEqual(config.callback_marker_timeout_seconds, 35)
            self.assertEqual(config.callback_return_timeout_seconds, 180)
            self.assertEqual(config.inter_call_seconds, 3)
            self.assertEqual(config.busy_retry_limit, 3)

    def test_enabled_config_must_be_private_and_have_leg_targets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "campaign.json"
            value = self.value(root)
            self.write(path, value, 0o644)
            with self.assertRaisesRegex(CampaignConfigError, "owner-only"):
                load_campaign_config(path)
            self.write(path, value, 0o600)
            del value["callback_request_environment"]["SIP_TARGET"]  # type: ignore[index]
            self.write(path, value, 0o600)
            with self.assertRaisesRegex(CampaignConfigError, "requires SIP_TARGET"):
                load_campaign_config(path)

    def test_disabled_public_example_is_safe(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "campaign.json"
            value = self.value(root)
            value["enabled"] = False
            self.write(path, value, 0o644)
            self.assertFalse(load_campaign_config(path).enabled)

        example = load_campaign_config(ROOT / "tools/live-campaign.example.json")
        self.assertFalse(example.enabled)

    def test_child_environment_scrubs_inherited_private_values(self) -> None:
        environment = child_environment(
            {"SIP_TARGET": "sip:new@example.invalid", "SIP_CODEC": "pcmu"},
            Path("/tmp/private-recording"),
            {
                "PATH": os.environ.get("PATH", ""),
                "SIP_PASSWORD": "old-secret",
                "SIP_TARGET": "sip:old@example.invalid",
            },
        )
        self.assertNotIn("SIP_PASSWORD", environment)
        self.assertEqual(environment["SIP_TARGET"], "sip:new@example.invalid")
        self.assertEqual(environment["SIP_CODEC"], "pcmu")
        self.assertEqual(environment["SIP_RECORD_PREFIX"], "/tmp/private-recording")

    def test_rejects_unknown_environment_and_unsafe_executable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "campaign.json"
            value = self.value(root)
            value["outbound_environment"]["HOME"] = "/private"  # type: ignore[index]
            self.write(path, value)
            with self.assertRaisesRegex(CampaignConfigError, "unsupported key"):
                load_campaign_config(path)
            value = self.value(root)
            value["candidate"]["executable"] = "../slmodemd"  # type: ignore[index]
            self.write(path, value)
            with self.assertRaisesRegex(CampaignConfigError, "safe relative"):
                load_campaign_config(path)

    def test_callback_sip_ports_are_role_specific(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "campaign.json"
            value = self.value(root)
            request = value["callback_request_environment"]
            assert isinstance(request, dict)
            request["SIP_PORT"] = "5060"
            self.write(path, value)
            with self.assertRaisesRegex(CampaignConfigError, "nonstandard"):
                load_campaign_config(path)

            value = self.value(root)
            answer = value["callback_answer_environment"]
            assert isinstance(answer, dict)
            answer["SIP_PORT"] = "5061"
            self.write(path, value)
            with self.assertRaisesRegex(CampaignConfigError, "standard port 5060"):
                load_campaign_config(path)

            value = self.value(root)
            request = value["callback_request_environment"]
            assert isinstance(request, dict)
            request["SIP_PORT"] = "auto"
            self.write(path, value)
            with self.assertRaisesRegex(CampaignConfigError, "decimal port"):
                load_campaign_config(path)


if __name__ == "__main__":
    unittest.main()
