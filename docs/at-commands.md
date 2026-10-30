# AT commands

This reference describes the AT frontend in the normal `slmodemd` build.
The native library uses explicit line controls and literal byte queues; payload
bytes sent through that API are never AT commands or escape sequences.

The implemented parser is [modem_at.c](../src/modem_at.c), terminal/result
handling is [modem_frontend.c](../src/modem_frontend.c), and startup defaults
are in [modem.c](../src/modem.c). The current build's `AT+MS=?` response is the
authoritative list of selectable modulations. See [RTP receive clock and
playout](media-clock.md) for media timing, buffering, and measured receive delay.

## Quick reference

Enter these in command mode and finish each line with Enter. After connecting,
a guarded `+++` returns to command mode; `ATO0` resumes data on the same carrier.

| Command | What it does |
| --- | --- |
| `AT` / `ATI` | Check responsiveness / identify the modem |
| `ATDT` / `ATDT<number>` / `ATDTsip:user@host` | Dial the configured target / phone number / SIP URI |
| `ATA` / `ATS0=1` | Answer once / automatically answer incoming calls |
| `ATH0` | Hang up |
| `AT+MS?` / `AT+MS=?` | Show the configured modulation and bounds / supported choices |
| `AT+MS=34` / `AT+MS=92` | Select V.34 / V.92 with the current fallback setting |
| `AT+V90D=2` / `AT+V92U=1` | Negotiate PCM roles automatically / enable PCM upstream |
| `AT&A7` / `ATI11` | Verbose connection details (already the default) / current or retained call report |
| `ATZ` | Hang up and restore defaults, including disabling auto-answer |

This document describes sippymodem’s implementation. The historical
[Smart Link AT Commands and S-Registers manual](https://bellet.info/laptop/smartlink_v92_12312001.pdf)
is useful background; its command coverage and defaults differ from this build.

## Starting settings

A newly created modem starts with these settings. Startup configuration can
override them; query the modem to check the effective settings.

| Setting | Default | Meaning |
| --- | --- | --- |
| `E`, `Q`, `V`, `X` | `E1 Q0 V1 X4` | Echo, show results, verbose results, include receive bitrate in CONNECT |
| `+MS` | `92,1,300,64000` | V.92 with automatic negotiation and fallback, within the configured rate range |
| `+V90D` | `2` | Negotiate the digital or analog PCM role automatically |
| `+V91` | `1` | Offer bidirectional digital PCM to compatible peers |
| `+V92Q` | `0` | Full startup; Quick Connect requires explicit enablement |
| `+V92U` | `0` | Conventional V.34 upload; PCM upstream requires explicit enablement |
| `+V90U` | `0,0,0` | Automatic conventional PCM-call upload bounds and symbol-rate ceiling |
| `+V34B` | `0` | Automatic V.34 symbol rate |
| `\N` | `1` | Attempt LAPM; allow a raw connection if the peer does not support it |
| `+DS` | `3,3` | Enable compression and offer both V.42bis and V.44 |
| `&A` | `7` | Include modulation, protocol, compression, and transmit bitrate before CONNECT |
| `S0` | `0` | Do not answer automatically |
| `+FCLASS` | `0` | Data modem |
| `+DS0` | `0` | PPP conversion when raw DS0 is selected |
| `+GCI` | `b5` | USA country profile |

Diagnostic connection output is enabled by default. Use `AT&A0` for a plain
CONNECT result, and `ATI11` after a call to inspect its retained report.
For a forced 9600-bit/s V.32 raw connection:

```text
AT\N0
AT+MS=32,0,9600,9600
ATDT<destination>
```

## Command syntax and modes

- Begin a command line with `AT` and finish with carriage return, normally
  byte 13. The configured line-feed character also ends a command. Commands
  are case insensitive. Spaces and tabs are removed before
  parsing, including spaces in dial destinations.
- Commands can usually be joined: `ATE0Q0V1&A7`. `A` and `D` start an
  asynchronous operation and must be last. `Z` also ends processing of the
  line, so issue settings after reset on another line.
- Basic command parameters are decimal numbers. Leading zeroes are ignored:
  `ATE001` means `ATE1`. An omitted parameter normally means zero: `ATE`
  means `ATE0`, and `ATH` means `ATH0`. Unsupported values produce ERROR
  before that command changes settings or call state. Commands with no numeric
  parameter reject a number, including zero.
- Extended commands below support `?` for the current value and `=?` for
  accepted values unless a row says otherwise. Query replies are followed by
  the normal result code. `Q1` suppresses result codes, not explicit query text.
- Numeric extended fields, S-register values, and ATI selectors use decimal;
  leading zeroes do not change the base. Most also accept explicit `0x`
  hexadecimal inputs as an extension. `+V34B` uses decimal; `+GCI` uses
  hexadecimal. The examples use decimal.
- The terminal command buffer holds at most 63 characters before the line
  terminator. Overflow produces ERROR. Commands are not transactional: an
  earlier command can take effect before a later command produces ERROR.
- `A/`, without `AT` or a line terminator, repeats the preceding command.
- In online data mode, a guarded `+++` switches to online command mode.
  Allow at least half a second without transmitted bytes before and after
  the three escape characters; the character is configurable with `S2`.
  This implementation uses a fixed half-second guard, independent of `S12`.
  Carrier, receive data, media processing, and error control stay active.
  `ATO0` resumes data; `ATO1` requests a physical retrain.
- A character sent while an outgoing call is connecting cancels the attempt.
  This is how terminal programs can abort dialing with a keypress.

## Call and terminal controls

| Command | Accepted values / default | Behavior |
| --- | --- | --- |
| `AT` | No argument | Return OK |
| `ATA` | No argument | Answer an incoming call |
| `ATD<destination>` | Destination text | Start a SIP call |
| `ATDT<destination>` | Tone modifier | Dial; the initial `T` is removed from the SIP destination |
| `ATDP<destination>` | Pulse modifier | Dial; the initial `P` is removed from the SIP destination |
| `ATDT` | Empty destination | Call the configured default SIP target, if present |
| `ATDL` | No argument | Redial the saved destination |
| `ATH0`, `ATH` | `0` | Hang up or cancel ringing |
| `ATH1` | `1` | Set off-hook state |
| `ATO0`, `ATO` | `0` | Resume data on the existing carrier without requesting a retrain |
| `ATO1` | `1` | Resume through a full physical retrain; unsupported datapumps resume without one |
| `ATZ`, `ATZ0` through `ATZ3` | All select the same reset | Hang up, restore the default profile, and enter command mode |
| `AT&F`, `AT&F0` | Factory profile `0` | Restore the default profile while preserving an active call |
| `ATE<n>` | `0,1`; default `1` | Command echo |
| `ATQ<n>` | `0,1`; default `0` | `1` suppresses result codes |
| `ATV<n>` | `0,1`; default `1` | `0` numeric results, `1` text results |
| `ATX<n>` | `0..4`; default `4` | `0` emits plain CONNECT; other values append negotiated receive bitrate |
| `ATN<n>` | `0,1`; default `1` | Disable/enable automatic modulation negotiation, equivalent to the second `+MS` field |
| `ATP`, `ATT` | Default `T` | Store pulse/tone preference |
| `ATL<n>` | `0..3`; default `3` | Store speaker volume and call the driver's speaker control |
| `ATM<n>` | `0..2`; default `1` | Store speaker policy and call the driver's speaker control |

SIP dialing does not generate analog pulse dialing, DTMF, pauses, or hook
flashes. Other Hayes dial modifiers are not interpreted as such; they remain
part of the destination text. The active SIP driver has no physical speaker.

`ATZ` and `AT&F` restore the modulation, rate, compression, output, and
legacy control defaults. They clear all S-registers before applying those
defaults, including resetting `S0` to `0`. Both retain the chosen country
profile and the last call report. `AT&F` preserves carrier, queued payload,
and the active negotiated data path. Only factory profile `0` is supported;
other `&F` values return ERROR. No stored profiles are implemented.

## Modulation and rate selection

### `AT+MS=<modulation>[,<auto>[,<minimum>[,<maximum>]]]`

`AT+MS?` returns four fields. `AT+MS=?` returns the modulation list,
`(0,1)` for automatic negotiation, and supported configuration bounds.
Omitted fields retain their current values, including empty comma-separated
fields. Minimum and maximum are inclusive bit/s bounds; they must satisfy
`45 <= minimum <= maximum <= 64000`. A bound does not imply that every integer
rate in the range is a supported wire rate. TX and RX may negotiate different
rates.

| Modulation selector | Name | Line rate or limit |
| --- | --- | --- |
| `21` | V.21 | 300 |
| `22` | V.22 | 1200 |
| `23` | V.23 | 1200 in one direction, 75 in the other |
| `122` | V.22bis | 2400 |
| `32` | V.32 | 9600 |
| `132` | V.32bis | 14400 |
| `34` | V.34 | 33600 |
| `64` | Raw DS0 / DOVBS | 56000 or 64000 in each direction |
| `45` | Baudot TTY/TDD | 45.45 or 50 baud |
| `103` | Bell 103 | 300 |
| `212` | Bell 212A | 1200 |
| `90` | V.90 | 56000 PCM / 33600 conventional |
| `91` | V.91 | 64000 in each digital direction |
| `92` | V.92 | 56000 PCM downstream / 48000 PCM upstream |

The default V.92 automatic mode negotiates with V.8 peers and detects supported
legacy answer signals. Disabling automatic negotiation fixes the requested
modulation; V.34 and PCM protocols still use their required startup exchanges.
Baudot is an explicit selection rather than a default fallback. Use
`AT+MS=45,0,45,45` for 45.45 baud or `AT+MS=45,0,50,50` for 50 baud;
its configured integer rate bounds must admit 45 or 50.

V.34 receive-quality recovery remains active while V.42 detection and LAPM
establishment are in progress, before `CONNECT`. Retraining preserves the
pending error-control negotiation and its retry budget.

V.32 and V.32bis can request a full retrain after 18 consecutive bad receive
quality windows (1.8 seconds at 2400 baud). The receiver gets one second to
settle after training. Two automatic attempts are allowed until 30 seconds of
clean reception rearm recovery; explicit `ATO1` and peer requests remain available.

`AT+V34B=<baud>` selects a V.34 symbol rate. Values are
`0,2400,2743,2800,3000,3200,3429`; `0` is automatic and is the default.
The rounded labels 2743 and 3429 stand for the corresponding fractional
standard symbol rates. For conventional upload within a PCM call, use the
separate `+V90U` symbol-rate ceiling.

### PCM controls

| Command | Values / default | Behavior |
| --- | --- | --- |
| `AT+V90D=<role>` | `0,1,2`; default `2` | `0` analog, `1` digital, `2` automatic PCM role negotiation |
| `AT+V91=<enable>` | `0,1`; default `1` | Enable V.91 offering to compatible digital peers; explicit V.91 calls fail while disabled |
| `AT+V92Q=<enable>` | `0,1`; default `0` | Enable optional V.92 Quick Connect |
| `AT+V90U=<min>,<max>[,<maxbaud>]` | Default `0,0,0` | Conventional V.34 upload bounds and maximum symbol rate in PCM calls |
| `AT+V92U=<enable>[,<maxrate>[,<LUucode>]]` | Default `0` | Enable V.92 PCM upstream and optionally constrain its rate and transmit level |

These commands all support `?` and `=?`. They configure subsequent calls and
do not change the negotiated mode of an established call.

For `+V90U`, each rate is `0` for automatic or a bound in `4800..33600`.
Both fields are required when setting it. If both are nonzero, the minimum
must not exceed the maximum. The optional maximum baud is `0` or one of the
`+V34B` values; omitting it retains the prior ceiling. `AT+V90U?` returning
`0,0,0` is the normal automatic configuration.

For `+V92U`, `0` disables PCM upstream and permits no further fields.
`AT+V92U=1` enables it with automatic rate and transmit level, resetting any
previous optional limits. A maximum rate of `0` is automatic; other accepted
rates are:

```text
24000 25333 26667 28000 29333 30667 32000 33333 34667 36000
37333 38667 40000 41333 42667 44000 45333 46667 48000
```

These are rounded integer bitrates for the discrete standard rates. The
capability response abbreviates them as `24000-48000`; intervening integers
are not accepted. `LUucode` is `0` for the automatic transmit-level choice or
`1..127` for an explicit PCM code. Specify the rate field, possibly `0`, before
specifying `LUucode`. The query omits unused optional fields, so the default
response is `0`, and automatic enabled PCM upstream returns `1`.

If initial automatic PCM-upstream training repeatedly fails at its 24 kbit/s
minimum, the final recovery tries V.34 upstream. This can retain PCM downstream.
After both B1 directions qualify, automatic recovery retains the PCM-upstream
offer through retraining and keeps the existing retry limits. The peer may
still select V.34 upstream. Each new call uses the configured `+V92U` setting.

V.91 is automatically offered in the default mode when both peers can use
bidirectional digital PCM. Each receiver chooses a usable rate from measured
impairments. `+V91=0` leaves ordinary V.90/V.92 role selection and fallback
available. `AT+MS=91` can still store the requested selector while V.91 is
disabled, but a call using it fails. `+V91=1` re-enables it, and `ATZ` restores
that enabled default.

### Raw DS0 / DOVBS

Select raw DS0 directly with `AT+MS=64`, or let V.8 automatic mode detect
a peer that supplies a strict HDLC proof. The configured rate range must admit
56000 or 64000 bit/s; the inherited default `300..64000` admits both.
The physical clock is 8000 codewords/s at either rate: 56000 carries seven
bits per codeword and 64000 carries eight. This mode connects directly over a compatible digital
bearer carrying exact G.711 codewords.

In V.8 automatic mode, DS0 detection requires at least 1200 consecutive exact
G.711 codewords that parse completely as 56- or 64-kbit/s HDLC, including at
least one complete frame with a valid FCS. Idle flags or all-one bits alone do
not select DS0. A bad FCS, invalid stuffing, runt or oversized frames, or
missing media resets the proof streak. Detection uses received codewords,
never values reconstructed from linear audio. It preserves the qualifying frames for the selected data path
and leaves the stored `+MS` setting unchanged. It does not advertise DS0 in a
V.8 menu. Disabling automatic negotiation or choosing a range that admits
neither 56000 nor 64000 disables this detection.

The peer must send a qualifying frame before detection can occur. A passive
LAPM answerer that waits for our XID supplies no such proof and requires an
explicit `AT+MS=64` selection. A detected DS0 call uses the configured `+DS0`
framing mode, just like an explicit selection.

To force a rate:

```text
AT+MS=64,0,56000,56000
AT+MS=64,0,64000,64000
```

`AT+DS0=<framing>` selects the DS0 byte interface. The default `0` converts
asynchronous PPP frames to and from synchronous HDLC. For that call, it bypasses
modem error control and compression, including a stored required-LAPM policy.
Value `1` enables the normal LAPM/HDLC path for the call, independently of stored
`\N`, and uses the `+DS`/`+DSC` compression settings. It skips asynchronous
V.42 detection. Both modes preserve the stored error-control and compression
settings for subsequent calls with other modulations.
`AT+DS0?` reports `0` or `1`; `AT+DS0=?` reports `(0,1)`. `ATZ` restores `0`.
The setting is backed by S228.

PPP mode reports bearer readiness once exact media processing begins, so the
client can send LCP even when the answerer waits for it. For an explicit DS0
call whose configured range admits both rates, DS0 starts at 64000 and observes
HDLC at both rates. After eight nominal seconds without a valid CRC-checked
frame, it tries 56000.
The first valid frame selects and locks the rate; a late valid 64000 frame can
still select 64000 after fallback. Initial CONNECT can therefore show the
provisional 64000 rate; current status and ATI11 track the selected rate.
A detected call starts at its proven rate. LAPM mode still waits for its
error-control establishment before CONNECT.
See [raw digital calls](ds0.md) for framing limits and throughput measurements.

## Error control and compression

| Command | Values / default | Behavior |
| --- | --- | --- |
| `AT\N<n>` | `0..5`; default `1` | Error-control policy, as described below |
| `AT+DS=<direction>[,<types>]` | Each `0..3`; default `3,3` | Configure compression enablement and allowed compression types |
| `AT+DSC=<types>` | `0..3`; default `3` | Set only the allowed compression types |
| `AT%C<n>`, `AT&K<n>` | `0..3`; default `3` | Aliases for the first `+DS` field |

`+DS` and `+DSC` support `?` and `=?`. Basic aliases are queried through
`S104` and `S105` rather than a `?` suffix.

`\N0` selects raw asynchronous framing. `\N1` and `\N3` attempt LAPM and
allow raw fallback. `\N2`, `\N4`, and `\N5` require successful error-control
detection where the selected modulation supports it. These values do not
select separate MNP implementations: the implemented error-control protocol
is V.42 LAPM. Both negotiated directions must run at least 1200 bit/s.
Bell 103, V.21, V.23, Baudot, and V.22 at 600 bit/s use their raw paths.

When seeking error control, detection allows up to 7.5 seconds of active
media for a delayed V.42 request, reply, or protocol flags over SIP. Physical
retraining pauses this budget. This is a compatibility extension to the
standard's 750-ms default. An explicit `\N0` answer retains 750 ms; valid
negotiation completes early, and missing replies still cause bounded fallback
or disconnection according to the selected policy. Convincing raw text also
ends detection early; its bytes and any partial next character are preserved.

The compression type field is a mask: `0` none, `1` V.42bis, `2` V.44,
`3` both. Omitting it from `+DS` retains the prior mask. Compression requires
LAPM and peer agreement; offering V.44 does not mean a peer will choose it.
The direction field is `0` for no compression, `1` for transmit only, `2`
for receive only, and `3` for both directions. These are local DTE directions;
XID negotiates compatible directions with the peer for either compression type.
`S104` uses its low two bits for this direction mask.

This two-field `+DS` form is a compatibility extension. The standard
[V.250 §6.6.1](https://www.itu.int/rec/T-REC-V.250-200307-I/en) form defines
V.42bis direction, required negotiation, dictionary size and maximum string
length. Only the direction field has that meaning in this compatibility form.

## Connection output and retained call report

`AT&A<n>` accepts `0..7`, default `7`. Its bits control extra lines before
verbose CONNECT:

| Bit | Output |
| --- | --- |
| `1` | `Modulation`, recognized `Startup`, and PCM `Upstream` mode |
| `2` | `Protocol: LAPM` or `Protocol: NONE` |
| Both `1` and `2` | Negotiated `Compression: V44`, `V42bis`, or `NONE` |
| `4` | `TxRate: <bit/s>` |

`AT&A7`, the default, enables all of these. The rate appended to CONNECT is
the receive bitrate; `TxRate` is the transmit bitrate. They describe line rates
rather than PTY baud, compressed throughput, or application transfer speed. Numeric
results (`V0`) omit these extra lines. The diagnostic lines and CONNECT form
one block, with a single line for each item. `ATI11` reports use the same spacing.

Incoming calls with a usable telephone user in the SIP From header produce a
separate `CallerID: <number>` line immediately before verbose RING and
CONNECT. The standard result lines remain intact. Telephone visual separators
are removed; an optional leading `+` is retained. Missing, anonymous,
withheld, oversized, or invalid identities are omitted. Caller identity and
the Request-URI called number are separate facts. Numeric or quiet result
mode omits these caller lines. Outgoing calls do not synthesize a caller
number from the local account name.

`ATI11` shows the latest accepted call attempt, including:

- Incoming/outgoing direction, active/completed state, and whether it reached
  modem/error-control readiness.
- Caller ID when available, modulation, TX/RX bitrates and baudrates, error
  control, and negotiated compression.
- Processed media time, local DTE bytes consumed for TX and delivered for RX,
  and completed physical retrains.
- Final result and the first disconnect origin/reason, with the protocol
  phase and step copied when that cause was recorded.

The report survives hangup, idle commands, and reset. A new accepted attempt
replaces it; during that attempt `ATI11` reports its live facts. Attempts that
fail before CONNECT are retained too. Before any attempt it returns
`Last call: none`.

Media time counts samples processed by the modem and excludes SIP waiting
without media. Byte counts describe local DTE work, not acknowledged bytes at
the peer. The report does not estimate BER. Baud and rates unavailable before
successful training may be zero or unknown. Inspect the terminal's live
training trace for intermediate negotiation steps.

## Identification and country profile

| Command | Reply |
| --- | --- |
| `ATI`, `ATI0` | Product name |
| `ATI1`, `ATI2` | Product, version, and author |
| `ATI3` | Device name and driver name |
| `ATI4` | S-registers 0 through 23 |
| `ATI5`, `ATI6` | Stored-profile heading only; no profile storage is implemented |
| `ATI7` | Current country name |
| `ATI11` | Retained latest-call report |
| `AT+GMI` | Author/manufacturer identification |
| `AT+GMM` | Product identification |
| `AT+GMR` | Version |
| `AT+GCAP` | Supported capability groups: `+MS`, `+DS`, and the `+FCLASS=0` stub |
| `AT+GCI=<hex>` | Choose a supported country code |
| `AT+GCI?` | Current code, prefixed `+GCI:` |
| `AT+GCI=?` | Available codes |
| `AT+FCLASS=0` | Data modem class (`0` only) |
| `AT+FCLASS?`, `AT+FCLASS=?` | `0` |

The data class accepts the shared numeric syntax: `AT+FCLASS=00` selects
class zero. Unsupported classes and fax operation commands produce ERROR.

Other ATI selectors produce ERROR. `+GMI`, `+GMM`, `+GMR`, and `+GCAP` are
information actions: bare execution returns information, and the `=?` test
returns OK without information. They accept no parameters or `?` read form.
Use `;` between an extended command and a following command, for example
`AT+GMI=?;+GMM=?;+GCAP=?`. Direct `+` chaining remains supported for compatibility.
These forms follow [V.250 sections 5.4.3 and 5.4.5](https://www.itu.int/rec/T-REC-V.250-200307-I/en).

Country selection changes the retained legacy regional
timing parameters; it does not select SIP destinations or codecs. Codes above
`ff` are retained legacy regional profile identifiers. Negative, overflowing,
unsupported, or extra values produce `ERROR` without changing the profile. `ATZ`
preserves the selected country.

## S-registers

Use `ATS<n>?` to read a register and `ATS<n>=<value>` to write it. All indices
`0..255` are accessible. Values must fit in one byte (`0..255`); S3, S4, and
S5 accept only `0..127`, as required by V.250 for interface characters. S228
accepts only 0 or 1. Negative numbers, overflow, and values outside these
ranges produce `ERROR` and leave the register unchanged. An omitted index
or value means zero. Queries return three decimal digits, including leading
zeroes. Other registers do not apply the validation performed by named AT
commands; prefer those controls for modulation and protocol configuration.

The defaults below apply to a newly created modem with the USA country profile.
Legacy hardware timing fields are retained for compatibility; most are not
SIP timers. Unlisted registers initially contain zero.

| Register | Initial value | Meaning / corresponding command |
| --- | --- | --- |
| S0 | 0 | Number of rings before auto-answer; 0 disables |
| S1 | 0 | Current ring counter |
| S2 | 43 | Escape character, `+` |
| S3 | 13 | Command terminator, carriage return |
| S4 | 10 | Result newline, line feed |
| S5 | 8 | Command-edit backspace |
| S6 | 2 | Legacy dial-tone wait, seconds |
| S7 | 60 | Started call attempt timeout in the executable, seconds |
| S8 | 2 | Legacy dial pause, seconds; country-dependent |
| S9 | 6 | Legacy carrier-detect response, tenths of a second |
| S10 | 7 | Legacy carrier-loss delay, tenths of a second |
| S11 | 100 | Legacy DTMF duration, milliseconds |
| S12 | 50 | Retained escape-delay field; the frontend uses a fixed half-second guard |
| S13 | 1 | Echo, `E` |
| S14 | 0 | Quiet result mode, `Q` |
| S15 | 1 | Verbose result mode, `V` |
| S16 | 1 | Tone/pulse preference, `T`/`P` |
| S18, S19 | 0 | Retained test/inactivity timer fields |
| S21, S22, S23 | 0 | Retained break/role/XOFF fields |
| S24 | 50 | Legacy hook-flash time, units of 10 milliseconds; country-dependent |
| S25, S26, S27 | 0 | Retained DTR/CTS/auto-answer timing fields |
| S28 | 0 | Pulse ratio, `&P` |
| S29 | 1 | Speaker policy, `M` |
| S30 | 3 | Speaker volume, `L` |
| S31 | 1 | Automatic negotiation, `N` and second `+MS` field |
| S32 | 92 | Requested datapump, first `+MS` field |
| S39 | 0 | Stored quality-control flag, `%E` |
| S56 | 4 | CONNECT result format, `X` |
| S60 | 0 | Stored carrier-control flag, `&C` |
| S62 | 0 | Stored flow-control flag, `&H` |
| S70 | 7 | Extra CONNECT output mask, `&A` |
| S71 | 0 | Stored CONNECT-speed-source flag, `&E` |
| S92 | 2 | Answer-side training delay, seconds |
| S103 | 1 | Error-control policy, `\N` |
| S104 | 3 | Compression enablement, `%C`, `&K`, first `+DS` field |
| S105 | 3 | Compression type mask, `+DSC`, second `+DS` field |
| S161 | 0 | Stored default-setting flag, `Y` |
| S222 | 2 | PCM role policy, `+V90D` |
| S224 | 0 | Quick Connect policy, `+V92Q` |
| S225 | 0 | PCM upstream enablement, `+V92U` |
| S226 | 0 | PCM upstream maximum data-rate number; 0 automatic |
| S227 | 1 | V.91 enablement, `+V91` |
| S228 | 0 | Raw DS0 framing mode, `+DS0` |

`+MS` rate bounds, `+V34B`, `+V90U`, and the PCM transmit-level override are
owned configuration fields rather than individual S-registers.

## Legacy forms and limits

| Form | Current behavior |
| --- | --- |
| `ATB` | Select fixed V.22 at 1200 bit/s; use `+MS` for normal modulation selection |
| `ATB0`, `ATB1` | Unsupported; produce ERROR without changing modulation selection |
| `AT&D[<n>]`, `AT&R[<n>]`, `AT&S[<n>]` | `0..9`, default 0; accepted no-ops; no DTR/RTS/DSR policy is applied |
| `AT&C<n>` | `0,1`, default 0; stores S60 without overriding actual carrier indication |
| `AT&E<n>` | `0,1`, default 0; stores S71 without changing CONNECT's physical receive rate |
| `AT&H<n>` | `0,1`, default 0; stores S62 without enabling RTS/CTS or XON/XOFF handling |
| `AT&P<n>` | `0..3`, default 0; stored pulse ratio; SIP does not pulse-dial |
| `AT%E<n>` | `0..2`, default 0; stored S39 flag; does not control automatic DSP quality recovery |
| `ATY<n>` | `0..3`, initially 0; stored S161 flag; does not select a persisted profile |
| `AT&V`, `AT&W` | ERROR; profile display/storage is unavailable |
| `AT*B`, `AT\A` | Unsupported; produce `ERROR` |
| Six-field `+MS` or four-field `+DS` legacy forms | ERROR; use the implemented four-field `+MS` and two-field `+DS` forms above |
| `AT+DS44`, `AT+DR`, `AT+IFC`, `AT+VCID`, `AT#CID` | Unsupported in the normal build |
| Fax class 1 and voice class 8 commands | Unavailable in the normal build |

Other command names produce ERROR in the normal build.

## Result codes

| Verbose (`V1`) | Numeric (`V0`) |
| --- | --- |
| OK | 0 |
| CONNECT | 1 |
| RING | 2 |
| NO CARRIER | 3 |
| ERROR | 4 |
| NO DIALTONE | 6 |
| BUSY | 7 |
| NO ANSWER | 8 |

Verbose replies (`V1`) begin and end with `S3 S4` (default CR LF). Numeric
results (`V0`) end with `S3` alone; information responses still end with
`S3 S4`. Command echo is independent of reply formatting.

`Q0` shows results; `Q1` suppresses them. CONNECT is reported after modem
training and any selected error-control establishment are ready. With `X1`
through `X4`, verbose CONNECT appends the receive bitrate. Numeric CONNECT
remains `1` regardless of rate. The remaining distinctions among historical
X modes do not enable physical dial-tone or busy-tone detection on SIP.
