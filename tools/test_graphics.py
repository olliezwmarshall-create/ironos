#!/usr/bin/env python3
"""Test VGA mode switching, held movement, collision, and terminal restoration."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import time
from test_install import VM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu', default='qemu-system-x86_64')
    ap.add_argument('--firmware', action='append', default=[])
    ap.add_argument('--screenshot')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='ironos-graphics-', dir='/tmp') as tmp:
        work = Path(tmp)
        with (work / 'target.img').open('wb') as f:
            f.truncate(128 * 1024 * 1024)
        subprocess.run(['python3', 'mkdisk.py', str(work / 'data.img'), 'disk'], check=True)
        vm = VM(args, work)
        def frame():
            path = work / 'frame.bin'
            vm.hmp(f'pmemsave 0xa0000 64000 "{path}"')
            return path.read_bytes()
        try:
            vm.expect('kernel:/$>')
            vm.send('demo3d'); time.sleep(.5)
            initial = frame()
            assert len(set(initial)) >= 6, 'Scene did not render'
            # Test rotation in an open area; a wall filling the whole view can
            # produce identical flat-colour frames even while the camera turns.
            vm.hmp('sendkey d 700'); time.sleep(1.0)
            assert frame() != initial, 'Turning did not rotate scene'
            vm.hmp('sendkey esc'); time.sleep(.2)
            vm.expect('3D demo closed.')
            vm.send('demo3d'); time.sleep(.3)
            initial = frame()
            vm.hmp('sendkey w 500'); time.sleep(.8)
            moved = frame()
            assert moved != initial, 'Held movement did not change camera'
            time.sleep(.2)
            assert frame() == moved, 'Movement continued after key release'
            if args.screenshot:
                vm.hmp(f'screendump "{args.screenshot}"')
            vm.hmp('sendkey w 3000'); time.sleep(3.3)
            blocked = frame()
            vm.hmp('sendkey w 1000'); time.sleep(1.3)
            assert frame() == blocked, 'Player crossed a wall'
            vm.hmp('sendkey esc'); time.sleep(.3)
            vm.expect('3D demo closed.')
            vm.send('cat hello.txt'); vm.expect('Hello from the IronOS disk!')
            vm.send('memtest'); vm.expect('memtest: PASSED')
            vm.send('desktop'); vm.expect('IronOS Desktop')
            vm.hmp('sendkey t'); time.sleep(.2)
            vm.expect('kernel:/$>')
            # Repeat a mode switch to catch damaged register/font restoration.
            vm.send('demo3d'); time.sleep(.3)
            assert len(set(frame())) >= 6
            vm.hmp('sendkey esc'); time.sleep(.3)
            vm.expect('3D demo closed.')
        finally:
            vm.close()
    print('PASS: rendered scene, held input, release, wall collision, turning, repeated')
    print('      VGA switching, terminal/file access, memory test and desktop restoration')


if __name__ == '__main__':
    main()
