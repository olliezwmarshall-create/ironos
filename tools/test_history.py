#!/usr/bin/env python3
"""Exercise command recall, parser preservation, drafts and wrapped input in QEMU."""
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
    with tempfile.TemporaryDirectory(prefix='ironos-history-', dir='/tmp') as tmp:
        work = Path(tmp)
        with (work / 'target.img').open('wb') as f:
            f.truncate(128 * 1024 * 1024)
        subprocess.run(['python3', 'mkdisk.py', str(work / 'data.img'), 'disk'], check=True)
        vm = VM(args, work)
        def key(name):
            vm.hmp('sendkey ' + name)
            time.sleep(.2)
        def ends(text):
            assert vm.screen().rstrip().endswith(text), vm.screen()
        try:
            vm.expect('kernel:/$>')
            vm.send('echo first')
            vm.send('echo second')
            key('up'); ends('echo second')
            key('up'); ends('echo first')
            # The CLI parser must not have overwritten saved arguments.
            before = vm.screen().count('first')
            key('ret')
            assert vm.screen().count('first') > before
            for ch in 'echo draft':
                key('spc' if ch == ' ' else ch)
            key('up'); ends('echo first')
            key('down'); ends('echo draft')
            key('backspace'); ends('echo draf')
            key('ret'); ends('kernel:/$>')
            long_command = 'echo ' + 'x' * 74
            vm.send(long_command)
            key('up')
            assert long_command in vm.screen().replace('\n', ''), vm.screen()
            key('backspace')
            key('ret'); ends('kernel:/$>')
            vm.send('memtest')
            vm.expect('memtest: PASSED')
        finally:
            vm.close()
    print('PASS: command recall, retained arguments, draft restoration, editing, wrapped input and memory test')


if __name__ == '__main__':
    main()
