# IronOS desktop

After `make run`, type `desktop` in the IronOS shell. This is a lightweight
keyboard-driven desktop using the existing VGA text display, with no extra
host dependencies. It works in both the live and installed OS.

- **H**: home/launcher.
- **F**: files on the currently mounted FAT volume. Up/Down selects an entry;
  Enter opens a directory or a text preview; Backspace opens the parent.
  PgUp/PgDn jumps by 16 entries; Home/End selects the first/last entry.
  Long names are supported. Up to 128 entries are shown per directory.
- **G**: launch the 3D demo; Escape returns to the desktop.
- **S**: total/free physical memory, disk and volume counts, mount status.
- **Tab**: cycle Home, Files and System.
- **T**, **Q**, or **Escape**: return to the terminal. In a file preview,
  Escape first returns to the file list; PgUp/PgDn scrolls the preview.

The top bar shows live uptime. File previews show the first 8 KiB, replace
non-printing bytes with dots and do not modify files. The browser uses the
current mount permissions and restores the terminal's working directory on
exit. The terminal's scrollback is preserved.

This is a text-mode desktop, not a graphical window manager. Networking and
mouse support are not implemented in this change.

Validation: `make test-desktop` runs a disposable QEMU VM and exercises the
launcher, system panel, file previews, directory navigation, working-directory
restoration, terminal return and the existing kernel memory test. Requires
native Linux QEMU, just like `make test-install`.

## Terminal command history

In the terminal, Up/Down recalls the last 32 nonempty commands; consecutive
duplicates are stored once. Down past the newest entry restores the input
you were typing. Recalled commands can be edited before pressing Enter.
History lasts until reboot. PgUp/PgDn still scroll terminal output.
Run `make test-history` for the QEMU regression test.

Terminal output now rolls over safely after 1000 lines, retaining recent output.
