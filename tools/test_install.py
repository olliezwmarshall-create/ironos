#!/usr/bin/env python3
"""Install onto disposable QEMU disks, then boot with live media removed.
Run after make: python3 tools/test_install.py [--qemu /path/to/qemu] [--firmware DIR ...]
Requires QEMU QMP and a BIOS-capable x86 emulator. Never accesses host disks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


class VM:
    def __init__(self, args, work, installed=False):
        self.work = work
        self.sock = work / 'qmp.sock'
        self.sock.unlink(missing_ok=True)
        flags = ['-display', 'none', '-serial', 'none', '-nic', 'none', '-accel', 'tcg',
                 '-qmp', f'unix:{self.sock},server=on,wait=off', '-no-reboot']
        for firmware in args.firmware:
            flags += ['-L', firmware]
        cmd = ['make', 'run-installed' if installed else 'run', f'QEMU={args.qemu}',
               'QEMU_MEMORY=256', 'QEMU_NETWORK=' + getattr(args, 'network', ''), f'INSTALL_DISK={work}/target.img',
               f'DISK={work}/data.img', 'QEMU_FLAGS=' + ' '.join(flags)]
        self.log = open(work / ('installed.log' if installed else 'live.log'), 'w')
        self.p = subprocess.Popen(cmd, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.p.poll() is not None:
                raise RuntimeError('QEMU exited; inspect ' + self.log.name)
            self.s = socket.socket(socket.AF_UNIX)
            try:
                self.s.connect(str(self.sock))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                self.s.close()
                time.sleep(.1)
        else:
            self.p.terminate()
            raise TimeoutError('QMP socket')
        self.s.settimeout(10)
        self.f = self.s.makefile('rwb')
        self.f.readline()
        self.qmp('qmp_capabilities')

    def qmp(self, cmd, arguments=None):
        self.f.write((json.dumps({'execute': cmd, 'arguments': arguments or {}}) + '\n').encode())
        self.f.flush()
        while True:
            msg = json.loads(self.f.readline())
            if 'error' in msg:
                raise RuntimeError(msg)
            if 'return' in msg:
                return msg['return']

    def hmp(self, cmd):
        return self.qmp('human-monitor-command', {'command-line': cmd})

    def screen(self):
        output = self.work / 'vga.bin'
        self.hmp(f'pmemsave 0xb8000 4000 "{output}"')
        data = output.read_bytes()[::2]
        return '\n'.join(data[i:i+80].decode('cp437') for i in range(0, 2000, 80))

    def expect(self, text, timeout=45):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            screen = self.screen()
            if text in screen:
                return screen
            time.sleep(.2)
        raise AssertionError(f'Expected {text!r}\n{screen}')

    def send(self, cmd):
        for c in cmd + '\n':
            key = {' ': 'spc', '\n': 'ret', '.': 'dot'}.get(c, c)
            if c.isupper():
                key = 'shift-' + c.lower()
            self.hmp('sendkey ' + key)
            time.sleep(.07)
        time.sleep(.2)

    def close(self):
        try:
            self.qmp('quit')
        finally:
            self.f.close()
            self.s.close()
            self.p.wait(timeout=10)
            self.log.close()


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu', default='qemu-system-x86_64')
    ap.add_argument('--firmware', action='append', default=[])
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='ironos-install-', dir='/tmp') as temp:
        work = Path(temp)
        target = work / 'target.img'
        with target.open('wb') as f:
            f.truncate(128 * 1024 * 1024)
        subprocess.run(['python3', 'mkdisk.py', str(work / 'data.img'), 'disk'], check=True)
        original = digest(target)
        vm = VM(args, work)
        try:
            vm.expect('kernel:/$>')
            vm.send('install ide1 ERASE')
            vm.expect('Not confirmed.')
            assert digest(target) == original
            vm.send('install ide0')
            vm.expect('Disk too small')
            vm.send('install missing')
            vm.expect('Not a writable disk')
            vm.send('install ide1')
            vm.expect('To confirm type: install ide1 ERASE')
            assert digest(target) == original
            vm.send('install ide1 no')
            vm.expect('Not confirmed.')
            assert digest(target) == original
            vm.send('clear')
            vm.send('install ide1')
            vm.expect('To confirm type: install ide1 ERASE')
            vm.send('install ide1 ERASE')
            print(vm.expect('Installation complete (verified).', timeout=120))
        finally:
            vm.close()
        payload = Path('build/install.img').read_bytes()[16:]
        with target.open('rb') as f:
            assert f.read(len(payload)) == payload, 'Installed image differs from payload'
        subprocess.run(['python3', 'tools/fsck.py', str(target)], check=True)
        vm = VM(args, work, installed=True)
        try:
            vm.expect('kernel:/$>')
            vm.send('cat hello.txt')
            vm.expect('Hello from the IronOS disk!')
            vm.send('memtest')
            vm.expect('memtest: PASSED')
            vm.send('install')
            vm.expect('Installer unavailable. Boot the live ISO')
        finally:
            vm.close()
        print('PASS: confirmation guards, target-size checks, verified installation, FAT consistency,')
        print('      disk-only boot, file reading, memory test and installed-mode guard')


if __name__ == '__main__':
    main()
