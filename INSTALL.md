# Install IronOS after live boot

This installer supports **BIOS/legacy boot** on disks supported by IronOS
(IDE, AHCI SATA, or NVMe with 512-byte sectors). The firmware must also be able
to boot the chosen disk in legacy mode. UEFI-only boot and USB disk drivers
are not implemented. You can still live boot from your existing flash-drive
image: GRUB loads the installation payload into RAM before starting IronOS.
The flash-drive conversion/burning process stays the same.

## Build and try it in WSL

On Debian/Ubuntu WSL, install the build prerequisites:

```sh
sudo apt-get update
sudo apt-get install build-essential binutils grub-pc-bin grub-common xorriso python3 qemu-system-x86
make run
```

`make run` builds the live ISO, retains the existing `disk.img` as the data disk,
and creates a separate **128 MiB `install-disk.img`** if it does not exist.
QEMU gets 256 MiB RAM. Native QEMU works with WSLg. The makefile also detects
QEMU's Windows executable on PATH and retains the previous
`/mnt/d/mysys2/ucrt64/bin/qemu-system-x86_64.exe` path as a fallback.
You can select your Windows installation explicitly:

```sh
make run QEMU=/mnt/c/Program\ Files/qemu/qemu-system-x86_64.exe
```

Keep the checkout on a filesystem accessible to that QEMU installation.
`QEMU_FLAGS` can select display options, e.g. `QEMU_FLAGS='-display sdl'`.
The live ISO contains an additional roughly 65 MiB installation payload;
use at least 128 MiB RAM (256 MiB recommended).

## Install from the live shell

1. Type `disks` and identify the **disk name**, model and capacity. Disk names
   such as `ide1` are different from the numbered FAT volumes used by `mount`.
2. Type `install ide1`, substituting the disk you actually want to replace.
3. Read the target summary. To proceed, type exactly `install ide1 ERASE`.
   An unconfirmed request never writes to the disk. Run `install ide1` again
   if you cancel or enter a different install command.
4. Wait for `Installation complete (verified)`. Do not interrupt the writes.
5. Type `shutdown`, remove the live media, and select the installed disk in
   your BIOS boot menu. In QEMU, exit the live VM and run `make run-installed`.

**Installation destroys the selected disk's partition layout and makes all
existing data inaccessible. Back up the disk first.** It creates an active
64 MiB FAT16 partition with the kernel and files from `disk/`, plus a GRUB
BIOS bootloader in the preceding 1 MiB area. Space beyond that is unused.
It is not a secure erase, an upgrade, or a dual-boot installer. It does not
copy changes made to another live-mounted data disk; the payload is generated
from the source tree at build time. `make clean` preserves `install-disk.img`.

The installer verifies the payload CRC before writing, reads back every
written block, and writes the MBR last. I/O failures stop the operation and
report an incomplete installation; this is not a power-loss-safe transaction.
After raw writes the old FAT mount is dropped, even on failure; use `disks`
and `mount <volume>` to inspect the target. Disks still mount read-only at boot.
The installed kernel does not carry another installer payload; use live media
again to reinstall.

## Automated validation

The automated test requires native Linux QEMU in WSL; Windows QEMU can still
be used for the interactive `make run` workflow. After `make`, run:

```sh
make test-installer-host
make test-install
```

The test uses disposable disk files, invokes `make run`, checks refusal of
unconfirmed/wrong/undersized targets, installs onto the separate target,
compares its bytes with the payload, runs the FAT checker, and invokes
`make run-installed` with no live ISO attached. It checks file reading and the
kernel memory test after disk-only boot. Optional `--qemu PATH` and repeated
`--firmware DIR` arguments support locally extracted QEMU installations.
Real hardware still needs separate validation.
