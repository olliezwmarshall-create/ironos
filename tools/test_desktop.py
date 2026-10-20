#!/usr/bin/env python3
"""QEMU functional test for the text desktop; run after make."""
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
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='ironos-desktop-', dir='/tmp') as tmp:
        work = Path(tmp)
        with (work / 'target.img').open('wb') as f:
            f.truncate(128 * 1024 * 1024)
        subprocess.run(['python3', 'mkdisk.py', str(work / 'data.img'), 'disk'], check=True)
        vm = VM(args, work)
        try:
            vm.expect('kernel:/$>')
            vm.send('textdesk')
            vm.expect('IronOS Desktop')
            vm.hmp('sendkey g'); time.sleep(.5)
            vm.hmp('sendkey esc'); time.sleep(.3)
            vm.expect('IronOS Desktop')
            vm.hmp('sendkey s'); time.sleep(.2)
            vm.expect('Memory total:')
            vm.expect('Filesystem: read-only')
            vm.hmp('sendkey f'); time.sleep(.2)
            vm.expect('hello.txt')
            vm.hmp('sendkey down'); time.sleep(.2); vm.hmp('sendkey down')
            # Wait between key presses to avoid overlapping PS/2 make/break events.
            time.sleep(.2)
            vm.hmp('sendkey ret')
            vm.expect('Hello from the IronOS disk!')
            vm.hmp('sendkey esc'); time.sleep(.2)
            vm.hmp('sendkey up'); time.sleep(.2)
            vm.hmp('sendkey ret')
            vm.expect('notes.txt')
            vm.hmp('sendkey t'); time.sleep(.2)
            vm.expect('kernel:/$>')
            vm.send('cat hello.txt')
            vm.expect('Hello from the IronOS disk!')
            vm.send('memtest')
            vm.expect('memtest: PASSED')
        finally:
            vm.close()
    print('PASS: desktop launcher, system panel, file preview, directory navigation,')
    print('      terminal return, working-directory restoration and memory test')


if __name__ == '__main__':
    main()
