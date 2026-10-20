# Running IronOS on a real computer (e.g. an old Sony VAIO)

## 1. Get it booting
`make` produces `ironos.iso`.
* **CD-R/CD-RW (safest for old machines):** burn `ironos.iso` as an *image* (not as a file on a data disc).
* **USB stick:** the ISO is hybrid, so writing it to a stick in "DD/raw image" mode usually works
  (Rufus: choose "DD Image mode"; or balenaEtcher). Very old BIOSes often can't boot USB at all - use a CD.
  **Writing an image to a USB stick erases the stick.**
* Press the BIOS/boot-menu key at power-on (on VAIOs: **F2** for BIOS setup, **F11** or **Esc/F12** for the boot menu on some models)
  and boot from the CD/USB.

## 2. What it can read
| Works | Doesn't work yet |
|---|---|
| IDE/PATA hard disks, and SATA disks when the BIOS has an **IDE / "legacy" / compatibility** mode | SATA in **AHCI** mode, NVMe, USB sticks, SD cards |
| FAT16 / FAT32 (also read-only FAT12), MBR partitions incl. extended/logical | **NTFS** (Windows XP/Vista/7 default!), exFAT, ext4, GPT-only disks |
| Disks up to 128 GB (the first 128 GB of bigger ones) | CD/DVD data discs |

Type `disks` to see what it found. Type `lspci` to see the machine's controllers
(for IDE it says `legacy ports` = supported, `native mode` = probably not yet; for SATA it says AHCI or not).
If the VAIO's Windows partition shows as `NTFS (not supported)`, IronOS can't read it - an NTFS driver is a possible later step.
A FAT32 partition, or a spare drive formatted FAT32, works today.

## 3. Staying safe with real data
* The disk is mounted **read-only** at boot. Nothing can be changed until you type `mount <n> rw`.
* Before enabling writing on a disk you care about: **back it up** (or practice on a spare disk / a copy).
* Don't power off or reset while a file is being saved; use `shutdown` / `reboot`, then (if you used it on a Windows disk)
  let Windows run its disk check (chkdsk/scandisk) on the next boot.
* The write code has been tested hard on disk images, but it has *not* been tested on real hardware yet.
  Treat the first real-disk writes as an experiment.

## 4. Checking an image from your PC
`python3 tools/fsck.py disk.img` verifies a FAT disk image (FAT copies, chains, long names, lost clusters).
Run it after editing files in QEMU to confirm the image is healthy.

## Step 5: disk controllers
- IDE/PATA (legacy and PCI native mode), 28-bit and 48-bit LBA (disks over 128 GB).
- SATA via AHCI (polling). If the BIOS is set to "IDE/compatibility" mode the disk shows up as IDE instead; both work.
- NVMe SSDs (512-byte sectors).
- Partition tables: bare FAT, MBR (primary + extended/logical), GPT (CRC checked, backup header fallback).
- Limits: 2 TB per disk, 4Kn disks and USB storage are not supported, wireless/USB/RAID-mode (Intel RST) controllers are not supported.
- Verified only against emulated controllers on a host PC, never booted on real hardware. Report problems!

## Installing from the live system

See [INSTALL.md](INSTALL.md) for the BIOS/legacy installer, disk erase confirmation,
WSL `make run` workflow, and booting the installed system with `make run-installed`.

## Networking

PCI RTL8139 Ethernet is supported through its legacy interface. See
[NETWORK.md](NETWORK.md) for DHCP, DNS, ping, QEMU NAT and current limitations.
Wi-Fi and USB network adapters still need separate drivers.

## Graphical shell and mouse

`desktop` opens the VGA graphical shell; `textdesk` retains the text interface.
Standard PS/2 mice are supported. Native USB mice and I2C touchpads are not.
See [DESKTOP.md](DESKTOP.md) for controls and hardware limitations.
