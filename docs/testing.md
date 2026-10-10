# Testing sippymodem

## Ordinary checks

```sh
make test
make install-hooks
```

The pre-commit hook runs the ordinary suite and checks the staged diff for
whitespace errors. The suite uses generated channels and local SIP sockets;
it does not dial an external destination. SIP-dependent checks skip clearly
when PJSIP is unavailable. Optional recording replays also skip when their
private input files are absent.

Focused binaries live under `tests/build`. For example:

```sh
make -C tests build/test_native_dte
./tests/build/test_native_dte
```

See [memory checks](memory-check.md) for Valgrind coverage and
[packet benchmarks](../tests/PERFORMANCE.md) for processing measurements.
The [big-endian build guide](building.md#big-endian-testing-with-qemu) shows
how to run compiled checks under QEMU with `TEST_RUNNER` and `test-c`.
Native generated-channel coverage establishes the tested behavior; it does
not establish interoperability with every modem or robustness on impaired
physical lines.

## Local interoperability

Copy `tools/local-interop.example.json` to an owner-only file under the ignored
`live-call-artifacts/` directory. Set the two executable/argument lists, choose
AT profiles, and enable it:

```sh
python3 tools/local_interop.py --config live-call-artifacts/local-interop.json
```

The runner starts both endpoints, constructs loopback SIP/RTP routing, waits
for `RING`, answers, and verifies distinct payloads in both directions. It
retains private logs, terminal traffic and media evidence. Its stdout contains
bounded, redacted status fields. The example is an inert raw V.21 baseline;
its AT lists can also select LAPM and compression.

Use the same configuration with `tools/fast_interop.py` for the supported
rate/profile matrix, both call orientations and both G.711 laws. `--ato1`
adds a local retrain and a fresh byte exchange. `--jobs 4` runs disjoint
loopback endpoints in waves; `--stop-on-failure` completes the current wave
before stopping. `--media-codec PCMU` or `PCMA` limits the codec, and
`--group v90` selects the PCM group. Use `--help` and the profile listing to
inspect current coverage rather than assuming every profile is runnable.

The local runners include settling intervals for peers that announce CONNECT
before their data decoder is ready. The native library's CONNECTED event has
its own readiness contract; see [the API guide](library-api.md#bytes-modes-and-readiness).

## Opt-in physical calls

Physical modem/ATA campaigns require explicit private configuration. Copy
`tools/live-campaign.example.json` to `.live-campaign.json`, make it owner-only,
fill the SIP environments and enable it:

```sh
cp tools/live-campaign.example.json .live-campaign.json
chmod 600 .live-campaign.json
python3 tools/live_campaign.py --list --suite smoke
```

Listing makes no calls. Select the desired cases and call budget before
running the campaign. Examples of selectors:

```sh
python3 tools/live_campaign.py --modulation v34 --attempts 3
python3 tools/live_campaign.py --modulation v92 --feature pcm-upstream --callbacks
python3 tools/live_campaign.py --rate 9600 --role callback
```

The runner freezes an exact revision and its pinned submodules in a private
temporary tree before building. Active development cannot replace its running
executable. `--prepare` performs only that build. SIP endpoints, credentials,
bind addresses and dial destinations belong in ignored configuration.

A complete call result requires the requested negotiated profile, initial
payload, remote retrain, local `ATO1`, and post-retrain payload where enabled.
CONNECT alone is training evidence. A connected fallback still exercises data,
but the report attributes it to the negotiated modulation and records the
requested-profile mismatch. PCM-upstream cases require explicit upstream-mode
evidence, not only the V.92 name or bitrate.

Callback cases use a bounded request call and a nonce-matched returned call;
request legs are reported separately. BUSY retries and inter-call cooldowns
are bounded and recorded. The ledger, seed, schedule, result records, logs and
captures remain in owner-only ignored artifacts. Do not publish those files
without checking them for site information.

### Two modems on one ATA

Use one owner-only campaign configuration per FXS port, with distinct
`reference_name` values such as `fxs1` and `fxs2`. Set both `SIP_TARGET` values
(outbound and callback request) to that FXS port's direct SIP URI. Keep the
second configuration disabled until its modem and reference getty are ready.
The foreground terminal also accepts an `ata2` profile; plain `ATDT` calls
its configured `SIP_TARGET`.

Callbacks need separate **local** listening ports. The ATA's FXS destination
port and our callback listening port are independent settings:

| Configuration | Direct target example | Local callback `SIP_PORT` |
| --- | --- | --- |
| FXS1 | `sip:modem@ata.example.invalid:5060` | `5060` |
| FXS2 | `sip:modem@ata.example.invalid:5062` | `5064` |

Configure each reference getty's callback number to reach its corresponding
listener. Grandstream direct-IP dialing encodes the destination port as a final
`*port`: for example, `*47192*0*2*10*5064` reaches `192.0.2.10:5064`.
See the [Grandstream direct-IP calling guide](https://documentation.grandstream.com/knowledge-base/ht81x-user-guide/).
Use ephemeral local SIP/RTP ports (`"0"`) on outbound/request legs and ephemeral
RTP ports on callback answerers. Both answerers may bind the same local address
on different SIP ports. Reserve each physical modem for one campaign at a time.

Run the campaigns in separate terminals:

```sh
python3 tools/live_campaign.py --config .live-campaign-fxs1.json --plain
python3 tools/live_campaign.py --config .live-campaign-fxs2.json --plain
```

Each campaign starts and configures its callback listener before dialing the
request leg. Separate listeners handle simultaneous returns; the returned
GETTY marker must also match that request's nonce. Results and campaign
manifests retain `reference_name`, and each run has its own artifact directory.
An occupied callback port fails setup before the reference is asked to call
back. Existing single-modem configurations keep the default callback port 5060.

## Recording replay

Large recordings stay outside Git. Inputs below are 8-kHz signed 16-bit
little-endian PCM unless a G.711 stream is specified. Replay tests bound input
size and carrier-acquisition time.

| Family | Input settings | Additional controls |
| --- | --- | --- |
| V.22bis | `V22_REPLAY_RX_PCM`, `V22_ANSWER_REPLAY_RX_PCM` | Originating / answering receive recording |
| V.32 | `V32_REPLAY_RX_PCM` | `V32_REPLAY_EXPECT_TEXT` requires decoded text |
| V.34 | `V34_REPLAY_RX_PCM`, `V34_ANSWER_REPLAY_RX_PCM` | Matching `*_SYMBOL_RATE`, `*_BIT_RATE`, `*_LINK_MODE`, `*_RETRAIN_SAMPLE`, `*_EXPECT_TEXT` settings |
| V.90/V.92 | `V90_REPLAY_RX_G711` (exact G.711 payload octets) | `V90_REPLAY_LAW`, `*_ROLE`, `*_PCM_ROLE`, `*_DOWNSTREAM_RATE`, `*_UPSTREAM_RATE`, `*_SYMBOL_RATE`, `*_LINK_MODE`, `*_RX_GAIN`, `*_EXPECT_TEXT` |

Use the controls that match the recording's call role and negotiated profile.
Text expectations distinguish a decoded data path from carrier alone. A replay
that selects V.34 fallback is reported as fallback, not a PCM data pass.

For short native integration runs, `MODEM_INTEGRATION_FILTER` selects ordinary
cases by name and `V34_MATRIX_FILTER` selects rate-matrix cases. Both reject a
filter that matches nothing. Remove them for the complete suite.
