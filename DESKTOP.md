# Graphical desktop and PS/2 mouse

Run `make run`, then type `desktop` (or `gui`) in IronOS. The new graphical
shell uses the existing 320x200 VGA graphics backend, with a software cursor,
clickable launcher, file browser and system panel. It remains CPU-rendered;
it does not require a GPU acceleration driver. The older text interface is
still available with `textdesk`.

## Mouse controls

- Click **Home**, **Files** or **System** in the sidebar to change panels.
- In Files, click a row to select it, then **Open**, or double-click the row.
  Folders are marked with `>`. Files open in a read-only text preview.
- **Up/Back**, **Prev** and **Next** navigate folders/previews and pages.
  Right-click in Files also goes back/up.
- **Demo3D** opens the first-person demo. Move the mouse to turn; Escape
  returns to the graphical shell. WASD and Q/E controls remain available.
- **Shell** returns to the command terminal. The window's X returns to Home,
  or to the terminal if Home is already open.

QEMU's standard PC provides a PS/2 mouse by default. Click inside the QEMU
window to give it input; use QEMU's mouse-grab/release shortcut when needed.
The shared keyboard/mouse controller routes auxiliary bytes separately so
pointer packets cannot become typed characters. Initialization resets wheel
protocols to the standard three-byte mode and only unmasks IRQ12/cascade after
successful setup. A missing mouse leaves the keyboard usable.

**Real-hardware support is limited to a standard PS/2 mouse or compatible
PS/2 touchpad.** Native USB mice and I2C touchpads need separate drivers.
Some firmware supplies USB legacy emulation, but this has not been validated.
The interface falls back to keyboard controls when no PS/2 mouse is detected.
VGA/legacy BIOS requirements are unchanged; real hardware still needs testing.

## Keyboard controls

- **H / F / S**: Home / Files / System. **Tab** cycles those panels.
- **G**: 3D demo. **T**, **Q**, or **Escape**: terminal. Escape in a preview
  first returns to the file list.
- **Up/Down** selects a file, **Enter** opens it, **Backspace** goes up/back.
  **PgUp/PgDn** pages; **Home/End** selects the first/last entry.

The graphical file browser displays up to 128 entries, ten per page. Previews
show the first 8 KiB and replace binary bytes with dots. It does not write to
the filesystem, and restores the terminal's working directory when closed.
The system panel shows memory, storage and Ethernet link state; a link being
up does not mean DHCP is configured. Network polling continues in the shell.

This is a cooperative, single-window shell, not a multitasking window manager.
It safely leaves graphics mode before launching the demo and restores the text
font and palette between applications. Terminal scrollback is preserved.

## Terminal and tests

Up/Down recalls the last 32 nonempty commands. Down past the newest restores
unfinished input; commands can be edited before Enter. History lasts until
reboot. PgUp/PgDn scrolls output; recent output is retained after 1,000 lines.

`make test-mouse-host` tests PS/2 initialization/failure, interleaved keyboard
and mouse packets, signed motion, clipping, click edges and queue overflow
with sanitizers. `make test-gui` uses actual QMP mouse events and framebuffer
pixel/font checks for clicking, previews, mode restoration and mouse-look;
it also verifies inbound networking while the graphical shell is open.
`make test-desktop` still tests the text desktop; `make test-history` tests
command recall. The automated QEMU tests require native Linux QEMU.
