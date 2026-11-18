# Building native code for an OABI ARMv5 BMC

These BMCs run **Linux 2.6.23 compiled for the old ARM ABI (OABI)** on an
**ARM926EJ-S (ARMv5TEJ, no VFP)**. The kernel accepts **only OABI syscalls**.

## Proving the ABI on your target

OABI encodes the syscall number in the `swi` immediate (`swi 0x900000 + nr`);
EABI uses `svc 0` with the number in `r7`. Test with two tiny static binaries:

```asm
@ OABI exit(42) — WORKS on these kernels
.global _start
_start:
    mov r0, #42
    swi 0x900001        @ 0x900000 + __NR_exit(1)
```

An EABI `svc 0` exit instead returns **SIGILL** (exit 132). Stock binaries on the
box confirm OABI: `objdump -d` shows `svc 0x00900025`-style syscalls and the
interpreter is `/lib/ld-linux.so.2`.

## Why modern toolchains fail

- **Go** (any `GOARM`): the runtime executes VFP/`ldrexd` → `SIGILL` (no VFP, no
  ARMv6 exclusive load/store on ARM926). Even a GOARM=5 hello-world dies.
- **musl / glibc EABI, Rust**: emit EABI `svc 0` syscalls → `SIGILL` on the OABI
  kernel. (OABI vs EABI is a different *C ABI* too, not just syscalls, so you
  cannot link EABI objects against the box's OABI libc.)
- `qemu-user` does **not** catch this — its emulated CPUs have VFP and are lenient
  about ARMv6 instructions. Always test on the real hardware.

## The toolchain you need

An **OABI** ARM-Linux cross-toolchain. The ideal match for a 2.6.23 / glibc-2.3.6
box built with gcc 3.4.5 is Dan Kegel's original **crosstool** (not crosstool-NG,
which dropped OABI) with the `gcc-3.4.5 + glibc-2.3.6` recipe and the
`demo-arm-soft-oabi.sh` target:

- crosstool recipe mirror: <https://github.com/esoule/crosstool> (`gcc-3.4.5-glibc-2.3.6.dat`)
- crosstool (archived): <https://web.archive.org/web/2021/http://kegel.com/crosstool>

Matching the kernel's gcc (3.4.5) is a bonus: the same toolchain builds OABI
userspace **and** a `tun.ko` for the 2.6.23 kernel.

### Modern-host gotcha

glibc 2.3.6 and gcc 3.4.5 will **not** build on a current host (gcc >= 5). Build
inside an old-host container (Debian/Ubuntu ~2010, host gcc 4.1–4.4) via
docker/podman, debootstrap, or a chroot. crosstool applies the needed old-glibc
patches automatically.

### Verifying the result

Compile `int main(){printf("hi\n");return 0;}` static and check the libc syscall
stubs use **`swi 0x9000xx`** (OABI), and `readelf -h` does **not** say
"Version5 EABI". Then run it on the target — it should print and exit 0.
