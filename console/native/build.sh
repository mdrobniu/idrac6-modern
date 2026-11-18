#!/bin/bash
# Cross-compile the iDRAC6 KVM relay for the BMC (ARM926EJ-S, OABI, glibc 2.3.6).
# Run INSIDE the container:
#   podman run --rm -v /opt/oabi-tc:/opt/oabi-tc \
#       -v /opt/relay-build:/work oabi-build:lenny /bin/bash /work/build.sh
set -e
export PATH=/opt/oabi-tc/bin:$PATH
cd /work

GCC=arm-softfloat-linux-gnu-gcc
OBJDUMP=arm-softfloat-linux-gnu-objdump

echo "=== compiling relay (mongoose 7.14 built-in TLS + OpenSSL 1.0.2 for 5900) ==="
$GCC -std=gnu99 -march=armv5te -Os -static \
  -DMG_ENABLE_EPOLL=0 -DMG_TLS=MG_TLS_BUILTIN \
  -I ssl-install/include \
  -o relay relay.c mongoose.c \
  ssl-install/lib/libssl.a ssl-install/lib/libcrypto.a \
  -lpthread -lrt -ldl 2>&1 | grep -iE "error|undefined|warning: implicit" | head -30 || true

echo "=== result ==="
if [ ! -f relay ]; then echo "BUILD FAILED"; exit 1; fi
ls -la relay
file relay
echo "=== OABI verification (swi must be >0, svc must be 0) ==="
# NOTE: use a literal TAB (printf) - this grep does not expand \t in the pattern.
TAB=$(printf '\t')
DIS=$($OBJDUMP -d relay 2>/dev/null)
# grep -c returns exit 1 on a zero count, so guard against set -e.
SWI=$(echo "$DIS" | grep -c "swi" || true)                 # CLAUDE.md check: must be >0
SVC=$(echo "$DIS" | grep -c "${TAB}svc" || true)           # EABI svc: must be 0
OABI=$(echo "$DIS" | grep -cE 'ef90[0-9a-f]{4} .swi.0x0090' || true)  # real OABI opcodes
echo "swi=$SWI  svc=$SVC  real-oabi-syscalls=$OABI"
if [ "$SWI" -le 0 ] || [ "$SVC" -ne 0 ] || [ "$OABI" -le 0 ]; then
  echo "OABI CHECK FAILED (need swi>0, svc==0, real-oabi-syscalls>0)"; exit 1
fi
echo "OABI OK"
