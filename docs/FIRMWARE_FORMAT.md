# The `.d6` firmware container

The vendor firmware image (`firmimg.d6`) is a simple container. **Integrity is
CRC32 only — there is no cryptographic signature**, so a modified image can be
rebuilt and flashed as long as the CRC fields are recomputed correctly.

## Header (first 0x200 bytes) — ALL LITTLE-ENDIAN

The biggest footgun: the header fields are **little-endian**. (Writing them
big-endian produces an image that passes the header check but fails the
"programming flash" CRC verify partway through.)

| Offset | Field | Notes |
|---|---|---|
| `0x00` | header CRC32 | `crc32(header[0x04:0x200])`, little-endian. Recompute **last**. |
| `0x20` | kernel section offset | `0x200` |
| `0x24` | kernel section size | |
| `0x28` | kernel section CRC32 | |
| `0x2C` | rootfs section offset | `0x445E00` on the reference target |
| `0x30` | rootfs section size | e.g. `0x031CA000`; **keep this = original section size** |
| `0x34` | rootfs section CRC32 | `crc32(image[off:off+size])`, little-endian |
| `0x38` | u-boot section offset | |
| `0x3C` | u-boot section size | |
| `0x40` | u-boot section CRC32 | |

The "magic" some tools report at offset 0 is actually this header CRC32.

## Sections

- **kernel**: a U-Boot legacy uImage (its own magic `0x27051956`, header-CRC and
  data-CRC inside). Leave it untouched unless you rebuild the kernel.
- **rootfs**: a **CramFS** (little-endian, v2, sorted_dirs). On the stock image the
  cramfs fills the section exactly. If your rebuilt cramfs is smaller, **zero-pad it
  up to the original section size** and keep `size@0x30` unchanged; the on-box
  updater copies and CRCs exactly `size@0x30` bytes.
- **u-boot**: the bootloader. Leave it untouched (its version is checked; don't
  downgrade). Keep its offset — i.e. splice the (padded) rootfs so u-boot stays put.

## The on-box updater (what it checks)

Reverse-engineered from the updater binary:
1. `readHeader`: verifies the header CRC32 at `0x00` over `header[0x04:0x200]`.
2. copies `size@0x30` bytes from the image into the rootfs flash partition,
   reads them back and `memcmp`s, then `crc32`s them and compares to `crc@0x34`.
3. same for the kernel/u-boot sections.
All checks are **CRC32**. No RSA, no SHA, no key — confirmed by disassembly.

See [`../tools/repack.py`](../tools/repack.py) for a reference implementation.
