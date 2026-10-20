#!/usr/bin/env python3
"""Build the BIOS installation payload; never opens host block devices.

GRUB's boot.img loads core.img at LBA 1; core's blocklist loads its
remaining sectors. Constants follow GRUB i386/pc/boot.h and diskboot.S.
The FAT partition stays at LBA 2048, leaving a bounded embedding area.
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile
import zlib


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--disk-dir', default='disk')
    ap.add_argument('--grub-dir', default='/usr/lib/grub/i386-pc')
    ap.add_argument('--grub-mkimage', default='grub-mkimage')
    ap.add_argument('--output', default='build/install.img')
    a = ap.parse_args()
    output = pathlib.Path(a.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tree = tmp / 'tree'
        shutil.copytree(a.disk_dir, tree)
        (tree / 'boot').mkdir(exist_ok=True)
        shutil.copyfile(a.kernel, tree / 'boot/kernel.bin')
        config = tmp / 'early.cfg'
        # root is the BIOS boot disk, not another IronOS disk attached to the PC.
        config.write_text('multiboot /boot/kernel.bin\nboot\n')
        corefile = tmp / 'core.img'
        subprocess.run([a.grub_mkimage, '-O', 'i386-pc', '-d', a.grub_dir,
                        '-p', '(,msdos1)/boot/grub', '-c', str(config),
                        '-o', str(corefile), 'biosdisk', 'part_msdos', 'fat',
                        'multiboot', 'boot'], check=True)
        subprocess.run(['python3', str(pathlib.Path(__file__).resolve().parents[1] / 'mkdisk.py'),
                        str(tmp / 'disk.img'), str(tree), '--mbr'], check=True)
        image = bytearray((tmp / 'disk.img').read_bytes())
        core = bytearray(corefile.read_bytes())
        sectors = (len(core) + 511) // 512
        if len(core) < 512 or sectors >= 2047:
            raise ValueError('GRUB core does not fit the MBR embedding area')
        # One contiguous blocklist, terminated by the preceding zero entry.
        if any(core[488:500]):
            raise ValueError('Unsupported GRUB diskboot blocklist layout')
        struct.pack_into('<QHH', core, 500, 2, sectors - 1, 0x820)
        boot = bytearray((pathlib.Path(a.grub_dir) / 'boot.img').read_bytes())
        if len(boot) != 512 or boot[510:] != b'\x55\xaa':
            raise ValueError('Invalid GRUB boot.img')
        struct.pack_into('<Q', boot, 0x5c, 1)
        boot[0x64] = 0xff  # use BIOS DL, not a hard-coded disk number
        image[:440] = boot[:440]  # preserve the FAT builder's partition table
        image[512:512 + len(core)] = core
        # A small envelope is loaded by GRUB as a module, not written to disk.
        header = struct.pack('<8sII', b'IRONINST', len(image), zlib.crc32(image))
        output.write_bytes(header + image)
        print(f'{output}: BIOS installer, {len(image) // 512} sectors, CRC32 verified at runtime')


if __name__ == '__main__':
    main()
