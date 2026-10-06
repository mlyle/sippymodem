# Memory checks

Run the native checks under Valgrind Memcheck from the repository root:

```sh
python3 tools/memcheck.py --output /tmp/modem-memcheck-core
```

Valgrind must be installed. The runner builds the executable, library, and
native tests before checking them. When using the bundled PJSIP installation,
set `PKG_CONFIG_PATH` to `.build/pjproject/install/lib/pkgconfig` first, as for
the normal build. Use a new output directory for each run.

## Coverage

The default `core` suite covers the library and compatibility frontend,
partial byte I/O, queue bounds, command mode, lifecycle and shutdown, SIP
resource failures, RTP, recording, DS0, LAPM, V.42bis, and V.44. Tests requiring
an unavailable build dependency are explicitly reported as skipped. Inspect
those skips before claiming library or SIP coverage.

Add representative modem calls with:

```sh
python3 tools/memcheck.py --integration --output /tmp/modem-memcheck-calls
```

These checks train paired modem cores over both PCMU and PCMA, exchange exact
payloads, and exercise resume or retraining. Profiles cover V.21, Bell 103,
V.22bis, Bell 212A, V.23, V.32bis, V.34, V.90, V.91, and V.92 PCM duplex.
The SIP tests use local loopback endpoints. These commands make no PSTN calls.
Optional recording replays report a skip when their external vectors are absent.

`--suite native` checks every registered native test except the large PCM call
and modem integration matrices. `--suite full` also runs all four PCM call
shards, the integration suite, and all six V.34 symbol-rate matrices. These can
take hours under Memcheck; increase `--timeout` from its default of 3600 seconds
per check when needed. A timeout is an incomplete check and fails the run.

Select individual registered tests with repeated `--test NAME` arguments.
`--skip-build` uses existing binaries; use it only after a successful build.
`--jobs` controls parallel checks and defaults to four.

## Results

Each check writes a `.out` test transcript and `.json` result. Its `.vg`
file retains the Memcheck report when available. `run.json` records the
revision, tracked changes, Valgrind version, selected checks, and unavailable core tests. Results retain each executable's
SHA-256 digest, command, selected environment overrides, elapsed time, error
count, heap at exit, leak counts, and invalid descriptor warnings.

The runner exits unsuccessfully for a failed test, memory error, definite,
indirect or possible leak, invalid descriptor warning, timeout, or missing
Memcheck summary. It adds no suppressions. Descriptor tracking also records
open handles: inspect these reports for unexpected sockets or files. The
Valgrind report descriptor itself is expected to remain open.

Whole-struct `memcmp` assertions can read unspecified padding even when every
declared field is initialized. Fix these tests with typed field comparisons;
preserve exact floating-point representations where the contract requires
them. Avoid suppressing undefined-value reports globally: a report may expose
a real missing field or an incorrectly initialized malformed-input fixture.
The broad native suite still contains padding-sensitive DSP snapshot tests,
so a clean core run does not establish that every native test is clean.

Keep large reports and recording evidence outside the Git tree.
