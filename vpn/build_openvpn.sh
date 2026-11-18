#!/bin/bash
# Cross-build OpenSSL 1.0.2u (static) + OpenVPN 2.3.18 for ARM926EJ-S OABI / glibc 2.3.6
# Runs INSIDE the oabi-build:lenny container.
set -e

TCBIN=/opt/oabi-tc/bin
CROSS=$TCBIN/arm-softfloat-linux-gnu-
export PATH=$TCBIN:$PATH
WORK=/work
SSL=$WORK/openssl-1.0.2u
OVPN=$WORK/openvpn-2.3.18

echo "########## sanity: cross gcc ##########"
${CROSS}gcc --version | head -1
echo 'int main(){return 0;}' > /tmp/t.c
${CROSS}gcc -march=armv5te -static -o /tmp/t /tmp/t.c && echo "cross link OK"
${CROSS}readelf -h /tmp/t | grep -iE 'Flags|Machine'

echo "########## 1) OpenSSL 1.0.2u static ##########"
cd "$SSL"
if [ ! -f libcrypto.a ] || [ ! -f libssl.a ]; then
  make clean >/dev/null 2>&1 || true
  ./Configure linux-generic32 no-shared no-dso no-asm no-engine no-hw \
      -march=armv5te \
      --cross-compile-prefix=$CROSS
  make links
  make depend
  # build only the libraries (avoid building the openssl app/tests)
  make build_crypto build_ssl
fi
ls -l libssl.a libcrypto.a
echo "-- verify OpenSSL objects are OABI (swi) --"
${CROSS}objdump -d libcrypto.a | grep -c 'swi' || true

echo "########## 2) OpenVPN 2.3.18 static ##########"
cd "$OVPN"
make distclean >/dev/null 2>&1 || true
./configure \
  --host=arm-softfloat-linux-gnu \
  --build=i686-pc-linux-gnu \
  CC=${CROSS}gcc \
  AR=${CROSS}ar \
  RANLIB=${CROSS}ranlib \
  --disable-lzo --disable-lz4 --disable-plugins \
  --disable-plugin-auth-pam --disable-plugin-down-root \
  --disable-debug --disable-pkcs11 --disable-management \
  --disable-shared --enable-static \
  --enable-small \
  --enable-iproute2=no \
  OPENSSL_CFLAGS="-I$SSL/include" \
  OPENSSL_LIBS="-L$SSL -lssl -lcrypto" \
  CFLAGS="-march=armv5te -Os -I$SSL/include" \
  LDFLAGS="-static -L$SSL" \
  IFCONFIG=/sbin/ifconfig \
  ROUTE=/sbin/route

make
# Force a FULLY static binary: libtool eats plain -static, so relink with -all-static.
rm -f src/openvpn/openvpn
make -C src/openvpn LDFLAGS="-all-static -L$SSL"

echo "########## result ##########"
ls -l "$OVPN/src/openvpn/openvpn"
cp "$OVPN/src/openvpn/openvpn" "$WORK/openvpn"
${CROSS}strip "$WORK/openvpn" || true
echo "DONE"
