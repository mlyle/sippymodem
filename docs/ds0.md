# Raw digital DS0 calls

Datapump 64 carries framed data in an octet-transparent, 8000-codeword/s
G.711 bearer. It requires a digital path that preserves the codewords;
analog conversion, transcoding and ordinary voice processing corrupt it.
Explicit selection bypasses V.8. Automatic V.8 startup can also recognize a
peer already transmitting raw DS0; it does not advertise DS0 in a V.8 menu.

| Command | Selection |
| --- | --- |
| `AT+MS=64` | Detect 56 or 64 kbit/s |
| `AT+MS=64,0,56000,56000` | Force 56 kbit/s |
| `AT+MS=64,0,64000,64000` | Force 64 kbit/s |
| `AT+DS0=0` | Asynchronous PPP conversion (default) |
| `AT+DS0=1` | Ordinary LAPM/HDLC data path |

The accepted rate range must include 56000 or 64000. Both variants use an
8000-codeword/s clock: 64 kbit/s uses all eight bits, while 56 kbit/s uses
bits 7 through 1 and ignores the low bit. The 56 variant tolerates robbed
low bits, including one codeword in six; the 64 variant requires all bits.
The reported bitrate is the nominal bearer rate. Wall-clock throughput also
depends on the media clock and framing overhead; see [media clock](media-clock.md).

## Automatic recognition

With automode enabled and the configured rate range admitting 56000 or 64000,
V.8 observes exact received G.711 codewords in both call roles. A candidate
requires at least 1200 consecutive whole codewords that parse cleanly as HDLC,
including a complete frame with a correct FCS. Verified flags, complete frames,
and legal mark idle (at least fifteen consecutive ones between flags) count;
speculative data does not. Bad FCS, invalid stuffing, runt
or oversized frames, and media gaps reset that candidate. Idle flags and
all-one silence alone cannot select DS0. The 56 candidate ignores robbed low
bits. The accepted prefix, including the proving frames, survives handoff.

Each line retains its configured PPP or LAPM framing policy. Stored modulation
settings remain unchanged. Peers that wait without sending a frame require
explicit `AT+MS=64`; a passive LAPM answerer waiting for caller XID is one such
case. A fixed protocol selection with automode disabled does not probe DS0.

## Framing and startup

`+DS0=0` converts literal asynchronous PPP frames to synchronous HDLC and
back, using 16-bit FCS and the framing in [RFC 1662](https://www.rfc-editor.org/rfc/rfc1662.html).
It escapes all outgoing control bytes. Native DTE bytes are literal; the converter interprets PPP frame flags
and PPP escaping. It does not interpret AT text or software flow control.

Incoming asynchronous frames start with receive ACCM `FFFFFFFF`. Valid LCP
negotiation selects the map requested by the remote peer after matching
Configure-Acks in both directions. Raw controls selected by that map are
removed before unescaping and FCS checking; escaped controls remain data.
A partially assembled frame keeps its initial map, and a new call restores
the default.

The compatibility frontend provides its usual AT and guarded `+++` controls. This selection bypasses modem error control and compression
for the call without changing the saved `\N`, `+DS` or `+DSC` settings.

There is no raw-bearer training exchange. Either role reports physical
readiness when media starts so its PPP endpoint can initiate LCP. An
automatic selection starts transmitting at 64 kbit/s while independently
trying both receive formats. A valid frame FCS selects the corresponding
rate. Without a valid frame, transmit falls to 56 after eight seconds of
nominal media samples; a later valid 64 frame can still select 64. PPP must
retry startup packets while the rate is being identified. A ready PPP
bearer alone does not establish a PPP session.

`+DS0=1` uses the shared LAPM HDLC transmitter and receiver, negotiation,
retransmission and compression. It forces LAPM for this call and skips the
asynchronous V.42 detection exchange; saved compression settings still
apply. CONNECT and native connected events wait for error-control readiness.

PPP frame bodies are bounded to 2048 bytes. Local oversize frames and
receive backpressure overflow end the call with an explicit local cause;
partial accepted input and output remain owned by the line. Missing media
aborts the partial HDLC frame rather than substituting audio silence for
its bits. PPP provides its own recovery; LAPM uses its shared retransmission
path. `ATZ` restores PPP mode, also exposed as bounded S-register 228.

## Verification

Deterministic tests cover both rates and G.711 laws, shared HDLC framing,
FCS rejection, partial byte I/O, media gaps, robbed low bits, automatic rate
selection, passive PPP startup, overflow, native SIP calls and command-mode
carrier/data preservation. Separate manual wire and PPP comparisons passed both
directions at 56 and 64 kbit/s, including 2048-byte frames, one-byte partial
input/output and robbed low bits on one codeword in six.

An owned localhost native SIP study with 20-ms packets transferred 65,536
PPP frame-body bytes each way in about 6.29 seconds (83.3 kbit/s) on both
G.711 laws with remote clocking. The synchronous codeword count implies a
6.27-second lower bound at the 4/3 ceiling. Both directions were byte exact,
with no concealed samples. A wall-clock control retained the nominal
64-kbit/s bearer clock. These are host measurements, not a Pi capacity claim.
