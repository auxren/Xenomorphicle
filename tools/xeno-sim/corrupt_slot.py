#!/usr/bin/env python3
"""Damage one preset container inside a --state file, for selfcheck.sh.

    corrupt_slot.py IN OUT SLOT SECTION [--fix-sum] [--sd]

Flips one bit inside the named section's payload (a few bytes past the
PhzConfig chunk header, so it lands in a record, not a signature). With
--fix-sum the container's own section checksum is recomputed so the damage
gets past the engine's container validation and reaches PhzConfig's reader:
that is the layer that used to accept a G section whose config half failed
its checksum as long as the data half passed.

--sd damages the copy on the card volume (an exported container) instead of
the one on internal flash, for the import path.

Container layout (PresetEngine.cpp): 16-byte header, then 6 x 12-byte section
entries {kind, pad, sum16, offset, length}, little-endian.
"""
import struct
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    fix_sum = '--fix-sum' in sys.argv
    vol = 'sd' if '--sd' in sys.argv else 'lfs'
    if len(args) != 4:
        sys.exit(__doc__)
    src, dst, slot, kind = args
    name = 'file %s PB_%02d.PBS ' % (vol, int(slot))
    out = []
    hit = False
    for line in open(src).read().split('\n'):
        if line.startswith(name):
            pre, hexs = line.rsplit(' ', 1)
            img = bytearray.fromhex(hexs)
            for i in range(6):
                e = 16 + 12 * i
                k, _pad, s, off, ln = struct.unpack_from('<BBHII', img, e)
                if k == ord(kind):
                    img[off + 12 + 3] ^= 0x40
                    if fix_sum:
                        s = sum(img[off:off + ln]) & 0xFFFF
                        struct.pack_into('<BBHII', img, e, k, _pad, s, off, ln)
                    hit = True
                    break
            line = pre + ' ' + img.hex().upper()
        out.append(line)
    if not hit:
        sys.exit('corrupt_slot.py: no section %s in slot %s on %s' % (kind, slot, vol))
    open(dst, 'w').write('\n'.join(out))


if __name__ == '__main__':
    main()
