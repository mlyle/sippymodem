#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Build the bounded local interoperability matrix."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib

if __package__:
    from .live_profiles import (
        ModemProfile,
        PROFILE_GROUPS,
        PROFILES,
        READINESS_RUNNABLE,
    )
else:
    from live_profiles import (
        ModemProfile,
        PROFILE_GROUPS,
        PROFILES,
        READINESS_RUNNABLE,
    )


MEDIA_CODECS = ("PCMU", "PCMA")
CANDIDATE_ORIGINATES = "candidate-originates"
BASELINE_ORIGINATES = "baseline-originates"
ORIGINATOR_ORDERS = (CANDIDATE_ORIGINATES, BASELINE_ORIGINATES)
FULL_DUPLEX = "full-duplex"
TURN_TAKING = "turn-taking"
PAYLOAD_EPOCHS = ("initial", "ato1")

# V.22 at 600 bit/s is excluded from ordinary live calls because both peers
# must select it explicitly. A matrix against a controlled peer can do so.
FAST_PROFILE_NAMES = (
    PROFILE_GROUPS["implemented"] + PROFILE_GROUPS["v22-600-matched"]
)


@dataclass(frozen=True)
class EndpointRates:
    receive: int
    transmit: int


@dataclass(frozen=True)
class MatrixCase:
    name: str
    profile: ModemProfile
    media_codec: str
    originator_order: str
    candidate_rates: EndpointRates
    baseline_rates: EndpointRates
    exchange: str

    @property
    def candidate_originates(self) -> bool:
        return self.originator_order == CANDIDATE_ORIGINATES


@dataclass(frozen=True)
class PayloadPair:
    candidate: bytes
    baseline: bytes


def supported_media_codecs(profile: ModemProfile) -> tuple[str, ...]:
    """Return media paths already exercised by the native integration gate."""

    return MEDIA_CODECS


def exchange_kind(profile: ModemProfile) -> str:
    return TURN_TAKING if profile.family == "baudot" else FULL_DUPLEX


def _profile_rates(profile: ModemProfile) -> tuple[EndpointRates, EndpointRates]:
    if profile.receive_rate is None or profile.transmit_rate is None:
        raise ValueError("fast matrix profiles need exact directional rates")
    originator = EndpointRates(profile.receive_rate, profile.transmit_rate)
    answerer = EndpointRates(profile.transmit_rate, profile.receive_rate)
    return originator, answerer


def _case(
    profile: ModemProfile, media_codec: str, originator_order: str
) -> MatrixCase:
    if profile.readiness != READINESS_RUNNABLE:
        raise ValueError("fast matrix profiles must be runnable")
    if media_codec not in supported_media_codecs(profile):
        raise ValueError("profile does not support the requested media codec")
    if originator_order not in ORIGINATOR_ORDERS:
        raise ValueError("invalid matrix originator order")
    originator_rates, answerer_rates = _profile_rates(profile)
    candidate_originates = originator_order == CANDIDATE_ORIGINATES
    return MatrixCase(
        name=f"{profile.name}-{media_codec.lower()}-{originator_order}",
        profile=profile,
        media_codec=media_codec,
        originator_order=originator_order,
        candidate_rates=(
            originator_rates if candidate_originates else answerer_rates
        ),
        baseline_rates=(
            answerer_rates if candidate_originates else originator_rates
        ),
        exchange=exchange_kind(profile),
    )


def build_fast_matrix(
    profile_names: tuple[str, ...] = FAST_PROFILE_NAMES,
) -> tuple[MatrixCase, ...]:
    """Expand exact-rate profiles across media codecs and call roles."""

    if len(set(profile_names)) != len(profile_names):
        raise ValueError("duplicate fast matrix profile")
    cases = tuple(
        _case(PROFILES[name], media_codec, originator_order)
        for name in profile_names
        for media_codec in supported_media_codecs(PROFILES[name])
        for originator_order in ORIGINATOR_ORDERS
    )
    if len({case.name for case in cases}) != len(cases):
        raise RuntimeError("duplicate fast matrix case")
    return cases


def payload_pair(case: MatrixCase, epoch: str) -> PayloadPair:
    """Generate short, unique evidence for each direction and ATO1 epoch."""

    if epoch not in PAYLOAD_EPOCHS:
        raise ValueError("invalid matrix payload epoch")
    digest = hashlib.sha256(f"{case.name}:{epoch}".encode("ascii")).hexdigest()
    token = digest[:16].upper().encode("ascii")
    if case.exchange == TURN_TAKING:
        candidate = b"A" + token + b" GA"
        baseline = b"B" + token + b" GA"
    else:
        candidate = b"FM CANDIDATE " + token + b" END"
        baseline = b"FM BASELINE " + token + b" END"
    if candidate in baseline or baseline in candidate:
        raise RuntimeError("matrix payloads are not independently observable")
    return PayloadPair(candidate=candidate, baseline=baseline)
