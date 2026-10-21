# Desktop, windows and apps

Run `make run`, then enter `desktop` (or `gui`). QEMU standard VGA uses an
800×600, 32-bit linear framebuffer through Bochs DISPI. Unsupported video
adapters retain the 320×200 VGA fallback. Drawing runs on the CPU; this does
not add GPU acceleration. The 3D demo keeps its existing 320×200 renderer.
`textdesk` remains available.

## Window controls

The top bar opens Home, Files, System, Network, Calc, Clock and Notes. Windows
stack and receive keyboard focus when clicked. Drag a title bar to move its
window; `_` minimizes and `X` closes. The bottom taskbar restores open windows.
Tab switches focus between visible windows. Window positions last until the
desktop is exited. Apps cooperate in one kernel event loop; this is not
process isolation, a userspace API or preemptive application multitasking.

Keyboard shortcuts: Ctrl+H Home, Ctrl+F Files, Ctrl+S System, Ctrl+N Network,
Ctrl+A Calc, Ctrl+C Clock, Ctrl+O Notes. **In Notes, Ctrl+S saves instead.**
Ctrl+G starts the 3D demo; Escape returns to the terminal (or backs out of a
file preview first). These controls work without a mouse.

- **Files:** select with click or Up/Down, then Enter/Open, or double-click.
  Back/Backspace goes up; Prev/Next or PgUp/PgDn changes pages. Lists show up
  to 128 entries; text previews read at most 8 KiB. The terminal's working
  directory is restored when leaving the desktop.
- **System:** memory, storage, uptime and the active video mode.
- **Network:** Ethernet driver/link, IP/mask/gateway/DNS and packet counts.
  D or the DHCP button obtains a lease; X/Disconnect clears IPv4 settings.
  DHCP temporarily returns to the terminal and then restores the desktop.
  See [NETWORK.md](NETWORK.md) for static settings and supported cards.
- **Calc:** signed 32-bit integer arithmetic with parentheses and + - * / %.
  Enter calculates; Ctrl+L clears. Invalid input, overflow and division by
  zero return an error. Also available as `calc (12 + 3) * 4` in the terminal.
- **Clock:** hardware RTC date/time and uptime; no time zone conversion/NTP.
- **Notes:** append text/newlines and erase with Backspace, up to 2,047 bytes.
  Text stays in RAM across window closure and desktop re-entry until reboot.
  Ctrl+S explicitly saves `desktop.txt` in the Files app's current folder.
  The disk must be mounted writable (`mount <n> rw`). Replacing an existing
  file requires a second Ctrl+S. The app does not automatically load a file;
  use `edit <file>` for the full-screen editor.

`apps` lists applications; `about` reports the main platform limitations.
Network polling continues while windows are open. Disk access, DHCP and other
blocking kernel operations can pause drawing.

## Hardware and validation

Only standard PS/2 mice and compatible PS/2 touchpads are supported. Native
USB mice, I2C touchpads and arbitrary GPU high-resolution modes need drivers.
The mouse bounds follow video mode changes; raw mouse motion still turns the
3D camera. Real hardware has not been validated. Legacy VGA/BIOS is required;
800×600 is currently supported on QEMU/Bochs-compatible DISPI video adapters.

`make test-gui` checks actual framebuffer pixels, QMP mouse dragging,
minimize/restore/close, app focus, calculator, notes permissions/saving, mode
restoration and incoming Ethernet traffic during desktop use.
`make test-mouse-host`, `make test-calc`, `make test-screen` use host sanitizers.
The QEMU tests use disposable disks and require native Linux QEMU.
