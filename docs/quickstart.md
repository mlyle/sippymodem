# Start using sippymodem

The Linux binary archive contains a single-line SIP modem, `bin/slmodemd`,
and the native SDK. Extract it and run it from the extracted directory.
Install C-Kermit if you want to dial from a terminal.

## Dial a direct SIP peer

Run the modem in one terminal. Replace the example URI with your SIP peer.

```sh
SIP_REGISTER=0 SIP_TARGET=sip:modem@peer.example.invalid ./bin/slmodemd
```

It prints its PTY path. In another terminal, run `kermit -Y` and use that
path in place of `/dev/pts/7`.

```text
set carrier-watch off
set flow-control none
set line /dev/pts/7
set speed 115200
connect
```

Type `AT` and press Enter. After `OK`, use `ATDT` to call the configured
peer. To answer a call, wait for `RING` and type `ATA`, or set `ATS0=1` to
answer automatically. Wait for CONNECT before sending data. Use `AT&A0`
with terminal software that expects a plain CONNECT line.

For a telephone number you need a SIP provider or a gateway that carries
modem audio over G.711. See the running and AT command guides in the
matching source archive. Use `ATI11` after a failed call for the last call
report.

## Link an SDK client

```sh
export PKG_CONFIG_PATH="$PWD/lib/pkgconfig"
cc app.c -o app -I"$PWD/include" -L"$PWD/lib" -lsippymodem
LD_LIBRARY_PATH="$PWD/lib" ./app
```

See the source archive's library guide for call control and multiple lines.
`SOURCE.json` identifies the exact source archive that accompanies this build.
