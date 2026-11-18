#!/bin/bash
set -e
export PATH=/opt/oabi-tc/bin:$PATH
cd /work/openssl-1.0.2u
./Configure linux-generic32 no-shared no-asm no-dso no-zlib \
  --prefix=/work/ssl-install \
  --cross-compile-prefix=arm-softfloat-linux-gnu- \
  -march=armv5te -Os >/work/ssl_conf.log 2>&1
make -j2 >/work/ssl_make.log 2>&1
make install_sw >/work/ssl_install.log 2>&1
echo "=== libs ==="; ls -la /work/ssl-install/lib/libssl.a /work/ssl-install/lib/libcrypto.a
echo "=== OABI check on libcrypto ==="
arm-softfloat-linux-gnu-objdump -d /work/ssl-install/lib/libcrypto.a 2>/dev/null | grep -cE "\tswi" 
