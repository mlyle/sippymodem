# Building and installing sippymodem

Install a C compiler with C11 support, GNU Make, Python 3, `pkg-config`, and
C development headers. PJSIP supplies SIP signaling; the modem owns its RTP
and DSP processing. GCC and Clang can be selected through `CC`.

| Host | Products and validation |
| --- | --- |
| Linux | PTY/AT executable and library; native regression suite |
| macOS | POSIX executable and `.dylib` build paths; source reviewed, native execution unverified |
| Windows | MinGW-w64 library with winpthreads; DLL and concurrent V.21 calls tested under Wine, native Windows execution unverified |

The terminal-server byte-array API is available on every target. The PTY
frontend and native fd/pppd adapter require POSIX. On Windows the fd adapter's
create operation returns NULL with `errno == ENOSYS`; use literal byte I/O.

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
| `make` or `make all` | Host products: executable on POSIX, plus static and shared libraries |
| `make slmodemd` | PTY/AT executable and its static core |
| `make libsippymodem` | `src/libsippymodem.a` and the host shared library, without frontend objects |
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
- the host shared library and `lib/libsippymodem.a`;
- `include/sippymodem.h` and `include/sippymodem_fd.h`;
- `lib/pkgconfig/sippymodem.pc`;
- the license under `share/doc/sippymodem`;
- inactive Linux service examples under `share/sippymodem/examples/systemd`.

On macOS the shared library is `lib/libsippymodem.dylib`. Windows installs
`bin/libsippymodem.dll` and its `lib/libsippymodem.dll.a` import library; it
omits the POSIX executable and systemd examples.

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

## macOS and Windows toolchains

On macOS, use GNU Make and a matching PJSIP SDK or the pinned submodule.
The build selects Mach-O linking, an `@rpath/libsippymodem.dylib` install name,
and a public-symbol export list. Set the consuming application's rpath to
its library directory. PTY creation uses `openpty`; native fd pumping uses
nonblocking socket pairs and kqueue for command-mode half-close detection.
The frontend uses `poll()` with waits rounded up by less than 1 ms.
Each DSP worker requests an 8 MiB stack for nested PCM training scratch.
These Apple-specific paths have not been executed on a Mac.

For native Windows, use MinGW-w64 with POSIX threading support (winpthreads),
a Unix build shell, and a PJSIP SDK built for that same target. `make` builds
the library products on this target. A cross build with an existing SDK is:

```sh
export PKG_CONFIG_PATH=/path/to/windows-pjsip/lib/pkgconfig
make -j2 PJSIP_PROVIDER=system CCACHE= \
  CC=x86_64-w64-mingw32-gcc-posix CXX=x86_64-w64-mingw32-g++-posix
```

Use your toolchain's executable names; some installations omit `-posix`.
The pinned PJSIP build also passes the MinGW compiler target to configure.
Build outputs include `.a`, `.dll`, and `.dll.a`. Install the matching compiler
runtime DLLs with the application, including winpthreads; inspect the resulting
DLL's imports for the complete list. The library targets Windows 10 or later.
MSVC and other Windows toolchains have not been validated.
The embedding application owns child-process handle inheritance. Disable
inheritance, or pass an explicit handle list for a service: PJSIP can create
inheritable internal sockets.

Use `sippymodem_wait()` between owner-loop process passes on every platform;
it handles media, DSP and event readiness and the most recent process deadline.
Applications with an existing native event loop can use the borrowed Windows
`sippymodem_event_handle()` or POSIX `sippymodem_event_fd()` and that deadline.
See the [API guide](library-api.md#portable-terminal-server-loop). SIP and socket
I/O remain on the native owner thread, and each started modem retains its own
DSP thread.

## 32-bit builds

Use a separate source checkout for each compiler/ABI. The PJSIP build and
modem objects must target the same ABI; changing `CC` does not make existing
objects compatible. With a multilib C/C++ development environment:

```sh
make -j2 CC='gcc -m32' CXX='g++ -m32'
make test CC='gcc -m32' CXX='g++ -m32'
```

The mixed-radix and PCM paths use exact portable wide arithmetic when the
compiler has no native 128-bit integer type. Ordinary tests exercise that
fallback on native builds too.

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
