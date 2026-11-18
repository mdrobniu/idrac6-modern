# Developing for the BMC (reproducible pipeline)

Everything here runs **natively on the BMC** (ARM926EJ-S, Linux 2.6.23.1, OABI,
glibc 2.3.6). Modern toolchains can't target it (see `OABI_TOOLCHAIN.md`), so the
whole flow is built with a period toolchain inside an old-host container.

## 0. The fast iteration trick (no reflashing)

Once you have a root shell on the box (the `overlay/` adds a 2nd sshd on :2222),
you do **not** reflash to test native code:

```bash
scp -P 2222 ./myprog root@<IDRAC_IP>:/tmp/    # copy
ssh -p 2222 root@<IDRAC_IP> '/tmp/myprog'      # run on the real ARM926
```

Reflash only to bake the final artifacts into the rootfs. A bad native binary or
`insmod` at worst reboots the BMC (nothing is persisted), so iterate freely.

## 1. OABI cross-toolchain (gcc-3.4.5 / glibc-2.3.6)

glibc-2.3.6 + gcc-3.4.5 won't build on a modern host, so build them inside a
Debian "lenny" container (host gcc 4.3) with Dan Kegel's crosstool:

```bash
# scripts in toolchain/ ; see toolchain/run_pathb.sh (crosstool + the
# gcc-3.4.5-glibc-2.3.6.dat + arm-softfloat.dat OABI recipe)
podman build -t oabi-build:lenny -f toolchain/Dockerfile .   # Debian lenny + build deps
podman run --rm -v $PWD/oabi-tc:/opt/oabi-tc oabi-build:lenny \
    /bin/bash /opt/oabi-tc/kegel/run_pathb.sh
# result: /opt/oabi-tc/bin/arm-softfloat-linux-gnu-gcc
```

Invocation (gcc 3.4.5 is period-correct — some modern flags don't exist):

```bash
TC=/opt/oabi-tc/bin
$TC/arm-softfloat-linux-gnu-gcc -static -march=armv5te myprog.c -o myprog
```

- soft-float is the **default** — do NOT pass `-mfloat-abi=soft` (use `-msoft-float`
  if ever needed); `-march=armv5te` is good; there is no `-mabi=` switch (OABI is the
  only ABI for this `-linux-gnu` target).
- **Verify** any binary: `objdump -d` must show `swi 0x9000xx` (OABI) and **zero**
  `svc 0` (EABI). `readelf -h` e_flags must be `0x202` ("GNU EABI software FP"),
  **not** "Version5 EABI". Then run it on the box.

## 2. Kernel modules (e.g. `tun.ko`)

The stock kernels ship no TUN. Build it from matching source — `kernel/build_tun.sh`:

- `linux-2.6.23` source, set `EXTRAVERSION=.1` (vermagic needs `2.6.23.1`).
- `make versatile_defconfig` (ARM926 = ARMv5), then force `CONFIG_PREEMPT=y`,
  `CONFIG_MODULE_UNLOAD=y`, `# CONFIG_MODVERSIONS is not set`, `CONFIG_TUN=m`.
- Build **inside the lenny container** (the kernel's host-tools won't compile under
  modern gcc) with `CROSS_COMPILE` pointing at the OABI toolchain.
- vermagic must read `2.6.23.1 preempt mod_unload ARMv5` (verified working). No
  `CONFIG_MODVERSIONS` means `insmod` only checks vermagic, not per-symbol CRCs.
- On the box: `insmod tun.ko`, `mkdir -p /dev/net`, `mknod /dev/net/tun c 10 200`.

## 3. Native services (mongoose, OpenSSL, OpenVPN)

- **OpenSSL 1.0.2** static for OABI: `toolchain/build_openssl.sh`
  (`./Configure linux-generic32 no-shared no-asm --cross-compile-prefix=... -march=armv5te`).
- **mongoose** (single-file HTTP/WS/TLS server) compiles with gcc 3.4.5 given two
  fixes for the old kernel/glibc:
  - `-DMG_ENABLE_EPOLL=0` (the box kernel 2.6.23 has no `epoll_create1`; use `poll()`).
  - link `-lrt` (`clock_gettime` lived in librt before glibc 2.17).
  - `-std=gnu99`. Use `-DMG_TLS=MG_TLS_OPENSSL` and link the OpenSSL static libs.
  A minimal mongoose HTTP server built this way serves requests natively on the box.
- **OpenVPN**: 2.3.x builds cleanly against OpenSSL 1.0.2 with this toolchain; needs
  `/dev/net/tun` (see step 2). Disable lzo/lz4/plugins/pkcs11 to minimize.

## 4. Repack + flash

See `FIRMWARE_FORMAT.md` and `tools/repack.py`. Assemble your overlay into the
extracted rootfs, `mkfs.cramfs -b 4096`, then `repack.py` (fixes the LE header CRCs).
Keep serial recovery ready before flashing.
