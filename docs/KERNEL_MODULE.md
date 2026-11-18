# Building a `tun.ko` (and other modules) for the stock 2.6.23 kernel

These kernels ship without TUN, so OpenVPN/WireGuard have no tunnel device. You
can build and `insmod` a matching `tun.ko` because:

- **vermagic is simple**: `2.6.23.1 preempt mod_unload ARMv5` (no SMP; 2.6.23
  does not bake the gcc version into vermagic).
- **No module CRC enforcement**: stock modules have no `__versions` section, i.e.
  `CONFIG_MODVERSIONS` is off. So a module only has to **match the vermagic string**
  — not per-symbol CRCs — to load.

## Steps

1. Build or obtain the matching OABI toolchain (see `OABI_TOOLCHAIN.md`); gcc 3.4.5
   matches the kernel's own compiler.
2. `wget https://mirrors.edge.kernel.org/pub/linux/kernel/v2.6/linux-2.6.23.1.tar.bz2`
3. Configure to match the running kernel's core options so vermagic matches:
   `ARCH=arm`, the right machine (ARM926), `CONFIG_PREEMPT=y`, `CONFIG_MODULE_UNLOAD=y`,
   `CONFIG_MODVERSIONS=n`, and `CONFIG_TUN=m`.
4. `make ARCH=arm CROSS_COMPILE=<oabi-prefix>- modules_prepare` then build just the
   module: `make ARCH=arm CROSS_COMPILE=<oabi-prefix>- M=drivers/net tun.ko`
   (or `SUBDIRS=drivers/net`).
5. Check vermagic: `modinfo tun.ko` (or `strings tun.ko | grep vermagic`) must read
   `vermagic=2.6.23.1 preempt mod_unload ARMv5`.
6. On the box: `mkdir -p /dev/net`, `insmod tun.ko`, then
   `mknod /dev/net/tun c 10 200` (misc major 10, TUN minor 200).

With `/dev/net/tun` present, a native OABI **OpenVPN** (linked against the box's
OpenSSL) can create a tunnel. Note 2.6.23 won't compile cleanly with gcc 4.x+
without a few small patches; gcc 3.4.5 is the safe choice.
