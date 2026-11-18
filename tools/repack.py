#!/usr/bin/env python3
"""
repack.py - rebuild an OABI/ARMv5 BMC `.d6` firmware image with a modified rootfs.

The `.d6` container is CRC32-only (no signature). This tool splices a new CramFS
into the rootfs section, pads it to the original section size so the following
section (u-boot) stays put, and recomputes the (LITTLE-ENDIAN) header CRCs that
the on-box updater checks. See ../docs/FIRMWARE_FORMAT.md.

You supply your own firmware image; none is distributed here.

Usage:
    # build a cramfs from a rootfs directory you assembled, then repack:
    mkfs.cramfs -b 4096 newroot new.cramfs
    python3 repack.py --orig firmimg.d6 --cramfs new.cramfs --out firmimg.new.d6 \
        --rootfs-off 0x445E00 --rootfs-size 0x031CA000

Find --rootfs-off / --rootfs-size for your image with `binwalk firmimg.d6`
(the CramFS offset) and by reading the header size field at 0x30 (little-endian).
"""
import argparse
import zlib
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--orig", required=True, help="original firmimg.d6 (your own copy)")
    ap.add_argument("--cramfs", required=True, help="new rootfs cramfs to splice in")
    ap.add_argument("--out", required=True)
    ap.add_argument("--rootfs-off", required=True, type=lambda x: int(x, 0))
    ap.add_argument("--rootfs-size", required=True, type=lambda x: int(x, 0),
                    help="original rootfs SECTION size (header field @0x30); keep unchanged")
    o = ap.parse_args()

    d = bytearray(open(o.orig, "rb").read())
    cr = open(o.cramfs, "rb").read()
    off, size = o.rootfs_off, o.rootfs_size
    if len(cr) > size:
        raise SystemExit("cramfs (%d) larger than the rootfs section (%d); trim it"
                         % (len(cr), size))

    # zero-fill the section, place the cramfs at the start (rest stays zero padding)
    region = bytearray(size)
    region[:len(cr)] = cr
    d[off:off + size] = region

    # header fields are LITTLE-ENDIAN. keep size@0x30; fix rootfs CRC@0x34; header CRC@0x00 last.
    struct.pack_into("<I", d, 0x30, size)
    struct.pack_into("<I", d, 0x34, zlib.crc32(bytes(d[off:off + size])) & 0xFFFFFFFF)
    struct.pack_into("<I", d, 0x00, zlib.crc32(bytes(d[4:0x200])) & 0xFFFFFFFF)

    open(o.out, "wb").write(d)
    print("rootfs CRC32 @0x34 = 0x%08x" % struct.unpack_from("<I", d, 0x34)[0])
    print("header CRC32 @0x00 = 0x%08x" % struct.unpack_from("<I", d, 0x00)[0])
    print("wrote %s (%d bytes)" % (o.out, len(d)))
    print("\nFlash with the vendor path (e.g. racadm fwupdate over TFTP). "
          "Have serial recovery ready. A failed image aborts without writing.")


if __name__ == "__main__":
    main()
