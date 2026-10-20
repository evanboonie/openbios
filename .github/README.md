# OpenBIOS for QEMU's `powermac7_3`

This is a fork of [OpenBIOS](https://github.com/openbios/openbios) for `powermac7_3`, the Power Mac G5 machine in
[evanboonie/qemu](https://github.com/evanboonie/qemu)'s `g5` branch. That machine models the AGP Power Mac G5,
`PowerMac7,3`, closely enough to boot Mac OS X 10.5 on the reader's own disk. QEMU's bundled OpenBIOS doesn't know
the machine, so `powermac7_3` needs this fork's build, given with `-bios`, and won't start without it.

- **`master`** mirrors upstream OpenBIOS, untouched.
- **`g5`,** the default branch, holds the work. Changes land on it by pull request. It's rebased onto upstream
  from time to time, so it doesn't always fast-forward, and each rebase first tags the old head `archive/g5-<date>`,
  so older commits stay fetchable.
- **Commits are signed off,** as upstream asks.
- **Nothing is sent upstream for now.** The code is written with Claude, an AI model, and QEMU, which ships
  OpenBIOS as its PowerPC firmware, declines contributions that include AI-generated code. The fork is the
  deliverable.

Upstream's own README is [`README`](https://github.com/evanboonie/openbios/blob/g5/README), and the licence is
upstream's, the GPL version 2 ([`COPYING`](https://github.com/evanboonie/openbios/blob/g5/COPYING)).

## What `g5` changes

QEMU's `powermac7_3` passes OpenBIOS a machine ID of its own through fw_cfg (4, `ARCH_POWERMAC7_3`). `mac99` and
the other machines keep upstream's device tree, and get only the shared fixes below.

**On `powermac7_3` only:**

- **Its identity:** the machine ID gets an entry of its own, set up as `mac99`'s U3 one is. The root node's model
  is `PowerMac7,3`, and its compatible is `PowerMac7,3`, `MacRISC4`, `Power Macintosh`.
- **U3:** the memory controller node is `u3` (compatible `u3`, model `AAPL,U3`, device-rev `0x35`), in place of
  `uni-n`.
- **The PCI I/O range** of `u3-agp` is at `0xf0000000`, 8 MB, as on the real machine, where `mac99` has it at
  `0xf2000000`.
- **The interrupt controller:** mac-io's is named `mpic`, and the cpu node's `AAPL,parentIC` points at it.
- **mac-io is named K2:** compatible `K2-Keylargo` and model `AAPL,K2-Keylargo`, as on the real machine, in place
  of KeyLargo's.
- **HID5's `dcbz` size:** at start-up, OpenBIOS sets the 970's HID5 to `0x80`, for every client, which makes a
  plain `dcbz` clear 32 bytes, as Mac OS X needs and as on the AGP G5 under Mac OS X.
- **The flash NVRAM.** With QEMU's `-drive if=pflash`, the NVRAM is an AMD-style flash, and OpenBIOS drives it:
  - the node's compatible is `amd-0137`, `nvram,flash`, and `#bytes` is one bank, `0x2000`
  - the flash holds a Core99 image: two 8 KB banks, each a header and then the CHRP partitions. A boot reads the
    valid bank with the higher generation, and formats the flash when it holds no usable image
  - a write goes to the other bank, with the next generation, through AMD erase and program commands
  - the numbers in the `common` partition are kept in Mac OS X's form (`-1`, decimal, or hex with `0x`), and
    converted to OpenBIOS's form as they're read
  - `-prom-env` comes from fw_cfg's `etc/prom-env`, set over the flash's variables at every boot
- **The free partition's name:** a CHRP free partition that OpenBIOS makes is named `wwwwwwwwwwww`, twelve `w`s with
  no NUL, as Mac OS X and Linux name it. The code is shared, but only an NVRAM that OpenBIOS formats itself has
  one, such as `powermac7_3`'s blank flash. QEMU formats `mac99`'s.

**Shared fixes,** which apply on every QEMU machine that runs the code:

- **The 970MP:** `ppc_defs` gets an entry for its PVR, `0x0044`, so `-cpu 970mp_v1.0` and `970mp_v1.1` no longer
  stop at boot with "Unknown cpu".
- **The timebase frequency** is read from fw_cfg before the PCI probe, so `udelay()` and the probe's timeouts, IDE's
  among them, are no longer 12.5 times short on the 25 MHz timebase of QEMU's `mac99` and `powermac7_3`.
- **IDE:** the master is selected before a soft reset, so the wait for BUSY reads the master's status, not that of
  an absent slave. A late reset no longer leaves the master unidentified.
- **via-cuda's `interrupt-parent`** is set only when the node exists, so a machine with a PMU in place of CUDA
  (`-M mac99,via=pmu`) no longer prints `set_property: NULL phandle`.

## Build

The build runs in upstream OpenBIOS's CI image, as its `.github/workflows/main.yml` does, named here by digest
since its `master` tag can move. It needs Docker, takes a few seconds on x86-64, and writes
`obj-ppc/openbios-qemu.elf`. The image is amd64 only, so on an arm64 host Docker runs it emulated, which is slower
and prints a platform warning.

```sh
git clone https://github.com/evanboonie/openbios.git
cd openbios
```

```sh
IMG=ghcr.io/openbios/openbios-builder@sha256:bf806644ccff091323dd56a853866ba09301b1a7fcb91e865fdb289499ef8b38
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v "$PWD":/src -w /src $IMG \
  sh -c './config/scripts/switch-arch ppc && make V=1'
ls -l obj-ppc/openbios-qemu.elf
```

`--user` keeps the output owned by you. The ELF's banner, `Welcome to OpenBIOS v1.1 built on …`, gives its build
time in UTC, which tells it apart from QEMU's bundled `openbios-ppc`.

## Run

Build `qemu-system-ppc64` from [evanboonie/qemu](https://github.com/evanboonie/qemu)'s `g5`, and give it this
build with `-bios`:

```sh
qemu-system-ppc64 -M powermac7_3 -bios /path/to/openbios/obj-ppc/openbios-qemu.elf ...
```

For the flash NVRAM, add `-drive if=pflash,format=raw,file=nvram.img`, with a 16 KB file. A new one should be
erased flash, all `0xFF` bytes, such as `head -c 16384 /dev/zero | LC_ALL=C tr '\0' '\377' > nvram.img`.
The machine's other options, its disks and how to boot an OS on it are in the QEMU fork's README and its
[`docs/system/ppc/powermac.rst`](https://github.com/evanboonie/qemu/blob/g5/docs/system/ppc/powermac.rst).

The same build also runs QEMU's `mac99`, and QEMU's `tests/functional/ppc64/test_mac99.py` passes with it.

## Tested at

The tag `qemu-g5-q7` on this fork and on the QEMU fork marks the pair of commits tested together. If this fork
doesn't have it yet, this branch's head is what's described here.
