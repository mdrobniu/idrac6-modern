# idrac6-modern — reviving OABI / ARMv5 BMCs (Dell iDRAC6 and friends)

Open tooling to **modernize very old Baseboard Management Controllers** built on
ARMv5 (ARM926EJ-S) running a 2.6.23-era **OABI** Linux — the Dell iDRAC6 being the
reference target. It replaces the dead Java/ActiveX KVM with a **browser-native
HTML5 console**, gives the stock web UI a **modern TLS certificate and ciphers**,
adds a **real root shell**, and (work in progress) a **native VPN**.

Everything is designed to run **natively on the BMC** — these chips are a dead end
for modern toolchains, and that problem (and its solution) is the heart of this repo.

> This repository ships **only tools, patches, and documentation**. It does **not**
> contain any vendor firmware image. You supply your own `firmimg.d6` (downloaded
> from the vendor) and the tools modify your copy locally.

## Why this is hard (and interesting)

The BMC is an **ARM926EJ-S (ARMv5TEJ, no VFP)** running **Linux 2.6.23.1** compiled
for the **old ARM ABI (OABI)**. That combination breaks every modern toolchain:

| Toolchain | Result on the BMC | Why |
|---|---|---|
| Go (any GOARM) | `SIGILL` | runtime emits VFP/`ldrexd`; even GOARM=5 hello-world dies |
| musl / glibc **EABI** C | `SIGILL` | EABI syscalls (`svc 0` + r7); kernel is **OABI only** |
| Rust | `SIGILL` | EABI only |
| **OABI** C (`swi 0x9000xx`) | **works** | matches the kernel's syscall ABI |

The kernel accepts **only OABI syscalls** (`swi 0x900000+nr`), proven on-target:
an EABI `svc 0` exit → `SIGILL`; an OABI `mov r0,#42; swi 0x900001` → exit 42.
So native code must be built with an **OABI toolchain** — see
[`docs/OABI_TOOLCHAIN.md`](docs/OABI_TOOLCHAIN.md).

## What works today

- **HTML5 KVM console** — the Avocent/DVC video protocol fully reverse-engineered
  ([`docs/PROTOCOL.md`](docs/PROTOCOL.md)); the video is **decoded in the browser**
  (see [`console/static/app.js`](console/static/app.js)). A reference relay is
  provided; the native (on-BMC, C) relay is in progress.
- **Modern TLS on the stock UI** — regenerate a valid cert on-box and force RSA-kx
  AES-GCM ciphers (the factory cert expired in 2024 and only offered DHE-1024).
- **Root shell** — a second `sshd` with `UsePAM no` bypasses the restricted CLI and
  lands you in `/bin/sh` (BusyBox), with key **and** password auth.
- **Firmware repack** — the `.d6` container is CRC32-only (no signature); rebuild the
  rootfs and fix the (little-endian!) header CRCs. See
  [`docs/FIRMWARE_FORMAT.md`](docs/FIRMWARE_FORMAT.md) and [`tools/repack.py`](tools/repack.py).

## In progress

- Native **OABI C** console relay (mongoose + built-in TLS).
- **`tun.ko`** built from the matching 2.6.23.1 kernel source (vermagic
  `2.6.23.1 preempt mod_unload ARMv5`, no module CRC enforcement) → enables…
- **OpenVPN** (native C, linked against the BMC's OpenSSL 1.0.2).
- Reverse-proxy so the console + stock UI share one port (443).

## Safety / recovery

The `.d6` has dual firmware banks and a U-Boot serial `RECOVER>` TFTP menu. A bad
flash is recoverable over the BMC's serial UART. **Wire up serial before flashing.**
Failed flashes abort without writing (the BMC stays on the previous firmware).

## Layout

- `docs/` — firmware format, console protocol, OABI toolchain, kernel notes
- `tools/` — `repack.py` (rebuild + CRC-fix a `.d6`)
- `overlay/` — init hook + sshd config templates added to the rootfs
- `console/` — the HTML5 console (browser DVC decoder + reference relay)

## License

Our code: MIT (see `LICENSE`). Third-party components keep their own licenses
(e.g. mongoose). Vendor firmware is **not** included and remains the vendor's.

## Status

Active reverse-engineering project. Contributions welcome, especially test reports
from other OABI ARMv5 BMCs (AMI MegaRAC, Avocent-based, etc.).
