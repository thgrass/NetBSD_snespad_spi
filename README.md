# snespad(4) — SNES gamepads on spi(4) buses

This is a general spi(4) driver for SNES gamepads.  It attaches
exclusively via `snespad* at spi? slave N` and uses `spi_configure()` /
`spi_recv()` only when compiled as built-in.  It compiles on any NetBSD
architecture (verified as an aarch64 module and as a built-in compile).
Use it on a machine with a working spi(4) bus. Using it as a module that
loads during runtime needs a patched kernel that enables `spi_reattach()`.
A patch (patch #3) doing that is provided with this repo.

## Hardware

The reference setup is the **SNES-Gamepads-to-SPI-Adaptor** board -
https://github.com/thgrass/SNES-Gamepads-to-SPI-Adaptor
(see its README and linked references for the protocol details).
It adapts up to two SNES controller sockets to one SPI port: each
socket's Latch is wired to that socket's chip select (CE0/CE1) and its
Data line is gated onto the shared MISO by a 74AHCT125 whose output
enable is the same chip select.  Plugged onto the Raspberry Pi SPI0
header it uses SCLK=GPIO11, MISO=GPIO9, CE0=GPIO8 (socket 1),
CE1=GPIO7 (socket 2), +5V (or +3V3), GND.  On any other machine, wire the
adaptor's SCLK/MISO/CE0/CE1/+5V/GND to the SPI port's clock, MISO and
two chip selects - slave N = CS N.

With a single controller you don't need the adaptor board at all:

The 74AHCT125 buffers on the adaptor board exist only because two pads
share one MISO line there: a SNES pad's Data output is push-pull, so
without per-socket tri-state buffers both pads would drive MISO at once
(bus contention).  With a single controller you can skip the adaptor
entirely and wire the pad's plug straight to any SPI port:

    SNES plug        SPI port
    ---------        --------
    +5V  (pin 1) ->  +5V
    GND  (pin 7) ->  GND
    Data (pin 2) ->  MISO
    Clock(pin 3) ->  SCLK
    Latch(pin 4) ->  a chip select, e.g. CE0  (slave 0) or CE1 (slave 1)

The driver works identical in both cases: it asserts the slave's CS (which
is the pad's Latch), then clocks out 16 mode-0 bits, MSB first,
active-low (B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R + 4
ones).  Configure the driver with the matching slave number
(`snespad* at spi? slave 0` for CE0, `slave 1` for CE1).  Only use one pad
per MISO line unless you add your own tri-state buffer per pad: that is
what the adaptor board does.

## What it exposes

  * a `wskbd(4)` keyboard per pad, so wscons/X11 applications and
    emulators (e.g. snes9x) see the pads as plain keyboards.
    Up to 4 pads (slave 0..3) are supported; by default the module
    config enables 2 (`snespad0`/`snespad1`; `snespad2`/`snespad3` are
    commented out in `src/module/snespad.ioconf` — uncomment for buses
    with more CS lines) and the driver's `SNESPAD_MAX_PORTS` defaults to
    4 (override at compile time via `CPPFLAGS+= -DSNESPAD_MAX_PORTS=N`
    in `src/module/Makefile`).  Each port maps its
    buttons to a **different key set** so multiple pads on the same
    keyboard stay distinguishable:
    - port 0 (slave 0): D-pad = arrows, A=a, B=b, X=x, Y=y, L=l, R=r,
      Start=Return, Select=Space
    - port 1 (slave 1): numeric keypad (D-pad = KP 8/2/4/6, A=KP 1,
      B=KP 3, X=KP 9, Y=KP 7, Start=KP Enter, Select=KP 0, L=KP -, R=KP +)
    - port 2 (slave 2): F1..F12
    - port 3 (slave 3): c, d, e, f, g, h, i, j, k, m, n, o
    Requires `wskbd* at wskbddev? mux 1` in the kernel config (the driver
    provides the `wskbddev` attribute).

Protocol (from the adaptor repo + linked references): assert the pad's
chip select (latch low) and 16 SPI mode-0 clocks shift out B, Y, Select,
Start, Up, Down, Left, Right, A, X, L, R + 4 ones, MSB first, active low;
idle pad = raw `0xffff`.  A pad is only trusted when the trailing four
one-bits are present (`raw & 0xf == 0xf`); an empty socket (floating data
line, often `0x0000` = "all pressed") is reported as no buttons, so it
cannot generate phantom key events.  A kernel thread samples the pads at
~60 Hz.  Debug logging: `sysctl -w hw.snespad_debug=1` (changes) or `=2`
(every sample), output kernel messages.

## Layout

    patches/01-files.spi.diff        -> sys/dev/spi/files.spi
    patches/02-modules-Makefile.diff -> sys/modules/Makefile
    patches/03-spi-rescan.diff       -> sys/dev/spi/spi.c       (OPTIONAL, to make kmod work)
    patches/04-manpage.diff          -> share/man/man4/Makefile + new share/man/man4/snespad.4
    src/snespad.c                    -> sys/dev/spi/snespad.c
    src/snespadio.h                  -> sys/dev/spi/snespadio.h (SNESPAD_BTN_* button bits)
    src/snespad.4                    -> share/man/man4/snespad.4
    src/module/Makefile              -> sys/modules/snespad/Makefile
    src/module/snespad.ioconf        -> sys/modules/snespad/snespad.ioconf
                                     (pseudo-root spi*; snespad0/1 active at
                                      slave 0/1, snespad2/3 commented out)

## Applying the patches

Apply every patch from the top of the source tree with `patch -p1`
(the `a/`/`b/` prefixes in the diffs map onto the real paths shown above):

    cd /usr/src
    for f in <bundle>/patches/0[1-4]*.diff; do patch -p1 < "$f"; done

What each patch does, and where it lands:

    01-files.spi.diff        sys/dev/spi/files.spi: declares `device snespad: wskbddev`,
                             `attach snespad at spi` and the source file, so the
                             driver can live on any spi(4) bus
    02-modules-Makefile.diff sys/modules/Makefile: SUBDIR+= snespad, so the LKM is
                             built by the regular modules build
    03-spi-rescan.diff       sys/dev/spi/spi.c (OPTIONAL): adds ca_rescan to spi(4)
                             so the module's "at spi?" cfdata attaches at runtime
                             (modload / /etc/modules.conf / "drvctl -r -a spi <bus>").
                             Without it, build snespad into the kernel config instead
    04-manpage.diff          share/man/man4/Makefile + new share/man/man4/snespad.4:
                             the snespad(4) man page (required for new drivers)

Then copy the sources into place:

    cp <bundle>/src/snespad.c <bundle>/src/snespadio.h sys/dev/spi/
    cp <bundle>/src/snespad.4 share/man/man4/
    mkdir -p sys/modules/snespad
    cp <bundle>/src/module/Makefile <bundle>/src/module/snespad.ioconf sys/modules/snespad/

## Build / use

    # as a module:
    cd sys/modules/snespad
    env KERNOBJDIR=<obj>/sys/arch/<arch>/compile/<CONFIG> TOOLDIR=<tools> \
        MACHINE=<m> MACHINE_ARCH=<a> MACHINE_CPU=<c> <tools>/bin/nbmake
    # install to /stand/<arch>/<ver>/modules/snespad/snespad.kmod, then:
    modload snespad          # needs patch 03 (ca_rescan) on stock spi(4) (15.11.2026)

    # or built into the kernel (no patch 03 needed) — kernel config:
    #   snespad* at spi? slave 0        # default 2 pads; add slave 1..3
    #                                    # for up to 4 (SNESPAD_MAX_PORTS)
    #   wskbd*   at wskbddev? mux 1     # keyboard attach for the pads

Test (watch the console / dmesg while pressing buttons):

    /sbin/sysctl -w hw.snespad_debug=1
    ...press buttons on the pad...
    /sbin/dmesg | tail          # one line per press/release, e.g. "+A", "-A"
    # on a wscons console/X11 the buttons also type their mapped keys

## Notes

* Patch 03 is the only kernel-file change and is optional: without it,
  build the driver into the kernel config (boot-time autoconf attaches
  `snespad* at spi? slave N` fine).  With it, `modload snespad` attaches
  to every present spi(4) bus automatically via the existing
  `rescan_with_cfdata()` machinery, and `drvctl -r -a spi spi0` works.
  Only with this the driver will work as a module.
* The module uses `MODULE(MODULE_CLASS_DRIVER, snespad, NULL)`(no
  dependency on a "spi" module) because usually spi(4) is built into
  the kernel. If spi(4) is modular on some machine, load the spi module
  first (or add "spi" to the MODULE() dependency list).

## Verified on real hardware: Raspberry Pi 3 B (NetBSD 11.1, evbarm)

