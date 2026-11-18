#!/bin/bash
# build-image.sh - assemble a modified BMC firmware image from YOUR OWN firmimg.d6.
#
# This ships NO vendor firmware. You provide:
#   --firmware  your own firmimg.d6 (extract it from the vendor .exe; see README)
#   --bindir    a dir with the native binaries from the GitHub Release:
#                 relay   (native HTML5 console)   tun.ko   openvpn   (optional)
#   --pubkey    your SSH public key (for the root-shell sshd)
#
# It extracts the rootfs, drops in the overlay + binaries + an init hook, rebuilds
# the CramFS, and repacks the .d6 with correct (little-endian) CRCs via repack.py.
# You then flash the result with your vendor's tool (e.g. racadm fwupdate). Have
# serial recovery ready. USE AT YOUR OWN RISK.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FW="" BINDIR="" PUBKEY="" OUT="firmimg.modified.d6"
ROOTFS_OFF=0x445E00 ROOTFS_SIZE=0x031CA000   # adjust per your image (binwalk + header@0x30)
while [ $# -gt 0 ]; do case "$1" in
  --firmware) FW="$2"; shift 2;; --bindir) BINDIR="$2"; shift 2;;
  --pubkey) PUBKEY="$2"; shift 2;; --out) OUT="$2"; shift 2;;
  --rootfs-off) ROOTFS_OFF="$2"; shift 2;; --rootfs-size) ROOTFS_SIZE="$2"; shift 2;;
  *) echo "unknown arg $1"; exit 1;; esac; done
[ -f "$FW" ] || { echo "need --firmware <your firmimg.d6>"; exit 1; }
[ -d "$BINDIR" ] || { echo "need --bindir <dir with relay/tun.ko/openvpn>"; exit 1; }
command -v mkfs.cramfs >/dev/null || { echo "need mkfs.cramfs (util-linux / cramfs-tools)"; exit 1; }

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
echo "[*] carving rootfs from $FW"
python3 - "$FW" "$ROOTFS_OFF" "$ROOTFS_SIZE" "$WORK/rootfs.cramfs" <<'PY'
import sys
fw,off,size,out=sys.argv[1],int(sys.argv[2],0),int(sys.argv[3],0),sys.argv[4]
d=open(fw,'rb').read(); open(out,'wb').write(d[off:off+size])
PY
echo "[*] extracting rootfs (needs root for cramfs mount, or uses cramfsck)"
mkdir -p "$WORK/newroot"
if command -v cramfsck >/dev/null; then cramfsck -x "$WORK/newroot" "$WORK/rootfs.cramfs";
else sudo mount -o loop,ro "$WORK/rootfs.cramfs" "$WORK/mnt" 2>/dev/null && cp -a "$WORK/mnt"/. "$WORK/newroot"/ && sudo umount "$WORK/mnt"; fi

echo "[*] applying overlay + native binaries + init hook"
cp -a "$HERE/overlay/." "$WORK/newroot/"
install -D -m755 "$BINDIR/relay"   "$WORK/newroot/usr/local/sbin/relay"
install -D -m644 "$BINDIR/tun.ko"  "$WORK/newroot/lib/modules/tun.ko"
[ -f "$BINDIR/openvpn" ] && install -D -m755 "$BINDIR/openvpn" "$WORK/newroot/usr/local/sbin/openvpn" || true
[ -n "$PUBKEY" ] && cp "$PUBKEY" "$WORK/newroot/etc/oob/authorized_keys_root"
# extend the init hook to load tun + start the console relay
cat >> "$WORK/newroot/etc/sysapps_script/S_7021_oobmgr.sh" <<'HOOK'

# --- native services added by build-image.sh ---
oob_native() {
  [ -e /dev/net/tun ] || { mkdir -p /dev/net; insmod /lib/modules/tun.ko 2>/dev/null; mknod /dev/net/tun c 10 200 2>/dev/null; }
  [ -x /usr/local/sbin/relay ] && (IDRAC_USER=root /usr/local/sbin/relay </dev/null >/var/log/relay.log 2>&1 &)
  # start openvpn only if a config is present on /flash:
  [ -x /usr/local/sbin/openvpn ] && [ -f /flash/data0/etc/oob/openvpn.conf ] && \
    (/usr/local/sbin/openvpn --config /flash/data0/etc/oob/openvpn.conf </dev/null >/var/log/openvpn.log 2>&1 &)
}
oob_native
HOOK
# ensure the hook is actually invoked (iDRAC6: explicit list in postinit_adv.sh)
PI="$WORK/newroot/etc/init.d/postinit_adv.sh"
[ -f "$PI" ] && ! grep -q S_7021_oobmgr "$PI" && \
  sed -i '/S_7020_sshd.sh start/a /etc/sysapps_script/S_7021_oobmgr.sh start' "$PI"

echo "[*] building CramFS (root-owned; use fakeroot or sudo for correct ownership)"
mkfs.cramfs -b 4096 "$WORK/newroot" "$WORK/new.cramfs"

echo "[*] repacking (LE CRC fix)"
python3 "$HERE/tools/repack.py" --orig "$FW" --cramfs "$WORK/new.cramfs" --out "$OUT" \
  --rootfs-off "$ROOTFS_OFF" --rootfs-size "$ROOTFS_SIZE"
echo "[*] done: $OUT"
echo "    Flash with your vendor tool. Serial recovery ready. USE AT YOUR OWN RISK."
