IronOS sample disk
==================
This folder becomes the disk image: run 'make disk' after changing it.
Try these commands in the kernel:
  ls
  cat hello.txt
  cd docs
  ls
  cat notes.txt
  cd ..

New in this version
-------------------
The disk starts READ-ONLY. To change files, enable writing first:
  disks               (list disks and partitions)
  mount 0 rw          (the prompt now ends in # instead of $)

Then you can edit and manage files:
  edit hello.txt      (full-screen editor: Ctrl+S saves, Ctrl+Q quits)
  edit newfile.txt    (creates a new file when you save)
  echo some text > a.txt       ('>>' appends instead)
  mkdir stuff   touch b.txt   cp a.txt stuff   mv b.txt stuff/c.txt
  rm a.txt   rmdir stuff   df   hexdump hello.txt

Machine tools: cpuinfo, lspci, meminfo, date, shutdown.
Use "double quotes" around file names that contain spaces.

Step 5: IronOS can now read IDE, SATA (AHCI) and NVMe disks, with MBR or GPT partitions.
