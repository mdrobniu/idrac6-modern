# Native VPN on the BMC (OpenVPN over a built `tun.ko`)

A full native VPN works on these boxes once TUN exists (see `KERNEL_MODULE.md`):
**OpenVPN 2.3.18 + OpenSSL 1.0.2u**, cross-compiled OABI static, verified running on
the real ARM926 (`openvpn --version` exits 0; `openvpn --mktun` opens a `tun0`).

## Build

`vpn/build_openvpn.sh` (run in the lenny container):
- OpenSSL **1.0.2u** static for OABI (`./Configure linux-generic32 no-shared no-asm
  --cross-compile-prefix=<oabi>- -march=armv5te`).
- OpenVPN **2.3.18** (the version that builds cleanly with gcc 3.4.5 / glibc 2.3.6 /
  OpenSSL 1.0.2): `./configure --host=arm-softfloat-linux-gnu CC=<oabi>-gcc
  --disable-lzo --disable-lz4 --disable-plugins --disable-pkcs11 --enable-small ...`
  Gotchas: put the OpenSSL `-L` in LDFLAGS (not just OPENSSL_LIBS) for the AC_CHECK_LIB
  tests; add `-I<ssl>/include` to CFLAGS; and relink with `-all-static` at the end
  (libtool silently drops a plain `-static`).
- Verify OABI: `objdump -d openvpn | grep -c swi` > 0, `grep -c '\tsvc'` == 0.

## Minimal routed TUN setup (pre-shared static key)

Simplest for a BMC (no PKI/entropy needs). Generate once:
`openvpn --genkey --secret static.key`, copy to both ends. See
`vpn/sample-configs/{server,client}.conf`. The BMC runs the client and **dials out**
(no inbound port needed), becoming reachable at its tunnel IP; add a `route` on the
server to reach a LAN behind the BMC. Use `cipher AES-128-CBC`, `auth SHA1`.

Note: a static-glibc OpenVPN prints a benign note about NSS libs if you use
hostnames — the sample configs use IPs, so it's moot.
