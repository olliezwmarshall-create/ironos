# First-person graphics foundation

Run `make run`, then type `demo3d` in IronOS.

- Hold **W/S** to move forwards/backwards.
- Hold **A/D** or the PS/2 mouse to turn; **Q/E** strafe.
- **Escape** returns to the terminal and restores VGA text mode and its font.

This is a real-time software raycaster with a 320x200, 256-colour VGA
framebuffer, a RAM back buffer, fixed-point camera/projection, wall collision,
a directional minimap and a crosshair. Brick patterns, distance/side shading,
coordinated colours and an on-screen control legend improve readability.
Diagonal movement is normalized to avoid a speed boost. Input tracks key presses and releases so movement
continues while a key is held. Rendering targets roughly 33 frames/sec on the
100 Hz timer; frame timing compensates for moderate slowdowns. It is a graphics
prototype running inside the kernel, not a separate game process. Requires
standard VGA-compatible hardware; QEMU's `-vga std` is supported by `make run`.
Modern UEFI framebuffer-only systems are not supported by this VGA backend.

`make test-graphics` exercises actual rendering, movement, key release,
collision, turning, repeated graphics/text mode switches, and returning to
file access, the memory test and the desktop in disposable QEMU disks.

## Toward Counter-Strike

This adds useful game infrastructure, but it does not run Counter-Strike or
provide its engine/API. A realistic next major milestone is running external
programs in protected user space: paging, an executable loader, system calls,
processes and scheduling. Networking, audio and a much richer graphics/API
implementation must follow. Existing Linux or Windows games also require
compatible application interfaces and libraries. CS2 additionally requires a
64-bit OS and modern GPU support; this 32-bit kernel cannot run it as-is.
CS 1.6 has lower hardware requirements but still needs those application and
driver foundations. Supporting a small independent game first is a practical
way to validate each layer.

The shared VGA backend now also drives the graphical `desktop` shell.
