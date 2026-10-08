# Building and installing sippymodem

The supported host is Linux. Install a C compiler with C11 support, GNU Make,
Python 3, `pkg-config`, and the usual C development headers. The build uses
POSIX threads and Linux PTYs. GCC and Clang can be selected through `CC`.
PJSIP supplies SIP signaling; the modem owns its RTP and DSP processing.

## Choose PJSIP

The default provider is `auto`: an initialized pjproject submodule takes
precedence, followed by a system `libpjproject` found through `pkg-config`.
To fetch the pinned submodule and build everything:

```sh
make pjproject-init
make -j2
```

Only `pjproject-init` accesses the network. Normal builds configure a minimal
static PJSIP under `.build/pjproject/install` and generate DSP profiles under
`.build/generated`. Generated tables are build artifacts.

To use an already installed development SDK instead:

```sh
pkg-config --exists libpjproject
make -j2 PJSIP_PROVIDER=system
```

Add its `lib/pkgconfig` directory to `PKG_CONFIG_PATH` if it is outside the
normal search path. `PJSIP_PROVIDER=submodule` pins the bundled choice;
`PJSIP_PROVIDER=none` permits the dependency-free tests and skips SIP checks.
A runnable binary or library requires PJSIP. A system SDK must be built with
position-independent code for shared-library linking and have sufficient
`PJSUA_MAX_CALLS` capacity for the configured server. The bundled settings
support 32 SIP call slots; retired SIP transactions can occupy slots briefly.

## Build targets

| Target | Result |
| --- | --- |
| `make` or `make all` | `src/slmodemd` and both native libraries |
| `make slmodemd` | PTY/AT executable and its static core |
| `make libsippymodem` | `src/libsippymodem.a` and `.so`, without frontend objects |
| `make test` | Native and Python checks, with local SIP tests when PJSIP is available |
| `make clean` | Remove modem/test outputs and generated profiles; retain PJSIP and compiler cache |
| `make clean-pjproject` | Remove the private PJSIP build |
| `make help` | Short target and option summary |

`CC`, `CXX`, `CPPFLAGS`, `CFLAGS`, and `LDFLAGS` select the toolchain and flags.
The default modem optimization is `-O2 -g`. If `ccache` is installed, compiles
use a cache under `.build/ccache`. Use `CCACHE=` to disable it, or
`CCACHE_DIR=/path/to/cache` to choose a cache. Test concurrency is controlled
by `TEST_JOBS`; `make test TEST_JOBS=4` is the default.

## Install

```sh
make install PREFIX=/usr/local
```

Run installation with write access to the chosen prefix. It installs:

- `bin/slmodemd`;
- `lib/libsippymodem.so` and `lib/libsippymodem.a`;
- `include/sippymodem.h` and `include/sippymodem_fd.h`;
- `lib/pkgconfig/sippymodem.pc`;
- the license under `share/doc/sippymodem`;
- inactive service examples under `share/sippymodem/examples/systemd`.

Override `bindir`, `libdir`, `includedir`, or `docdir` for platform conventions.
For a package staging tree:

```sh
make install PREFIX=/usr DESTDIR=/tmp/sippymodem-stage
```

`DESTDIR` is a staging prefix and does not enter the pkg-config metadata.
`make uninstall` with the same directory settings removes the installed files;
it leaves unrelated files and directories in place. Installation does not
create accounts, change SIP settings, or start services.

The shared library currently has an unversioned filename and no stable ABI
release promise. Recompile consumers against the installed headers when
updating the library. The `2.9.11` metadata and `AT+GMR` value are the inherited
modem compatibility version.

## Link an application

```sh
cc app.c -o app $(pkg-config --cflags --libs sippymodem)
```

Set `PKG_CONFIG_PATH` to the installation's `lib/pkgconfig` if necessary.
The shared library contains the PJSIP objects when built with the bundled
static SDK. The dynamic loader must also find `libsippymodem.so`: configure
its normal system search path, use an application rpath, or set
`LD_LIBRARY_PATH` for a local development run.

The `.pc` file declares PJSIP as a private dependency. Its development metadata
must be discoverable for `pkg-config --cflags`, and static linking additionally
needs its libraries. With the bundled SDK, add
`$PWD/.build/pjproject/install/lib/pkgconfig` to `PKG_CONFIG_PATH`; it is not
installed by `make install`. For deployment of a static SDK, supply the matching
PJSIP development package as well.

To force the modem archive while retaining its private link dependencies:

```sh
cc app.c -o app $(pkg-config --cflags sippymodem) \
  "$(pkg-config --variable=libdir sippymodem)/libsippymodem.a" \
  $(pkg-config --static --libs libpjproject) -lm
```

This selects the modem archive explicitly; the executable can still use the
host's shared C/system libraries. `pkg-config --static --libs sippymodem` also
reports the full dependency list for build systems that choose archives
themselves. See the [API guide](library-api.md) for the lifecycle and ownership
rules.
