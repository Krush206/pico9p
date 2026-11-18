# pico9p

A lightweight 9P file server ported to the Raspberry Pi Pico RP2040, built on
the c9 9P library. It serves a small flash-backed 9P tree over USB CDC (no UART
available on this target).

## Layout

```
src/
  main.c          # entry point: LED, srv_init, TinyUSB init, c9 context, s9proc loop
  9psrv.c         # 9P server logic (node pool, fid table, GPIO, flash store, dispatch)
  9psrv.h         # Srv/SrvNode/SrvFid structs, QID constants, srv_init/srv_t declarations
  usb.c           # TinyUSB CDC-only device descriptors
  include/
    c9/proto.h    # 9P (de)serialization, ctx/server response helpers
    c9/fs.h       # POSIX-like blocking file API
    c9/fs9p.h     # 9P client file API
    tusb_config.h # TinyUSB configuration (CDC-only, RP2040)
  lib/
    c9/
      proto.c     # c9 protocol (de)serialization
      fs9p.c      # c9 POSIX-like file API
CMakeLists.txt    # build: c9 library + pico9p executable
```

## Served 9P tree

```
/                 directory
/dev/             directory
/dev/gpio0..gpio29  read => "0"/"1", write "0"/"1" to drive the pin
/flash/           directory
/flash/<name>     file stored in the reserved flash region
```

## Transport

USB CDC only (TinyUSB device stack, bare metal, no RTOS). SDK stdio over USB and
UART is disabled so the CDC interface is owned entirely by the 9P server.

Device: VID 0xCafe, PID 0xC001, single CDC interface.

## Build

Requires the Raspberry Pi Pico SDK and `arm-none-eabi-gcc`:

```sh
mkdir build
cd build
cmake -DPICO_SDK_PATH=/path/to/pico-sdk-2.3.1 \
      -DPICO_TOOLCHAIN_PATH=/path/to/gcc-arm-none-eabi-10.3-2021.10 \
      ..
make
```

Outputs: `build/pico9p.elf`, `build/pico9p.bin`, `build/pico9p.uf2`.

The flash storage region defaults to the last 256 KiB of flash
(`PICO_9P_FLASH_OFFSET=0x00180000`, `PICO_9P_FLASH_SIZE=0x00040000`). Override
with `-DPICO_9P_FLASH_OFFSET=... -DPICO_9P_FLASH_SIZE=...` if needed.

## c9 library

The bundled c9 library (in `src/lib/c9/` with headers in `src/include/c9/`) is a
freestanding C 9P implementation: zero dependencies, no dynamic memory, only
`<stdint.h>`/`<string.h>`. It provides:

- `proto.[ch]` — 9P message (de)serialization and tag allocation
- `fs.h` / `fs9p.[ch]` — POSIX-like blocking file API over a 9P context

The library is built with both client and server interfaces enabled
(`fs9p.c` needs the client functions).
