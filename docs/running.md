# Running sippymodem

`slmodemd` runs one modem in the foreground. Configure SIP through environment
variables, then start `./src/slmodemd` (or the installed `slmodemd`). It needs no
hardware-device argument. It prints one PTY device path on stdout; diagnostics
go to stderr. Open the PTY with a terminal program or BBS process. The PTY is
owned by the daemon's user and remains valid only while that process runs.
SIGINT and SIGTERM stop the modem and close its SIP/media resources.

## Interactive console

Install C-Kermit and `splitvt`, then create private profiles:

```sh
cp tools/sip-terminal.example.json .sip-terminal.json
chmod 600 .sip-terminal.json
```

Edit the placeholders for your account or direct SIP peer. Then run:

```sh
python3 tools/modem_terminal.py pstn
python3 tools/modem_terminal.py ata
```

The `ata` profile calls a direct SIP target without registration. `ATDT` with
no destination calls that target. Provider profiles register their configured
account; use `ATDT<number>` with the number format accepted by the provider.
`ATDTsip:<user>@<host>` selects an explicit SIP destination. SIP dialing does
not generate analog DTMF or pulse dialing.

The upper pane shows trace output; the lower pane runs Kermit on the PTY.
`Ctrl-W` switches panes. `Ctrl-O` followed by `q` quits both panes and stops the
modem. Kermit's `Ctrl-\` followed by `Q` also exits; `Ctrl-\` followed by `C`
opens its command prompt, where `exit` does the same cleanup.
The launcher skips Kermit startup files, ignores ambient `SIP_*` variables,
and passes profile settings only to the modem. `--config` selects another
owner-only JSON file, and `--modem` selects another executable.

The console enables tracing but does not record audio. Its temporary trace can
contain network addresses and dialed numbers. Keep captures private and redact
site information before sharing them.

## Direct launch and SIP settings

For a direct peer, replace the example URI before dialing:

```sh
SIP_REGISTER=0 SIP_TARGET=sip:modem@peer.example.invalid ./src/slmodemd
```

Read the printed PTY path and open it with, for example,
`kermit -Y -l /dev/pts/<number> -b 115200`. In Kermit, disable carrier watching
and flow control before entering connect mode. The PTY speed is a terminal
setting; the negotiated modem bitrate is independent of it.

The executable reads these settings once at startup:

| Setting | Default / purpose |
| --- | --- |
| `SIP_LOGIN` | Compact `user:password@server[:port]` account form |
| `SIP_USER`, `SIP_PASSWORD` | Explicit authentication credentials |
| `SIP_DOMAIN`, `SIP_SERVER` | Account/dialing domain and server |
| `SIP_ID`, `SIP_REG_URI`, `SIP_PROXY`, `SIP_REALM` | Explicit identity, registrar, proxy and authentication realm |
| `SIP_REGISTER` | `1`; set `0` for direct unregistered calls |
| `SIP_TARGET` | Destination used by an empty `ATDT` |
| `SIP_BOUND_ADDR`, `SIP_PORT` | Local SIP bind address and UDP port (`5060`) |
| `SIP_PUBLIC_ADDR` | Optional advertised address for an already configured NAT route |
| `SIP_RTP_PORT` | Local RTP UDP port; `0` requests an ephemeral port |
| `SIP_CODEC` | `PCMU`; also `PCMA` or `CLEARMODE` where the bearer permits it |
| `SIP_FRAME_MS` | `10` for the executable; native library defaults to `20` |
| `SIP_REMOTE_CLOCK` | `1`; pace media from accepted incoming RTP |
| `SIP_JB_MS` | `60`; gap-recovery hold with remote clocking, prefill with local clocking |
| `SIP_GAIN`, `SIP_TX_GAIN`, `SIP_RX_GAIN` | Linear gains, default `1.0`; direction-specific settings override the shared value |
| `SIP_PCM_CODEC_GRID` | `0`; enable only for a known direct digital codec path with no analog conversion |
| `SIP_AUTO_ANSWER` | `0`; when `1`, SIP accepts incoming calls and starts the answering modem without waiting for `ATA` |
| `SIP_TRACE`, `SIP_LOG_LEVEL` | Trace enable (`0`) and PJSIP level (`4`) |
| `SIP_RECORD_PREFIX` | Optional fresh private capture prefix; existing capture files are never overwritten |
| `MODEM_LOG_TARGET` | `stderr` or `syslog`; default `stderr` |

Leave public-address overrides unset on a direct route. This executable does
not discover public addresses or configure a router. Static forwarding must
preserve the selected SIP/RTP ports. Symmetric RTP learns the sending peer;
it is not STUN/TURN and cannot traverse every NAT or CGNAT arrangement.
Keep account settings in private files rather than command history.

## Dial-in and automatic negotiation

For a normal application-controlled incoming line, leave `SIP_AUTO_ANSWER=0`.
Attach to the PTY, then issue `ATS0=1`; or wait for `RING` and issue `ATA`.
`ATZ` restores `S0=0`, so reapply auto-answer after a reset. A BBS can perform
this initialization itself. If the process should accept every incoming SIP
call immediately, set `SIP_AUTO_ANSWER=1` before launch and ensure its attached
application is ready to receive data.

The default automatic profile supports V.8 negotiation and legacy answer tones.
Answering begins with ANSam and probes the supported older families if no menu
arrives. Baudot requires explicit selection. PCM digital/analog roles are
negotiated automatically (`AT+V90D=2`); PCM upstream requires `AT+V92U=1`.
Quick Connect is disabled by default. Use the [AT reference](at-commands.md) for
rate caps, forced modes, compression settings and complete defaults.

The executable handles one call at a time. For several calls to one SIP
account, or multiple accounts routed to line pools, embed
[libsippymodem](library-api.md) and configure its
[endpoint/huntgroup interface](terminal-server-integration.md#sip-endpoints-and-huntgroups).

## Diagnostics and recording

`SIP_TRACE=1` reports `CALL` once per second, including SIP state, modem phase,
datapump, current protocol step, directional bit/symbol rates, and audio peaks.
`TRAIN` reports changing training steps at most twice per second.
`DISCONNECT` retains the first cause and says whether the local modem or peer
ended the call. A rate marked `?` is still unknown. The reported line bitrate
is before framing and compression.

`RTP` lines show interval transmit/receive counts, the codec and media endpoint,
accepted/rejected packets, buffering, concealment and late samples. `RX=0`
means no datagrams arrived in that interval. Nonzero RX with zero accepted
packets means received packets were rejected. Buffered/played samples can be
silence; they do not prove that modem data was decoded. Audio peaks are signed
16-bit magnitudes, with `32768` representing full scale.

Select POSIX syslog with `slmodemd --log-target syslog` or
`MODEM_LOG_TARGET=syslog`. The command line overrides the environment. Syslog
uses the executable name, PID and `daemon` facility. Configuration errors before
logger setup still go to stderr. The PTY path and terminal data remain separate
from logs. Authentication records are omitted, but other SIP metadata can
contain phone numbers and addresses.

`ATI11` retains the latest accepted attempt, including failed training, rates,
error control, compression, retrains and the ending phase/reason. It survives
hangup and `ATZ`. Its byte counters describe local queue consumption/delivery,
not peer acknowledgments or BER. Incoming telephone caller identity appears
before verbose `RING` and `CONNECT` when valid and not withheld.

For full evidence, choose a fresh `SIP_RECORD_PREFIX` in a private directory.
The recorder owns a bounded file set, stores exact RTP octets and canonical
little-endian PCM, and latches write errors. `<prefix>.rtp.packets.tsv` records
packet events, monotonic time, RTP sequence/timestamp/SSRC, payload type/size,
and local playout/concealment counters. It contains no payload or addresses;
associated signaling/media files still require private handling.

For unattended PTY operation, see the
[systemd template](../examples/systemd/README.md).
