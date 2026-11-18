# Toolchain sources (GPL/LGPL corresponding source)

This project's OABI cross-toolchain is built entirely from **unmodified upstream
sources** with Dan Kegel's `crosstool`. We do not fork or patch the compilers; the
repo carries the **exact version pins + build recipe** so anyone can rebuild the
identical toolchain from source. The prebuilt toolchain published in GitHub Releases
is accompanied by these pointers to satisfy GPLv3/GPLv2/LGPL "corresponding source".

## Exact component versions

| Component | Version | Upstream source |
|---|---|---|
| binutils | 2.15 | https://ftp.gnu.org/gnu/binutils/binutils-2.15.tar.bz2 |
| gcc | 3.4.5 | https://ftp.gnu.org/gnu/gcc/gcc-3.4.5/gcc-3.4.5.tar.bz2 |
| glibc | 2.3.6 | https://ftp.gnu.org/gnu/glibc/glibc-2.3.6.tar.bz2 (+ glibc-linuxthreads-2.3.6) |
| linux headers | 2.6.12.0 | linux-libc-headers-2.6.12.0 (archive.org / kernel.org) |
| crosstool | esoule mirror | https://github.com/esoule/crosstool |

Target tuple: `arm-softfloat-linux-gnu` (OABI / APCS-GNU, armv5te, soft-float, LE).

## Rebuild from source

See `toolchain/Dockerfile` (Debian lenny build host) and the scripts in this dir
(`build_in_container.sh`, `run_pathb.sh`) plus the crosstool `.dat` recipe files.
In short: build the lenny container, drop the five tarballs above into the crosstool
downloads dir, and run crosstool with the `gcc-3.4.5-glibc-2.3.6.dat` + `arm-softfloat.dat`
targets. The result is bit-for-bit reproducible given the same upstream tarballs.

## Licenses

- gcc: GPLv2/GPLv3 (+ GCC Runtime Library Exception); binutils: GPLv3; glibc: LGPLv2.1.
- The toolchain itself is upstream GNU software; this repo adds only the recipe (MIT).
- Kernel (`tun.ko`) builds from Linux 2.6.23 source (GPLv2) -- see `docs/KERNEL_MODULE.md`.
